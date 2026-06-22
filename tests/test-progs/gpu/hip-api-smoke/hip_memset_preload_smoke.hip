#include <hip/hip_runtime.h>
#include <gem5/m5ops.h>

#include <cstddef>
#include <cstdio>

int
main()
{
    constexpr std::size_t bytes = 1024;
    constexpr unsigned char expected = 0xa5;

    unsigned char *deviceData = nullptr;
    hipError_t error = hipSetDevice(0);
    if (error == hipSuccess) {
        error = hipMalloc(&deviceData, bytes);
    }
    if (error == hipSuccess) {
        error = hipMemset(deviceData, expected, bytes);
    }
    if (error != hipSuccess) {
        std::fprintf(
            stderr,
            "HIP memset setup failed: %s\n",
            hipGetErrorString(error));
        return 1;
    }

    volatile const unsigned char *observed = deviceData;
    for (std::size_t index = 0; index < bytes; ++index) {
        if (observed[index] != expected) {
            std::fprintf(
                stderr,
                "HIP memset mismatch at %zu: actual=0x%02x "
                "expected=0x%02x\n",
                index,
                static_cast<unsigned int>(observed[index]),
                static_cast<unsigned int>(expected));
            return 1;
        }
    }

    std::printf("HIP_MEMSET_PRELOAD_PASSED\n");
    std::fflush(stdout);
    m5_exit(0);
    return 2;
}
