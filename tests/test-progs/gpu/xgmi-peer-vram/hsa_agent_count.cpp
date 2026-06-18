#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
#include <hsakmt.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{

const char *
device_type_name(hsa_device_type_t type)
{
    switch (type) {
      case HSA_DEVICE_TYPE_CPU:
        return "CPU";
      case HSA_DEVICE_TYPE_GPU:
        return "GPU";
      case HSA_DEVICE_TYPE_DSP:
        return "DSP";
      default:
        return "UNKNOWN";
    }
}

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

void
print_file_excerpt(const char *path)
{
    std::printf("sysfs_check path=%s ", path);

    struct stat st = {};
    if (stat(path, &st) != 0) {
        std::printf("stat=FAIL errno=%d %s\n", errno, std::strerror(errno));
        return;
    }

    std::printf("stat=OK mode=%o size=%ld", st.st_mode,
                static_cast<long>(st.st_size));
    if (S_ISDIR(st.st_mode)) {
        std::printf(" type=dir entries=");
        DIR *dir = opendir(path);
        if (!dir) {
            std::printf("<opendir failed errno=%d %s>\n", errno,
                        std::strerror(errno));
            return;
        }
        bool first = true;
        while (dirent *entry = readdir(dir)) {
            if (std::strcmp(entry->d_name, ".") == 0 ||
                std::strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            std::printf("%s%s", first ? "" : ",", entry->d_name);
            first = false;
        }
        closedir(dir);
        std::printf("\n");
        return;
    }

    FILE *file = std::fopen(path, "r");
    if (!file) {
        std::printf(" fopen=FAIL errno=%d %s\n", errno, std::strerror(errno));
        return;
    }

    char buffer[256] = {};
    size_t count = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    for (size_t i = 0; i < count; ++i) {
        if (buffer[i] == '\n') {
            buffer[i] = ';';
        }
    }
    std::printf(" data=\"%s\"\n", buffer);
}

void
print_sysfs_visibility()
{
    print_file_excerpt("/sys/devices/virtual/kfd/kfd/topology");
    print_file_excerpt("/sys/devices/virtual/kfd/kfd/topology/generation_id");
    print_file_excerpt(
        "/sys/devices/virtual/kfd/kfd/topology/system_properties");
    print_file_excerpt("/sys/devices/virtual/kfd/kfd/topology/nodes");
    print_file_excerpt("/sys/devices/virtual/kfd/kfd/topology/nodes/0/gpu_id");
    print_file_excerpt(
        "/sys/devices/virtual/kfd/kfd/topology/nodes/0/properties");
    print_file_excerpt("/sys/devices/virtual/kfd/kfd/topology/nodes/1/gpu_id");
    print_file_excerpt(
        "/sys/devices/virtual/kfd/kfd/topology/nodes/1/properties");
    print_file_excerpt("/dev/dri/renderD128");
    print_file_excerpt("/dev/dri/renderD129");
}

void
print_kmt_topology()
{
    HsaVersionInfo version = {};
    HSAKMT_STATUS kmt_status = hsaKmtGetVersion(&version);
    std::printf("hsaKmtGetVersion: %s(%d) version=%u.%u\n",
                kmt_status_name(kmt_status), static_cast<int>(kmt_status),
                version.KernelInterfaceMajorVersion,
                version.KernelInterfaceMinorVersion);
    if (kmt_status != HSAKMT_STATUS_SUCCESS) {
        return;
    }

    hsaKmtReleaseSystemProperties();

    HsaSystemProperties system = {};
    kmt_status = hsaKmtAcquireSystemProperties(&system);
    std::printf("hsaKmtAcquireSystemProperties: %s(%d) nodes=%u "
                "platform_oem=%u platform_id=%u platform_rev=%u\n",
                kmt_status_name(kmt_status), static_cast<int>(kmt_status),
                system.NumNodes, system.PlatformOem, system.PlatformId,
                system.PlatformRev);
    if (kmt_status != HSAKMT_STATUS_SUCCESS) {
        return;
    }

    for (uint32_t node = 0; node < system.NumNodes; ++node) {
        HsaNodeProperties props = {};
        kmt_status = hsaKmtGetNodeProperties(node, &props);
        std::printf("kmt_node[%u]: %s(%d) cpu_cores=%u simd=%u "
                    "mem_banks=%u io_links=%u vendor=0x%x "
                    "device=0x%x gfx=%u.%u.%u drm_minor=%d hive=%lu\n",
                    node, kmt_status_name(kmt_status),
                    static_cast<int>(kmt_status), props.NumCPUCores,
                    props.NumFComputeCores, props.NumMemoryBanks,
                    props.NumIOLinks, props.VendorId, props.DeviceId,
                    props.EngineId.ui32.Major,
                    props.EngineId.ui32.Minor,
                    props.EngineId.ui32.Stepping, props.DrmRenderMinor,
                    props.HiveID);
    }
}

struct Counts
{
    int total = 0;
    int gpu = 0;
};

hsa_status_t
agent_callback(hsa_agent_t agent, void *data)
{
    auto *counts = static_cast<Counts *>(data);
    counts->total++;

    hsa_device_type_t type = HSA_DEVICE_TYPE_CPU;
    hsa_status_t status = hsa_agent_get_info(
        agent, HSA_AGENT_INFO_DEVICE, &type);
    if (status != HSA_STATUS_SUCCESS) {
        std::fprintf(stderr, "hsa_agent_get_info(DEVICE) failed: %s\n",
                     status_name(status));
        return status;
    }

    char name[64] = {};
    status = hsa_agent_get_info(agent, HSA_AGENT_INFO_NAME, name);
    if (status != HSA_STATUS_SUCCESS) {
        std::fprintf(stderr, "hsa_agent_get_info(NAME) failed: %s\n",
                     status_name(status));
        return status;
    }

    hsa_isa_t isa = {};
    status = hsa_agent_get_info(agent, HSA_AGENT_INFO_ISA, &isa);
    const bool has_isa = status == HSA_STATUS_SUCCESS;

    if (type == HSA_DEVICE_TYPE_GPU) {
        counts->gpu++;
    }

    std::printf("agent[%d] type=%s name=%s isa_handle=0x%lx\n",
                counts->total - 1, device_type_name(type), name,
                has_isa ? isa.handle : 0);
    return HSA_STATUS_SUCCESS;
}

} // namespace

int
main()
{
    hsa_status_t status = hsa_init();
    if (status != HSA_STATUS_SUCCESS) {
        std::fprintf(stderr, "hsa_init failed: %s\n", status_name(status));
        return 1;
    }

    print_sysfs_visibility();
    print_kmt_topology();

    Counts counts;
    status = hsa_iterate_agents(agent_callback, &counts);
    if (status != HSA_STATUS_SUCCESS) {
        std::fprintf(stderr, "hsa_iterate_agents failed: %s\n",
                     status_name(status));
        hsa_shut_down();
        return 1;
    }

    std::printf("HSA agents total=%d gpu=%d\n", counts.total, counts.gpu);
    hsa_shut_down();
    return counts.gpu >= 2 ? 0 : 1;
}
