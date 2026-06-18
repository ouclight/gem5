/*
 * SE multi-GPU peer-VRAM microbenchmark.
 *
 * Expected behavior under gem5:
 *   hipSetDevice(1); hipMalloc(&mem, ...)
 *   hipSetDevice(0); kernel<<<...>>>(mem, ...)
 *
 * The kernel running on GPU0 directly dereferences a pointer backed by GPU1
 * local VRAM. The simulator should authorize the peer map and route the
 * request to GPU1's VRAM physical range.
 */

#include <hip/hip_runtime.h>
#include <hsa/hsa.h>
#include <gem5/m5ops.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifndef PEER_VRAM_HSACO_PATH
#define PEER_VRAM_HSACO_PATH "peer_vram_kernels.hsaco"
#endif

struct StartupMarker
{
    StartupMarker()
    {
        std::printf("[peer_vram_hip] global constructors ran\n");
        std::fflush(stdout);
    }
};

static StartupMarker startup_marker;

#define MARK(msg)                                                           \
    do {                                                                    \
        std::printf("[peer_vram_hip] %s\n", msg);                           \
        std::fflush(stdout);                                                \
    } while (0)

#define HIP_CHECK(call)                                                     \
    do {                                                                    \
        std::printf("[peer_vram_hip] begin %s\n", #call);                  \
        std::fflush(stdout);                                                \
        hipError_t err = (call);                                             \
        if (err != hipSuccess) {                                             \
            std::fprintf(stderr, "%s:%d: %s failed: %s\n", __FILE__,       \
                         __LINE__, #call, hipGetErrorString(err));          \
            return 1;                                                       \
        }                                                                   \
        std::printf("[peer_vram_hip] end %s\n", #call);                    \
        std::fflush(stdout);                                                \
    } while (0)

#define HSA_CHECK(call)                                                     \
    do {                                                                    \
        std::printf("[peer_vram_hip] begin %s\n", #call);                  \
        std::fflush(stdout);                                                \
        hsa_status_t status = (call);                                       \
        if (status != HSA_STATUS_SUCCESS) {                                 \
            const char *status_name = nullptr;                              \
            hsa_status_string(status, &status_name);                        \
            std::fprintf(stderr, "%s:%d: %s failed: %s(%d)\n", __FILE__,   \
                         __LINE__, #call,                                   \
                         status_name ? status_name : "unknown", status);    \
            return 1;                                                       \
        }                                                                   \
        std::printf("[peer_vram_hip] end %s\n", #call);                    \
        std::fflush(stdout);                                                \
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

    long size = std::ftell(file);
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
    size_t bytes_read = std::fread(data.data(), 1, data.size(), file);
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
    hsa_status_t status = hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE,
                                             &type);
    if (status != HSA_STATUS_SUCCESS) {
        return status;
    }

    if (type == HSA_DEVICE_TYPE_GPU) {
        char name[64] = {};
        hsa_agent_get_info(agent, HSA_AGENT_INFO_NAME, name);
        std::printf("[peer_vram_hip] HSA gpu agent[%zu] handle=0x%lx "
                    "name=%s\n", agents->gpus.size(), agent.handle, name);
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
    hsa_region_get_info(region, HSA_REGION_INFO_RUNTIME_ALLOC_ALLOWED,
                        &alloc_allowed);
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
        std::printf("[peer_vram_hip] hsa symbol fallback %s\n", kd_name);
        std::fflush(stdout);
        status = hsa_executable_get_symbol_by_name(
            executable, kd_name, &agent, &kernel->symbol);
    }
    if (status != HSA_STATUS_SUCCESS) {
        const char *status_name = nullptr;
        hsa_status_string(status, &status_name);
        std::fprintf(stderr, "hsa_executable_get_symbol_by_name(%s) failed: "
                     "%s(%d)\n", name,
                     status_name ? status_name : "unknown", status);
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
        kernel->symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE,
        &kernel->group_size));
    HSA_CHECK(hsa_executable_symbol_get_info(
        kernel->symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE,
        &kernel->private_size));
    std::printf("[peer_vram_hip] kernel %s object=0x%lx kernarg=%u "
                "group=%u private=%u\n", name, kernel->object,
                kernel->kernarg_size, kernel->group_size,
                kernel->private_size);
    std::fflush(stdout);
    return 0;
}

static int
dispatch_hsa_kernel(hsa_queue_t *queue, const HsaKernel &kernel,
                    hsa_region_t kernarg_region, const void *args,
                    size_t args_size, uint32_t grid_size,
                    uint32_t workgroup_size)
{
    std::printf("[peer_vram_hip] dispatch queue=%p base=%p size=%u id=%lu "
                "doorbell=0x%lx kernel=0x%lx\n",
                queue, queue ? queue->base_address : nullptr,
                queue ? queue->size : 0, queue ? queue->id : 0,
                queue ? queue->doorbell_signal.handle : 0, kernel.object);
    std::fflush(stdout);
    if (queue == nullptr || queue->base_address == nullptr ||
        queue->size == 0 || (queue->size & (queue->size - 1)) != 0) {
        std::fprintf(stderr, "Invalid HSA queue returned by runtime\n");
        return 1;
    }

    void *kernarg = nullptr;
    const size_t kernarg_size = kernel.kernarg_size == 0 ?
        args_size : kernel.kernarg_size;
    HSA_CHECK(hsa_memory_allocate(kernarg_region, kernarg_size, &kernarg));
    std::memset(kernarg, 0, kernarg_size);
    std::memcpy(kernarg, args, args_size);

    hsa_signal_t completion = {};
    HSA_CHECK(hsa_signal_create(1, 0, nullptr, &completion));

    MARK("before hsa_queue_add_write_index_scacq_screl");
    uint64_t index = hsa_queue_add_write_index_scacq_screl(queue, 1);
    std::printf("[peer_vram_hip] queue write index=%lu\n", index);
    std::fflush(stdout);

    auto *ring = static_cast<hsa_kernel_dispatch_packet_t *>(
        queue->base_address);
    hsa_kernel_dispatch_packet_t *packet = &ring[index & (queue->size - 1)];
    std::printf("[peer_vram_hip] packet=%p slot=%lu\n", packet,
                index & (queue->size - 1));
    std::fflush(stdout);

    MARK("before initialize AQL packet");
    std::memset(packet, 0, sizeof(*packet));

    packet->setup = 1 << HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;
    packet->workgroup_size_x = workgroup_size;
    packet->workgroup_size_y = 1;
    packet->workgroup_size_z = 1;
    packet->grid_size_x = grid_size;
    packet->grid_size_y = 1;
    packet->grid_size_z = 1;
    packet->private_segment_size = kernel.private_size;
    packet->group_segment_size = kernel.group_size;
    packet->kernel_object = kernel.object;
    packet->kernarg_address = kernarg;
    packet->completion_signal = completion;
    MARK("AQL packet body initialized");

    const uint16_t header =
        (HSA_PACKET_TYPE_KERNEL_DISPATCH << HSA_PACKET_HEADER_TYPE) |
        (1 << HSA_PACKET_HEADER_BARRIER) |
        (HSA_FENCE_SCOPE_SYSTEM <<
            HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE) |
        (HSA_FENCE_SCOPE_SYSTEM <<
            HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
    MARK("before publish AQL header");
    __atomic_store_n(&packet->header, header, __ATOMIC_RELEASE);
    MARK("before ring HSA doorbell");
    hsa_signal_store_screlease(queue->doorbell_signal, index);
    MARK("before wait for HSA completion");

    hsa_signal_value_t value = hsa_signal_wait_scacquire(
        completion, HSA_SIGNAL_CONDITION_LT, 1, UINT64_MAX,
        HSA_WAIT_STATE_BLOCKED);
    if (value != 0) {
        std::fprintf(stderr, "HSA dispatch completion signal ended at %ld\n",
                     static_cast<long>(value));
        hsa_signal_destroy(completion);
        hsa_memory_free(kernarg);
        return 1;
    }

    HSA_CHECK(hsa_signal_destroy(completion));
    HSA_CHECK(hsa_memory_free(kernarg));
    return 0;
}

int
main()
{
    int device_count = 0;
    HIP_CHECK(hipGetDeviceCount(&device_count));
    std::printf("[peer_vram_hip] device_count=%d\n", device_count);
    std::fflush(stdout);
    if (device_count < 2) {
        std::fprintf(stderr, "Need at least two HIP devices, found %d\n",
                     device_count);
        return 1;
    }

    constexpr uint32_t count = 256;
    constexpr uint32_t seed = 0x5a17c0de;
    constexpr size_t bytes = count * sizeof(uint32_t);

    HIP_CHECK(hipSetDevice(1));
    uint32_t *gpu1_mem = nullptr;
    HIP_CHECK(hipMalloc(&gpu1_mem, bytes));
    std::printf("[peer_vram_hip] gpu1_mem=%p bytes=%zu\n", gpu1_mem, bytes);
    std::fflush(stdout);
    MARK("skip hipMemset(gpu1_mem): GPU0 kernel overwrites the range");

    uint32_t *errors = nullptr;
    HIP_CHECK(hipMalloc(&errors, bytes));
    std::printf("[peer_vram_hip] errors=%p\n", errors);
    std::fflush(stdout);
    MARK("skip hipMemset(errors): verify kernel writes every flag");

    HIP_CHECK(hipSetDevice(0));

    HSA_CHECK(hsa_init());
    HsaAgents hsa_agents;
    HSA_CHECK(hsa_iterate_agents(collect_gpu_agents, &hsa_agents));
    if (hsa_agents.gpus.size() < 2) {
        std::fprintf(stderr, "Need at least two HSA GPU agents, found %zu\n",
                     hsa_agents.gpus.size());
        return 1;
    }

    KernargRegion kernarg_region;
    hsa_status_t region_status = hsa_agent_iterate_regions(
        hsa_agents.gpus[0], find_kernarg_region_cb, &kernarg_region);
    if (region_status != HSA_STATUS_SUCCESS &&
        region_status != HSA_STATUS_INFO_BREAK) {
        const char *status_name = nullptr;
        hsa_status_string(region_status, &status_name);
        std::fprintf(stderr, "hsa_agent_iterate_regions failed: %s(%d)\n",
                     status_name ? status_name : "unknown", region_status);
        return 1;
    }
    if (kernarg_region.region.handle == 0) {
        std::fprintf(stderr, "Could not find a kernarg-capable HSA region\n");
        return 1;
    }

    std::vector<uint8_t> hsaco_image;
    std::printf("[peer_vram_hip] begin read HSACO %s\n",
                PEER_VRAM_HSACO_PATH);
    std::fflush(stdout);
    if (!read_file(PEER_VRAM_HSACO_PATH, hsaco_image)) {
        return 1;
    }
    std::printf("[peer_vram_hip] end read HSACO bytes=%zu\n",
                hsaco_image.size());
    std::fflush(stdout);

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
        executable, hsa_agents.gpus[0], reader, nullptr, &loaded0));
    HSA_CHECK(hsa_executable_load_agent_code_object(
        executable, hsa_agents.gpus[1], reader, nullptr, &loaded1));
    HSA_CHECK(hsa_executable_freeze(executable, nullptr));

    HsaKernel write_kernel;
    HsaKernel verify_kernel;
    if (load_hsa_kernel(executable, hsa_agents.gpus[0], "write_peer_vram",
                        &write_kernel) != 0 ||
        load_hsa_kernel(executable, hsa_agents.gpus[1], "verify_peer_vram",
                        &verify_kernel) != 0) {
        return 1;
    }

    MARK("begin hipDeviceEnablePeerAccess(1, 0)");
    hipError_t peer_status = hipDeviceEnablePeerAccess(1, 0);
    if (peer_status != hipSuccess &&
        peer_status != hipErrorPeerAccessAlreadyEnabled) {
        std::fprintf(stderr, "hipDeviceEnablePeerAccess failed: %s\n",
                     hipGetErrorString(peer_status));
        return 1;
    }
    MARK("end hipDeviceEnablePeerAccess(1, 0)");

    hsa_queue_t *gpu0_queue = nullptr;
    hsa_queue_t *gpu1_queue = nullptr;
    HSA_CHECK(hsa_queue_create(hsa_agents.gpus[0], 64, HSA_QUEUE_TYPE_MULTI,
                               nullptr, nullptr, 0, 0, &gpu0_queue));
    HSA_CHECK(hsa_queue_create(hsa_agents.gpus[1], 64, HSA_QUEUE_TYPE_MULTI,
                               nullptr, nullptr, 0, 0, &gpu1_queue));

    MARK("launch write_peer_vram");
    uint8_t write_args[72] = {};
    size_t write_offset = 0;
    write_offset = append_arg(write_args, write_offset, gpu1_mem);
    write_offset = append_arg(write_args, write_offset, count);
    write_offset = append_arg(write_args, write_offset, seed);
    if (dispatch_hsa_kernel(gpu0_queue, write_kernel,
                            kernarg_region.region, write_args, write_offset,
                            count, 256) != 0) {
        return 1;
    }

    HIP_CHECK(hipSetDevice(1));
    MARK("launch verify_peer_vram");
    uint8_t verify_args[80] = {};
    size_t verify_offset = 0;
    verify_offset = append_arg(verify_args, verify_offset, gpu1_mem);
    verify_offset = append_arg(verify_args, verify_offset, errors);
    verify_offset = append_arg(verify_args, verify_offset, count);
    verify_offset = append_arg(verify_args, verify_offset, seed);
    if (dispatch_hsa_kernel(gpu1_queue, verify_kernel,
                            kernarg_region.region, verify_args, verify_offset,
                            count, 256) != 0) {
        return 1;
    }

    std::vector<uint32_t> host_errors(count);
    MARK("begin CPU read of mapped GPU1 errors");
    volatile const uint32_t *cpu_errors = errors;
    for (uint32_t i = 0; i < count; ++i) {
        host_errors[i] = cpu_errors[i];
    }
    MARK("end CPU read of mapped GPU1 errors");

    uint32_t error_count = 0;
    for (uint32_t error : host_errors) {
        error_count += error;
    }

    if (error_count != 0) {
        std::fprintf(stderr, "peer VRAM verification failed: %u errors\n",
                     error_count);
        return 1;
    }

    std::printf("peer VRAM verification passed\n");
    std::fflush(stdout);

    MARK("skip ROCm teardown: unsupported in SE timing mode");
    MARK("exit simulation with m5_exit");
    m5_exit(0);

    std::fprintf(stderr, "m5_exit returned unexpectedly\n");
    return 2;
}
