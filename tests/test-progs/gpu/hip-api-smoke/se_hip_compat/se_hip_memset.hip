#include <hip/hip_runtime.h>

#include <cstddef>
#include <cstdint>

extern "C" __global__ void
seHipMemsetKernel(
    unsigned char *dst,
    std::uint32_t value,
    std::size_t size)
{
    const std::size_t index =
        static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index < size) {
        dst[index] = static_cast<unsigned char>(value);
    }
}
