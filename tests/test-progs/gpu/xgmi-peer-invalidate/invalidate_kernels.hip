#include <hip/hip_runtime.h>

#include <cstdint>

extern "C" __global__ void
write_value(volatile uint32_t *target, uint32_t value)
{
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        *target = value;
    }
}

extern "C" __global__ void
read_value(const volatile uint32_t *target, uint32_t *observed)
{
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        observed[0] = *target;
    }
}

extern "C" __global__ void
classify_value(const volatile uint32_t *target, uint32_t *observed,
               uint32_t value_a, uint32_t value_b)
{
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        const uint32_t value = *target;
        observed[1] = value;
        observed[2] = value == value_b ? 0 : (value == value_a ? 1 : 2);
    }
}
