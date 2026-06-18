#include <hip/hip_runtime.h>

#include <cstdint>

extern "C" __global__ void
write_peer_vram(uint32_t *mem, uint32_t count, uint32_t seed)
{
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid < count) {
        mem[tid] = seed ^ tid;
    }
}

extern "C" __global__ void
verify_peer_vram(uint32_t *mem, uint32_t *errors, uint32_t count,
                 uint32_t seed)
{
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid < count) {
        errors[tid] = mem[tid] == (seed ^ tid) ? 0 : 1;
    }
}
