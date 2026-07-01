#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifndef REMOTE_CACHE_HSACO_PATH
#define REMOTE_CACHE_HSACO_PATH "remote_cache_kernels.hsaco"
#endif

namespace
{

constexpr uint32_t ValueA = 0x11112222u;
constexpr uint32_t ValueB = 0x33334444u;
constexpr uint32_t ValueC = 0x55556666u;
constexpr uint32_t SpinLimit = 100000000u;
constexpr uint64_t QueueSize = 64;
constexpr uint32_t MultiOffsetDwords = 4;
constexpr size_t MultiOffsetBytes = MultiOffsetDwords * sizeof(uint32_t);
constexpr uint32_t MultiLineDwords = 32;
constexpr size_t MultiLineBytes = MultiLineDwords * sizeof(uint32_t);
constexpr uint32_t LargeCopyDwords = 1024;
constexpr size_t LargeCopyBytes = LargeCopyDwords * sizeof(uint32_t);
constexpr const char *DisableNextLaunchAcquireMarkerPrefix =
    "/tmp/gem5-disable-next-gpu-launch-acquire-gpu";

enum RemoteCacheClassification : uint32_t
{
    RemoteCacheUnset = 0,
    RemoteCacheUpdated = 1,
    RemoteCacheStale = 2,
    RemoteCacheUnexpected = 3,
};

struct RemoteCacheControl
{
    volatile uint32_t first_read_done;
    volatile uint32_t allow_second_read;
    volatile uint32_t reader_done;
    volatile uint32_t first_value;
    volatile uint32_t second_value;
    volatile uint32_t classification;
    volatile uint32_t first_values[1024];
    volatile uint32_t second_values[1024];
    volatile uint32_t classifications[1024];
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
    std::printf("[hsa_remote_cache_read] %s: %s(%d)\n",
                label, status_name(status), status);
    std::fflush(stdout);
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
                     "[hsa_remote_cache_read] invalid HSACO size %ld\n",
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
                     "[hsa_remote_cache_read] read %zu of %zu HSACO bytes\n",
                     bytes, data.size());
        return false;
    }
    return true;
}

void
disable_next_launch_acquire_marker_path(uint32_t gpu_index, char *path,
                                        size_t size)
{
    std::snprintf(path, size, "%s%u",
                  DisableNextLaunchAcquireMarkerPrefix, gpu_index);
}

void
cleanup_disable_next_launch_acquire_markers(size_t gpu_count)
{
    for (uint32_t gpu_index = 0; gpu_index < gpu_count; ++gpu_index) {
        char path[128] = {};
        disable_next_launch_acquire_marker_path(gpu_index, path,
                                                sizeof(path));
        std::remove(path);
    }
}

bool
request_disable_next_launch_acquire(uint32_t gpu_index)
{
    char path[128] = {};
    disable_next_launch_acquire_marker_path(gpu_index, path, sizeof(path));
    std::FILE *file = std::fopen(path, "w");
    if (file == nullptr) {
        std::perror(path);
        return false;
    }
    std::fprintf(file, "disable-next-launch-acquire gpu=%u\n", gpu_index);
    if (std::fclose(file) != 0) {
        std::perror(path);
        return false;
    }
    std::printf("[hsa_remote_cache_read] requested next launch acquire skip "
                "for GPU%u marker=%s\n", gpu_index, path);
    std::fflush(stdout);
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

    std::printf("[hsa_remote_cache_read] agent handle=0x%lx type=%d name=%s\n",
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

    std::printf("[hsa_remote_cache_read] pool handle=0x%lx segment=%d "
                "alloc=%d size=%zu global_flags=0x%x\n",
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

    std::printf("[hsa_remote_cache_read] kernel %s object=0x%lx "
                "kernarg=%u group=%u private=%u\n",
                name, kernel->object, kernel->kernarg_size,
                kernel->group_size, kernel->private_size);
    std::fflush(stdout);
    return 0;
}

hsa_status_t
wait_for_signal_lt_one(const char *label, hsa_signal_t signal)
{
    std::printf("[hsa_remote_cache_read] wait %s completion\n", label);
    std::fflush(stdout);
    hsa_signal_value_t value = hsa_signal_wait_scacquire(
        signal, HSA_SIGNAL_CONDITION_LT, 1, UINT64_MAX,
        HSA_WAIT_STATE_BLOCKED);
    if (value < 1) {
        return HSA_STATUS_SUCCESS;
    }
    std::fprintf(stderr,
                 "[hsa_remote_cache_read] %s completion wait returned %ld\n",
                 label, static_cast<long>(value));
    return HSA_STATUS_ERROR;
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

    std::printf("[hsa_remote_cache_read] begin %s async copy bytes=%zu\n",
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

hsa_status_t
allow_access(const char *label, const std::vector<hsa_agent_t> &agents,
             const void *ptr)
{
    std::printf("[hsa_remote_cache_read] begin %s allow access\n", label);
    std::fflush(stdout);
    hsa_status_t status = hsa_amd_agents_allow_access(
        agents.size(), agents.data(), nullptr, ptr);
    print_status("hsa_amd_agents_allow_access", status);
    return status;
}

int
launch_reader_async(hsa_queue_t *queue, const HsaKernel &kernel,
                    hsa_region_t kernarg_region, const void *args,
                    size_t args_size, PendingDispatch *pending)
{
    if (queue == nullptr || queue->base_address == nullptr ||
        queue->size == 0 || (queue->size & (queue->size - 1)) != 0) {
        std::fprintf(stderr, "[hsa_remote_cache_read] invalid HSA queue\n");
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
    std::printf("[hsa_remote_cache_read] ring reader queue=%p index=%lu "
                "doorbell=0x%lx kernel=0x%lx\n",
                queue, index, queue->doorbell_signal.handle, kernel.object);
    std::fflush(stdout);
    hsa_signal_store_screlease(queue->doorbell_signal, index);
    return 0;
}

bool
wait_for_control_field(const char *label, volatile uint32_t *field,
                       uint32_t expected)
{
    constexpr uint64_t HostPollLimit = 1000000000ull;
    for (uint64_t i = 0; i < HostPollLimit; ++i) {
        if (*field == expected) {
            std::printf("[hsa_remote_cache_read] observed %s=%u after %lu "
                        "host polls\n",
                        label, expected, i);
            std::fflush(stdout);
            return true;
        }
    }
    std::fprintf(stderr,
                 "[hsa_remote_cache_read] timeout waiting for %s=%u, "
                 "current=%u\n",
                 label, expected, *field);
    return false;
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

enum RemoteCacheMode
{
    ModeSingle,
    ModeRepeated,
    ModeReverse,
    ModeMultiOffset,
    ModeMultiLine,
    ModeLargeCopy,
    ModeShaderStore,
    ModeReverseShaderStore,
    ModeShaderMultiLine,
    ModeReverseShaderMultiLine,
    ModeShaderLarge,
    ModeReverseShaderLarge,
    ModeShaderLargeRepeated,
    ModeReverseShaderLargeRepeated,
    ModeShaderSeparateDispatch,
    ModeReverseShaderSeparateDispatch,
};

int
run_remote_cache_round(RemoteCacheMode mode, uint32_t round, hsa_agent_t cpu,
                       hsa_agent_t source_agent, hsa_agent_t target_agent,
                       hsa_queue_t *reader_queue,
                       const HsaKernel &reader_kernel,
                       hsa_region_t kernarg_region, uint32_t *source,
                       uint32_t *target, RemoteCacheControl *control,
                       uint32_t expected_first, uint32_t expected_second,
                       uint32_t *classification_out)
{
    std::memset(control, 0, sizeof(*control));
    __sync_synchronize();

    char source_label[64] = {};
    std::snprintf(source_label, sizeof(source_label),
                  mode == ModeReverse ? "H2D_GPU1_SOURCE" :
                  round == 0 ? "H2D_GPU0_SOURCE" :
                               "H2D_GPU0_SOURCE_ROUND%u",
                  round);
    hsa_status_t status = async_copy_and_wait(
        source_label, source, source_agent, &expected_second, cpu,
        sizeof(expected_second));
    if (status != HSA_STATUS_SUCCESS) {
        return 1;
    }

    uint8_t args[64] = {};
    size_t offset = 0;
    offset = append_arg(args, offset, target);
    offset = append_arg(args, offset, control);
    offset = append_arg(args, offset, expected_first);
    offset = append_arg(args, offset, expected_second);
    offset = append_arg(args, offset, SpinLimit);

    PendingDispatch pending;
    if (launch_reader_async(reader_queue, reader_kernel, kernarg_region,
                            args, offset, &pending) != 0) {
        return 1;
    }

    if (!wait_for_control_field("first_read_done",
                                &control->first_read_done, 1)) {
        cleanup_dispatch(&pending);
        return 1;
    }

    char overwrite_label[64] = {};
    std::snprintf(overwrite_label, sizeof(overwrite_label),
                  mode == ModeReverse ? "GPU1_TO_GPU0_OVERWRITE" :
                  round == 0 ? "GPU0_TO_GPU1_OVERWRITE" :
                               "GPU0_TO_GPU1_OVERWRITE_ROUND%u",
                  round);
    status = async_copy_and_wait(overwrite_label, target, target_agent,
                                 source, source_agent, sizeof(uint32_t));
    if (status != HSA_STATUS_SUCCESS) {
        cleanup_dispatch(&pending);
        return 1;
    }

    __sync_synchronize();
    control->allow_second_read = 1;
    __sync_synchronize();

    if (!wait_for_control_field("reader_done", &control->reader_done, 1)) {
        cleanup_dispatch(&pending);
        return 1;
    }

    status = wait_for_signal_lt_one("reader dispatch", pending.completion);
    print_status("reader dispatch completion", status);
    cleanup_dispatch(&pending);
    if (status != HSA_STATUS_SUCCESS) {
        return 1;
    }

    const uint32_t first = control->first_value;
    const uint32_t second = control->second_value;
    const uint32_t classification = control->classification;
    *classification_out = classification;

    if (mode == ModeReverse) {
        std::printf("REMOTE_CACHE_READ_REVERSE_RESULT first=0x%08x "
                    "second=0x%08x classification=%u\n",
                    first, second, classification);
    } else if (round == 0) {
        std::printf("REMOTE_CACHE_READ_RESULT first=0x%08x second=0x%08x "
                    "classification=%u\n",
                    first, second, classification);
    } else {
        std::printf("REMOTE_CACHE_READ_REPEATED_ROUND round=%u "
                    "first=0x%08x second=0x%08x classification=%u\n",
                    round, first, second, classification);
    }
    std::fflush(stdout);
    return 0;
}

int
run_remote_cache_shader_store_round(RemoteCacheMode mode,
                       hsa_agent_t writer_agent,
                       hsa_queue_t *reader_queue,
                       hsa_queue_t *writer_queue,
                       const HsaKernel &reader_kernel,
                       const HsaKernel &writer_kernel,
                       hsa_region_t reader_kernarg_region,
                       hsa_region_t writer_kernarg_region,
                       uint32_t *target, RemoteCacheControl *control,
                       uint32_t *classification_out)
{
    std::memset(control, 0, sizeof(*control));
    __sync_synchronize();

    uint8_t reader_args[64] = {};
    size_t reader_offset = 0;
    reader_offset = append_arg(reader_args, reader_offset, target);
    reader_offset = append_arg(reader_args, reader_offset, control);
    reader_offset = append_arg(reader_args, reader_offset, ValueA);
    reader_offset = append_arg(reader_args, reader_offset, ValueB);
    reader_offset = append_arg(reader_args, reader_offset, SpinLimit);

    PendingDispatch reader_pending;
    if (launch_reader_async(reader_queue, reader_kernel, reader_kernarg_region,
                            reader_args, reader_offset,
                            &reader_pending) != 0) {
        return 1;
    }

    if (!wait_for_control_field("first_read_done",
                                &control->first_read_done, 1)) {
        cleanup_dispatch(&reader_pending);
        return 1;
    }

    const char *write_label = mode == ModeReverseShaderStore ?
        "GPU1_TO_GPU0_SHADER_STORE" : "GPU0_TO_GPU1_SHADER_STORE";
    std::printf("[hsa_remote_cache_read] begin %s kernel store\n",
                write_label);
    std::fflush(stdout);

    uint8_t writer_args[64] = {};
    size_t writer_offset = 0;
    writer_offset = append_arg(writer_args, writer_offset, target);
    writer_offset = append_arg(writer_args, writer_offset, ValueB);

    PendingDispatch writer_pending;
    if (launch_reader_async(writer_queue, writer_kernel, writer_kernarg_region,
                            writer_args, writer_offset,
                            &writer_pending) != 0) {
        cleanup_dispatch(&reader_pending);
        return 1;
    }

    hsa_status_t status = wait_for_signal_lt_one(write_label,
                                                 writer_pending.completion);
    print_status("writer dispatch completion", status);
    cleanup_dispatch(&writer_pending);
    if (status != HSA_STATUS_SUCCESS) {
        cleanup_dispatch(&reader_pending);
        return 1;
    }

    __sync_synchronize();
    control->allow_second_read = 1;
    __sync_synchronize();

    if (!wait_for_control_field("reader_done", &control->reader_done, 1)) {
        cleanup_dispatch(&reader_pending);
        return 1;
    }

    status = wait_for_signal_lt_one("shader-store reader dispatch",
                                    reader_pending.completion);
    print_status("shader-store reader dispatch completion", status);
    cleanup_dispatch(&reader_pending);
    if (status != HSA_STATUS_SUCCESS) {
        return 1;
    }

    const uint32_t first = control->first_value;
    const uint32_t second = control->second_value;
    const uint32_t classification = control->classification;
    *classification_out = classification;

    if (mode == ModeReverseShaderStore) {
        std::printf("REMOTE_CACHE_READ_REVERSE_SHADER_STORE_RESULT "
                    "first=0x%08x second=0x%08x classification=%u\n",
                    first, second, classification);
    } else {
        std::printf("REMOTE_CACHE_READ_SHADER_STORE_RESULT first=0x%08x "
                    "second=0x%08x classification=%u\n",
                    first, second, classification);
    }
    std::fflush(stdout);
    (void)writer_agent;
    return 0;
}

int
run_remote_cache_shader_store_vector_round(RemoteCacheMode mode,
                       uint32_t round,
                       uint32_t dwords,
                       hsa_queue_t *reader_queue,
                       hsa_queue_t *writer_queue,
                       const HsaKernel &reader_kernel,
                       const HsaKernel &writer_kernel,
                       hsa_region_t reader_kernarg_region,
                       hsa_region_t writer_kernarg_region,
                       uint32_t *target, RemoteCacheControl *control,
                       uint32_t expected_first, uint32_t expected_second,
                       uint32_t *classification_out)
{
    std::memset(control, 0, sizeof(*control));
    __sync_synchronize();

    uint8_t reader_args[64] = {};
    size_t reader_offset = 0;
    reader_offset = append_arg(reader_args, reader_offset, target);
    reader_offset = append_arg(reader_args, reader_offset, control);
    reader_offset = append_arg(reader_args, reader_offset, expected_first);
    reader_offset = append_arg(reader_args, reader_offset, expected_second);
    reader_offset = append_arg(reader_args, reader_offset, dwords);
    reader_offset = append_arg(reader_args, reader_offset, SpinLimit);

    PendingDispatch reader_pending;
    if (launch_reader_async(reader_queue, reader_kernel, reader_kernarg_region,
                            reader_args, reader_offset,
                            &reader_pending) != 0) {
        return 1;
    }

    if (!wait_for_control_field("first_read_done",
                                &control->first_read_done, 1)) {
        cleanup_dispatch(&reader_pending);
        return 1;
    }

    const bool reverse_shader_vector =
        mode == ModeReverseShaderMultiLine ||
        mode == ModeReverseShaderLarge ||
        mode == ModeReverseShaderLargeRepeated;
    const bool shader_large =
        mode == ModeShaderLarge || mode == ModeReverseShaderLarge ||
        mode == ModeShaderLargeRepeated ||
        mode == ModeReverseShaderLargeRepeated;
    const bool shader_large_repeated =
        mode == ModeShaderLargeRepeated || mode == ModeReverseShaderLargeRepeated;
    const char *write_label = reverse_shader_vector ?
        (shader_large ? "GPU1_TO_GPU0_SHADER_LARGE_STORE" :
                        "GPU1_TO_GPU0_SHADER_MULTI_LINE_STORE") :
        (shader_large ? "GPU0_TO_GPU1_SHADER_LARGE_STORE" :
                        "GPU0_TO_GPU1_SHADER_MULTI_LINE_STORE");
    std::printf("[hsa_remote_cache_read] begin %s kernel store "
                "dwords=%u\n",
                write_label, dwords);
    std::fflush(stdout);

    uint8_t writer_args[64] = {};
    size_t writer_offset = 0;
    writer_offset = append_arg(writer_args, writer_offset, target);
    writer_offset = append_arg(writer_args, writer_offset, expected_second);
    writer_offset = append_arg(writer_args, writer_offset, dwords);

    PendingDispatch writer_pending;
    if (launch_reader_async(writer_queue, writer_kernel, writer_kernarg_region,
                            writer_args, writer_offset,
                            &writer_pending) != 0) {
        cleanup_dispatch(&reader_pending);
        return 1;
    }

    hsa_status_t status = wait_for_signal_lt_one(write_label,
                                                 writer_pending.completion);
    print_status("vector writer dispatch completion", status);
    cleanup_dispatch(&writer_pending);
    if (status != HSA_STATUS_SUCCESS) {
        cleanup_dispatch(&reader_pending);
        return 1;
    }

    __sync_synchronize();
    control->allow_second_read = 1;
    __sync_synchronize();

    if (!wait_for_control_field("reader_done", &control->reader_done, 1)) {
        cleanup_dispatch(&reader_pending);
        return 1;
    }

    status = wait_for_signal_lt_one(shader_large ?
                                    "shader-large reader dispatch" :
                                    "shader-multi-line reader dispatch",
                                    reader_pending.completion);
    print_status(shader_large ?
                 "shader-large reader dispatch completion" :
                 "shader-multi-line reader dispatch completion", status);
    cleanup_dispatch(&reader_pending);
    if (status != HSA_STATUS_SUCCESS) {
        return 1;
    }

    const uint32_t classification = control->classification;
    *classification_out = classification;
    const char *entry_marker = reverse_shader_vector ?
        (shader_large_repeated ?
            "REMOTE_CACHE_READ_REVERSE_SHADER_LARGE_REPEATED_ROUND" :
         shader_large ? "REMOTE_CACHE_READ_REVERSE_SHADER_LARGE_ENTRY" :
                        "REMOTE_CACHE_READ_REVERSE_SHADER_MULTI_LINE_ENTRY") :
        (shader_large_repeated ?
            "REMOTE_CACHE_READ_SHADER_LARGE_REPEATED_ROUND" :
         shader_large ? "REMOTE_CACHE_READ_SHADER_LARGE_ENTRY" :
                        "REMOTE_CACHE_READ_SHADER_MULTI_LINE_ENTRY");
    const char *result_marker = reverse_shader_vector ?
        (shader_large_repeated ?
            "REMOTE_CACHE_READ_REVERSE_SHADER_LARGE_REPEATED_RESULT" :
         shader_large ? "REMOTE_CACHE_READ_REVERSE_SHADER_LARGE_RESULT" :
                        "REMOTE_CACHE_READ_REVERSE_SHADER_MULTI_LINE_RESULT") :
        (shader_large_repeated ?
            "REMOTE_CACHE_READ_SHADER_LARGE_REPEATED_RESULT" :
         shader_large ? "REMOTE_CACHE_READ_SHADER_LARGE_RESULT" :
                        "REMOTE_CACHE_READ_SHADER_MULTI_LINE_RESULT");
    for (uint32_t i = 0; i < dwords; ++i) {
        if (shader_large_repeated) {
            std::printf("%s round=%u offset=%u "
                        "first=0x%08x second=0x%08x classification=%u\n",
                        entry_marker, round, i, control->first_values[i],
                        control->second_values[i],
                        control->classifications[i]);
        } else {
            std::printf("%s offset=%u "
                    "first=0x%08x second=0x%08x classification=%u\n",
                    entry_marker, i, control->first_values[i],
                    control->second_values[i],
                    control->classifications[i]);
        }
    }
    if (shader_large_repeated) {
        std::printf("%s round=%u count=%u classification=%u\n",
                    result_marker, round, dwords, classification);
    } else {
        std::printf("%s count=%u classification=%u\n", result_marker, dwords,
                    classification);
    }
    std::fflush(stdout);
    return 0;
}

int
launch_snapshot_reader_and_wait(const char *label,
                       hsa_queue_t *reader_queue,
                       const HsaKernel &snapshot_reader_kernel,
                       hsa_region_t kernarg_region,
                       uint32_t *target,
                       volatile uint32_t *values,
                       uint32_t dwords)
{
    uint8_t args[64] = {};
    size_t offset = 0;
    offset = append_arg(args, offset, target);
    offset = append_arg(args, offset, values);
    offset = append_arg(args, offset, dwords);

    PendingDispatch pending;
    if (launch_reader_async(reader_queue, snapshot_reader_kernel,
                            kernarg_region, args, offset, &pending) != 0) {
        return 1;
    }

    hsa_status_t status = wait_for_signal_lt_one(label, pending.completion);
    print_status("snapshot reader dispatch completion", status);
    cleanup_dispatch(&pending);
    return status == HSA_STATUS_SUCCESS ? 0 : 1;
}

int
run_remote_cache_shader_separate_dispatch_round(RemoteCacheMode mode,
                       uint32_t dwords,
                       uint32_t reader_gpu_index,
                       bool disable_second_reader_launch_acquire,
                       hsa_queue_t *reader_queue,
                       hsa_queue_t *writer_queue,
                       const HsaKernel &snapshot_reader_kernel,
                       const HsaKernel &writer_kernel,
                       hsa_region_t reader_kernarg_region,
                       hsa_region_t writer_kernarg_region,
                       uint32_t *target, RemoteCacheControl *control,
                       uint32_t *classification_out)
{
    std::memset(control, 0, sizeof(*control));
    __sync_synchronize();

    const bool reverse =
        mode == ModeReverseShaderSeparateDispatch;
    const char *reader1_label = reverse ?
        "reverse shader separate-dispatch reader1" :
        "shader separate-dispatch reader1";
    const char *reader2_label = reverse ?
        "reverse shader separate-dispatch reader2" :
        "shader separate-dispatch reader2";
    const char *write_label = reverse ?
        "GPU1_TO_GPU0_SHADER_SEPARATE_DISPATCH_STORE" :
        "GPU0_TO_GPU1_SHADER_SEPARATE_DISPATCH_STORE";

    if (launch_snapshot_reader_and_wait(reader1_label, reader_queue,
            snapshot_reader_kernel, reader_kernarg_region, target,
            control->first_values, dwords) != 0) {
        return 1;
    }

    std::printf("[hsa_remote_cache_read] begin %s kernel store dwords=%u\n",
                write_label, dwords);
    std::fflush(stdout);

    uint8_t writer_args[64] = {};
    size_t writer_offset = 0;
    writer_offset = append_arg(writer_args, writer_offset, target);
    writer_offset = append_arg(writer_args, writer_offset, ValueB);
    writer_offset = append_arg(writer_args, writer_offset, dwords);

    PendingDispatch writer_pending;
    if (launch_reader_async(writer_queue, writer_kernel, writer_kernarg_region,
                            writer_args, writer_offset,
                            &writer_pending) != 0) {
        return 1;
    }

    hsa_status_t status = wait_for_signal_lt_one(write_label,
                                                 writer_pending.completion);
    print_status("separate-dispatch writer completion", status);
    cleanup_dispatch(&writer_pending);
    if (status != HSA_STATUS_SUCCESS) {
        return 1;
    }

    if (disable_second_reader_launch_acquire &&
        !request_disable_next_launch_acquire(reader_gpu_index)) {
        return 1;
    }

    if (launch_snapshot_reader_and_wait(reader2_label, reader_queue,
            snapshot_reader_kernel, reader_kernarg_region, target,
            control->second_values, dwords) != 0) {
        return 1;
    }

    uint32_t first_ok = 0;
    uint32_t second_updated = 0;
    uint32_t stale = 0;
    uint32_t unexpected = 0;
    for (uint32_t i = 0; i < dwords; ++i) {
        const uint32_t expected_first = ValueA + i;
        const uint32_t expected_second = ValueB + i;
        const uint32_t first = control->first_values[i];
        const uint32_t second = control->second_values[i];
        if (first == expected_first) {
            ++first_ok;
        } else {
            ++unexpected;
            continue;
        }
        if (second == expected_second) {
            ++second_updated;
        } else if (second == expected_first) {
            ++stale;
        } else {
            ++unexpected;
        }
    }

    uint32_t classification = RemoteCacheUnexpected;
    if (first_ok == dwords && second_updated == dwords &&
        stale == 0 && unexpected == 0) {
        classification = RemoteCacheUpdated;
    } else if (first_ok == dwords && stale > 0 && unexpected == 0) {
        classification = RemoteCacheStale;
    }
    control->classification = classification;
    *classification_out = classification;

    const char *result_marker = reverse ?
        "REMOTE_CACHE_READ_REVERSE_SHADER_SEPARATE_DISPATCH_RESULT" :
        "REMOTE_CACHE_READ_SHADER_SEPARATE_DISPATCH_RESULT";
    std::printf("%s count=%u first_ok=%u second_updated=%u stale=%u "
                "unexpected=%u classification=%u\n",
                result_marker, dwords, first_ok, second_updated, stale,
                unexpected, classification);
    std::fflush(stdout);
    return 0;
}

int
run_remote_cache_vector_round(const char *source_label,
                       const char *overwrite_label,
                       const char *dispatch_label,
                       const char *entry_marker,
                       const char *result_marker,
                       uint32_t dwords,
                       hsa_agent_t cpu, hsa_agent_t source_agent,
                       hsa_agent_t target_agent, hsa_queue_t *reader_queue,
                       const HsaKernel &reader_kernel,
                       hsa_region_t kernarg_region, uint32_t *source,
                       uint32_t *target, RemoteCacheControl *control,
                       uint32_t *classification_out)
{
    std::memset(control, 0, sizeof(*control));
    __sync_synchronize();

    uint32_t source_values[LargeCopyDwords] = {};
    for (uint32_t i = 0; i < dwords; ++i) {
        source_values[i] = ValueB + i;
    }
    hsa_status_t status = async_copy_and_wait(
        source_label, source, source_agent, source_values, cpu,
        dwords * sizeof(uint32_t));
    if (status != HSA_STATUS_SUCCESS) {
        return 1;
    }

    uint8_t args[64] = {};
    size_t offset = 0;
    offset = append_arg(args, offset, target);
    offset = append_arg(args, offset, control);
    offset = append_arg(args, offset, ValueA);
    offset = append_arg(args, offset, ValueB);
    offset = append_arg(args, offset, dwords);
    offset = append_arg(args, offset, SpinLimit);

    PendingDispatch pending;
    if (launch_reader_async(reader_queue, reader_kernel, kernarg_region,
                            args, offset, &pending) != 0) {
        return 1;
    }

    if (!wait_for_control_field("first_read_done",
                                &control->first_read_done, 1)) {
        cleanup_dispatch(&pending);
        return 1;
    }

    status = async_copy_and_wait(overwrite_label, target, target_agent,
                                 source, source_agent,
                                 dwords * sizeof(uint32_t));
    if (status != HSA_STATUS_SUCCESS) {
        cleanup_dispatch(&pending);
        return 1;
    }

    __sync_synchronize();
    control->allow_second_read = 1;
    __sync_synchronize();

    if (!wait_for_control_field("reader_done", &control->reader_done, 1)) {
        cleanup_dispatch(&pending);
        return 1;
    }

    status = wait_for_signal_lt_one(dispatch_label, pending.completion);
    print_status("vector reader dispatch completion", status);
    cleanup_dispatch(&pending);
    if (status != HSA_STATUS_SUCCESS) {
        return 1;
    }

    const uint32_t classification = control->classification;
    *classification_out = classification;
    for (uint32_t i = 0; i < dwords; ++i) {
        std::printf("%s offset=%u "
                    "first=0x%08x second=0x%08x classification=%u\n",
                    entry_marker, i, control->first_values[i],
                    control->second_values[i],
                    control->classifications[i]);
    }
    std::printf("%s count=%u classification=%u\n", result_marker, dwords,
                classification);
    std::fflush(stdout);
    return 0;
}

} // namespace

int
main(int argc, char **argv)
{
    bool repeated_mode = false;
    bool reverse_mode = false;
    bool multi_offset_mode = false;
    bool multi_line_mode = false;
    bool reverse_multi_line_mode = false;
    bool large_copy_mode = false;
    bool reverse_large_copy_mode = false;
    bool shader_store_mode = false;
    bool reverse_shader_store_mode = false;
    bool shader_multi_line_mode = false;
    bool reverse_shader_multi_line_mode = false;
    bool shader_large_mode = false;
    bool reverse_shader_large_mode = false;
    bool shader_large_repeated_mode = false;
    bool reverse_shader_large_repeated_mode = false;
    bool shader_separate_dispatch_mode = false;
    bool reverse_shader_separate_dispatch_mode = false;
    bool disable_second_reader_launch_acquire = false;
    int mode_argc = argc;
    if (argc == 4 &&
        std::strcmp(argv[3], "--disable-second-reader-launch-acquire") == 0) {
        disable_second_reader_launch_acquire = true;
        mode_argc = 3;
    }
    if (mode_argc == 3 && std::strcmp(argv[1], "--mode") == 0 &&
        std::strcmp(argv[2], "repeated") == 0) {
        repeated_mode = true;
    } else if (mode_argc == 3 && std::strcmp(argv[1], "--mode") == 0 &&
        std::strcmp(argv[2], "reverse") == 0) {
        reverse_mode = true;
    } else if (mode_argc == 3 && std::strcmp(argv[1], "--mode") == 0 &&
        std::strcmp(argv[2], "multi-offset") == 0) {
        multi_offset_mode = true;
    } else if (mode_argc == 3 && std::strcmp(argv[1], "--mode") == 0 &&
        std::strcmp(argv[2], "multi-line") == 0) {
        multi_line_mode = true;
    } else if (mode_argc == 3 && std::strcmp(argv[1], "--mode") == 0 &&
        std::strcmp(argv[2], "reverse-multi-line") == 0) {
        reverse_multi_line_mode = true;
    } else if (mode_argc == 3 && std::strcmp(argv[1], "--mode") == 0 &&
        std::strcmp(argv[2], "large-copy") == 0) {
        large_copy_mode = true;
    } else if (mode_argc == 3 && std::strcmp(argv[1], "--mode") == 0 &&
        std::strcmp(argv[2], "reverse-large-copy") == 0) {
        reverse_large_copy_mode = true;
    } else if (mode_argc == 3 && std::strcmp(argv[1], "--mode") == 0 &&
        std::strcmp(argv[2], "shader-store") == 0) {
        shader_store_mode = true;
    } else if (mode_argc == 3 && std::strcmp(argv[1], "--mode") == 0 &&
        std::strcmp(argv[2], "reverse-shader-store") == 0) {
        reverse_shader_store_mode = true;
    } else if (mode_argc == 3 && std::strcmp(argv[1], "--mode") == 0 &&
        std::strcmp(argv[2], "shader-multi-line") == 0) {
        shader_multi_line_mode = true;
    } else if (mode_argc == 3 && std::strcmp(argv[1], "--mode") == 0 &&
        std::strcmp(argv[2], "reverse-shader-multi-line") == 0) {
        reverse_shader_multi_line_mode = true;
    } else if (mode_argc == 3 && std::strcmp(argv[1], "--mode") == 0 &&
        std::strcmp(argv[2], "shader-large") == 0) {
        shader_large_mode = true;
    } else if (mode_argc == 3 && std::strcmp(argv[1], "--mode") == 0 &&
        std::strcmp(argv[2], "reverse-shader-large") == 0) {
        reverse_shader_large_mode = true;
    } else if (mode_argc == 3 && std::strcmp(argv[1], "--mode") == 0 &&
        std::strcmp(argv[2], "shader-large-repeated") == 0) {
        shader_large_repeated_mode = true;
    } else if (mode_argc == 3 && std::strcmp(argv[1], "--mode") == 0 &&
        std::strcmp(argv[2], "reverse-shader-large-repeated") == 0) {
        reverse_shader_large_repeated_mode = true;
    } else if (mode_argc == 3 && std::strcmp(argv[1], "--mode") == 0 &&
        std::strcmp(argv[2], "shader-separate-dispatch") == 0) {
        shader_separate_dispatch_mode = true;
    } else if (mode_argc == 3 && std::strcmp(argv[1], "--mode") == 0 &&
        std::strcmp(argv[2], "reverse-shader-separate-dispatch") == 0) {
        reverse_shader_separate_dispatch_mode = true;
    } else if (mode_argc != 1) {
        std::fprintf(stderr,
                     "usage: %s [--mode repeated|reverse|multi-offset|"
                     "multi-line|reverse-multi-line|large-copy|"
                     "reverse-large-copy|shader-store|"
                     "reverse-shader-store|shader-multi-line|"
                     "reverse-shader-multi-line|shader-large|"
                     "reverse-shader-large|shader-large-repeated|"
                     "reverse-shader-large-repeated|"
                     "shader-separate-dispatch|"
                     "reverse-shader-separate-dispatch] "
                     "[--disable-second-reader-launch-acquire]\n",
                     argv[0]);
        return 1;
    }
    if (disable_second_reader_launch_acquire &&
        !shader_separate_dispatch_mode &&
        !reverse_shader_separate_dispatch_mode) {
        std::fprintf(stderr,
                     "[hsa_remote_cache_read] "
                     "--disable-second-reader-launch-acquire requires a "
                     "separate-dispatch mode\n");
        return 1;
    }

    std::printf("[hsa_remote_cache_read] begin hsa_init\n");
    std::fflush(stdout);
    hsa_status_t status = hsa_init();
    print_status("end hsa_init", status);
    if (status != HSA_STATUS_SUCCESS) {
        return 1;
    }

    AgentSearch agents;
    status = hsa_iterate_agents(find_cpu_and_gpus, &agents);
    print_status("end hsa_iterate_agents", status);
    if (status != HSA_STATUS_SUCCESS ||
        agents.cpu.handle == 0 ||
        agents.gpus.size() < 2) {
        std::fprintf(stderr,
                     "[hsa_remote_cache_read] required CPU/two GPU agents "
                     "not found\n");
        hsa_shut_down();
        return 1;
    }

    hsa_agent_t gpu0 = agents.gpus[0];
    hsa_agent_t gpu1 = agents.gpus[1];
    cleanup_disable_next_launch_acquire_markers(agents.gpus.size());
    const bool reverse_direction = reverse_mode || reverse_multi_line_mode ||
                                   reverse_large_copy_mode ||
                                   reverse_shader_store_mode ||
                                   reverse_shader_multi_line_mode ||
                                   reverse_shader_large_mode ||
                                   reverse_shader_large_repeated_mode ||
                                   reverse_shader_separate_dispatch_mode;
    const bool shader_separate_dispatch_direction =
        shader_separate_dispatch_mode || reverse_shader_separate_dispatch_mode;
    const bool shader_store_direction =
        shader_store_mode || reverse_shader_store_mode ||
        shader_multi_line_mode || reverse_shader_multi_line_mode ||
        shader_large_mode || reverse_shader_large_mode ||
        shader_large_repeated_mode || reverse_shader_large_repeated_mode ||
        shader_separate_dispatch_direction;
    const bool shader_vector_direction =
        shader_multi_line_mode || reverse_shader_multi_line_mode ||
        shader_large_mode || reverse_shader_large_mode ||
        shader_large_repeated_mode || reverse_shader_large_repeated_mode;
    const bool reverse_shader_vector =
        reverse_shader_multi_line_mode || reverse_shader_large_mode ||
        reverse_shader_large_repeated_mode;
    const bool reverse_shader_writer =
        reverse_shader_store_mode || reverse_shader_vector ||
        reverse_shader_separate_dispatch_mode;
    const bool shader_large_repeated_direction =
        shader_large_repeated_mode || reverse_shader_large_repeated_mode;
    hsa_agent_t reader_agent = reverse_direction ? gpu0 : gpu1;
    hsa_agent_t source_agent = reverse_direction ? gpu1 : gpu0;
    hsa_agent_t target_agent = reverse_direction ? gpu0 : gpu1;
    hsa_agent_t writer_agent = source_agent;
    const uint32_t reader_gpu_index = reverse_direction ? 0 : 1;

    PoolSearch cpu_pool{ {}, sizeof(RemoteCacheControl) };
    status = hsa_amd_agent_iterate_memory_pools(
        agents.cpu, find_allocable_global_pool, &cpu_pool);
    print_status("end CPU hsa_amd_agent_iterate_memory_pools", status);

    PoolSearch gpu0_pool{ {}, LargeCopyBytes };
    status = hsa_amd_agent_iterate_memory_pools(
        gpu0, find_allocable_global_pool, &gpu0_pool);
    print_status("end GPU0 hsa_amd_agent_iterate_memory_pools", status);

    PoolSearch gpu1_pool{ {}, LargeCopyBytes };
    status = hsa_amd_agent_iterate_memory_pools(
        gpu1, find_allocable_global_pool, &gpu1_pool);
    print_status("end GPU1 hsa_amd_agent_iterate_memory_pools", status);

    if (cpu_pool.pool.handle == 0 ||
        gpu0_pool.pool.handle == 0 ||
        gpu1_pool.pool.handle == 0) {
        std::fprintf(stderr,
                     "[hsa_remote_cache_read] missing allocable memory pool\n");
        hsa_shut_down();
        return 1;
    }

    uint32_t *gpu0_source = nullptr;
    uint32_t *gpu1_target = nullptr;
    RemoteCacheControl *control = nullptr;
    status = hsa_amd_memory_pool_allocate(
        gpu0_pool.pool, LargeCopyBytes, 0,
        reinterpret_cast<void **>(&gpu0_source));
    print_status("GPU0 hsa_amd_memory_pool_allocate", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_shut_down();
        return 1;
    }
    status = hsa_amd_memory_pool_allocate(
        gpu1_pool.pool, LargeCopyBytes, 0,
        reinterpret_cast<void **>(&gpu1_target));
    print_status("GPU1 hsa_amd_memory_pool_allocate", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_amd_memory_pool_free(gpu0_source);
        hsa_shut_down();
        return 1;
    }
    status = hsa_amd_memory_pool_allocate(
        cpu_pool.pool, sizeof(RemoteCacheControl), 0,
        reinterpret_cast<void **>(&control));
    print_status("CPU hsa_amd_memory_pool_allocate control", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_amd_memory_pool_free(gpu1_target);
        hsa_amd_memory_pool_free(gpu0_source);
        hsa_shut_down();
        return 1;
    }

    std::memset(control, 0, sizeof(*control));

    std::vector<hsa_agent_t> access_agents = {
        agents.cpu,
        gpu0,
        gpu1,
    };
    if (allow_access("GPU0 source", access_agents, gpu0_source) !=
            HSA_STATUS_SUCCESS ||
        allow_access("GPU1 target", access_agents, gpu1_target) !=
            HSA_STATUS_SUCCESS ||
        allow_access("control", access_agents, control) !=
            HSA_STATUS_SUCCESS) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(gpu1_target);
        hsa_amd_memory_pool_free(gpu0_source);
        hsa_shut_down();
        return 1;
    }

    uint32_t *source = reverse_direction ? gpu1_target : gpu0_source;
    uint32_t *target = reverse_direction ? gpu0_source : gpu1_target;

    const uint32_t vector_dwords =
        (large_copy_mode || reverse_large_copy_mode ||
         shader_large_mode || reverse_shader_large_mode ||
         shader_large_repeated_mode || reverse_shader_large_repeated_mode ||
         shader_separate_dispatch_direction) ?
        LargeCopyDwords :
        (multi_line_mode || reverse_multi_line_mode ||
         shader_multi_line_mode || reverse_shader_multi_line_mode) ?
        MultiLineDwords : MultiOffsetDwords;
    const size_t vector_bytes = vector_dwords * sizeof(uint32_t);
    uint32_t initial_values[LargeCopyDwords] = {};
    for (uint32_t i = 0; i < vector_dwords; ++i) {
        initial_values[i] = ValueA + i;
    }
    if (async_copy_and_wait(reverse_direction ? "H2D_GPU0_INITIAL" :
                                                "H2D_GPU1_INITIAL",
                            target, target_agent,
                            initial_values, agents.cpu,
                            (multi_offset_mode || multi_line_mode ||
                             reverse_multi_line_mode || large_copy_mode ||
                             reverse_large_copy_mode ||
                             shader_multi_line_mode ||
                             reverse_shader_multi_line_mode ||
                             shader_large_mode ||
                             reverse_shader_large_mode ||
                             shader_large_repeated_mode ||
                             reverse_shader_large_repeated_mode ||
                             shader_separate_dispatch_direction) ?
                                                vector_bytes :
                                                sizeof(initial_values[0])) !=
            HSA_STATUS_SUCCESS) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(gpu1_target);
        hsa_amd_memory_pool_free(gpu0_source);
        hsa_shut_down();
        return 1;
    }

    KernargRegion kernarg_region;
    hsa_status_t region_status = hsa_agent_iterate_regions(
        reader_agent, find_kernarg_region_cb, &kernarg_region);
    if (region_status != HSA_STATUS_SUCCESS &&
        region_status != HSA_STATUS_INFO_BREAK) {
        print_status("hsa_agent_iterate_regions", region_status);
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(gpu1_target);
        hsa_amd_memory_pool_free(gpu0_source);
        hsa_shut_down();
        return 1;
    }
    if (kernarg_region.region.handle == 0) {
        std::fprintf(stderr,
                     "[hsa_remote_cache_read] no kernarg-capable region\n");
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(gpu1_target);
        hsa_amd_memory_pool_free(gpu0_source);
        hsa_shut_down();
        return 1;
    }

    std::vector<uint8_t> hsaco;
    std::printf("[hsa_remote_cache_read] begin read HSACO %s\n",
                REMOTE_CACHE_HSACO_PATH);
    std::fflush(stdout);
    if (!read_file(REMOTE_CACHE_HSACO_PATH, hsaco)) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(gpu1_target);
        hsa_amd_memory_pool_free(gpu0_source);
        hsa_shut_down();
        return 1;
    }

    hsa_code_object_reader_t reader = {};
    status = hsa_code_object_reader_create_from_memory(
        hsaco.data(), hsaco.size(), &reader);
    print_status("hsa_code_object_reader_create_from_memory", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(gpu1_target);
        hsa_amd_memory_pool_free(gpu0_source);
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
        hsa_amd_memory_pool_free(gpu1_target);
        hsa_amd_memory_pool_free(gpu0_source);
        hsa_shut_down();
        return 1;
    }

    hsa_loaded_code_object_t loaded = {};
    status = hsa_executable_load_agent_code_object(
        executable, reader_agent, reader, nullptr, &loaded);
    print_status("hsa_executable_load_agent_code_object", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(gpu1_target);
        hsa_amd_memory_pool_free(gpu0_source);
        hsa_shut_down();
        return 1;
    }
    hsa_loaded_code_object_t writer_loaded = {};
    if (shader_store_direction && writer_agent.handle != reader_agent.handle) {
        status = hsa_executable_load_agent_code_object(
            executable, writer_agent, reader, nullptr, &writer_loaded);
        print_status("hsa_executable_load_agent_code_object writer", status);
        if (status != HSA_STATUS_SUCCESS) {
            hsa_amd_memory_pool_free(control);
            hsa_amd_memory_pool_free(gpu1_target);
            hsa_amd_memory_pool_free(gpu0_source);
            hsa_shut_down();
            return 1;
        }
    }
    status = hsa_executable_freeze(executable, nullptr);
    print_status("hsa_executable_freeze", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(gpu1_target);
        hsa_amd_memory_pool_free(gpu0_source);
        hsa_shut_down();
        return 1;
    }

    HsaKernel reader_kernel;
    const char *reader_kernel_name =
        shader_separate_dispatch_direction ? "remote_cache_snapshot_reader" :
        (multi_offset_mode || multi_line_mode || reverse_multi_line_mode ||
         large_copy_mode || reverse_large_copy_mode ||
         shader_multi_line_mode || reverse_shader_multi_line_mode ||
         shader_large_mode || reverse_shader_large_mode ||
         shader_large_repeated_mode || reverse_shader_large_repeated_mode) ?
        "remote_cache_multi_offset_reader" : "remote_cache_reader";
    if (load_hsa_kernel(executable, reader_agent, reader_kernel_name,
                        &reader_kernel) != 0) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(gpu1_target);
        hsa_amd_memory_pool_free(gpu0_source);
        hsa_shut_down();
        return 1;
    }

    HsaKernel writer_kernel;
    KernargRegion writer_kernarg_region = kernarg_region;
    if (shader_store_direction) {
        hsa_status_t writer_region_status = hsa_agent_iterate_regions(
            writer_agent, find_kernarg_region_cb, &writer_kernarg_region);
        if (writer_region_status != HSA_STATUS_SUCCESS &&
            writer_region_status != HSA_STATUS_INFO_BREAK) {
            print_status("writer hsa_agent_iterate_regions",
                         writer_region_status);
            hsa_amd_memory_pool_free(control);
            hsa_amd_memory_pool_free(gpu1_target);
            hsa_amd_memory_pool_free(gpu0_source);
            hsa_shut_down();
            return 1;
        }
        if (writer_kernarg_region.region.handle == 0) {
            std::fprintf(stderr,
                         "[hsa_remote_cache_read] no writer "
                         "kernarg-capable region\n");
            hsa_amd_memory_pool_free(control);
            hsa_amd_memory_pool_free(gpu1_target);
            hsa_amd_memory_pool_free(gpu0_source);
            hsa_shut_down();
            return 1;
        }
        const char *writer_kernel_name =
            (shader_vector_direction || shader_separate_dispatch_direction) ?
            "remote_cache_multi_offset_writer" : "remote_cache_writer";
        if (load_hsa_kernel(executable, writer_agent, writer_kernel_name,
                            &writer_kernel) != 0) {
            hsa_amd_memory_pool_free(control);
            hsa_amd_memory_pool_free(gpu1_target);
            hsa_amd_memory_pool_free(gpu0_source);
            hsa_shut_down();
            return 1;
        }
    }

    hsa_queue_t *reader_queue = nullptr;
    status = hsa_queue_create(reader_agent, QueueSize, HSA_QUEUE_TYPE_MULTI,
                              nullptr, nullptr, 0, 0, &reader_queue);
    print_status(reverse_direction ? "hsa_queue_create GPU0" :
                                     "hsa_queue_create GPU1", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_amd_memory_pool_free(control);
        hsa_amd_memory_pool_free(gpu1_target);
        hsa_amd_memory_pool_free(gpu0_source);
        hsa_shut_down();
        return 1;
    }

    hsa_queue_t *writer_queue = nullptr;
    if (shader_store_direction) {
        status = hsa_queue_create(writer_agent, QueueSize,
                                  HSA_QUEUE_TYPE_MULTI,
                                  nullptr, nullptr, 0, 0, &writer_queue);
        print_status(reverse_shader_writer ?
                         "hsa_queue_create GPU1 writer" :
                         "hsa_queue_create GPU0 writer",
                     status);
        if (status != HSA_STATUS_SUCCESS) {
            hsa_queue_destroy(reader_queue);
            hsa_amd_memory_pool_free(control);
            hsa_amd_memory_pool_free(gpu1_target);
            hsa_amd_memory_pool_free(gpu0_source);
            hsa_shut_down();
            return 1;
        }
    }

    uint32_t classification = RemoteCacheUnset;
    const RemoteCacheMode mode = reverse_shader_large_repeated_mode ?
                                 ModeReverseShaderLargeRepeated :
                                 shader_large_repeated_mode ?
                                 ModeShaderLargeRepeated :
                                 reverse_shader_separate_dispatch_mode ?
                                 ModeReverseShaderSeparateDispatch :
                                 shader_separate_dispatch_mode ?
                                 ModeShaderSeparateDispatch :
                                 reverse_shader_large_mode ?
                                 ModeReverseShaderLarge :
                                 shader_large_mode ?
                                 ModeShaderLarge :
                                 reverse_shader_multi_line_mode ?
                                 ModeReverseShaderMultiLine :
                                 shader_multi_line_mode ?
                                 ModeShaderMultiLine :
                                 reverse_shader_store_mode ?
                                 ModeReverseShaderStore :
                                 shader_store_mode ? ModeShaderStore :
                                 reverse_large_copy_mode ? ModeLargeCopy :
                                 large_copy_mode ? ModeLargeCopy :
                                 reverse_multi_line_mode ? ModeMultiLine :
                                 multi_line_mode ? ModeMultiLine :
                                 multi_offset_mode ? ModeMultiOffset :
                                 reverse_mode ? ModeReverse :
                                 repeated_mode ? ModeRepeated : ModeSingle;
    if (shader_separate_dispatch_direction) {
        status = run_remote_cache_shader_separate_dispatch_round(
            mode, vector_dwords, reader_gpu_index,
            disable_second_reader_launch_acquire,
            reader_queue, writer_queue, reader_kernel,
            writer_kernel, kernarg_region.region,
            writer_kernarg_region.region, target, control,
            &classification) == 0 ? HSA_STATUS_SUCCESS : HSA_STATUS_ERROR;
    } else if (shader_vector_direction) {
        status = run_remote_cache_shader_store_vector_round(
            mode, 1, vector_dwords, reader_queue, writer_queue, reader_kernel,
            writer_kernel, kernarg_region.region,
            writer_kernarg_region.region, target, control, ValueA, ValueB,
            &classification) == 0 ? HSA_STATUS_SUCCESS : HSA_STATUS_ERROR;
    } else if (shader_store_direction) {
        status = run_remote_cache_shader_store_round(
            mode, writer_agent, reader_queue, writer_queue, reader_kernel,
            writer_kernel, kernarg_region.region,
            writer_kernarg_region.region, target, control,
            &classification) == 0 ? HSA_STATUS_SUCCESS : HSA_STATUS_ERROR;
    } else if (multi_offset_mode || multi_line_mode || reverse_multi_line_mode ||
        large_copy_mode || reverse_large_copy_mode) {
        status = run_remote_cache_vector_round(
            reverse_large_copy_mode ? "H2D_GPU1_LARGE_COPY_SOURCE" :
            large_copy_mode ? "H2D_GPU0_LARGE_COPY_SOURCE" :
            reverse_multi_line_mode ? "H2D_GPU1_MULTI_LINE_SOURCE" :
            multi_line_mode ? "H2D_GPU0_MULTI_LINE_SOURCE" :
                              "H2D_GPU0_MULTI_OFFSET_SOURCE",
            reverse_large_copy_mode ? "GPU1_TO_GPU0_LARGE_COPY_OVERWRITE" :
            large_copy_mode ? "GPU0_TO_GPU1_LARGE_COPY_OVERWRITE" :
            reverse_multi_line_mode ? "GPU1_TO_GPU0_MULTI_LINE_OVERWRITE" :
            multi_line_mode ? "GPU0_TO_GPU1_MULTI_LINE_OVERWRITE" :
                              "GPU0_TO_GPU1_MULTI_OFFSET_OVERWRITE",
            reverse_large_copy_mode ? "reverse large-copy reader dispatch" :
            large_copy_mode ? "large-copy reader dispatch" :
            reverse_multi_line_mode ? "reverse multi-line reader dispatch" :
            multi_line_mode ? "multi-line reader dispatch" :
                              "multi-offset reader dispatch",
            reverse_large_copy_mode ?
                "REMOTE_CACHE_READ_REVERSE_LARGE_COPY_ENTRY" :
            large_copy_mode ? "REMOTE_CACHE_READ_LARGE_COPY_ENTRY" :
            reverse_multi_line_mode ?
                "REMOTE_CACHE_READ_REVERSE_MULTI_LINE_ENTRY" :
            multi_line_mode ? "REMOTE_CACHE_READ_MULTI_LINE_ENTRY" :
                              "REMOTE_CACHE_READ_MULTI_OFFSET_ENTRY",
            reverse_large_copy_mode ?
                "REMOTE_CACHE_READ_REVERSE_LARGE_COPY_RESULT" :
            large_copy_mode ? "REMOTE_CACHE_READ_LARGE_COPY_RESULT" :
            reverse_multi_line_mode ?
                "REMOTE_CACHE_READ_REVERSE_MULTI_LINE_RESULT" :
            multi_line_mode ? "REMOTE_CACHE_READ_MULTI_LINE_RESULT" :
                              "REMOTE_CACHE_READ_MULTI_OFFSET_RESULT",
            vector_dwords, agents.cpu, source_agent, target_agent,
            reader_queue, reader_kernel, kernarg_region.region, source,
            target, control, &classification) == 0 ?
            HSA_STATUS_SUCCESS : HSA_STATUS_ERROR;
    } else {
        status = run_remote_cache_round(mode, 0, agents.cpu, source_agent,
                                        target_agent, reader_queue,
                                        reader_kernel, kernarg_region.region,
                                        source, target, control, ValueA,
                                        ValueB, &classification) == 0 ?
            HSA_STATUS_SUCCESS : HSA_STATUS_ERROR;
    }

    uint32_t round2_classification = RemoteCacheUnset;
    if (status == HSA_STATUS_SUCCESS && shader_large_repeated_direction &&
        classification == RemoteCacheUpdated) {
        if (run_remote_cache_shader_store_vector_round(
                mode, 2, vector_dwords, reader_queue, writer_queue,
                reader_kernel, writer_kernel, kernarg_region.region,
                writer_kernarg_region.region, target, control,
                ValueB, ValueC, &round2_classification) != 0) {
            status = HSA_STATUS_ERROR;
        }
    }
    if (status == HSA_STATUS_SUCCESS && repeated_mode &&
        classification == RemoteCacheUpdated) {
        if (run_remote_cache_round(mode, 2, agents.cpu, source_agent,
                                   target_agent, reader_queue, reader_kernel,
                                   kernarg_region.region, source, target,
                                   control,
                                   ValueB, ValueC,
                                   &round2_classification) != 0) {
            status = HSA_STATUS_ERROR;
        }
    }

    hsa_status_t destroy_writer_queue_status = HSA_STATUS_SUCCESS;
    if (writer_queue != nullptr) {
        destroy_writer_queue_status = hsa_queue_destroy(writer_queue);
        print_status("hsa_queue_destroy writer", destroy_writer_queue_status);
    }
    hsa_status_t destroy_queue_status = hsa_queue_destroy(reader_queue);
    print_status("hsa_queue_destroy", destroy_queue_status);
    hsa_status_t free_control_status = hsa_amd_memory_pool_free(control);
    print_status("free control", free_control_status);
    hsa_status_t free_gpu1_status = hsa_amd_memory_pool_free(gpu1_target);
    print_status("free GPU1 target", free_gpu1_status);
    hsa_status_t free_gpu0_status = hsa_amd_memory_pool_free(gpu0_source);
    print_status("free GPU0 source", free_gpu0_status);
    hsa_status_t shutdown_status = hsa_shut_down();
    print_status("end hsa_shut_down", shutdown_status);

    if (status != HSA_STATUS_SUCCESS ||
        destroy_writer_queue_status != HSA_STATUS_SUCCESS ||
        destroy_queue_status != HSA_STATUS_SUCCESS ||
        free_control_status != HSA_STATUS_SUCCESS ||
        free_gpu1_status != HSA_STATUS_SUCCESS ||
        free_gpu0_status != HSA_STATUS_SUCCESS ||
        shutdown_status != HSA_STATUS_SUCCESS) {
        return 1;
    }

    if (reverse_shader_separate_dispatch_mode) {
        if (classification == RemoteCacheUpdated) {
            std::printf(
                "REMOTE_CACHE_READ_REVERSE_SHADER_SEPARATE_DISPATCH_PASSED_UPDATED\n");
            std::fflush(stdout);
            return 0;
        }
        if (classification == RemoteCacheStale) {
            std::printf(
                "REMOTE_CACHE_READ_REVERSE_SHADER_SEPARATE_DISPATCH_OBSERVED_STALE\n");
            std::fflush(stdout);
            return 0;
        }
        std::printf(
            "REMOTE_CACHE_READ_REVERSE_SHADER_SEPARATE_DISPATCH_FAILED_UNEXPECTED\n");
        std::fflush(stdout);
        return 1;
    }

    if (shader_separate_dispatch_mode) {
        if (classification == RemoteCacheUpdated) {
            std::printf(
                "REMOTE_CACHE_READ_SHADER_SEPARATE_DISPATCH_PASSED_UPDATED\n");
            std::fflush(stdout);
            return 0;
        }
        if (classification == RemoteCacheStale) {
            std::printf(
                "REMOTE_CACHE_READ_SHADER_SEPARATE_DISPATCH_OBSERVED_STALE\n");
            std::fflush(stdout);
            return 0;
        }
        std::printf(
            "REMOTE_CACHE_READ_SHADER_SEPARATE_DISPATCH_FAILED_UNEXPECTED\n");
        std::fflush(stdout);
        return 1;
    }

    if (reverse_shader_large_repeated_mode) {
        if (classification == RemoteCacheUpdated &&
            round2_classification == RemoteCacheUpdated) {
            std::printf(
                "REMOTE_CACHE_READ_REVERSE_SHADER_LARGE_REPEATED_PASSED_UPDATED\n");
            std::fflush(stdout);
            return 0;
        }
        if (classification == RemoteCacheStale ||
            round2_classification == RemoteCacheStale) {
            std::printf(
                "REMOTE_CACHE_READ_REVERSE_SHADER_LARGE_REPEATED_OBSERVED_STALE\n");
            std::fflush(stdout);
            return 0;
        }
        std::printf(
            "REMOTE_CACHE_READ_REVERSE_SHADER_LARGE_REPEATED_FAILED_UNEXPECTED\n");
        std::fflush(stdout);
        return 1;
    }

    if (shader_large_repeated_mode) {
        if (classification == RemoteCacheUpdated &&
            round2_classification == RemoteCacheUpdated) {
            std::printf(
                "REMOTE_CACHE_READ_SHADER_LARGE_REPEATED_PASSED_UPDATED\n");
            std::fflush(stdout);
            return 0;
        }
        if (classification == RemoteCacheStale ||
            round2_classification == RemoteCacheStale) {
            std::printf(
                "REMOTE_CACHE_READ_SHADER_LARGE_REPEATED_OBSERVED_STALE\n");
            std::fflush(stdout);
            return 0;
        }
        std::printf(
            "REMOTE_CACHE_READ_SHADER_LARGE_REPEATED_FAILED_UNEXPECTED\n");
        std::fflush(stdout);
        return 1;
    }

    if (reverse_shader_large_mode) {
        if (classification == RemoteCacheUpdated) {
            std::printf(
                "REMOTE_CACHE_READ_REVERSE_SHADER_LARGE_PASSED_UPDATED\n");
            std::fflush(stdout);
            return 0;
        }
        if (classification == RemoteCacheStale) {
            std::printf(
                "REMOTE_CACHE_READ_REVERSE_SHADER_LARGE_OBSERVED_STALE\n");
            std::fflush(stdout);
            return 0;
        }
        std::printf(
            "REMOTE_CACHE_READ_REVERSE_SHADER_LARGE_FAILED_UNEXPECTED\n");
        std::fflush(stdout);
        return 1;
    }

    if (shader_large_mode) {
        if (classification == RemoteCacheUpdated) {
            std::printf("REMOTE_CACHE_READ_SHADER_LARGE_PASSED_UPDATED\n");
            std::fflush(stdout);
            return 0;
        }
        if (classification == RemoteCacheStale) {
            std::printf("REMOTE_CACHE_READ_SHADER_LARGE_OBSERVED_STALE\n");
            std::fflush(stdout);
            return 0;
        }
        std::printf("REMOTE_CACHE_READ_SHADER_LARGE_FAILED_UNEXPECTED\n");
        std::fflush(stdout);
        return 1;
    }

    if (reverse_shader_multi_line_mode) {
        if (classification == RemoteCacheUpdated) {
            std::printf(
                "REMOTE_CACHE_READ_REVERSE_SHADER_MULTI_LINE_PASSED_UPDATED\n");
            std::fflush(stdout);
            return 0;
        }
        if (classification == RemoteCacheStale) {
            std::printf(
                "REMOTE_CACHE_READ_REVERSE_SHADER_MULTI_LINE_OBSERVED_STALE\n");
            std::fflush(stdout);
            return 0;
        }
        std::printf(
            "REMOTE_CACHE_READ_REVERSE_SHADER_MULTI_LINE_FAILED_UNEXPECTED\n");
        std::fflush(stdout);
        return 1;
    }

    if (shader_multi_line_mode) {
        if (classification == RemoteCacheUpdated) {
            std::printf(
                "REMOTE_CACHE_READ_SHADER_MULTI_LINE_PASSED_UPDATED\n");
            std::fflush(stdout);
            return 0;
        }
        if (classification == RemoteCacheStale) {
            std::printf(
                "REMOTE_CACHE_READ_SHADER_MULTI_LINE_OBSERVED_STALE\n");
            std::fflush(stdout);
            return 0;
        }
        std::printf("REMOTE_CACHE_READ_SHADER_MULTI_LINE_FAILED_UNEXPECTED\n");
        std::fflush(stdout);
        return 1;
    }

    if (reverse_shader_store_mode) {
        if (classification == RemoteCacheUpdated) {
            std::printf(
                "REMOTE_CACHE_READ_REVERSE_SHADER_STORE_PASSED_UPDATED\n");
            std::fflush(stdout);
            return 0;
        }
        if (classification == RemoteCacheStale) {
            std::printf(
                "REMOTE_CACHE_READ_REVERSE_SHADER_STORE_OBSERVED_STALE\n");
            std::fflush(stdout);
            return 0;
        }
        std::printf(
            "REMOTE_CACHE_READ_REVERSE_SHADER_STORE_FAILED_UNEXPECTED\n");
        std::fflush(stdout);
        return 1;
    }

    if (shader_store_mode) {
        if (classification == RemoteCacheUpdated) {
            std::printf("REMOTE_CACHE_READ_SHADER_STORE_PASSED_UPDATED\n");
            std::fflush(stdout);
            return 0;
        }
        if (classification == RemoteCacheStale) {
            std::printf("REMOTE_CACHE_READ_SHADER_STORE_OBSERVED_STALE\n");
            std::fflush(stdout);
            return 0;
        }
        std::printf("REMOTE_CACHE_READ_SHADER_STORE_FAILED_UNEXPECTED\n");
        std::fflush(stdout);
        return 1;
    }

    if (reverse_large_copy_mode) {
        if (classification == RemoteCacheUpdated) {
            std::printf(
                "REMOTE_CACHE_READ_REVERSE_LARGE_COPY_PASSED_UPDATED\n");
            std::fflush(stdout);
            return 0;
        }
        if (classification == RemoteCacheStale) {
            std::printf(
                "REMOTE_CACHE_READ_REVERSE_LARGE_COPY_OBSERVED_STALE\n");
            std::fflush(stdout);
            return 0;
        }
        std::printf("REMOTE_CACHE_READ_REVERSE_LARGE_COPY_FAILED_UNEXPECTED\n");
        std::fflush(stdout);
        return 1;
    }

    if (large_copy_mode) {
        if (classification == RemoteCacheUpdated) {
            std::printf("REMOTE_CACHE_READ_LARGE_COPY_PASSED_UPDATED\n");
            std::fflush(stdout);
            return 0;
        }
        if (classification == RemoteCacheStale) {
            std::printf("REMOTE_CACHE_READ_LARGE_COPY_OBSERVED_STALE\n");
            std::fflush(stdout);
            return 0;
        }
        std::printf("REMOTE_CACHE_READ_LARGE_COPY_FAILED_UNEXPECTED\n");
        std::fflush(stdout);
        return 1;
    }

    if (reverse_multi_line_mode) {
        if (classification == RemoteCacheUpdated) {
            std::printf(
                "REMOTE_CACHE_READ_REVERSE_MULTI_LINE_PASSED_UPDATED\n");
            std::fflush(stdout);
            return 0;
        }
        if (classification == RemoteCacheStale) {
            std::printf(
                "REMOTE_CACHE_READ_REVERSE_MULTI_LINE_OBSERVED_STALE\n");
            std::fflush(stdout);
            return 0;
        }
        std::printf("REMOTE_CACHE_READ_REVERSE_MULTI_LINE_FAILED_UNEXPECTED\n");
        std::fflush(stdout);
        return 1;
    }

    if (multi_line_mode) {
        if (classification == RemoteCacheUpdated) {
            std::printf("REMOTE_CACHE_READ_MULTI_LINE_PASSED_UPDATED\n");
            std::fflush(stdout);
            return 0;
        }
        if (classification == RemoteCacheStale) {
            std::printf("REMOTE_CACHE_READ_MULTI_LINE_OBSERVED_STALE\n");
            std::fflush(stdout);
            return 0;
        }
        std::printf("REMOTE_CACHE_READ_MULTI_LINE_FAILED_UNEXPECTED\n");
        std::fflush(stdout);
        return 1;
    }

    if (multi_offset_mode) {
        if (classification == RemoteCacheUpdated) {
            std::printf("REMOTE_CACHE_READ_MULTI_OFFSET_PASSED_UPDATED\n");
            std::fflush(stdout);
            return 0;
        }
        if (classification == RemoteCacheStale) {
            std::printf("REMOTE_CACHE_READ_MULTI_OFFSET_OBSERVED_STALE\n");
            std::fflush(stdout);
            return 0;
        }
        std::printf("REMOTE_CACHE_READ_MULTI_OFFSET_FAILED_UNEXPECTED\n");
        std::fflush(stdout);
        return 1;
    }

    if (reverse_mode) {
        if (classification == RemoteCacheUpdated) {
            std::printf("REMOTE_CACHE_READ_REVERSE_PASSED_UPDATED\n");
            std::fflush(stdout);
            return 0;
        }
        if (classification == RemoteCacheStale) {
            std::printf("REMOTE_CACHE_READ_REVERSE_OBSERVED_STALE\n");
            std::fflush(stdout);
            return 0;
        }
        std::printf("REMOTE_CACHE_READ_REVERSE_FAILED_UNEXPECTED\n");
        std::fflush(stdout);
        return 1;
    }

    if (repeated_mode) {
        if (classification == RemoteCacheUpdated &&
            round2_classification == RemoteCacheUpdated) {
            std::printf("REMOTE_CACHE_READ_REPEATED_PASSED_UPDATED\n");
            std::fflush(stdout);
            return 0;
        }
        if (classification == RemoteCacheStale ||
            round2_classification == RemoteCacheStale) {
            std::printf("REMOTE_CACHE_READ_REPEATED_OBSERVED_STALE\n");
            std::fflush(stdout);
            return 0;
        }
        std::printf("REMOTE_CACHE_READ_REPEATED_FAILED_UNEXPECTED\n");
        std::fflush(stdout);
        return 1;
    }

    if (classification == RemoteCacheUpdated) {
        std::printf("REMOTE_CACHE_READ_PASSED_UPDATED\n");
        std::fflush(stdout);
        return 0;
    }
    if (classification == RemoteCacheStale) {
        std::printf("REMOTE_CACHE_READ_OBSERVED_STALE\n");
        std::fflush(stdout);
        return 0;
    }

    std::printf("REMOTE_CACHE_READ_FAILED_UNEXPECTED\n");
    std::fflush(stdout);
    return 1;
}
