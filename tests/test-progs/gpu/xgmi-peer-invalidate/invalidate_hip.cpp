/*
 * SE multi-GPU peer invalidation diagnostic.
 *
 * GPU1 owns the target allocation. GPU0 first reads value A from that target,
 * GPU1 overwrites it with value B, and GPU0 reads it again. The final result
 * distinguishes an observed invalidation from a stale remote cache line.
 */

#include <gem5/m5ops.h>
#include <hip/hip_runtime.h>
#include <hsa/hsa.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifndef INVALIDATE_HSACO_PATH
#define INVALIDATE_HSACO_PATH "invalidate_kernels.hsaco"
#endif

#define MARK(msg)                                                          \
    do {                                                                   \
        std::printf("[peer_invalidate] %s\n", msg);                        \
        std::fflush(stdout);                                               \
    } while (0)

#define HIP_CHECK(call)                                                    \
    do {                                                                   \
        std::printf("[peer_invalidate] begin %s\n", #call);                \
        std::fflush(stdout);                                               \
        hipError_t err = (call);                                           \
        if (err != hipSuccess) {                                           \
            std::fprintf(stderr, "%s:%d: %s failed: %s\n", __FILE__,      \
                         __LINE__, #call, hipGetErrorString(err));          \
            return 1;                                                      \
        }                                                                  \
        std::printf("[peer_invalidate] end %s\n", #call);                  \
        std::fflush(stdout);                                               \
    } while (0)

#define HSA_CHECK(call)                                                    \
    do {                                                                   \
        std::printf("[peer_invalidate] begin %s\n", #call);                \
        std::fflush(stdout);                                               \
        hsa_status_t status = (call);                                      \
        if (status != HSA_STATUS_SUCCESS) {                                \
            const char *status_name = nullptr;                             \
            hsa_status_string(status, &status_name);                       \
            std::fprintf(stderr, "%s:%d: %s failed: %s(%d)\n", __FILE__,  \
                         __LINE__, #call,                                  \
                         status_name ? status_name : "unknown", status);   \
            return 1;                                                      \
        }                                                                  \
        std::printf("[peer_invalidate] end %s\n", #call);                  \
        std::fflush(stdout);                                               \
    } while (0)

static bool
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
        std::fprintf(stderr, "invalid HSACO size %ld for %s\n", size, path);
        std::fclose(file);
        return false;
    }

    if (std::fseek(file, 0, SEEK_SET) != 0) {
        std::perror("fseek");
        std::fclose(file);
        return false;
    }

    data.resize(static_cast<size_t>(size));
    const size_t bytes_read = std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    if (bytes_read != data.size()) {
        std::fprintf(stderr, "read %zu of %zu bytes from %s\n", bytes_read,
                     data.size(), path);
        return false;
    }
    return true;
}

template <typename T>
static size_t
append_arg(uint8_t *buffer, size_t offset, const T &value)
{
    const size_t align = alignof(T);
    offset = (offset + align - 1) & ~(align - 1);
    std::memcpy(buffer + offset, &value, sizeof(T));
    return offset + sizeof(T);
}

struct HsaAgents
{
    std::vector<hsa_agent_t> gpus;
};

static hsa_status_t
collect_gpu_agents(hsa_agent_t agent, void *data)
{
    auto *agents = static_cast<HsaAgents *>(data);
    hsa_device_type_t type = HSA_DEVICE_TYPE_CPU;
    hsa_status_t status = hsa_agent_get_info(
        agent, HSA_AGENT_INFO_DEVICE, &type);
    if (status != HSA_STATUS_SUCCESS) {
        return status;
    }

    if (type == HSA_DEVICE_TYPE_GPU) {
        char name[64] = {};
        hsa_agent_get_info(agent, HSA_AGENT_INFO_NAME, name);
        std::printf("[peer_invalidate] HSA gpu agent[%zu] handle=0x%lx "
                    "name=%s\n",
                    agents->gpus.size(), agent.handle, name);
        std::fflush(stdout);
        agents->gpus.push_back(agent);
    }
    return HSA_STATUS_SUCCESS;
}

struct KernargRegion
{
    hsa_region_t region = {};
};

static hsa_status_t
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
    hsa_region_get_info(
        region, HSA_REGION_INFO_RUNTIME_ALLOC_ALLOWED, &alloc_allowed);
    if (alloc_allowed && (flags & HSA_REGION_GLOBAL_FLAG_KERNARG)) {
        result->region = region;
        return HSA_STATUS_INFO_BREAK;
    }
    return HSA_STATUS_SUCCESS;
}

struct HsaKernel
{
    hsa_executable_symbol_t symbol = {};
    uint64_t object = 0;
    uint32_t kernarg_size = 0;
    uint32_t group_size = 0;
    uint32_t private_size = 0;
};

static int
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
        const char *status_name = nullptr;
        hsa_status_string(status, &status_name);
        std::fprintf(stderr,
                     "hsa_executable_get_symbol_by_name(%s) failed: %s(%d)\n",
                     name, status_name ? status_name : "unknown", status);
        return 1;
    }

    HSA_CHECK(hsa_executable_symbol_get_info(
        kernel->symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT,
        &kernel->object));
    HSA_CHECK(hsa_executable_symbol_get_info(
        kernel->symbol,
        HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE,
        &kernel->kernarg_size));
    HSA_CHECK(hsa_executable_symbol_get_info(
        kernel->symbol,
        HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE,
        &kernel->group_size));
    HSA_CHECK(hsa_executable_symbol_get_info(
        kernel->symbol,
        HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE,
        &kernel->private_size));
    std::printf("[peer_invalidate] kernel %s object=0x%lx kernarg=%u "
                "group=%u private=%u\n",
                name, kernel->object, kernel->kernarg_size,
                kernel->group_size, kernel->private_size);
    std::fflush(stdout);
    return 0;
}

static int
dispatch_hsa_kernel(hsa_queue_t *queue, const HsaKernel &kernel,
                    hsa_region_t kernarg_region, const void *args,
                    size_t args_size)
{
    if (queue == nullptr || queue->base_address == nullptr ||
        queue->size == 0 || (queue->size & (queue->size - 1)) != 0) {
        std::fprintf(stderr, "invalid HSA queue\n");
        return 1;
    }

    void *kernarg = nullptr;
    const size_t kernarg_size =
        kernel.kernarg_size == 0 ? args_size : kernel.kernarg_size;
    HSA_CHECK(hsa_memory_allocate(kernarg_region, kernarg_size, &kernarg));
    std::memset(kernarg, 0, kernarg_size);
    std::memcpy(kernarg, args, args_size);

    hsa_signal_t completion = {};
    HSA_CHECK(hsa_signal_create(1, 0, nullptr, &completion));

    const uint64_t index = hsa_queue_add_write_index_scacq_screl(queue, 1);
    auto *ring = static_cast<hsa_kernel_dispatch_packet_t *>(
        queue->base_address);
    hsa_kernel_dispatch_packet_t *packet =
        &ring[index & (queue->size - 1)];
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
    packet->kernarg_address = kernarg;
    packet->completion_signal = completion;

    const uint16_t header =
        (HSA_PACKET_TYPE_KERNEL_DISPATCH << HSA_PACKET_HEADER_TYPE) |
        (1 << HSA_PACKET_HEADER_BARRIER) |
        (HSA_FENCE_SCOPE_SYSTEM <<
            HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE) |
        (HSA_FENCE_SCOPE_SYSTEM <<
            HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
    __atomic_store_n(&packet->header, header, __ATOMIC_RELEASE);
    hsa_signal_store_screlease(queue->doorbell_signal, index);

    const hsa_signal_value_t value = hsa_signal_wait_scacquire(
        completion, HSA_SIGNAL_CONDITION_LT, 1, UINT64_MAX,
        HSA_WAIT_STATE_BLOCKED);
    if (value != 0) {
        std::fprintf(stderr, "HSA completion signal ended at %ld\n",
                     static_cast<long>(value));
        return 1;
    }

    HSA_CHECK(hsa_signal_destroy(completion));
    HSA_CHECK(hsa_memory_free(kernarg));
    return 0;
}

int
main()
{
    constexpr uint32_t value_a = 0x13579bdf;
    constexpr uint32_t value_b = 0x2468ace0;
    constexpr size_t cache_line_bytes = 64;
    constexpr size_t result_bytes = 3 * sizeof(uint32_t);

    int device_count = 0;
    HIP_CHECK(hipGetDeviceCount(&device_count));
    if (device_count < 2) {
        std::fprintf(stderr, "need two HIP devices, found %d\n", device_count);
        return 1;
    }

    HIP_CHECK(hipSetDevice(1));
    uint32_t *target = nullptr;
    uint32_t *results = nullptr;
    HIP_CHECK(hipMalloc(&target, cache_line_bytes));
    HIP_CHECK(hipMalloc(&results, result_bytes));
    std::printf("[peer_invalidate] target=%p results=%p\n", target, results);
    std::fflush(stdout);

    HIP_CHECK(hipSetDevice(0));
    MARK("begin hipDeviceEnablePeerAccess(1, 0)");
    hipError_t peer_status = hipDeviceEnablePeerAccess(1, 0);
    if (peer_status != hipSuccess &&
        peer_status != hipErrorPeerAccessAlreadyEnabled) {
        std::fprintf(stderr, "hipDeviceEnablePeerAccess failed: %s\n",
                     hipGetErrorString(peer_status));
        return 1;
    }
    MARK("end hipDeviceEnablePeerAccess(1, 0)");

    HSA_CHECK(hsa_init());
    HsaAgents agents;
    HSA_CHECK(hsa_iterate_agents(collect_gpu_agents, &agents));
    if (agents.gpus.size() < 2) {
        std::fprintf(stderr, "need two HSA GPU agents, found %zu\n",
                     agents.gpus.size());
        return 1;
    }

    KernargRegion kernarg_region;
    const hsa_status_t region_status = hsa_agent_iterate_regions(
        agents.gpus[0], find_kernarg_region_cb, &kernarg_region);
    if (region_status != HSA_STATUS_SUCCESS &&
        region_status != HSA_STATUS_INFO_BREAK) {
        std::fprintf(stderr, "hsa_agent_iterate_regions failed: %d\n",
                     region_status);
        return 1;
    }
    if (kernarg_region.region.handle == 0) {
        std::fprintf(stderr, "no kernarg-capable HSA region\n");
        return 1;
    }

    std::vector<uint8_t> hsaco_image;
    if (!read_file(INVALIDATE_HSACO_PATH, hsaco_image)) {
        return 1;
    }

    hsa_code_object_reader_t reader = {};
    HSA_CHECK(hsa_code_object_reader_create_from_memory(
        hsaco_image.data(), hsaco_image.size(), &reader));

    hsa_executable_t executable = {};
    HSA_CHECK(hsa_executable_create_alt(
        HSA_PROFILE_FULL, HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT, nullptr,
        &executable));

    hsa_loaded_code_object_t loaded0 = {};
    hsa_loaded_code_object_t loaded1 = {};
    HSA_CHECK(hsa_executable_load_agent_code_object(
        executable, agents.gpus[0], reader, nullptr, &loaded0));
    HSA_CHECK(hsa_executable_load_agent_code_object(
        executable, agents.gpus[1], reader, nullptr, &loaded1));
    HSA_CHECK(hsa_executable_freeze(executable, nullptr));

    HsaKernel write_kernel;
    HsaKernel read_kernel;
    HsaKernel classify_kernel;
    if (load_hsa_kernel(
            executable, agents.gpus[1], "write_value", &write_kernel) != 0 ||
        load_hsa_kernel(
            executable, agents.gpus[0], "read_value", &read_kernel) != 0 ||
        load_hsa_kernel(executable, agents.gpus[0], "classify_value",
                        &classify_kernel) != 0) {
        return 1;
    }

    hsa_queue_t *gpu0_queue = nullptr;
    hsa_queue_t *gpu1_queue = nullptr;
    HSA_CHECK(hsa_queue_create(
        agents.gpus[0], 64, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr, 0, 0,
        &gpu0_queue));
    HSA_CHECK(hsa_queue_create(
        agents.gpus[1], 64, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr, 0, 0,
        &gpu1_queue));

    MARK("phase 1: GPU1 write A");
    uint8_t write_a_args[32] = {};
    size_t write_a_offset = 0;
    write_a_offset = append_arg(write_a_args, write_a_offset, target);
    write_a_offset = append_arg(write_a_args, write_a_offset, value_a);
    if (dispatch_hsa_kernel(gpu1_queue, write_kernel, kernarg_region.region,
                            write_a_args, write_a_offset) != 0) {
        return 1;
    }

    MARK("phase 2: GPU0 read A");
    uint8_t read_args[32] = {};
    size_t read_offset = 0;
    read_offset = append_arg(read_args, read_offset, target);
    read_offset = append_arg(read_args, read_offset, results);
    if (dispatch_hsa_kernel(gpu0_queue, read_kernel, kernarg_region.region,
                            read_args, read_offset) != 0) {
        return 1;
    }

    MARK("phase 3: GPU1 write B");
    uint8_t write_b_args[32] = {};
    size_t write_b_offset = 0;
    write_b_offset = append_arg(write_b_args, write_b_offset, target);
    write_b_offset = append_arg(write_b_args, write_b_offset, value_b);
    if (dispatch_hsa_kernel(gpu1_queue, write_kernel, kernarg_region.region,
                            write_b_args, write_b_offset) != 0) {
        return 1;
    }

    MARK("phase 4: GPU0 read and classify");
    uint8_t classify_args[48] = {};
    size_t classify_offset = 0;
    classify_offset = append_arg(classify_args, classify_offset, target);
    classify_offset = append_arg(classify_args, classify_offset, results);
    classify_offset = append_arg(classify_args, classify_offset, value_a);
    classify_offset = append_arg(classify_args, classify_offset, value_b);
    if (dispatch_hsa_kernel(
            gpu0_queue, classify_kernel, kernarg_region.region,
            classify_args, classify_offset) != 0) {
        return 1;
    }

    uint32_t host_results[3] = {};
    volatile const uint32_t *cpu_results = results;
    for (size_t i = 0; i < 3; ++i) {
        host_results[i] = cpu_results[i];
    }

    std::printf("[peer_invalidate] A=0x%08x B=0x%08x "
                "first=0x%08x second=0x%08x class=%u\n",
                value_a, value_b, host_results[0], host_results[1],
                host_results[2]);
    std::fflush(stdout);
    MARK("skip ROCm teardown: unsupported in SE timing mode");

    if (host_results[0] != value_a) {
        std::printf("setup read failed\n");
        std::fflush(stdout);
        m5_fail(0, 2);
    } else if (host_results[1] == value_b && host_results[2] == 0) {
        std::printf("invalidation observed\n");
        std::fflush(stdout);
        m5_exit(0);
    } else if (host_results[1] == value_a && host_results[2] == 1) {
        std::printf("stale cache line observed\n");
        std::fflush(stdout);
        m5_fail(0, 1);
    } else {
        std::printf("unexpected value observed\n");
        std::fflush(stdout);
        m5_fail(0, 2);
    }

    std::fprintf(stderr, "m5 pseudo instruction returned unexpectedly\n");
    return 3;
}
