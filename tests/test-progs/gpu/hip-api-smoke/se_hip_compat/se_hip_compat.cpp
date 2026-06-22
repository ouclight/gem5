#include <hip/hip_runtime_api.h>
#include <hsa/hsa.h>

#include <dlfcn.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{

using GetDevice = hipError_t (*)(int *);

struct Runtime
{
    hsa_agent_t agent = {};
    hsa_region_t kernargRegion = {};
    hsa_executable_t executable = {};
    hsa_queue_t *queue = nullptr;
    std::uint64_t kernelObject = 0;
    std::uint32_t kernargSize = 0;
    std::uint32_t groupSize = 0;
    std::uint32_t privateSize = 0;
};

struct AgentList
{
    std::vector<hsa_agent_t> gpus;
};

struct RegionResult
{
    hsa_region_t region = {};
};

std::once_flag initializeOnce;
Runtime runtime;
hipError_t initializationError = hipSuccess;

hipError_t
hsaError(const char *operation, hsa_status_t status)
{
    const char *name = nullptr;
    hsa_status_string(status, &name);
    std::fprintf(
        stderr,
        "[se_hip_compat] %s failed: %s(%d)\n",
        operation,
        name == nullptr ? "unknown" : name,
        static_cast<int>(status));
    return hipErrorUnknown;
}

hsa_status_t
collectGpuAgents(hsa_agent_t agent, void *data)
{
    hsa_device_type_t type = HSA_DEVICE_TYPE_CPU;
    hsa_status_t status =
        hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type);
    if (status == HSA_STATUS_SUCCESS && type == HSA_DEVICE_TYPE_GPU) {
        static_cast<AgentList *>(data)->gpus.push_back(agent);
    }
    return status;
}

hsa_status_t
findKernargRegion(hsa_region_t region, void *data)
{
    hsa_region_segment_t segment = HSA_REGION_SEGMENT_GLOBAL;
    hsa_status_t status =
        hsa_region_get_info(region, HSA_REGION_INFO_SEGMENT, &segment);
    if (status != HSA_STATUS_SUCCESS ||
        segment != HSA_REGION_SEGMENT_GLOBAL) {
        return status;
    }

    std::uint32_t flags = 0;
    bool allocAllowed = false;
    status = hsa_region_get_info(
        region, HSA_REGION_INFO_GLOBAL_FLAGS, &flags);
    if (status != HSA_STATUS_SUCCESS) {
        return status;
    }
    status = hsa_region_get_info(
        region, HSA_REGION_INFO_RUNTIME_ALLOC_ALLOWED, &allocAllowed);
    if (status != HSA_STATUS_SUCCESS) {
        return status;
    }
    if (allocAllowed && (flags & HSA_REGION_GLOBAL_FLAG_KERNARG)) {
        static_cast<RegionResult *>(data)->region = region;
        return HSA_STATUS_INFO_BREAK;
    }
    return HSA_STATUS_SUCCESS;
}

bool
readFile(const char *path, std::vector<std::uint8_t> *data)
{
    std::FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        std::perror(path);
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    const long fileSize = std::ftell(file);
    if (fileSize <= 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }
    data->resize(static_cast<std::size_t>(fileSize));
    const std::size_t bytes =
        std::fread(data->data(), 1, data->size(), file);
    std::fclose(file);
    return bytes == data->size();
}

bool
getSymbolInfo(
    hsa_executable_symbol_t symbol,
    hsa_executable_symbol_info_t attribute,
    void *value)
{
    const hsa_status_t status =
        hsa_executable_symbol_get_info(symbol, attribute, value);
    if (status != HSA_STATUS_SUCCESS) {
        initializationError =
            hsaError("hsa_executable_symbol_get_info", status);
        return false;
    }
    return true;
}

void
initialize()
{
    const hsa_status_t initStatus = hsa_init();
    if (initStatus != HSA_STATUS_SUCCESS) {
        initializationError = hsaError("hsa_init", initStatus);
        return;
    }

    AgentList agents;
    const hsa_status_t agentStatus =
        hsa_iterate_agents(collectGpuAgents, &agents);
    if (agentStatus != HSA_STATUS_SUCCESS) {
        initializationError =
            hsaError("hsa_iterate_agents", agentStatus);
        return;
    }

    auto getDevice = reinterpret_cast<GetDevice>(
        dlsym(RTLD_NEXT, "hipGetDevice"));
    if (getDevice == nullptr) {
        initializationError = hipErrorSharedObjectSymbolNotFound;
        return;
    }
    int device = 0;
    initializationError = getDevice(&device);
    if (initializationError != hipSuccess) {
        return;
    }
    if (device < 0 ||
        static_cast<std::size_t>(device) >= agents.gpus.size()) {
        initializationError = hipErrorInvalidDevice;
        return;
    }
    runtime.agent = agents.gpus[device];

    RegionResult region;
    const hsa_status_t regionStatus =
        hsa_agent_iterate_regions(
            runtime.agent, findKernargRegion, &region);
    if (regionStatus != HSA_STATUS_SUCCESS &&
        regionStatus != HSA_STATUS_INFO_BREAK) {
        initializationError =
            hsaError("hsa_agent_iterate_regions", regionStatus);
        return;
    }
    if (region.region.handle == 0) {
        initializationError = hipErrorNotFound;
        return;
    }
    runtime.kernargRegion = region.region;

    const char *hsaco = std::getenv("SE_HIP_MEMSET_HSACO");
    if (hsaco == nullptr || hsaco[0] == '\0') {
        initializationError = hipErrorFileNotFound;
        return;
    }
    std::vector<std::uint8_t> image;
    if (!readFile(hsaco, &image)) {
        initializationError = hipErrorFileNotFound;
        return;
    }

    hsa_code_object_reader_t reader = {};
    hsa_status_t status = hsa_code_object_reader_create_from_memory(
        image.data(), image.size(), &reader);
    if (status != HSA_STATUS_SUCCESS) {
        initializationError =
            hsaError("hsa_code_object_reader_create_from_memory", status);
        return;
    }

    status = hsa_executable_create_alt(
        HSA_PROFILE_FULL,
        HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT,
        nullptr,
        &runtime.executable);
    if (status == HSA_STATUS_SUCCESS) {
        hsa_loaded_code_object_t loaded = {};
        status = hsa_executable_load_agent_code_object(
            runtime.executable,
            runtime.agent,
            reader,
            nullptr,
            &loaded);
    }
    if (status == HSA_STATUS_SUCCESS) {
        status = hsa_executable_freeze(runtime.executable, nullptr);
    }
    hsa_code_object_reader_destroy(reader);
    if (status != HSA_STATUS_SUCCESS) {
        initializationError =
            hsaError("load memset code object", status);
        return;
    }

    hsa_executable_symbol_t symbol = {};
    status = hsa_executable_get_symbol_by_name(
        runtime.executable,
        "seHipMemsetKernel",
        &runtime.agent,
        &symbol);
    if (status != HSA_STATUS_SUCCESS) {
        status = hsa_executable_get_symbol_by_name(
            runtime.executable,
            "seHipMemsetKernel@kd",
            &runtime.agent,
            &symbol);
    }
    if (status != HSA_STATUS_SUCCESS) {
        initializationError =
            hsaError("hsa_executable_get_symbol_by_name", status);
        return;
    }

    if (!getSymbolInfo(
            symbol,
            HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT,
            &runtime.kernelObject) ||
        !getSymbolInfo(
            symbol,
            HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE,
            &runtime.kernargSize) ||
        !getSymbolInfo(
            symbol,
            HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE,
            &runtime.groupSize) ||
        !getSymbolInfo(
            symbol,
            HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE,
            &runtime.privateSize)) {
        return;
    }

    status = hsa_queue_create(
        runtime.agent,
        64,
        HSA_QUEUE_TYPE_MULTI,
        nullptr,
        nullptr,
        0,
        0,
        &runtime.queue);
    if (status != HSA_STATUS_SUCCESS) {
        initializationError = hsaError("hsa_queue_create", status);
    }
}

template <typename T>
std::size_t
appendArgument(
    std::uint8_t *buffer,
    std::size_t offset,
    const T &value)
{
    const std::size_t alignment = alignof(T);
    offset = (offset + alignment - 1) & ~(alignment - 1);
    std::memcpy(buffer + offset, &value, sizeof(T));
    return offset + sizeof(T);
}

hipError_t
dispatch(void *dst, int value, std::size_t size)
{
    constexpr std::uint32_t workgroupSize = 256;
    const std::size_t gridSize =
        ((size + workgroupSize - 1) / workgroupSize) * workgroupSize;
    if (gridSize > UINT32_MAX) {
        return hipErrorInvalidValue;
    }

    const std::size_t allocationSize =
        runtime.kernargSize == 0 ? 24 : runtime.kernargSize;
    void *kernarg = nullptr;
    hsa_status_t status = hsa_memory_allocate(
        runtime.kernargRegion, allocationSize, &kernarg);
    if (status != HSA_STATUS_SUCCESS) {
        return hsaError("hsa_memory_allocate", status);
    }
    std::memset(kernarg, 0, allocationSize);
    auto *bytes = static_cast<std::uint8_t *>(kernarg);
    std::size_t offset = 0;
    offset = appendArgument(bytes, offset, dst);
    const std::uint32_t byteValue =
        static_cast<unsigned char>(value);
    offset = appendArgument(bytes, offset, byteValue);
    offset = appendArgument(bytes, offset, size);
    if (offset > allocationSize) {
        hsa_memory_free(kernarg);
        return hipErrorInvalidValue;
    }

    hsa_signal_t completion = {};
    status = hsa_signal_create(1, 0, nullptr, &completion);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_memory_free(kernarg);
        return hsaError("hsa_signal_create", status);
    }

    const std::uint64_t index =
        hsa_queue_add_write_index_scacq_screl(runtime.queue, 1);
    auto *ring = static_cast<hsa_kernel_dispatch_packet_t *>(
        runtime.queue->base_address);
    auto *packet = &ring[index & (runtime.queue->size - 1)];
    std::memset(packet, 0, sizeof(*packet));
    packet->setup =
        1 << HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;
    packet->workgroup_size_x = workgroupSize;
    packet->workgroup_size_y = 1;
    packet->workgroup_size_z = 1;
    packet->grid_size_x = static_cast<std::uint32_t>(gridSize);
    packet->grid_size_y = 1;
    packet->grid_size_z = 1;
    packet->private_segment_size = runtime.privateSize;
    packet->group_segment_size = runtime.groupSize;
    packet->kernel_object = runtime.kernelObject;
    packet->kernarg_address = kernarg;
    packet->completion_signal = completion;

    const std::uint16_t header =
        (HSA_PACKET_TYPE_KERNEL_DISPATCH << HSA_PACKET_HEADER_TYPE) |
        (1 << HSA_PACKET_HEADER_BARRIER) |
        (HSA_FENCE_SCOPE_SYSTEM <<
            HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE) |
        (HSA_FENCE_SCOPE_SYSTEM <<
            HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
    __atomic_store_n(&packet->header, header, __ATOMIC_RELEASE);
    hsa_signal_store_screlease(runtime.queue->doorbell_signal, index);

    const hsa_signal_value_t result = hsa_signal_wait_scacquire(
        completion,
        HSA_SIGNAL_CONDITION_LT,
        1,
        UINT64_MAX,
        HSA_WAIT_STATE_BLOCKED);
    hsa_signal_destroy(completion);
    hsa_memory_free(kernarg);
    return result == 0 ? hipSuccess : hipErrorLaunchFailure;
}

} // anonymous namespace

extern "C" hipError_t
hipMemset(void *dst, int value, size_t sizeBytes)
{
    if (sizeBytes == 0) {
        return hipSuccess;
    }
    if (dst == nullptr) {
        return hipErrorInvalidDevicePointer;
    }
    std::call_once(initializeOnce, initialize);
    if (initializationError != hipSuccess) {
        return initializationError;
    }
    return dispatch(dst, value, sizeBytes);
}
