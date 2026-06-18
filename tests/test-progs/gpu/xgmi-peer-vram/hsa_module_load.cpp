#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
#include <hsakmt.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef PEER_VRAM_HSACO_PATH
#define PEER_VRAM_HSACO_PATH "peer_vram_kernels.hsaco"
#endif

namespace
{

const char *
status_name(hsa_status_t status)
{
    const char *name = nullptr;
    if (hsa_status_string(status, &name) == HSA_STATUS_SUCCESS && name) {
        return name;
    }
    return "unknown HSA error";
}

const char *
kmt_status_name(HSAKMT_STATUS status)
{
    switch (status) {
      case HSAKMT_STATUS_SUCCESS:
        return "HSAKMT_STATUS_SUCCESS";
      case HSAKMT_STATUS_ERROR:
        return "HSAKMT_STATUS_ERROR";
      case HSAKMT_STATUS_INVALID_PARAMETER:
        return "HSAKMT_STATUS_INVALID_PARAMETER";
      case HSAKMT_STATUS_NO_MEMORY:
        return "HSAKMT_STATUS_NO_MEMORY";
      case HSAKMT_STATUS_NOT_SUPPORTED:
        return "HSAKMT_STATUS_NOT_SUPPORTED";
      default:
        return "unknown HSAKMT status";
    }
}

const char *
kmt_heap_name(HSA_HEAPTYPE heap)
{
    switch (heap) {
      case HSA_HEAPTYPE_SYSTEM:
        return "SYSTEM";
      case HSA_HEAPTYPE_FRAME_BUFFER_PRIVATE:
        return "FRAME_BUFFER_PRIVATE";
      case HSA_HEAPTYPE_FRAME_BUFFER_PUBLIC:
        return "FRAME_BUFFER_PUBLIC";
      case HSA_HEAPTYPE_GPU_LDS:
        return "GPU_LDS";
      case HSA_HEAPTYPE_GPU_GDS:
        return "GPU_GDS";
      case HSA_HEAPTYPE_GPU_SCRATCH:
        return "GPU_SCRATCH";
      case HSA_HEAPTYPE_MMIO_REMAP:
        return "MMIO_REMAP";
      case HSA_HEAPTYPE_DEVICE_SVM:
        return "DEVICE_SVM";
      default:
        return "UNKNOWN";
    }
}

bool
read_file(const char *path, std::vector<uint8_t> &data)
{
    std::FILE *file = std::fopen(path, "rb");
    if (!file) {
        std::fprintf(stderr, "%s: %s\n", path, std::strerror(errno));
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fprintf(stderr, "fseek(%s): %s\n", path, std::strerror(errno));
        std::fclose(file);
        return false;
    }
    long size = std::ftell(file);
    if (size <= 0) {
        std::fprintf(stderr, "invalid size %ld for %s\n", size, path);
        std::fclose(file);
        return false;
    }
    if (std::fseek(file, 0, SEEK_SET) != 0) {
        std::fprintf(stderr, "fseek(%s): %s\n", path, std::strerror(errno));
        std::fclose(file);
        return false;
    }
    data.resize(static_cast<size_t>(size));
    size_t read = std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    if (read != data.size()) {
        std::fprintf(stderr, "read %zu of %zu bytes from %s\n", read,
                     data.size(), path);
        return false;
    }
    return true;
}

const char *
device_name(hsa_device_type_t type)
{
    switch (type) {
      case HSA_DEVICE_TYPE_CPU:
        return "CPU";
      case HSA_DEVICE_TYPE_GPU:
        return "GPU";
      default:
        return "OTHER";
    }
}

hsa_status_t
isa_callback(hsa_isa_t isa, void *)
{
    uint32_t name_size = 0;
    hsa_status_t status = hsa_isa_get_info_alt(
        isa, HSA_ISA_INFO_NAME_LENGTH, &name_size);
    if (status != HSA_STATUS_SUCCESS) {
        std::printf("  isa handle=0x%lx name_length status=%s(%d)\n",
                    isa.handle, status_name(status), status);
        return HSA_STATUS_SUCCESS;
    }

    std::vector<char> name(name_size + 1, 0);
    status = hsa_isa_get_info_alt(isa, HSA_ISA_INFO_NAME, name.data());
    std::printf("  isa handle=0x%lx name=%s status=%s(%d)\n", isa.handle,
                name.data(), status_name(status), status);
    return HSA_STATUS_SUCCESS;
}

struct AgentData
{
    hsa_agent_t first_gpu = {};
    int count = 0;
};

struct RegionData
{
    int count = 0;
    size_t test_size = 4096;
};

struct PoolData
{
    int count = 0;
    size_t test_size = 4096;
};

const char *
segment_name(hsa_region_segment_t segment)
{
    switch (segment) {
      case HSA_REGION_SEGMENT_GLOBAL:
        return "GLOBAL";
      case HSA_REGION_SEGMENT_READONLY:
        return "READONLY";
      case HSA_REGION_SEGMENT_PRIVATE:
        return "PRIVATE";
      case HSA_REGION_SEGMENT_GROUP:
        return "GROUP";
      case HSA_REGION_SEGMENT_KERNARG:
        return "KERNARG";
      default:
        return "UNKNOWN";
    }
}

const char *
amd_segment_name(hsa_amd_segment_t segment)
{
    switch (segment) {
      case HSA_AMD_SEGMENT_GLOBAL:
        return "GLOBAL";
      case HSA_AMD_SEGMENT_READONLY:
        return "READONLY";
      case HSA_AMD_SEGMENT_PRIVATE:
        return "PRIVATE";
      case HSA_AMD_SEGMENT_GROUP:
        return "GROUP";
      default:
        return "UNKNOWN";
    }
}

hsa_status_t
pool_callback(hsa_amd_memory_pool_t pool, void *data)
{
    auto *pool_data = static_cast<PoolData *>(data);

    hsa_amd_segment_t segment = HSA_AMD_SEGMENT_GLOBAL;
    uint32_t global_flags = 0;
    size_t size = 0;
    bool alloc_allowed = false;
    size_t granule = 0;
    size_t alignment = 0;

    hsa_status_t status = hsa_amd_memory_pool_get_info(
        pool, HSA_AMD_MEMORY_POOL_INFO_SEGMENT, &segment);
    if (status != HSA_STATUS_SUCCESS) {
        std::printf("  pool[%d] segment status=%s(%d)\n",
                    pool_data->count, status_name(status), status);
        pool_data->count++;
        return HSA_STATUS_SUCCESS;
    }

    hsa_amd_memory_pool_get_info(
        pool, HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS, &global_flags);
    hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_SIZE, &size);
    hsa_amd_memory_pool_get_info(
        pool, HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_ALLOWED,
        &alloc_allowed);
    if (alloc_allowed) {
        hsa_amd_memory_pool_get_info(
            pool, HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_GRANULE, &granule);
        hsa_amd_memory_pool_get_info(
            pool, HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_ALIGNMENT,
            &alignment);
    }

    std::printf("  pool[%d] handle=0x%lx segment=%s global_flags=0x%x "
                "size=%zu alloc=%d granule=%zu align=%zu\n",
                pool_data->count, pool.handle, amd_segment_name(segment),
                global_flags, size, alloc_allowed, granule, alignment);

    if (alloc_allowed && size >= pool_data->test_size) {
        void *ptr = nullptr;
        status = hsa_amd_memory_pool_allocate(
            pool, pool_data->test_size, 0, &ptr);
        std::printf("    hsa_amd_memory_pool_allocate(%zu): %s(%d) ptr=%p\n",
                    pool_data->test_size, status_name(status), status, ptr);
        if (status == HSA_STATUS_SUCCESS) {
            hsa_status_t free_status = hsa_amd_memory_pool_free(ptr);
            std::printf("    hsa_amd_memory_pool_free(%p): %s(%d)\n", ptr,
                        status_name(free_status), free_status);
        }
    }

    pool_data->count++;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t
region_callback(hsa_region_t region, void *data)
{
    auto *region_data = static_cast<RegionData *>(data);

    hsa_region_segment_t segment = HSA_REGION_SEGMENT_GLOBAL;
    uint32_t global_flags = 0;
    size_t size = 0;
    size_t max_size = 0;
    bool alloc_allowed = false;
    size_t granule = 0;
    size_t alignment = 0;

    hsa_status_t status = hsa_region_get_info(
        region, HSA_REGION_INFO_SEGMENT, &segment);
    if (status != HSA_STATUS_SUCCESS) {
        std::printf("  region[%d] segment status=%s(%d)\n",
                    region_data->count, status_name(status), status);
        region_data->count++;
        return HSA_STATUS_SUCCESS;
    }

    hsa_region_get_info(region, HSA_REGION_INFO_GLOBAL_FLAGS, &global_flags);
    hsa_region_get_info(region, HSA_REGION_INFO_SIZE, &size);
    hsa_region_get_info(region, HSA_REGION_INFO_ALLOC_MAX_SIZE, &max_size);
    hsa_region_get_info(
        region, HSA_REGION_INFO_RUNTIME_ALLOC_ALLOWED, &alloc_allowed);
    if (alloc_allowed) {
        hsa_region_get_info(
            region, HSA_REGION_INFO_RUNTIME_ALLOC_GRANULE, &granule);
        hsa_region_get_info(
            region, HSA_REGION_INFO_RUNTIME_ALLOC_ALIGNMENT, &alignment);
    }

    std::printf("  region[%d] handle=0x%lx segment=%s global_flags=0x%x "
                "size=%zu max=%zu alloc=%d granule=%zu align=%zu\n",
                region_data->count, region.handle, segment_name(segment),
                global_flags, size, max_size, alloc_allowed, granule,
                alignment);

    if (alloc_allowed && max_size >= region_data->test_size) {
        void *ptr = nullptr;
        status = hsa_memory_allocate(region, region_data->test_size, &ptr);
        std::printf("    hsa_memory_allocate(%zu): %s(%d) ptr=%p\n",
                    region_data->test_size, status_name(status), status, ptr);
        if (status == HSA_STATUS_SUCCESS) {
            hsa_status_t free_status = hsa_memory_free(ptr);
            std::printf("    hsa_memory_free(%p): %s(%d)\n", ptr,
                        status_name(free_status), free_status);
        }
    }

    region_data->count++;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t
agent_callback(hsa_agent_t agent, void *data)
{
    auto *agent_data = static_cast<AgentData *>(data);
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

    std::printf("agent[%d] handle=0x%lx type=%s name=%s\n",
                agent_data->count, agent.handle, device_name(type), name);
    hsa_agent_iterate_isas(agent, isa_callback, nullptr);
    RegionData region_data;
    hsa_status_t region_status =
        hsa_agent_iterate_regions(agent, region_callback, &region_data);
    std::printf("  hsa_agent_iterate_regions: %s(%d) regions=%d\n",
                status_name(region_status), region_status, region_data.count);

    PoolData pool_data;
    hsa_status_t pool_status = hsa_amd_agent_iterate_memory_pools(
        agent, pool_callback, &pool_data);
    std::printf("  hsa_amd_agent_iterate_memory_pools: %s(%d) pools=%d\n",
                status_name(pool_status), pool_status, pool_data.count);

    if (type == HSA_DEVICE_TYPE_GPU && agent_data->first_gpu.handle == 0) {
        agent_data->first_gpu = agent;
    }
    agent_data->count++;
    return HSA_STATUS_SUCCESS;
}

void
print_status(const char *label, hsa_status_t status)
{
    std::printf("%s: %s(%d)\n", label, status_name(status), status);
}

void
print_kmt_memory_properties()
{
    HsaSystemProperties system = {};
    HSAKMT_STATUS kmt_status = hsaKmtAcquireSystemProperties(&system);
    std::printf("kmt_acquire_for_mem_props: %s(%d) nodes=%u\n",
                kmt_status_name(kmt_status), static_cast<int>(kmt_status),
                system.NumNodes);
    if (kmt_status != HSAKMT_STATUS_SUCCESS) {
        return;
    }

    for (uint32_t node = 0; node < system.NumNodes; ++node) {
        HsaNodeProperties node_props = {};
        kmt_status = hsaKmtGetNodeProperties(node, &node_props);
        std::printf("kmt_node_mem_header[%u]: %s(%d) gpu_id? mem_banks=%u "
                    "cpu_cores=%u simd=%u local_mem=%lu drm_minor=%d\n",
                    node, kmt_status_name(kmt_status),
                    static_cast<int>(kmt_status), node_props.NumMemoryBanks,
                    node_props.NumCPUCores, node_props.NumFComputeCores,
                    node_props.LocalMemSize, node_props.DrmRenderMinor);
        if (kmt_status != HSAKMT_STATUS_SUCCESS ||
            node_props.NumMemoryBanks == 0) {
            continue;
        }

        std::vector<HsaMemoryProperties> mem_props(
            node_props.NumMemoryBanks);
        kmt_status = hsaKmtGetNodeMemoryProperties(
            node, node_props.NumMemoryBanks, mem_props.data());
        std::printf("  kmt_node_mem_props[%u]: %s(%d)\n", node,
                    kmt_status_name(kmt_status),
                    static_cast<int>(kmt_status));
        if (kmt_status != HSAKMT_STATUS_SUCCESS) {
            continue;
        }

        for (uint32_t bank = 0; bank < node_props.NumMemoryBanks; ++bank) {
            const auto &mem = mem_props[bank];
            std::printf("    bank[%u] heap=%s(%u) size=%lu base=0x%lx "
                        "flags=0x%x width=%u clk=%u\n",
                        bank, kmt_heap_name(mem.HeapType), mem.HeapType,
                        mem.SizeInBytes, mem.VirtualBaseAddress,
                        mem.Flags.MemoryProperty, mem.Width,
                        mem.MemoryClockMax);
        }
    }
}

bool
async_signal_handler(hsa_signal_value_t value, void *)
{
    std::printf("async_signal_handler invoked value=%ld\n",
                static_cast<long>(value));
    return false;
}

bool
probe_async_signal_handler()
{
    hsa_signal_t signal = {};
    hsa_status_t status = hsa_signal_create(0, 0, nullptr, &signal);
    print_status("hsa_signal_create(async_probe)", status);
    if (status != HSA_STATUS_SUCCESS) {
        return false;
    }

    status = hsa_amd_signal_async_handler(
        signal, HSA_SIGNAL_CONDITION_NE, 0, async_signal_handler, nullptr);
    print_status("hsa_amd_signal_async_handler(async_probe)", status);

    hsa_status_t destroy_status = hsa_signal_destroy(signal);
    print_status("hsa_signal_destroy(async_probe)", destroy_status);
    return status == HSA_STATUS_SUCCESS;
}

} // namespace

int
main()
{
    std::vector<uint8_t> hsaco;
    if (!read_file(PEER_VRAM_HSACO_PATH, hsaco)) {
        return 1;
    }
    std::printf("hsaco path=%s bytes=%zu magic=%02x %02x %02x %02x\n",
                PEER_VRAM_HSACO_PATH, hsaco.size(), hsaco[0], hsaco[1],
                hsaco[2], hsaco[3]);

    hsa_status_t status = hsa_init();
    print_status("hsa_init", status);
    if (status != HSA_STATUS_SUCCESS) {
        return 1;
    }

    print_kmt_memory_properties();

    AgentData agent_data;
    status = hsa_iterate_agents(agent_callback, &agent_data);
    print_status("hsa_iterate_agents", status);
    if (status != HSA_STATUS_SUCCESS || agent_data.first_gpu.handle == 0) {
        hsa_shut_down();
        return 1;
    }

    bool async_probe_ok = probe_async_signal_handler();
    std::printf("async_probe_ok=%d\n", async_probe_ok);

    hsa_code_object_reader_t reader = {};
    status = hsa_code_object_reader_create_from_memory(
        hsaco.data(), hsaco.size(), &reader);
    print_status("hsa_code_object_reader_create_from_memory", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_shut_down();
        return 1;
    }

    hsa_executable_t executable = {};
    status = hsa_executable_create_alt(
        HSA_PROFILE_FULL, HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT, nullptr,
        &executable);
    print_status("hsa_executable_create_alt", status);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_code_object_reader_destroy(reader);
        hsa_shut_down();
        return 1;
    }

    hsa_loaded_code_object_t loaded = {};
    status = hsa_executable_load_agent_code_object(
        executable, agent_data.first_gpu, reader, nullptr, &loaded);
    print_status("hsa_executable_load_agent_code_object", status);

    if (status == HSA_STATUS_SUCCESS) {
        status = hsa_executable_freeze(executable, nullptr);
        print_status("hsa_executable_freeze", status);
    }

    hsa_executable_destroy(executable);
    hsa_code_object_reader_destroy(reader);
    hsa_shut_down();
    return status == HSA_STATUS_SUCCESS ? 0 : 1;
}
