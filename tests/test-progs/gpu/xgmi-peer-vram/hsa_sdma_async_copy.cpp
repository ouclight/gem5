#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{

constexpr size_t CopyBytes = 4096;

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
    std::printf("[hsa_sdma_async_copy] %s: %s(%d)\n",
                label, status_name(status), status);
    std::fflush(stdout);
}

struct AgentSearch
{
    hsa_agent_t cpu = {};
    hsa_agent_t gpu = {};
};

struct PoolSearch
{
    hsa_amd_memory_pool_t pool = {};
};

hsa_status_t
find_cpu_and_gpu(hsa_agent_t agent, void *data)
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

    std::printf("[hsa_sdma_async_copy] agent handle=0x%lx type=%d name=%s\n",
                agent.handle, static_cast<int>(type), name);
    std::fflush(stdout);

    if (type == HSA_DEVICE_TYPE_CPU && search->cpu.handle == 0) {
        search->cpu = agent;
    } else if (type == HSA_DEVICE_TYPE_GPU && search->gpu.handle == 0) {
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

    std::printf("[hsa_sdma_async_copy] pool handle=0x%lx segment=%d "
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
        buffer[i] = static_cast<uint8_t>((i * 37 + 0x5a) & 0xff);
    }
}

bool
verify_pattern(const std::vector<uint8_t> &expected,
               const std::vector<uint8_t> &actual)
{
    for (size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] != actual[i]) {
            std::fprintf(stderr,
                         "[hsa_sdma_async_copy] mismatch index=%zu "
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
    std::printf("[hsa_sdma_async_copy] wait %s completion\n", label);
    std::fflush(stdout);
    hsa_signal_value_t value = hsa_signal_wait_scacquire(
        signal, HSA_SIGNAL_CONDITION_LT, 1, UINT64_MAX,
        HSA_WAIT_STATE_BLOCKED);
    if (value < 1) {
        return HSA_STATUS_SUCCESS;
    }
    std::fprintf(stderr,
                 "[hsa_sdma_async_copy] %s completion wait returned %ld\n",
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

    std::printf("[hsa_sdma_async_copy] begin %s async copy bytes=%zu\n",
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

} // namespace

int
main()
{
    std::printf("[hsa_sdma_async_copy] begin hsa_init\n");
    std::fflush(stdout);
    hsa_status_t status = hsa_init();
    print_status("end hsa_init", status);
    if (status != HSA_STATUS_SUCCESS) {
        return 1;
    }

    AgentSearch agent_search;
    std::printf("[hsa_sdma_async_copy] begin hsa_iterate_agents\n");
    std::fflush(stdout);
    status = hsa_iterate_agents(find_cpu_and_gpu, &agent_search);
    print_status("end hsa_iterate_agents", status);
    if (status != HSA_STATUS_SUCCESS ||
        agent_search.cpu.handle == 0 ||
        agent_search.gpu.handle == 0) {
        std::fprintf(stderr,
                     "[hsa_sdma_async_copy] required CPU/GPU agents not found\n");
        hsa_shut_down();
        return 1;
    }

    PoolSearch pool_search;
    std::printf("[hsa_sdma_async_copy] begin "
                "hsa_amd_agent_iterate_memory_pools\n");
    std::fflush(stdout);
    status = hsa_amd_agent_iterate_memory_pools(
        agent_search.gpu, find_allocable_global_pool, &pool_search);
    print_status("end hsa_amd_agent_iterate_memory_pools", status);
    if (status != HSA_STATUS_SUCCESS || pool_search.pool.handle == 0) {
        std::fprintf(stderr,
                     "[hsa_sdma_async_copy] no allocable GPU pool found\n");
        hsa_shut_down();
        return 1;
    }

    void *device_data = nullptr;
    std::printf("[hsa_sdma_async_copy] begin hsa_amd_memory_pool_allocate\n");
    std::fflush(stdout);
    status = hsa_amd_memory_pool_allocate(
        pool_search.pool, CopyBytes, 0, &device_data);
    print_status("end hsa_amd_memory_pool_allocate", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_shut_down();
        return 1;
    }

    std::vector<uint8_t> host_src(CopyBytes);
    std::vector<uint8_t> host_dst(CopyBytes, 0);
    fill_pattern(host_src);

    status = async_copy_and_wait("H2D",
                                 device_data, agent_search.gpu,
                                 host_src.data(), agent_search.cpu,
                                 CopyBytes);
    hsa_status_t d2h_status = HSA_STATUS_SUCCESS;
    if (status == HSA_STATUS_SUCCESS) {
        d2h_status = async_copy_and_wait("D2H",
                                         host_dst.data(), agent_search.cpu,
                                         device_data, agent_search.gpu,
                                         CopyBytes);
    }

    bool data_ok = false;
    if (status == HSA_STATUS_SUCCESS && d2h_status == HSA_STATUS_SUCCESS) {
        data_ok = verify_pattern(host_src, host_dst);
    }

    hsa_status_t free_status = hsa_amd_memory_pool_free(device_data);
    print_status("end hsa_amd_memory_pool_free", free_status);

    hsa_status_t shutdown_status = hsa_shut_down();
    print_status("end hsa_shut_down", shutdown_status);

    if (status != HSA_STATUS_SUCCESS ||
        d2h_status != HSA_STATUS_SUCCESS ||
        free_status != HSA_STATUS_SUCCESS ||
        shutdown_status != HSA_STATUS_SUCCESS ||
        !data_ok) {
        return 1;
    }

    std::printf("HSA_SDMA_ASYNC_COPY_PASSED\n");
    std::fflush(stdout);
    return 0;
}
