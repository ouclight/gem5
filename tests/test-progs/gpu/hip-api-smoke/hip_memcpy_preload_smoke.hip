#include <hip/hip_runtime.h>
#include <gem5/m5ops.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

int
main()
{
    constexpr std::size_t count = 256;
    constexpr std::size_t bytes = count * sizeof(std::uint32_t);

    std::uint32_t input[count];
    std::uint32_t output[count];
    for (std::size_t index = 0; index < count; ++index) {
        input[index] = 0x60000000u + static_cast<std::uint32_t>(index);
        output[index] = 0;
    }

    std::uint32_t *device_data = nullptr;
    hipError_t error = hipSetDevice(0);
    if (error == hipSuccess) {
        error = hipMalloc(&device_data, bytes);
    }
    if (error == hipSuccess) {
        error = hipMemcpy(device_data, input, bytes, hipMemcpyHostToDevice);
    }
    if (error == hipSuccess) {
        error = hipMemcpy(output, device_data, bytes, hipMemcpyDeviceToHost);
    }
    if (error != hipSuccess) {
        std::fprintf(
            stderr,
            "HIP memcpy preload smoke failed: %s\n",
            hipGetErrorString(error));
        return 1;
    }

    for (std::size_t index = 0; index < count; ++index) {
        if (output[index] != input[index]) {
            std::fprintf(
                stderr,
                "HIP memcpy mismatch at %zu actual=0x%08x "
                "expected=0x%08x\n",
                index,
                output[index],
                input[index]);
            return 1;
        }
    }

    std::printf("HIP_MEMCPY_PRELOAD_PASSED\n");
    std::fflush(stdout);
    m5_exit(0);
    return 2;
}
