#include <hip/hip_runtime.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

constexpr uint32_t count = 256;
constexpr size_t bytes = count * sizeof(uint32_t);
constexpr uint8_t memset_byte = 0xa5;
constexpr uint32_t memset_word = 0xa5a5a5a5;
constexpr uint32_t kernel_seed = 0x5a17c0de;

enum class Stage
{
    Malloc = 0,
    Memset = 1,
    Memcpy = 2,
    Launch = 3,
    Lifecycle = 4,
};

#define HIP_CHECK(call)                                                     \
    do {                                                                    \
        std::printf("[hip_api_smoke] begin %s\n", #call);                  \
        std::fflush(stdout);                                                \
        hipError_t error = (call);                                          \
        if (error != hipSuccess) {                                          \
            std::fprintf(stderr, "%s:%d: %s failed: %s\n", __FILE__,       \
                         __LINE__, #call, hipGetErrorString(error));         \
            return 1;                                                       \
        }                                                                   \
        std::printf("[hip_api_smoke] end %s\n", #call);                    \
        std::fflush(stdout);                                                \
    } while (0)

#define HIP_STAGE_CHECK(call)                                               \
    do {                                                                    \
        std::printf("[hip_api_smoke] begin %s\n", #call);                  \
        std::fflush(stdout);                                                \
        hipError_t error = (call);                                          \
        if (error != hipSuccess) {                                          \
            std::fprintf(stderr, "%s:%d: %s failed: %s\n", __FILE__,       \
                         __LINE__, #call, hipGetErrorString(error));         \
            return false;                                                   \
        }                                                                   \
        std::printf("[hip_api_smoke] end %s\n", #call);                    \
        std::fflush(stdout);                                                \
    } while (0)

static bool
parse_stage(int argc, char **argv, Stage *stage, const char **stage_name)
{
    if (argc != 3 || std::strcmp(argv[1], "--stage") != 0) {
        return false;
    }

    struct StageEntry
    {
        const char *name;
        Stage stage;
    };
    static constexpr StageEntry stages[] = {
        {"malloc", Stage::Malloc},
        {"memset", Stage::Memset},
        {"memcpy", Stage::Memcpy},
        {"launch", Stage::Launch},
        {"lifecycle", Stage::Lifecycle},
    };

    for (const auto &entry : stages) {
        if (std::strcmp(argv[2], entry.name) == 0) {
            *stage = entry.stage;
            *stage_name = entry.name;
            return true;
        }
    }
    return false;
}

__global__ void
transform_kernel(uint32_t *data, uint32_t size, uint32_t seed)
{
    const uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index < size) {
        data[index] ^= seed;
    }
}

static bool
report_mismatches(const char *label, const std::vector<uint32_t> &actual,
                  const std::vector<uint32_t> &expected)
{
    uint32_t mismatches = 0;
    uint32_t first_index = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (actual[i] != expected[i]) {
            if (mismatches == 0) {
                first_index = i;
            }
            ++mismatches;
        }
    }
    if (mismatches != 0) {
        std::fprintf(
            stderr,
            "%s failed: mismatches=%u first_index=%u actual=0x%08x "
            "expected=0x%08x\n",
            label, mismatches, first_index, actual[first_index],
            expected[first_index]);
        return false;
    }
    return true;
}

static bool
validate_memset(const std::vector<uint32_t> &actual)
{
    return report_mismatches(
        "validate_memset", actual,
        std::vector<uint32_t>(count, memset_word));
}

static bool
validate_round_trip(const std::vector<uint32_t> &actual,
                    const std::vector<uint32_t> &input)
{
    return report_mismatches("validate_round_trip", actual, input);
}

static bool
validate_kernel(const std::vector<uint32_t> &actual,
                const std::vector<uint32_t> &input)
{
    std::vector<uint32_t> expected(count);
    for (uint32_t i = 0; i < count; ++i) {
        expected[i] = input[i] ^ kernel_seed;
    }
    return report_mismatches("validate_kernel", actual, expected);
}

static bool
run_malloc_stage(uint32_t **device_data)
{
    int device_count = 0;
    HIP_STAGE_CHECK(hipGetDeviceCount(&device_count));
    if (device_count < 1) {
        std::fprintf(stderr, "No HIP devices found\n");
        return false;
    }

    HIP_STAGE_CHECK(hipSetDevice(0));
    HIP_STAGE_CHECK(hipMalloc(device_data, bytes));
    if (*device_data == nullptr) {
        std::fprintf(stderr, "hipMalloc returned a null pointer\n");
        return false;
    }
    return true;
}

static bool
run_memset_stage(uint32_t *device_data)
{
    HIP_STAGE_CHECK(hipMemset(device_data, memset_byte, bytes));
    HIP_STAGE_CHECK(hipDeviceSynchronize());

    std::vector<uint32_t> output(count);
    HIP_STAGE_CHECK(hipMemcpy(
        output.data(), device_data, bytes, hipMemcpyDeviceToHost));
    return validate_memset(output);
}

static bool
run_memcpy_stage(uint32_t *device_data, std::vector<uint32_t> *input)
{
    input->resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        (*input)[i] = 0x10000000u + i;
    }

    HIP_STAGE_CHECK(hipMemcpy(
        device_data, input->data(), bytes, hipMemcpyHostToDevice));
    std::vector<uint32_t> output(count);
    HIP_STAGE_CHECK(hipMemcpy(
        output.data(), device_data, bytes, hipMemcpyDeviceToHost));
    return validate_round_trip(output, *input);
}

static bool
run_launch_stage(uint32_t *device_data,
                 const std::vector<uint32_t> &input)
{
    constexpr uint32_t threads = 64;
    constexpr uint32_t blocks = (count + threads - 1) / threads;

    std::printf("[hip_api_smoke] begin transform_kernel<<<%u, %u>>>\n",
                blocks, threads);
    std::fflush(stdout);
    transform_kernel<<<blocks, threads>>>(device_data, count, kernel_seed);
    std::printf("[hip_api_smoke] end transform_kernel<<<%u, %u>>>\n",
                blocks, threads);
    std::fflush(stdout);

    HIP_STAGE_CHECK(hipGetLastError());
    HIP_STAGE_CHECK(hipDeviceSynchronize());

    std::vector<uint32_t> output(count);
    HIP_STAGE_CHECK(hipMemcpy(
        output.data(), device_data, bytes, hipMemcpyDeviceToHost));
    return validate_kernel(output, input);
}

int
main(int argc, char **argv)
{
    Stage selected;
    const char *stage_name = nullptr;
    if (!parse_stage(argc, argv, &selected, &stage_name)) {
        std::fprintf(
            stderr,
            "usage: %s --stage malloc|memset|memcpy|launch|lifecycle\n",
            argv[0]);
        return 2;
    }

    uint32_t *device_data = nullptr;
    if (!run_malloc_stage(&device_data)) {
        return 1;
    }
    if (selected == Stage::Malloc) {
        std::printf("API_STAGE_PASSED stage=%s\n", stage_name);
        std::fflush(stdout);
        return 0;
    }

    if (!run_memset_stage(device_data)) {
        return 1;
    }
    if (selected == Stage::Memset) {
        std::printf("API_STAGE_PASSED stage=%s\n", stage_name);
        std::fflush(stdout);
        return 0;
    }

    std::vector<uint32_t> input;
    if (!run_memcpy_stage(device_data, &input)) {
        return 1;
    }
    if (selected == Stage::Memcpy) {
        std::printf("API_STAGE_PASSED stage=%s\n", stage_name);
        std::fflush(stdout);
        return 0;
    }

    if (!run_launch_stage(device_data, input)) {
        return 1;
    }
    if (selected == Stage::Launch) {
        std::printf("API_STAGE_PASSED stage=%s\n", stage_name);
        std::fflush(stdout);
        return 0;
    }

    HIP_CHECK(hipFree(device_data));
    std::printf("LIFECYCLE_PASSED\n");
    std::fflush(stdout);
    return 0;
}
