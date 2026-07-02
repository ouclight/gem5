#include <hip/hip_runtime.h>

#include <cstdint>

struct ResidentFlagControl
{
    uint32_t first_read_done;
    uint32_t reader_done;
    uint32_t first_value;
    uint32_t second_value;
    uint32_t seen_flag;
    uint32_t spins;
    uint32_t classification;
};

enum ResidentFlagClassification : uint32_t
{
    ResidentFlagUnset = 0,
    ResidentFlagUpdated = 1,
    ResidentFlagStaleValue = 2,
    ResidentFlagTimeout = 3,
    ResidentFlagUnexpected = 4,
};

enum ResidentFlagSyncMode : uint32_t
{
    ResidentFlagSyncStrict = 0,
    ResidentFlagSyncWriterFenceOnly = 1,
    ResidentFlagSyncNoFence = 2,
};

static __device__ __forceinline__ uint32_t
volatile_load_u32(const uint32_t *ptr)
{
    return *reinterpret_cast<const volatile uint32_t *>(ptr);
}

extern "C" __global__ void
resident_flag_reader(const uint32_t *target,
                     volatile ResidentFlagControl *control,
                     uint32_t value_dword,
                     uint32_t flag_dword,
                     uint32_t old_value,
                     uint32_t updated_value,
                     uint32_t spin_limit,
                     uint32_t sync_mode)
{
    if (blockIdx.x != 0 || threadIdx.x != 0) {
        return;
    }

    const uint32_t first = target[value_dword];
    control->first_value = first;
    __threadfence_system();
    control->first_read_done = 1;
    __threadfence_system();

    uint32_t spins = 0;
    uint32_t flag = 0;
    while (spins < spin_limit) {
        flag = volatile_load_u32(&target[flag_dword]);
        if (flag == 1) {
            break;
        }
        ++spins;
    }

    control->seen_flag = flag;
    control->spins = spins;

    if (flag != 1) {
        control->second_value = 0xffffffffu;
        control->classification = ResidentFlagTimeout;
        __threadfence_system();
        control->reader_done = 1;
        return;
    }

    if (sync_mode == ResidentFlagSyncStrict) {
        __threadfence_system();
    }

    const uint32_t second = target[value_dword];
    control->second_value = second;

    if (first == old_value && second == updated_value) {
        control->classification = ResidentFlagUpdated;
    } else if (first == old_value && second == old_value) {
        control->classification = ResidentFlagStaleValue;
    } else {
        control->classification = ResidentFlagUnexpected;
    }

    __threadfence_system();
    control->reader_done = 1;
}

extern "C" __global__ void
resident_flag_writer(uint32_t *target,
                     uint32_t value_dword,
                     uint32_t flag_dword,
                     uint32_t updated_value,
                     uint32_t sync_mode)
{
    if (blockIdx.x != 0 || threadIdx.x != 0) {
        return;
    }

    target[value_dword] = updated_value;
    if (sync_mode != ResidentFlagSyncNoFence) {
        __threadfence_system();
    }

    target[flag_dword] = 1;
    if (sync_mode == ResidentFlagSyncStrict) {
        __threadfence_system();
    }
}
