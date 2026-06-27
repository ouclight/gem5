#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{

constexpr size_t CopyBytes = 64 * 1024;

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
    std::printf("[hsa_sdma_peer_async_copy] %s: %s(%d)\n",
                label, status_name(status), status);
    std::fflush(stdout);
}

struct AgentSearch
{
    hsa_agent_t cpu = {};
    std::vector<hsa_agent_t> gpus;
};

struct PoolSearch
{
    hsa_amd_memory_pool_t pool = {};
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

    std::printf("[hsa_sdma_peer_async_copy] agent handle=0x%lx type=%d "
                "name=%s\n",
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
    hsa_amd_memory_pool_get_info(
        pool, HSA_AMD_MEMORY_POOL_INFO_SIZE, &size);

    uint32_t global_flags = 0;
    hsa_amd_memory_pool_get_info(
        pool, HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS, &global_flags);

    std::printf("[hsa_sdma_peer_async_copy] pool handle=0x%lx segment=%d "
                "alloc=%d size=%zu global_flags=0x%x\n",
                pool.handle, static_cast<int>(segment), alloc_allowed,
                size, global_flags);
    std::fflush(stdout);

    if (segment == HSA_AMD_SEGMENT_GLOBAL && alloc_allowed &&
        size >= CopyBytes) {
        search->pool = pool;
    }
    return HSA_STATUS_SUCCESS;
}

void
fill_pattern(std::vector<uint8_t> &buffer)
{
    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = static_cast<uint8_t>((i * 17 + (i >> 7) + 0x3d) & 0xff);
    }
}

bool
verify_pattern(const std::vector<uint8_t> &expected,
               const std::vector<uint8_t> &actual)
{
    for (size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] != actual[i]) {
            std::fprintf(stderr,
                         "[hsa_sdma_peer_async_copy] mismatch index=%zu "
                         "expected=0x%02x actual=0x%02x\n",
                         i, expected[i], actual[i]);
            return false;
        }
    }
    return true;
}

hsa_status_t
wait_for_copy(const char *label, hsa_signal_t signal)
{
    std::printf("[hsa_sdma_peer_async_copy] wait %s completion\n", label);
    std::fflush(stdout);

    hsa_signal_value_t value = hsa_signal_wait_scacquire(
        signal, HSA_SIGNAL_CONDITION_LT, 1, UINT64_MAX,
        HSA_WAIT_STATE_BLOCKED);
    if (value < 1) {
        return HSA_STATUS_SUCCESS;
    }

    std::fprintf(stderr,
                 "[hsa_sdma_peer_async_copy] %s completion wait returned %ld\n",
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

    std::printf("[hsa_sdma_peer_async_copy] begin %s async copy bytes=%zu\n",
                label, bytes);
    std::fflush(stdout);
    status = hsa_amd_memory_async_copy(dst, dst_agent, src, src_agent, bytes,
                                       0, nullptr, signal);
    print_status("hsa_amd_memory_async_copy", status);
    if (status == HSA_STATUS_SUCCESS) {
        status = wait_for_copy(label, signal);
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
    std::printf("[hsa_sdma_peer_async_copy] begin %s allow access\n", label);
    std::fflush(stdout);
    hsa_status_t status = hsa_amd_agents_allow_access(
        agents.size(), agents.data(), nullptr, ptr);
    print_status("hsa_amd_agents_allow_access", status);
    return status;
}

} // namespace

int
main()
{
    std::printf("[hsa_sdma_peer_async_copy] begin hsa_init\n");
    std::fflush(stdout);
    hsa_status_t status = hsa_init();
    print_status("end hsa_init", status);
    if (status != HSA_STATUS_SUCCESS) {
        return 1;
    }

    AgentSearch agent_search;
    std::printf("[hsa_sdma_peer_async_copy] begin hsa_iterate_agents\n");
    std::fflush(stdout);
    status = hsa_iterate_agents(find_cpu_and_gpus, &agent_search);
    print_status("end hsa_iterate_agents", status);
    if (status != HSA_STATUS_SUCCESS ||
        agent_search.cpu.handle == 0 ||
        agent_search.gpus.size() < 2) {
        std::fprintf(stderr,
                     "[hsa_sdma_peer_async_copy] required CPU/two GPU "
                     "agents not found\n");
        hsa_shut_down();
        return 1;
    }

    hsa_agent_t gpu0 = agent_search.gpus[0];
    hsa_agent_t gpu1 = agent_search.gpus[1];

    PoolSearch gpu0_pool;
    std::printf("[hsa_sdma_peer_async_copy] begin GPU0 "
                "hsa_amd_agent_iterate_memory_pools\n");
    std::fflush(stdout);
    status = hsa_amd_agent_iterate_memory_pools(
        gpu0, find_allocable_global_pool, &gpu0_pool);
    print_status("end GPU0 hsa_amd_agent_iterate_memory_pools", status);
    if (status != HSA_STATUS_SUCCESS || gpu0_pool.pool.handle == 0) {
        std::fprintf(stderr,
                     "[hsa_sdma_peer_async_copy] no allocable GPU0 pool found\n");
        hsa_shut_down();
        return 1;
    }

    PoolSearch gpu1_pool;
    std::printf("[hsa_sdma_peer_async_copy] begin GPU1 "
                "hsa_amd_agent_iterate_memory_pools\n");
    std::fflush(stdout);
    status = hsa_amd_agent_iterate_memory_pools(
        gpu1, find_allocable_global_pool, &gpu1_pool);
    print_status("end GPU1 hsa_amd_agent_iterate_memory_pools", status);
    if (status != HSA_STATUS_SUCCESS || gpu1_pool.pool.handle == 0) {
        std::fprintf(stderr,
                     "[hsa_sdma_peer_async_copy] no allocable GPU1 pool found\n");
        hsa_shut_down();
        return 1;
    }

    void *gpu0_data = nullptr;
    std::printf("[hsa_sdma_peer_async_copy] begin GPU0 "
                "hsa_amd_memory_pool_allocate\n");
    std::fflush(stdout);
    status = hsa_amd_memory_pool_allocate(
        gpu0_pool.pool, CopyBytes, 0, &gpu0_data);
    print_status("end GPU0 hsa_amd_memory_pool_allocate", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_shut_down();
        return 1;
    }

    void *gpu1_data = nullptr;
    std::printf("[hsa_sdma_peer_async_copy] begin GPU1 "
                "hsa_amd_memory_pool_allocate\n");
    std::fflush(stdout);
    status = hsa_amd_memory_pool_allocate(
        gpu1_pool.pool, CopyBytes, 0, &gpu1_data);
    print_status("end GPU1 hsa_amd_memory_pool_allocate", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_amd_memory_pool_free(gpu0_data);
        hsa_shut_down();
        return 1;
    }

    std::vector<hsa_agent_t> access_agents = {
        agent_search.cpu,
        gpu0,
        gpu1,
    };

    status = allow_access("GPU0 buffer", access_agents, gpu0_data);
    hsa_status_t allow_gpu1_status = HSA_STATUS_SUCCESS;
    if (status == HSA_STATUS_SUCCESS) {
        allow_gpu1_status = allow_access("GPU1 buffer", access_agents,
                                         gpu1_data);
    }

    std::vector<uint8_t> host_src(CopyBytes);
    std::vector<uint8_t> host_dst(CopyBytes, 0);
    fill_pattern(host_src);

    hsa_status_t h2d_status = HSA_STATUS_SUCCESS;
    hsa_status_t peer_status = HSA_STATUS_SUCCESS;
    hsa_status_t d2h_status = HSA_STATUS_SUCCESS;
    if (status == HSA_STATUS_SUCCESS &&
        allow_gpu1_status == HSA_STATUS_SUCCESS) {
        h2d_status = async_copy_and_wait("H2D_GPU0",
                                         gpu0_data, gpu0,
                                         host_src.data(), agent_search.cpu,
                                         CopyBytes);
    }
    if (h2d_status == HSA_STATUS_SUCCESS) {
        peer_status = async_copy_and_wait("GPU0_TO_GPU1",
                                          gpu1_data, gpu1,
                                          gpu0_data, gpu0,
                                          CopyBytes);
    }
    if (peer_status == HSA_STATUS_SUCCESS) {
        d2h_status = async_copy_and_wait("D2H_GPU1",
                                         host_dst.data(), agent_search.cpu,
                                         gpu1_data, gpu1,
                                         CopyBytes);
    }

    bool data_ok = false;
    if (h2d_status == HSA_STATUS_SUCCESS &&
        peer_status == HSA_STATUS_SUCCESS &&
        d2h_status == HSA_STATUS_SUCCESS) {
        data_ok = verify_pattern(host_src, host_dst);
    }

    hsa_status_t free_gpu0_status = hsa_amd_memory_pool_free(gpu0_data);
    print_status("end GPU0 hsa_amd_memory_pool_free", free_gpu0_status);
    hsa_status_t free_gpu1_status = hsa_amd_memory_pool_free(gpu1_data);
    print_status("end GPU1 hsa_amd_memory_pool_free", free_gpu1_status);
    hsa_status_t shutdown_status = hsa_shut_down();
    print_status("end hsa_shut_down", shutdown_status);

    if (status != HSA_STATUS_SUCCESS ||
        allow_gpu1_status != HSA_STATUS_SUCCESS ||
        h2d_status != HSA_STATUS_SUCCESS ||
        peer_status != HSA_STATUS_SUCCESS ||
        d2h_status != HSA_STATUS_SUCCESS ||
        free_gpu0_status != HSA_STATUS_SUCCESS ||
        free_gpu1_status != HSA_STATUS_SUCCESS ||
        shutdown_status != HSA_STATUS_SUCCESS ||
        !data_ok) {
        return 1;
    }

    std::printf("HSA_SDMA_PEER_ASYNC_COPY_PASSED\n");
    std::fflush(stdout);
    return 0;
}
