#include <hip/hip_runtime.h>
#include <gem5/m5ops.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static bool
parse_device(int argc, char **argv, int *device)
{
    *device = 0;
    if (argc == 1) {
        return true;
    }
    if (argc != 3 || std::strcmp(argv[1], "--device") != 0) {
        return false;
    }

    char *end = nullptr;
    const long parsed = std::strtol(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0' || parsed < 0 ||
        parsed > INT32_MAX) {
        return false;
    }
    *device = static_cast<int>(parsed);
    return true;
}

int
main(int argc, char **argv)
{
    constexpr std::size_t bytes = 1024;

    int device = 0;
    if (!parse_device(argc, argv, &device)) {
        std::fprintf(stderr, "usage: %s [--device N]\n", argv[0]);
        return 2;
    }

    unsigned char *device_data = nullptr;
    hipError_t error = hipSetDevice(device);
    if (error == hipSuccess) {
        error = hipMalloc(&device_data, bytes);
    }
    if (error != hipSuccess || device_data == nullptr) {
        std::fprintf(
            stderr,
            "HIP malloc smoke failed: error=%s ptr=%p\n",
            hipGetErrorString(error),
            static_cast<void *>(device_data));
        return 1;
    }

    std::printf("HIP_API_MALLOC_PASSED device=%d ptr=%p bytes=%zu\n",
                device, static_cast<void *>(device_data), bytes);
    std::fflush(stdout);
    m5_exit(0);
    return 2;
}
