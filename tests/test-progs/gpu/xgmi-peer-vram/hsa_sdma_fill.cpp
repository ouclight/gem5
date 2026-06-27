#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

#include <cstdint>
#include <cstdio>

namespace
{

constexpr size_t FillBytes = 4096;
constexpr uint32_t FillPattern = 0x5a6b7c8d;

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
    std::printf("[hsa_sdma_fill] %s: %s(%d)\n",
                label, status_name(status), status);
    std::fflush(stdout);
}

struct AgentSearch
{
    hsa_agent_t gpu = {};
};

struct PoolSearch
{
    hsa_amd_memory_pool_t pool = {};
};

hsa_status_t
find_first_gpu(hsa_agent_t agent, void *data)
{
    auto *search = static_cast<AgentSearch *>(data);
    if (search->gpu.handle != 0) {
        return HSA_STATUS_SUCCESS;
    }

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

    std::printf("[hsa_sdma_fill] agent handle=0x%lx type=%d name=%s\n",
                agent.handle, static_cast<int>(type), name);
    std::fflush(stdout);

    if (type == HSA_DEVICE_TYPE_GPU) {
        search->gpu = agent;
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
    hsa_amd_memory_pool_get_info(
        pool, HSA_AMD_MEMORY_POOL_INFO_SIZE, &size);

    uint32_t global_flags = 0;
    hsa_amd_memory_pool_get_info(
        pool, HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS, &global_flags);

    std::printf("[hsa_sdma_fill] pool handle=0x%lx segment=%d "
                "alloc=%d size=%zu global_flags=0x%x\n",
                pool.handle, static_cast<int>(segment), alloc_allowed,
                size, global_flags);
    std::fflush(stdout);

    if (segment == HSA_AMD_SEGMENT_GLOBAL && alloc_allowed &&
        size >= FillBytes) {
        search->pool = pool;
    }
    return HSA_STATUS_SUCCESS;
}

} // namespace

int
main()
{
    std::printf("[hsa_sdma_fill] begin hsa_init\n");
    std::fflush(stdout);
    hsa_status_t status = hsa_init();
    print_status("end hsa_init", status);
    if (status != HSA_STATUS_SUCCESS) {
        return 1;
    }

    AgentSearch agent_search;
    std::printf("[hsa_sdma_fill] begin hsa_iterate_agents\n");
    std::fflush(stdout);
    status = hsa_iterate_agents(find_first_gpu, &agent_search);
    print_status("end hsa_iterate_agents", status);
    if (status != HSA_STATUS_SUCCESS || agent_search.gpu.handle == 0) {
        std::fprintf(stderr, "[hsa_sdma_fill] no GPU agent found\n");
        hsa_shut_down();
        return 1;
    }

    PoolSearch pool_search;
    std::printf("[hsa_sdma_fill] begin "
                "hsa_amd_agent_iterate_memory_pools\n");
    std::fflush(stdout);
    status = hsa_amd_agent_iterate_memory_pools(
        agent_search.gpu, find_allocable_global_pool, &pool_search);
    print_status("end hsa_amd_agent_iterate_memory_pools", status);
    if (status != HSA_STATUS_SUCCESS || pool_search.pool.handle == 0) {
        std::fprintf(stderr, "[hsa_sdma_fill] no allocable GPU pool found\n");
        hsa_shut_down();
        return 1;
    }

    void *device_data = nullptr;
    std::printf("[hsa_sdma_fill] begin hsa_amd_memory_pool_allocate\n");
    std::fflush(stdout);
    status = hsa_amd_memory_pool_allocate(
        pool_search.pool, FillBytes, 0, &device_data);
    print_status("end hsa_amd_memory_pool_allocate", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_shut_down();
        return 1;
    }

    std::printf("[hsa_sdma_fill] allocated ptr=%p bytes=%zu pattern=0x%x\n",
                device_data, FillBytes, FillPattern);
    std::printf("[hsa_sdma_fill] begin hsa_amd_memory_fill\n");
    std::fflush(stdout);
    status = hsa_amd_memory_fill(
        device_data, FillPattern, FillBytes / sizeof(uint32_t));
    print_status("end hsa_amd_memory_fill", status);

    hsa_status_t free_status = hsa_amd_memory_pool_free(device_data);
    print_status("end hsa_amd_memory_pool_free", free_status);

    hsa_status_t shutdown_status = hsa_shut_down();
    print_status("end hsa_shut_down", shutdown_status);

    if (status != HSA_STATUS_SUCCESS ||
        free_status != HSA_STATUS_SUCCESS ||
        shutdown_status != HSA_STATUS_SUCCESS) {
        return 1;
    }

    std::printf("HSA_SDMA_FILL_PASSED\n");
    std::fflush(stdout);
    return 0;
}
