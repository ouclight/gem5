#include <gem5/m5ops.h>

#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef RESIDENT_FLAG_HSACO_PATH
#define RESIDENT_FLAG_HSACO_PATH "resident_flag_kernels.hsaco"
#endif

namespace
{

constexpr uint32_t OldValue = 0x11112222u;
constexpr uint32_t UpdatedValue = 0x33334444u;
constexpr uint32_t DefaultSpinLimit = 100000000u;
constexpr uint32_t SameLineValueDword = 0;
constexpr uint32_t SameLineFlagDword = 1;
constexpr uint32_t SplitLineValueDword = 0;
constexpr uint32_t SplitLineFlagDword = 32;
constexpr size_t TargetDwords = 64;
constexpr size_t TargetBytes = TargetDwords * sizeof(uint32_t);
constexpr uint64_t QueueSize = 64;

enum ResidentFlagLayout
{
    LayoutSameLine,
    LayoutSplitLine,
};

enum ResidentFlagSync
{
    SyncStrict = 0,
    SyncWriterFenceOnly = 1,
    SyncNoFence = 2,
};

enum ResidentFlagClassification : uint32_t
{
    ResidentFlagUnset = 0,
    ResidentFlagUpdated = 1,
    ResidentFlagStaleValue = 2,
    ResidentFlagTimeout = 3,
    ResidentFlagUnexpected = 4,
};

struct ResidentFlagControl
{
    volatile uint32_t first_read_done;
    volatile uint32_t reader_done;
    volatile uint32_t first_value;
    volatile uint32_t second_value;
    volatile uint32_t seen_flag;
    volatile uint32_t spins;
    volatile uint32_t classification;
};

struct Options
{
    bool reverse = false;
    ResidentFlagLayout layout = LayoutSameLine;
    ResidentFlagSync sync = SyncStrict;
    uint32_t spin_limit = DefaultSpinLimit;
};

struct AgentSearch
{
    hsa_agent_t cpu = {};
    std::vector<hsa_agent_t> gpus;
};

struct PoolSearch
{
    hsa_amd_memory_pool_t pool = {};
    size_t min_size = 0;
};

struct KernargRegion
{
    hsa_region_t region = {};
};

struct HsaKernel
{
    hsa_executable_symbol_t symbol = {};
    uint64_t object = 0;
    uint32_t kernarg_size = 0;
    uint32_t group_size = 0;
    uint32_t private_size = 0;
};

struct PendingDispatch
{
    hsa_signal_t completion = {};
    void *kernarg = nullptr;
};

const char *
status_name(hsa_status_t status)
{
    const char *name = nullptr;
    if (hsa_status_string(status, &name) == HSA_STATUS_SUCCESS && name) {
        return name;
    }
    return "unknown HSA error";
}

void
print_status(const char *label, hsa_status_t status)
{
    std::printf("[hsa_remote_shader_resident_flag] %s: %s(%d)\n",
                label, status_name(status), status);
    std::fflush(stdout);
}

void
usage(const char *argv0)
{
    std::fprintf(stderr,
        "usage: %s [--reverse] [--layout same-line|split-line] "
        "[--sync strict|writer-fence-only|no-fence] [--spin-limit N]\n",
        argv0);
}

const char *
layout_name(ResidentFlagLayout layout)
{
    return layout == LayoutSplitLine ? "split-line" : "same-line";
}

const char *
sync_name(ResidentFlagSync sync)
{
    switch (sync) {
      case SyncStrict:
        return "strict";
      case SyncWriterFenceOnly:
        return "writer-fence-only";
      case SyncNoFence:
        return "no-fence";
    }
    return "unknown";
}

bool
parse_options(int argc, char **argv, Options *options)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--reverse") == 0) {
            options->reverse = true;
        } else if (std::strcmp(argv[i], "--layout") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return false;
            }
            if (std::strcmp(argv[i], "same-line") == 0) {
                options->layout = LayoutSameLine;
            } else if (std::strcmp(argv[i], "split-line") == 0) {
                options->layout = LayoutSplitLine;
            } else {
                usage(argv[0]);
                return false;
            }
        } else if (std::strcmp(argv[i], "--sync") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return false;
            }
            if (std::strcmp(argv[i], "strict") == 0) {
                options->sync = SyncStrict;
            } else if (std::strcmp(argv[i], "writer-fence-only") == 0) {
                options->sync = SyncWriterFenceOnly;
            } else if (std::strcmp(argv[i], "no-fence") == 0) {
                options->sync = SyncNoFence;
            } else {
                usage(argv[0]);
                return false;
            }
        } else if (std::strcmp(argv[i], "--spin-limit") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return false;
            }
            char *end = nullptr;
            const unsigned long value = std::strtoul(argv[i], &end, 0);
            if (end == argv[i] || *end != '\0' || value == 0 ||
                value > UINT32_MAX) {
                usage(argv[0]);
                return false;
            }
            options->spin_limit = static_cast<uint32_t>(value);
        } else {
            usage(argv[0]);
            return false;
        }
    }
    return true;
}

bool
read_file(const char *path, std::vector<uint8_t> &data)
{
    std::FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        std::perror(path);
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::perror("fseek");
        std::fclose(file);
        return false;
    }
    const long size = std::ftell(file);
    if (size <= 0) {
        std::fprintf(stderr,
            "[hsa_remote_shader_resident_flag] invalid HSACO size %ld\n",
            size);
        std::fclose(file);
        return false;
    }
    if (std::fseek(file, 0, SEEK_SET) != 0) {
        std::perror("fseek");
        std::fclose(file);
        return false;
    }

    data.resize(static_cast<size_t>(size));
    const size_t bytes = std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    if (bytes != data.size()) {
        std::fprintf(stderr,
            "[hsa_remote_shader_resident_flag] read %zu of %zu HSACO bytes\n",
            bytes, data.size());
        return false;
    }
    return true;
}

template <typename T>
size_t
append_arg(uint8_t *buffer, size_t offset, const T &value)
{
    const size_t align = alignof(T);
    offset = (offset + align - 1) & ~(align - 1);
    std::memcpy(buffer + offset, &value, sizeof(T));
    return offset + sizeof(T);
}

hsa_status_t
find_cpu_and_gpus(hsa_agent_t agent, void *data)
{
    auto *search = static_cast<AgentSearch *>(data);

    hsa_device_type_t type = HSA_DEVICE_TYPE_CPU;
    hsa_status_t status = hsa_agent_get_info(
        agent, HSA_AGENT_INFO_DEVICE, &type);
    if (status != HSA_STATUS_SUCCESS) {
        return status;
    }

    char name[64] = {};
    status = hsa_agent_get_info(agent, HSA_AGENT_INFO_NAME, name);
    if (status != HSA_STATUS_SUCCESS) {
        return status;
    }

    std::printf("[hsa_remote_shader_resident_flag] agent handle=0x%lx "
                "type=%d name=%s\n",
                agent.handle, static_cast<int>(type), name);
    std::fflush(stdout);

    if (type == HSA_DEVICE_TYPE_CPU && search->cpu.handle == 0) {
        search->cpu = agent;
    } else if (type == HSA_DEVICE_TYPE_GPU) {
        search->gpus.push_back(agent);
    }
    return HSA_STATUS_SUCCESS;
}

hsa_status_t
find_allocable_global_pool(hsa_amd_memory_pool_t pool, void *data)
{
    auto *search = static_cast<PoolSearch *>(data);
    if (search->pool.handle != 0) {
        return HSA_STATUS_SUCCESS;
    }

    hsa_amd_segment_t segment = HSA_AMD_SEGMENT_GLOBAL;
    hsa_status_t status = hsa_amd_memory_pool_get_info(
        pool, HSA_AMD_MEMORY_POOL_INFO_SEGMENT, &segment);
    if (status != HSA_STATUS_SUCCESS) {
        return HSA_STATUS_SUCCESS;
    }

    bool alloc_allowed = false;
    status = hsa_amd_memory_pool_get_info(
        pool, HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_ALLOWED,
        &alloc_allowed);
    if (status != HSA_STATUS_SUCCESS) {
        return HSA_STATUS_SUCCESS;
    }

    size_t size = 0;
    hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_SIZE, &size);

    uint32_t flags = 0;
    hsa_amd_memory_pool_get_info(
        pool, HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS, &flags);

    std::printf("[hsa_remote_shader_resident_flag] pool handle=0x%lx "
                "segment=%d alloc=%d size=%zu global_flags=0x%x\n",
                pool.handle, static_cast<int>(segment), alloc_allowed,
                size, flags);
    std::fflush(stdout);

    if (segment == HSA_AMD_SEGMENT_GLOBAL && alloc_allowed &&
        size >= search->min_size) {
        search->pool = pool;
    }
    return HSA_STATUS_SUCCESS;
}

hsa_status_t
find_kernarg_region_cb(hsa_region_t region, void *data)
{
    auto *result = static_cast<KernargRegion *>(data);
    hsa_region_segment_t segment = HSA_REGION_SEGMENT_GLOBAL;
    hsa_region_get_info(region, HSA_REGION_INFO_SEGMENT, &segment);
    if (segment != HSA_REGION_SEGMENT_GLOBAL) {
        return HSA_STATUS_SUCCESS;
    }

    uint32_t flags = 0;
    hsa_region_get_info(region, HSA_REGION_INFO_GLOBAL_FLAGS, &flags);
    bool alloc_allowed = false;
    hsa_region_get_info(region, HSA_REGION_INFO_RUNTIME_ALLOC_ALLOWED,
                        &alloc_allowed);
    if (alloc_allowed && (flags & HSA_REGION_GLOBAL_FLAG_KERNARG)) {
        result->region = region;
        return HSA_STATUS_INFO_BREAK;
    }
    return HSA_STATUS_SUCCESS;
}

int
load_hsa_kernel(hsa_executable_t executable, hsa_agent_t agent,
                const char *name, HsaKernel *kernel)
{
    hsa_status_t status = hsa_executable_get_symbol_by_name(
        executable, name, &agent, &kernel->symbol);
    if (status != HSA_STATUS_SUCCESS) {
        char kd_name[128] = {};
        std::snprintf(kd_name, sizeof(kd_name), "%s@kd", name);
        status = hsa_executable_get_symbol_by_name(
            executable, kd_name, &agent, &kernel->symbol);
    }
    if (status != HSA_STATUS_SUCCESS) {
        print_status("hsa_executable_get_symbol_by_name", status);
        return 1;
    }

    status = hsa_executable_symbol_get_info(
        kernel->symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT,
        &kernel->object);
    if (status != HSA_STATUS_SUCCESS) {
        print_status("kernel object lookup", status);
        return 1;
    }
    status = hsa_executable_symbol_get_info(
        kernel->symbol,
        HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE,
        &kernel->kernarg_size);
    if (status != HSA_STATUS_SUCCESS) {
        print_status("kernel kernarg size lookup", status);
        return 1;
    }
    status = hsa_executable_symbol_get_info(
        kernel->symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE,
        &kernel->group_size);
    if (status != HSA_STATUS_SUCCESS) {
        print_status("kernel group size lookup", status);
        return 1;
    }
    status = hsa_executable_symbol_get_info(
        kernel->symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE,
        &kernel->private_size);
    if (status != HSA_STATUS_SUCCESS) {
        print_status("kernel private size lookup", status);
        return 1;
    }

    std::printf("[hsa_remote_shader_resident_flag] kernel %s object=0x%lx "
                "kernarg=%u group=%u private=%u\n",
                name, kernel->object, kernel->kernarg_size,
                kernel->group_size, kernel->private_size);
    std::fflush(stdout);
    return 0;
}

hsa_status_t
wait_for_signal_lt_one(const char *label, hsa_signal_t signal)
{
    std::printf("[hsa_remote_shader_resident_flag] wait %s completion\n",
                label);
    std::fflush(stdout);
    hsa_signal_value_t value = hsa_signal_wait_scacquire(
        signal, HSA_SIGNAL_CONDITION_LT, 1, UINT64_MAX,
        HSA_WAIT_STATE_BLOCKED);
    if (value < 1) {
        return HSA_STATUS_SUCCESS;
    }
    std::fprintf(stderr,
        "[hsa_remote_shader_resident_flag] %s completion wait returned %ld\n",
        label, static_cast<long>(value));
    return HSA_STATUS_ERROR;
}

bool
wait_for_control_field(const char *label, volatile uint32_t *field,
                       uint32_t expected)
{
    constexpr uint64_t HostPollLimit = 1000000000ull;
    for (uint64_t i = 0; i < HostPollLimit; ++i) {
        if (*field == expected) {
            std::printf("[hsa_remote_shader_resident_flag] observed %s=%u "
                        "after %lu host polls\n",
                        label, expected, i);
            std::fflush(stdout);
            return true;
        }
    }
    std::fprintf(stderr,
        "[hsa_remote_shader_resident_flag] timeout waiting for %s=%u, "
        "current=%u\n",
        label, expected, *field);
    return false;
}

hsa_status_t
allow_access(const char *label, const std::vector<hsa_agent_t> &agents,
             const void *ptr)
{
    std::printf("[hsa_remote_shader_resident_flag] begin %s allow access\n",
                label);
    std::fflush(stdout);
    hsa_status_t status = hsa_amd_agents_allow_access(
        agents.size(), agents.data(), nullptr, ptr);
    print_status("hsa_amd_agents_allow_access", status);
    return status;
}

hsa_status_t
async_copy_and_wait(const char *label,
                    void *dst, hsa_agent_t dst_agent,
                    const void *src, hsa_agent_t src_agent,
                    size_t bytes)
{
    hsa_signal_t signal = {};
    hsa_status_t status = hsa_signal_create(1, 0, nullptr, &signal);
    print_status("hsa_signal_create", status);
    if (status != HSA_STATUS_SUCCESS) {
        return status;
    }

    std::printf("[hsa_remote_shader_resident_flag] begin %s async copy "
                "bytes=%zu\n",
                label, bytes);
    std::fflush(stdout);
    status = hsa_amd_memory_async_copy(dst, dst_agent, src, src_agent, bytes,
                                       0, nullptr, signal);
    print_status("hsa_amd_memory_async_copy", status);
    if (status == HSA_STATUS_SUCCESS) {
        status = wait_for_signal_lt_one(label, signal);
        print_status("wait completion", status);
    }

    hsa_status_t destroy_status = hsa_signal_destroy(signal);
    print_status("hsa_signal_destroy", destroy_status);
    if (status == HSA_STATUS_SUCCESS) {
        status = destroy_status;
    }
    return status;
}

int
launch_kernel_async(hsa_queue_t *queue, const HsaKernel &kernel,
                    hsa_region_t kernarg_region, const void *args,
                    size_t args_size, PendingDispatch *pending)
{
    if (queue == nullptr || queue->base_address == nullptr ||
        queue->size == 0 || (queue->size & (queue->size - 1)) != 0) {
        std::fprintf(stderr,
                     "[hsa_remote_shader_resident_flag] invalid HSA queue\n");
        return 1;
    }

    const size_t kernarg_size = kernel.kernarg_size == 0 ?
        args_size : kernel.kernarg_size;
    hsa_status_t status = hsa_memory_allocate(
        kernarg_region, kernarg_size, &pending->kernarg);
    print_status("hsa_memory_allocate kernarg", status);
    if (status != HSA_STATUS_SUCCESS) {
        return 1;
    }
    std::memset(pending->kernarg, 0, kernarg_size);
    std::memcpy(pending->kernarg, args, args_size);

    status = hsa_signal_create(1, 0, nullptr, &pending->completion);
    print_status("hsa_signal_create dispatch", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_memory_free(pending->kernarg);
        pending->kernarg = nullptr;
        return 1;
    }

    const uint64_t index = hsa_queue_add_write_index_scacq_screl(queue, 1);
    auto *ring = static_cast<hsa_kernel_dispatch_packet_t *>(
        queue->base_address);
    hsa_kernel_dispatch_packet_t *packet = &ring[index & (queue->size - 1)];

    std::memset(packet, 0, sizeof(*packet));
    packet->setup = 1 << HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;
    packet->workgroup_size_x = 1;
    packet->workgroup_size_y = 1;
    packet->workgroup_size_z = 1;
    packet->grid_size_x = 1;
    packet->grid_size_y = 1;
    packet->grid_size_z = 1;
    packet->private_segment_size = kernel.private_size;
    packet->group_segment_size = kernel.group_size;
    packet->kernel_object = kernel.object;
    packet->kernarg_address = pending->kernarg;
    packet->completion_signal = pending->completion;

    const uint16_t header =
        (HSA_PACKET_TYPE_KERNEL_DISPATCH << HSA_PACKET_HEADER_TYPE) |
        (1 << HSA_PACKET_HEADER_BARRIER) |
        (HSA_FENCE_SCOPE_SYSTEM <<
            HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE) |
        (HSA_FENCE_SCOPE_SYSTEM <<
            HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
    __atomic_store_n(&packet->header, header, __ATOMIC_RELEASE);
    std::printf("[hsa_remote_shader_resident_flag] ring queue=%p index=%lu "
                "doorbell=0x%lx kernel=0x%lx\n",
                queue, index, queue->doorbell_signal.handle, kernel.object);
    std::fflush(stdout);
    hsa_signal_store_screlease(queue->doorbell_signal, index);
    return 0;
}

void
cleanup_dispatch(PendingDispatch *pending)
{
    if (pending->completion.handle != 0) {
        hsa_signal_destroy(pending->completion);
        pending->completion = {};
    }
    if (pending->kernarg != nullptr) {
        hsa_memory_free(pending->kernarg);
        pending->kernarg = nullptr;
    }
}

int
get_kernarg_region(hsa_agent_t agent, const char *label,
                   KernargRegion *region)
{
    hsa_status_t status = hsa_agent_iterate_regions(
        agent, find_kernarg_region_cb, region);
    if (status != HSA_STATUS_SUCCESS && status != HSA_STATUS_INFO_BREAK) {
        print_status(label, status);
        return 1;
    }
    if (region->region.handle == 0) {
        std::fprintf(stderr,
            "[hsa_remote_shader_resident_flag] no %s kernarg region\n",
            label);
        return 1;
    }
    return 0;
}

void
print_result_and_exit_if_complete(const Options &options,
                                  const ResidentFlagControl *control)
{
    const char *direction = options.reverse ? "reverse" : "forward";
    std::printf("RESIDENT_FLAG_RESULT direction=%s layout=%s sync=%s "
                "first=0x%08x second=0x%08x flag=%u spins=%u "
                "classification=%u\n",
                direction, layout_name(options.layout), sync_name(options.sync),
                control->first_value, control->second_value,
                control->seen_flag, control->spins, control->classification);

    switch (control->classification) {
      case ResidentFlagUpdated:
        std::printf("RESIDENT_FLAG_PASSED_UPDATED\n");
        std::fflush(stdout);
        m5_exit(0);
        break;
      case ResidentFlagStaleValue:
        std::printf("RESIDENT_FLAG_OBSERVED_STALE_VALUE\n");
        std::fflush(stdout);
        m5_exit(0);
        break;
      case ResidentFlagTimeout:
        std::printf("RESIDENT_FLAG_FLAG_TIMEOUT\n");
        break;
      default:
        std::printf("RESIDENT_FLAG_FAILED_UNEXPECTED\n");
        break;
    }
    std::fflush(stdout);
}

} // anonymous namespace

int
main(int argc, char **argv)
{
    Options options;
    if (!parse_options(argc, argv, &options)) {
        return 1;
    }

    const uint32_t value_dword = options.layout == LayoutSameLine ?
        SameLineValueDword : SplitLineValueDword;
    const uint32_t flag_dword = options.layout == LayoutSameLine ?
        SameLineFlagDword : SplitLineFlagDword;

    std::printf("[hsa_remote_shader_resident_flag] begin hsa_init()\n");
    std::fflush(stdout);
    hsa_status_t status = hsa_init();
    print_status("hsa_init", status);
    if (status != HSA_STATUS_SUCCESS) {
        return 1;
    }

    AgentSearch agents;
    status = hsa_iterate_agents(find_cpu_and_gpus, &agents);
    print_status("hsa_iterate_agents", status);
    if (status != HSA_STATUS_SUCCESS || agents.cpu.handle == 0 ||
        agents.gpus.size() < 2) {
        std::fprintf(stderr,
            "[hsa_remote_shader_resident_flag] need CPU and at least 2 GPUs\n");
        hsa_shut_down();
        return 1;
    }

    hsa_agent_t gpu0 = agents.gpus[0];
    hsa_agent_t gpu1 = agents.gpus[1];
    hsa_agent_t reader_agent = options.reverse ? gpu0 : gpu1;
    hsa_agent_t writer_agent = options.reverse ? gpu1 : gpu0;

    PoolSearch cpu_pool{ {}, sizeof(ResidentFlagControl) };
    status = hsa_amd_agent_iterate_memory_pools(
        agents.cpu, find_allocable_global_pool, &cpu_pool);
    print_status("CPU hsa_amd_agent_iterate_memory_pools", status);

    PoolSearch gpu0_pool{ {}, TargetBytes };
    status = hsa_amd_agent_iterate_memory_pools(
        gpu0, find_allocable_global_pool, &gpu0_pool);
    print_status("GPU0 hsa_amd_agent_iterate_memory_pools", status);

    PoolSearch gpu1_pool{ {}, TargetBytes };
    status = hsa_amd_agent_iterate_memory_pools(
        gpu1, find_allocable_global_pool, &gpu1_pool);
    print_status("GPU1 hsa_amd_agent_iterate_memory_pools", status);

    if (cpu_pool.pool.handle == 0 ||
        gpu0_pool.pool.handle == 0 ||
        gpu1_pool.pool.handle == 0) {
        std::fprintf(stderr,
            "[hsa_remote_shader_resident_flag] missing allocable pool\n");
        hsa_shut_down();
        return 1;
    }

    hsa_amd_memory_pool_t target_pool = options.reverse ?
        gpu0_pool.pool : gpu1_pool.pool;

    uint32_t *target = nullptr;
    ResidentFlagControl *control = nullptr;
    status = hsa_amd_memory_pool_allocate(
        target_pool, TargetBytes, 0, reinterpret_cast<void **>(&target));
    print_status("reader target hsa_amd_memory_pool_allocate", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_shut_down();
        return 1;
    }

    status = hsa_amd_memory_pool_allocate(
        cpu_pool.pool, sizeof(ResidentFlagControl), 0,
        reinterpret_cast<void **>(&control));
    print_status("CPU control hsa_amd_memory_pool_allocate", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    std::vector<hsa_agent_t> access_agents = {
        agents.cpu,
        gpu0,
        gpu1,
    };
    if (allow_access("target", access_agents, target) != HSA_STATUS_SUCCESS ||
        allow_access("control", access_agents, control) !=
            HSA_STATUS_SUCCESS) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    uint32_t initial[TargetDwords] = {};
    initial[value_dword] = OldValue;
    initial[flag_dword] = 0;
    std::memset(control, 0, sizeof(*control));
    __sync_synchronize();

    status = async_copy_and_wait(
        options.reverse ? "H2D_GPU0_INITIAL" : "H2D_GPU1_INITIAL",
        target, reader_agent, initial, agents.cpu, sizeof(initial));
    if (status != HSA_STATUS_SUCCESS) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    KernargRegion reader_kernarg;
    if (get_kernarg_region(reader_agent, "reader", &reader_kernarg) != 0) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }
    KernargRegion writer_kernarg;
    if (get_kernarg_region(writer_agent, "writer", &writer_kernarg) != 0) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    std::vector<uint8_t> hsaco;
    std::printf("[hsa_remote_shader_resident_flag] begin read HSACO %s\n",
                RESIDENT_FLAG_HSACO_PATH);
    std::fflush(stdout);
    if (!read_file(RESIDENT_FLAG_HSACO_PATH, hsaco)) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    hsa_code_object_reader_t code_reader = {};
    status = hsa_code_object_reader_create_from_memory(
        hsaco.data(), hsaco.size(), &code_reader);
    print_status("hsa_code_object_reader_create_from_memory", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    hsa_executable_t executable = {};
    status = hsa_executable_create_alt(
        HSA_PROFILE_FULL, HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT, nullptr,
        &executable);
    print_status("hsa_executable_create_alt", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    hsa_loaded_code_object_t reader_loaded = {};
    status = hsa_executable_load_agent_code_object(
        executable, reader_agent, code_reader, nullptr, &reader_loaded);
    print_status("hsa_executable_load_agent_code_object reader", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    hsa_loaded_code_object_t writer_loaded = {};
    status = hsa_executable_load_agent_code_object(
        executable, writer_agent, code_reader, nullptr, &writer_loaded);
    print_status("hsa_executable_load_agent_code_object writer", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    status = hsa_executable_freeze(executable, nullptr);
    print_status("hsa_executable_freeze", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    HsaKernel reader_kernel;
    if (load_hsa_kernel(executable, reader_agent, "resident_flag_reader",
                        &reader_kernel) != 0) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }
    HsaKernel writer_kernel;
    if (load_hsa_kernel(executable, writer_agent, "resident_flag_writer",
                        &writer_kernel) != 0) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    hsa_queue_t *reader_queue = nullptr;
    status = hsa_queue_create(reader_agent, QueueSize, HSA_QUEUE_TYPE_MULTI,
                              nullptr, nullptr, 0, 0, &reader_queue);
    print_status(options.reverse ? "hsa_queue_create GPU0 reader" :
                                   "hsa_queue_create GPU1 reader", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    hsa_queue_t *writer_queue = nullptr;
    status = hsa_queue_create(writer_agent, QueueSize, HSA_QUEUE_TYPE_MULTI,
                              nullptr, nullptr, 0, 0, &writer_queue);
    print_status(options.reverse ? "hsa_queue_create GPU1 writer" :
                                   "hsa_queue_create GPU0 writer", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_queue_destroy(reader_queue);
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    uint8_t reader_args[128] = {};
    size_t reader_offset = 0;
    reader_offset = append_arg(reader_args, reader_offset, target);
    reader_offset = append_arg(reader_args, reader_offset, control);
    reader_offset = append_arg(reader_args, reader_offset, value_dword);
    reader_offset = append_arg(reader_args, reader_offset, flag_dword);
    reader_offset = append_arg(reader_args, reader_offset, OldValue);
    reader_offset = append_arg(reader_args, reader_offset, UpdatedValue);
    reader_offset = append_arg(reader_args, reader_offset, options.spin_limit);
    reader_offset = append_arg(
        reader_args, reader_offset, static_cast<uint32_t>(options.sync));

    PendingDispatch reader_pending;
    if (launch_kernel_async(reader_queue, reader_kernel,
                            reader_kernarg.region, reader_args,
                            reader_offset, &reader_pending) != 0) {
        hsa_queue_destroy(writer_queue);
        hsa_queue_destroy(reader_queue);
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    if (!wait_for_control_field("first_read_done",
                                &control->first_read_done, 1)) {
        cleanup_dispatch(&reader_pending);
        hsa_queue_destroy(writer_queue);
        hsa_queue_destroy(reader_queue);
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    uint8_t writer_args[64] = {};
    size_t writer_offset = 0;
    writer_offset = append_arg(writer_args, writer_offset, target);
    writer_offset = append_arg(writer_args, writer_offset, value_dword);
    writer_offset = append_arg(writer_args, writer_offset, flag_dword);
    writer_offset = append_arg(writer_args, writer_offset, UpdatedValue);
    writer_offset = append_arg(
        writer_args, writer_offset, static_cast<uint32_t>(options.sync));

    PendingDispatch writer_pending;
    if (launch_kernel_async(writer_queue, writer_kernel,
                            writer_kernarg.region, writer_args,
                            writer_offset, &writer_pending) != 0) {
        cleanup_dispatch(&reader_pending);
        hsa_queue_destroy(writer_queue);
        hsa_queue_destroy(reader_queue);
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    status = wait_for_signal_lt_one("resident flag writer",
                                    writer_pending.completion);
    print_status("resident flag writer completion", status);
    cleanup_dispatch(&writer_pending);
    if (status != HSA_STATUS_SUCCESS) {
        cleanup_dispatch(&reader_pending);
        hsa_queue_destroy(writer_queue);
        hsa_queue_destroy(reader_queue);
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    if (!wait_for_control_field("reader_done", &control->reader_done, 1)) {
        cleanup_dispatch(&reader_pending);
        hsa_queue_destroy(writer_queue);
        hsa_queue_destroy(reader_queue);
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    status = wait_for_signal_lt_one("resident flag reader",
                                    reader_pending.completion);
    print_status("resident flag reader completion", status);
    cleanup_dispatch(&reader_pending);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_queue_destroy(writer_queue);
        hsa_queue_destroy(reader_queue);
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(target);
        hsa_shut_down();
        return 1;
    }

    print_result_and_exit_if_complete(options, control);

    const uint32_t classification = control->classification;
    hsa_queue_destroy(writer_queue);
    hsa_queue_destroy(reader_queue);
    hsa_amd_memory_pool_free(control);
    hsa_amd_memory_pool_free(target);
    hsa_shut_down();

    return classification == ResidentFlagUpdated ||
           classification == ResidentFlagStaleValue ? 0 : 1;
}
