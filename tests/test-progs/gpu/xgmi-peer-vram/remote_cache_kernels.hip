#include <hip/hip_runtime.h>

#include <cstdint>

struct RemoteCacheControl
{
    uint32_t first_read_done;
    uint32_t allow_second_read;
    uint32_t reader_done;
    uint32_t first_value;
    uint32_t second_value;
    uint32_t classification;
    uint32_t first_values[1024];
    uint32_t second_values[1024];
    uint32_t classifications[1024];
};

enum RemoteCacheClassification : uint32_t
{
    RemoteCacheUnset = 0,
    RemoteCacheUpdated = 1,
    RemoteCacheStale = 2,
    RemoteCacheUnexpected = 3,
};

extern "C" __global__ void remote_cache_reader(const uint32_t *target,
                    volatile RemoteCacheControl *control,
                    uint32_t value_a,
                    uint32_t value_b,
                    uint32_t spin_limit)
{
    if (blockIdx.x != 0 || threadIdx.x != 0) {
        return;
    }

    const uint32_t first = target[0];
    control->first_value = first;
    __threadfence_system();
    control->first_read_done = 1;
    __threadfence_system();

    uint32_t spins = 0;
    while (control->allow_second_read == 0 && spins < spin_limit) {
        ++spins;
    }

    if (control->allow_second_read == 0) {
        control->second_value = 0xffffffffu;
        control->classification = RemoteCacheUnexpected;
        __threadfence_system();
        control->reader_done = 1;
        return;
    }

    const uint32_t second = target[0];
    control->second_value = second;

    if (first == value_a && second == value_b) {
        control->classification = RemoteCacheUpdated;
    } else if (first == value_a && second == value_a) {
        control->classification = RemoteCacheStale;
    } else {
        control->classification = RemoteCacheUnexpected;
    }

    __threadfence_system();
    control->reader_done = 1;
}

extern "C" __global__ void
remote_cache_multi_offset_reader(const uint32_t *target,
                    volatile RemoteCacheControl *control,
                    uint32_t first_base,
                    uint32_t second_base,
                    uint32_t count,
                    uint32_t spin_limit)
{
    if (blockIdx.x != 0 || threadIdx.x != 0) {
        return;
    }
    if (count > 1024) {
        count = 1024;
    }

    for (uint32_t i = 0; i < count; ++i) {
        control->first_values[i] = target[i];
    }
    __threadfence_system();
    control->first_read_done = 1;
    __threadfence_system();

    uint32_t spins = 0;
    while (control->allow_second_read == 0 && spins < spin_limit) {
        ++spins;
    }

    if (control->allow_second_read == 0) {
        control->classification = RemoteCacheUnexpected;
        __threadfence_system();
        control->reader_done = 1;
        return;
    }

    uint32_t aggregate = RemoteCacheUpdated;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t first = control->first_values[i];
        const uint32_t second = target[i];
        const uint32_t expected_first = first_base + i;
        const uint32_t expected_second = second_base + i;
        uint32_t classification = RemoteCacheUnexpected;

        control->second_values[i] = second;
        if (first == expected_first && second == expected_second) {
            classification = RemoteCacheUpdated;
        } else if (first == expected_first && second == expected_first) {
            classification = RemoteCacheStale;
            if (aggregate == RemoteCacheUpdated) {
                aggregate = RemoteCacheStale;
            }
        } else {
            aggregate = RemoteCacheUnexpected;
        }
        control->classifications[i] = classification;
    }

    control->classification = aggregate;
    __threadfence_system();
    control->reader_done = 1;
}

extern "C" __global__ void
remote_cache_snapshot_reader(const uint32_t *target,
                    volatile uint32_t *values,
                    uint32_t count)
{
    if (blockIdx.x != 0 || threadIdx.x != 0) {
        return;
    }
    if (count > 1024) {
        count = 1024;
    }

    for (uint32_t i = 0; i < count; ++i) {
        values[i] = target[i];
    }
    __threadfence_system();
}

extern "C" __global__ void
remote_cache_writer(uint32_t *target, uint32_t value)
{
    if (blockIdx.x != 0 || threadIdx.x != 0) {
        return;
    }

    *target = value;
    __threadfence_system();
}

extern "C" __global__ void
remote_cache_multi_offset_writer(uint32_t *target,
                    uint32_t value_base,
                    uint32_t count)
{
    if (blockIdx.x != 0 || threadIdx.x != 0) {
        return;
    }
    if (count > 1024) {
        count = 1024;
    }

    for (uint32_t i = 0; i < count; ++i) {
        target[i] = value_base + i;
    }
    __threadfence_system();
}
