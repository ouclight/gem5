#include <hip/hip_runtime.h>
#include <gem5/m5ops.h>

#include <cstddef>
#include <cstdio>

namespace
{

void
logPhase(const char *message)
{
    std::printf("[hip_free_smoke] %s\n", message);
    std::fflush(stdout);
}

bool
checkHip(hipError_t error, const char *operation)
{
    if (error == hipSuccess) {
        return true;
    }

    std::fprintf(
        stderr,
        "HIP free smoke failed during %s: %s\n",
        operation,
        hipGetErrorString(error));
    return false;
}

} // anonymous namespace

int
main()
{
    constexpr std::size_t bytes = 1024;

    unsigned char *device_data = nullptr;

    logPhase("begin hipSetDevice(0)");
    if (!checkHip(hipSetDevice(0), "hipSetDevice")) {
        return 1;
    }
    logPhase("end hipSetDevice(0)");

    logPhase("begin hipMalloc");
    if (!checkHip(hipMalloc(&device_data, bytes), "hipMalloc")) {
        return 1;
    }
    logPhase("end hipMalloc");

    logPhase("begin hipFree");
    if (!checkHip(hipFree(device_data), "hipFree")) {
        return 1;
    }
    logPhase("end hipFree");

    std::printf("HIP_FREE_PASSED\n");
    std::fflush(stdout);
    m5_exit(0);
    return 2;
}
