#include <hip/hip_runtime.h>
#include <gem5/m5ops.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace
{

bool
checkHip(hipError_t error, const char *operation)
{
    if (error == hipSuccess) {
        return true;
    }

    std::fprintf(
        stderr,
        "HIP lifecycle preload smoke failed during %s: %s\n",
        operation,
        hipGetErrorString(error));
    return false;
}

void
cleanupDeviceData(std::uint32_t *ptr)
{
    if (ptr != nullptr) {
        hipFree(ptr);
    }
}

void
logPhase(const char *message)
{
    std::printf("[hip_lifecycle_preload] %s\n", message);
    std::fflush(stdout);
}

bool
checkWords(
    const std::uint32_t *actual,
    const std::uint32_t *expected,
    std::size_t count,
    const char *phase)
{
    for (std::size_t index = 0; index < count; ++index) {
        if (actual[index] != expected[index]) {
            std::fprintf(
                stderr,
                "HIP lifecycle %s mismatch at %zu actual=0x%08x "
                "expected=0x%08x\n",
                phase,
                index,
                actual[index],
                expected[index]);
            return false;
        }
    }
    return true;
}

bool
checkBytes(
    volatile const unsigned char *actual,
    unsigned char expected,
    std::size_t bytes,
    const char *phase)
{
    for (std::size_t index = 0; index < bytes; ++index) {
        if (actual[index] != expected) {
            std::fprintf(
                stderr,
                "HIP lifecycle %s mismatch at byte %zu actual=0x%02x "
                "expected=0x%02x\n",
                phase,
                index,
                static_cast<unsigned int>(actual[index]),
                static_cast<unsigned int>(expected));
            return false;
        }
    }
    return true;
}

} // anonymous namespace

int
main()
{
    constexpr std::size_t count = 256;
    constexpr std::size_t bytes = count * sizeof(std::uint32_t);
    constexpr int memset_byte = 0x5a;

    std::uint32_t host_input[count];
    std::uint32_t host_output[count];
    for (std::size_t index = 0; index < count; ++index) {
        host_input[index] = 0x70000000u + static_cast<std::uint32_t>(index);
        host_output[index] = 0;
    }

    std::uint32_t *device_data = nullptr;
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

    logPhase("begin hipMemset");
    if (!checkHip(
            hipMemset(device_data, memset_byte, bytes),
            "hipMemset")) {
        cleanupDeviceData(device_data);
        return 1;
    }
    logPhase("end hipMemset");
    if (!checkBytes(
            reinterpret_cast<volatile const unsigned char *>(device_data),
            static_cast<unsigned char>(memset_byte),
            bytes,
            "memset")) {
        cleanupDeviceData(device_data);
        return 1;
    }

    logPhase("begin hipMemcpy H2D");
    if (!checkHip(
            hipMemcpy(device_data, host_input, bytes, hipMemcpyHostToDevice),
            "hipMemcpy H2D")) {
        cleanupDeviceData(device_data);
        return 1;
    }
    logPhase("end hipMemcpy H2D");

    logPhase("begin hipMemcpy D2H");
    if (!checkHip(
            hipMemcpy(host_output, device_data, bytes, hipMemcpyDeviceToHost),
            "hipMemcpy D2H")) {
        cleanupDeviceData(device_data);
        return 1;
    }
    logPhase("end hipMemcpy D2H");
    if (!checkWords(host_output, host_input, count, "copy")) {
        cleanupDeviceData(device_data);
        return 1;
    }

    logPhase("begin hipFree");
    if (!checkHip(hipFree(device_data), "hipFree")) {
        return 1;
    }
    logPhase("end hipFree");

    std::printf("HIP_LIFECYCLE_PRELOAD_PASSED\n");
    std::fflush(stdout);
    m5_exit(0);
    return 2;
}
