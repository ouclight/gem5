# HIP/ROCm API Compatibility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a staged HIP/ROCm API compatibility baseline for gem5 SE multi-GPU, using the existing coherence smoke as the functional visibility baseline.

**Architecture:** Keep native ROCm for APIs that already complete, and use a narrow `LD_PRELOAD` compatibility library for APIs that route through unsupported or impractically slow SE paths. Each API gets an isolated smoke test with an explicit pass marker before it is added back into cumulative testing.

**Tech Stack:** gem5 SE VEGA_X86, ROCm 4.0.1 HIP/HSA, `hipcc`, HSA runtime, `LD_PRELOAD`, gem5 `m5_exit`, pyunit structural tests.

---

## File map

- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`
  - Add structural tests for new smoke binaries and preload contracts.
- Modify: `tests/test-progs/gpu/hip-api-smoke/Makefile`
  - Add build targets for standalone HIP API smokes.
- Modify: `tests/test-progs/gpu/hip-api-smoke/se_hip_compat/Makefile`
  - Keep building `libse_hip_compat.so`; add no new target unless a new HSACO is required.
- Modify: `tests/test-progs/gpu/hip-api-smoke/se_hip_compat/se_hip_compat.cpp`
  - Add narrow `hipMemcpy` interposition after diagnostics.
- Create: `tests/test-progs/gpu/hip-api-smoke/hip_malloc_smoke.hip`
  - Native `hipMalloc` standalone smoke.
- Create: `tests/test-progs/gpu/hip-api-smoke/hip_memcpy_preload_smoke.hip`
  - Preload H2D/D2H memcpy smoke.
- Create: `tests/test-progs/gpu/hip-api-smoke/hip_launch_native_smoke.hip`
  - Native kernel-launch smoke that validates with direct CPU read.
- Create: `tests/test-progs/gpu/hip-api-smoke/hip_free_native_smoke.hip`
  - Diagnostic native `hipFree` smoke.
- Modify: `docs/debug/se-multigpu-status.md`
  - Record every meaningful diagnostic step and the next single action.
- Read-only reference: ROCm 4.0.1 source tree
  - Use only when an API blocks or fails in native ROCm.
  - Preferred local location: `.deps/rocm-src-4.0.1`.
  - The user approved automatic download into `.deps` when source is missing.
  - Do not modify ROCm source as part of this plan.

## Task 1: Add native `hipMalloc` standalone smoke

**Files:**
- Create: `tests/test-progs/gpu/hip-api-smoke/hip_malloc_smoke.hip`
- Modify: `tests/test-progs/gpu/hip-api-smoke/Makefile`
- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`

- [ ] **Step 1: Add failing structural pyunit**

Append a test near the existing HIP API smoke tests in
`tests/pyunit/stdlib/test_se_viper_multigpu.py`:

```python
    def test_hip_malloc_smoke_contract(self):
        source = Path(
            "tests/test-progs/gpu/hip-api-smoke/hip_malloc_smoke.hip"
        ).read_text()
        makefile = Path(
            "tests/test-progs/gpu/hip-api-smoke/Makefile"
        ).read_text()

        self.assertIn("hipSetDevice(0)", source)
        self.assertIn("hipMalloc(&device_data", source)
        self.assertIn("HIP_API_MALLOC_PASSED", source)
        self.assertIn("m5_exit(0)", source)
        self.assertNotIn("hipFree(", source)
        self.assertIn("hip_malloc_smoke", makefile)
```

- [ ] **Step 2: Run pyunit and verify it fails**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest test_se_viper_multigpu.SEViperMultiGPUTest.test_hip_malloc_smoke_contract
```

Expected:

```text
FileNotFoundError
```

- [ ] **Step 3: Create `hip_malloc_smoke.hip`**

Create `tests/test-progs/gpu/hip-api-smoke/hip_malloc_smoke.hip`:

```cpp
#include <hip/hip_runtime.h>
#include <gem5/m5ops.h>

#include <cstddef>
#include <cstdio>

int
main()
{
    constexpr std::size_t bytes = 1024;

    unsigned char *device_data = nullptr;
    hipError_t error = hipSetDevice(0);
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

    std::printf("HIP_API_MALLOC_PASSED ptr=%p bytes=%zu\n",
                static_cast<void *>(device_data), bytes);
    std::fflush(stdout);
    m5_exit(0);
    return 2;
}
```

- [ ] **Step 4: Add Makefile target**

Modify `tests/test-progs/gpu/hip-api-smoke/Makefile`:

```make
MALLOC_SMOKE_TARGET := hip_malloc_smoke
MALLOC_SMOKE_SRC := hip_malloc_smoke.hip
```

Change:

```make
all: $(TARGET) $(PRELOAD_SMOKE_TARGET)
```

to:

```make
all: $(TARGET) $(PRELOAD_SMOKE_TARGET) $(MALLOC_SMOKE_TARGET)
```

Add:

```make
$(MALLOC_SMOKE_TARGET): $(MALLOC_SMOKE_SRC) $(M5OP_LIB)
	$(HIPCC) $(HIPCCFLAGS) -O2 --offload-arch=gfx900 \
		-I$(GEM5_ROOT)/include -o $@ $< -L. -lm5op_x86
```

Change clean to include the new target:

```make
clean:
	$(RM) $(TARGET) $(PRELOAD_SMOKE_TARGET) $(MALLOC_SMOKE_TARGET) \
		$(M5OP_OBJ) $(M5OP_LIB)
```

- [ ] **Step 5: Run pyunit and build**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest test_se_viper_multigpu.SEViperMultiGPUTest.test_hip_malloc_smoke_contract
make -C tests/test-progs/gpu/hip-api-smoke hip_malloc_smoke ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 HIPCC=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/bin/hipcc
strings tests/test-progs/gpu/hip-api-smoke/hip_malloc_smoke | rg "HIP_API_MALLOC_PASSED"
git diff --check
```

Expected:

```text
OK
HIP_API_MALLOC_PASSED
```

- [ ] **Step 6: User-owned simulation command**

Ask the user to run:

```bash
build/VEGA_X86/gem5.opt -d m5out-hip-api-malloc-native -r -e --stdout-file=simout.txt --stderr-file=simerr.txt --listener-mode=off configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py --cpu-type timing --app tests/test-progs/gpu/hip-api-smoke/hip_malloc_smoke --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 --env LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu --env HSA_ENABLE_SDMA=1
```

Expected stdout marker:

```text
HIP_API_MALLOC_PASSED
```

## Task 2: Stabilize existing preload `hipMemset` baseline

**Files:**
- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- Modify: `docs/debug/se-multigpu-status.md`

- [ ] **Step 1: Add structural coverage for preload run requirements**

Extend the existing `test_se_hip_memset_compatibility_layer_contract` with:

```python
        preload_smoke = Path(
            "tests/test-progs/gpu/hip-api-smoke/hip_memset_preload_smoke.hip"
        ).read_text()
        self.assertIn("HIP_MEMSET_PRELOAD_PASSED", preload_smoke)
        self.assertIn("volatile const unsigned char *observed", preload_smoke)
        self.assertIn("m5_exit(0)", preload_smoke)
        self.assertNotIn("hipMemcpy", preload_smoke)
```

- [ ] **Step 2: Run pyunit**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest test_se_viper_multigpu.SEViperMultiGPUTest.test_se_hip_memset_compatibility_layer_contract
```

Expected:

```text
OK
```

- [ ] **Step 3: Rebuild compatibility artifacts**

Run:

```bash
make -C tests/test-progs/gpu/hip-api-smoke/se_hip_compat ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 HIPCC=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/bin/hipcc CXX=g++
make -C tests/test-progs/gpu/hip-api-smoke hip_memset_preload_smoke ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 HIPCC=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/bin/hipcc
git diff --check
```

Expected files:

```text
tests/test-progs/gpu/hip-api-smoke/se_hip_compat/libse_hip_compat.so
tests/test-progs/gpu/hip-api-smoke/se_hip_compat/se_hip_memset.hsaco
tests/test-progs/gpu/hip-api-smoke/hip_memset_preload_smoke
```

- [ ] **Step 4: User-owned simulation command**

Ask the user to run:

```bash
build/VEGA_X86/gem5.opt -d m5out-hip-api-memset-preload -r -e --stdout-file=simout.txt --stderr-file=simerr.txt --listener-mode=off --debug-flags=GPUDriver,HSAPacketProcessor,SESDMAEngine,GPUDisp,GPUAgentDisp --debug-file=hip-api-memset-preload.trace configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py --cpu-type timing --app tests/test-progs/gpu/hip-api-smoke/hip_memset_preload_smoke --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 --env LD_PRELOAD=/home/zhangds/gem5/tests/test-progs/gpu/hip-api-smoke/se_hip_compat/libse_hip_compat.so --env SE_HIP_MEMSET_HSACO=/home/zhangds/gem5/tests/test-progs/gpu/hip-api-smoke/se_hip_compat/se_hip_memset.hsaco --env LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu --env HSA_ENABLE_SDMA=1
```

Expected stdout marker:

```text
HIP_MEMSET_PRELOAD_PASSED
```

## Task 3: Add preload `hipMemcpy` H2D/D2H smoke

**Files:**
- Create: `tests/test-progs/gpu/hip-api-smoke/hip_memcpy_preload_smoke.hip`
- Modify: `tests/test-progs/gpu/hip-api-smoke/Makefile`
- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`

- [ ] **Step 1: Add failing structural pyunit**

Add:

```python
    def test_hip_memcpy_preload_smoke_contract(self):
        source = Path(
            "tests/test-progs/gpu/hip-api-smoke/hip_memcpy_preload_smoke.hip"
        ).read_text()
        makefile = Path(
            "tests/test-progs/gpu/hip-api-smoke/Makefile"
        ).read_text()

        self.assertIn("hipMemcpy(device_data, input", source)
        self.assertIn("hipMemcpy(output", source)
        self.assertIn("hipMemcpyHostToDevice", source)
        self.assertIn("hipMemcpyDeviceToHost", source)
        self.assertIn("HIP_MEMCPY_PRELOAD_PASSED", source)
        self.assertIn("m5_exit(0)", source)
        self.assertIn("hip_memcpy_preload_smoke", makefile)
```

- [ ] **Step 2: Run pyunit and verify it fails**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest test_se_viper_multigpu.SEViperMultiGPUTest.test_hip_memcpy_preload_smoke_contract
```

Expected:

```text
FileNotFoundError
```

- [ ] **Step 3: Create H2D/D2H smoke**

Create `tests/test-progs/gpu/hip-api-smoke/hip_memcpy_preload_smoke.hip`:

```cpp
#include <hip/hip_runtime.h>
#include <gem5/m5ops.h>

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
        error = hipMemcpy(
            device_data, input, bytes, hipMemcpyHostToDevice);
    }
    if (error == hipSuccess) {
        error = hipMemcpy(
            output, device_data, bytes, hipMemcpyDeviceToHost);
    }
    if (error != hipSuccess) {
        std::fprintf(stderr, "HIP memcpy preload smoke failed: %s\n",
                     hipGetErrorString(error));
        return 1;
    }

    for (std::size_t index = 0; index < count; ++index) {
        if (output[index] != input[index]) {
            std::fprintf(
                stderr,
                "HIP memcpy mismatch at %zu actual=0x%08x expected=0x%08x\n",
                index, output[index], input[index]);
            return 1;
        }
    }

    std::printf("HIP_MEMCPY_PRELOAD_PASSED\n");
    std::fflush(stdout);
    m5_exit(0);
    return 2;
}
```

- [ ] **Step 4: Add Makefile target**

Modify `tests/test-progs/gpu/hip-api-smoke/Makefile`:

```make
MEMCPY_PRELOAD_SMOKE_TARGET := hip_memcpy_preload_smoke
MEMCPY_PRELOAD_SMOKE_SRC := hip_memcpy_preload_smoke.hip
```

Add the target to `all`:

```make
all: $(TARGET) $(PRELOAD_SMOKE_TARGET) $(MALLOC_SMOKE_TARGET) \
	$(MEMCPY_PRELOAD_SMOKE_TARGET)
```

Add:

```make
$(MEMCPY_PRELOAD_SMOKE_TARGET): $(MEMCPY_PRELOAD_SMOKE_SRC) $(M5OP_LIB)
	$(HIPCC) $(HIPCCFLAGS) -O2 --offload-arch=gfx900 \
		-I$(GEM5_ROOT)/include -o $@ $< -L. -lm5op_x86
```

Update clean:

```make
clean:
	$(RM) $(TARGET) $(PRELOAD_SMOKE_TARGET) $(MALLOC_SMOKE_TARGET) \
		$(MEMCPY_PRELOAD_SMOKE_TARGET) $(M5OP_OBJ) $(M5OP_LIB)
```

- [ ] **Step 5: Run pyunit and build**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest test_se_viper_multigpu.SEViperMultiGPUTest.test_hip_memcpy_preload_smoke_contract
make -C tests/test-progs/gpu/hip-api-smoke hip_memcpy_preload_smoke ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 HIPCC=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/bin/hipcc
strings tests/test-progs/gpu/hip-api-smoke/hip_memcpy_preload_smoke | rg "HIP_MEMCPY_PRELOAD_PASSED"
git diff --check
```

Expected:

```text
OK
HIP_MEMCPY_PRELOAD_PASSED
```

## Task 4: Implement narrow `hipMemcpy` preload support

**Files:**
- Modify: `tests/test-progs/gpu/hip-api-smoke/se_hip_compat/se_hip_compat.cpp`
- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`

- [ ] **Step 1: Add failing structural pyunit**

Add:

```python
    def test_se_hip_memcpy_compatibility_layer_contract(self):
        wrapper = Path(
            "tests/test-progs/gpu/hip-api-smoke/se_hip_compat/"
            "se_hip_compat.cpp"
        ).read_text()

        self.assertIn("hipMemcpy(void *dst", wrapper)
        self.assertIn("hipMemcpyHostToDevice", wrapper)
        self.assertIn("hipMemcpyDeviceToHost", wrapper)
        self.assertIn("hipMemcpyDeviceToDevice", wrapper)
        self.assertIn("hsa_amd_memory_async_copy", wrapper)
        self.assertIn("hsa_signal_wait_scacquire", wrapper)
        self.assertIn("RTLD_NEXT", wrapper)
```

- [ ] **Step 2: Run pyunit and verify it fails**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest test_se_viper_multigpu.SEViperMultiGPUTest.test_se_hip_memcpy_compatibility_layer_contract
```

Expected:

```text
AssertionError: 'hipMemcpy(void *dst' not found
```

- [ ] **Step 3: Add real function pointer and helper types**

In `se_hip_compat.cpp`, near `using GetDevice`, add:

```cpp
using RealHipMemcpy =
    hipError_t (*)(void *, const void *, size_t, hipMemcpyKind);
```

- [ ] **Step 4: Add HSA async-copy helper**

Add this helper inside the anonymous namespace:

```cpp
hipError_t
asyncCopyAndWait(void *dst, const void *src, std::size_t sizeBytes)
{
    hsa_signal_t completion = {};
    hsa_status_t status = hsa_signal_create(1, 0, nullptr, &completion);
    if (status != HSA_STATUS_SUCCESS) {
        return hsaError("hsa_signal_create", status);
    }

    std::call_once(initializeOnce, initialize);
    if (initializationError != hipSuccess) {
        hsa_signal_destroy(completion);
        return initializationError;
    }

    status = hsa_amd_memory_async_copy(
        dst,
        runtime.agent,
        src,
        runtime.agent,
        sizeBytes,
        0,
        nullptr,
        completion);
    if (status != HSA_STATUS_SUCCESS) {
        hsa_signal_destroy(completion);
        return hsaError("hsa_amd_memory_async_copy", status);
    }

    const hsa_signal_value_t result = hsa_signal_wait_scacquire(
        completion,
        HSA_SIGNAL_CONDITION_LT,
        1,
        UINT64_MAX,
        HSA_WAIT_STATE_BLOCKED);
    hsa_signal_destroy(completion);
    return result == 0 ? hipSuccess : hipErrorUnknown;
}
```

- [ ] **Step 5: Add `hipMemcpy` interposer**

Add after `hipMemset`:

```cpp
extern "C" hipError_t
hipMemcpy(void *dst, const void *src, size_t sizeBytes, hipMemcpyKind kind)
{
    if (sizeBytes == 0) {
        return hipSuccess;
    }
    if (dst == nullptr || src == nullptr) {
        return hipErrorInvalidValue;
    }

    if (kind == hipMemcpyHostToHost) {
        std::memcpy(dst, src, sizeBytes);
        return hipSuccess;
    }

    if (kind == hipMemcpyHostToDevice ||
        kind == hipMemcpyDeviceToHost ||
        kind == hipMemcpyDeviceToDevice) {
        return asyncCopyAndWait(dst, src, sizeBytes);
    }

    auto realHipMemcpy = reinterpret_cast<RealHipMemcpy>(
        dlsym(RTLD_NEXT, "hipMemcpy"));
    if (realHipMemcpy == nullptr) {
        return hipErrorSharedObjectSymbolNotFound;
    }
    return realHipMemcpy(dst, src, sizeBytes, kind);
}
```

- [ ] **Step 6: Run pyunit and build**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest test_se_viper_multigpu.SEViperMultiGPUTest.test_se_hip_memcpy_compatibility_layer_contract
make -C tests/test-progs/gpu/hip-api-smoke/se_hip_compat ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 HIPCC=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/bin/hipcc CXX=g++
nm -D tests/test-progs/gpu/hip-api-smoke/se_hip_compat/libse_hip_compat.so | rg "hipMemcpy|hipMemset"
git diff --check
```

Expected:

```text
OK
hipMemcpy
hipMemset
```

- [ ] **Step 7: User-owned simulation command**

Ask the user to run:

```bash
build/VEGA_X86/gem5.opt -d m5out-hip-api-memcpy-preload -r -e --stdout-file=simout.txt --stderr-file=simerr.txt --listener-mode=off --debug-flags=GPUDriver,HSAPacketProcessor,SESDMAEngine,GPUDisp,GPUAgentDisp --debug-file=hip-api-memcpy-preload.trace configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py --cpu-type timing --app tests/test-progs/gpu/hip-api-smoke/hip_memcpy_preload_smoke --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 --env LD_PRELOAD=/home/zhangds/gem5/tests/test-progs/gpu/hip-api-smoke/se_hip_compat/libse_hip_compat.so --env SE_HIP_MEMSET_HSACO=/home/zhangds/gem5/tests/test-progs/gpu/hip-api-smoke/se_hip_compat/se_hip_memset.hsaco --env LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu --env HSA_ENABLE_SDMA=1
```

Expected stdout marker:

```text
HIP_MEMCPY_PRELOAD_PASSED
```

## Task 5: Add native kernel-launch smoke with direct CPU validation

**Files:**
- Create: `tests/test-progs/gpu/hip-api-smoke/hip_launch_native_smoke.hip`
- Modify: `tests/test-progs/gpu/hip-api-smoke/Makefile`
- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`

- [ ] **Step 1: Add failing structural pyunit**

Add:

```python
    def test_hip_launch_native_smoke_contract(self):
        source = Path(
            "tests/test-progs/gpu/hip-api-smoke/hip_launch_native_smoke.hip"
        ).read_text()
        makefile = Path(
            "tests/test-progs/gpu/hip-api-smoke/Makefile"
        ).read_text()

        self.assertIn("__global__ void", source)
        self.assertIn("<<<", source)
        self.assertIn("hipDeviceSynchronize()", source)
        self.assertIn("volatile const std::uint32_t *observed", source)
        self.assertIn("HIP_LAUNCH_NATIVE_PASSED", source)
        self.assertIn("m5_exit(0)", source)
        self.assertNotIn("hipMemcpy", source)
        self.assertIn("hip_launch_native_smoke", makefile)
```

- [ ] **Step 2: Run pyunit and verify it fails**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest test_se_viper_multigpu.SEViperMultiGPUTest.test_hip_launch_native_smoke_contract
```

Expected:

```text
FileNotFoundError
```

- [ ] **Step 3: Create launch smoke**

Create `tests/test-progs/gpu/hip-api-smoke/hip_launch_native_smoke.hip`:

```cpp
#include <hip/hip_runtime.h>
#include <gem5/m5ops.h>

#include <cstdint>
#include <cstdio>

__global__ void
fillKernel(std::uint32_t *data, std::uint32_t count, std::uint32_t seed)
{
    const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index < count) {
        data[index] = seed ^ index;
    }
}

int
main()
{
    constexpr std::uint32_t count = 256;
    constexpr std::size_t bytes = count * sizeof(std::uint32_t);
    constexpr std::uint32_t seed = 0x13579bdf;

    std::uint32_t *device_data = nullptr;
    hipError_t error = hipSetDevice(0);
    if (error == hipSuccess) {
        error = hipMalloc(&device_data, bytes);
    }
    if (error != hipSuccess) {
        std::fprintf(stderr, "HIP launch malloc failed: %s\n",
                     hipGetErrorString(error));
        return 1;
    }

    fillKernel<<<4, 64>>>(device_data, count, seed);
    error = hipGetLastError();
    if (error == hipSuccess) {
        error = hipDeviceSynchronize();
    }
    if (error != hipSuccess) {
        std::fprintf(stderr, "HIP launch failed: %s\n",
                     hipGetErrorString(error));
        return 1;
    }

    volatile const std::uint32_t *observed = device_data;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t expected = seed ^ index;
        if (observed[index] != expected) {
            std::fprintf(
                stderr,
                "HIP launch mismatch at %u actual=0x%08x expected=0x%08x\n",
                index, observed[index], expected);
            return 1;
        }
    }

    std::printf("HIP_LAUNCH_NATIVE_PASSED\n");
    std::fflush(stdout);
    m5_exit(0);
    return 2;
}
```

- [ ] **Step 4: Add Makefile target**

Add variables:

```make
LAUNCH_NATIVE_SMOKE_TARGET := hip_launch_native_smoke
LAUNCH_NATIVE_SMOKE_SRC := hip_launch_native_smoke.hip
```

Add to `all` and `clean`, and add:

```make
$(LAUNCH_NATIVE_SMOKE_TARGET): $(LAUNCH_NATIVE_SMOKE_SRC) $(M5OP_LIB)
	$(HIPCC) $(HIPCCFLAGS) -O2 --offload-arch=gfx900 \
		-I$(GEM5_ROOT)/include -o $@ $< -L. -lm5op_x86
```

- [ ] **Step 5: Run pyunit and build**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest test_se_viper_multigpu.SEViperMultiGPUTest.test_hip_launch_native_smoke_contract
make -C tests/test-progs/gpu/hip-api-smoke hip_launch_native_smoke ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 HIPCC=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/bin/hipcc
strings tests/test-progs/gpu/hip-api-smoke/hip_launch_native_smoke | rg "HIP_LAUNCH_NATIVE_PASSED"
git diff --check
```

Expected:

```text
OK
HIP_LAUNCH_NATIVE_PASSED
```

## Task 6: Add native `hipFree` diagnostic smoke

**Files:**
- Create: `tests/test-progs/gpu/hip-api-smoke/hip_free_native_smoke.hip`
- Modify: `tests/test-progs/gpu/hip-api-smoke/Makefile`
- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`

- [ ] **Step 1: Add failing structural pyunit**

Add:

```python
    def test_hip_free_native_smoke_contract(self):
        source = Path(
            "tests/test-progs/gpu/hip-api-smoke/hip_free_native_smoke.hip"
        ).read_text()
        makefile = Path(
            "tests/test-progs/gpu/hip-api-smoke/Makefile"
        ).read_text()

        self.assertIn("hipMalloc(&device_data", source)
        self.assertIn("begin hipFree", source)
        self.assertIn("hipFree(device_data)", source)
        self.assertIn("HIP_FREE_NATIVE_PASSED", source)
        self.assertIn("m5_exit(0)", source)
        self.assertIn("hip_free_native_smoke", makefile)
```

- [ ] **Step 2: Run pyunit and verify it fails**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest test_se_viper_multigpu.SEViperMultiGPUTest.test_hip_free_native_smoke_contract
```

Expected:

```text
FileNotFoundError
```

- [ ] **Step 3: Create free diagnostic smoke**

Create `tests/test-progs/gpu/hip-api-smoke/hip_free_native_smoke.hip`:

```cpp
#include <hip/hip_runtime.h>
#include <gem5/m5ops.h>

#include <cstdio>

int
main()
{
    constexpr std::size_t bytes = 1024;

    unsigned char *device_data = nullptr;
    hipError_t error = hipSetDevice(0);
    if (error == hipSuccess) {
        error = hipMalloc(&device_data, bytes);
    }
    if (error != hipSuccess || device_data == nullptr) {
        std::fprintf(stderr, "HIP free diagnostic malloc failed: %s\n",
                     hipGetErrorString(error));
        return 1;
    }

    std::printf("[hip_free_native_smoke] begin hipFree(device_data)\n");
    std::fflush(stdout);
    error = hipFree(device_data);
    std::printf("[hip_free_native_smoke] end hipFree(device_data)\n");
    std::fflush(stdout);

    if (error != hipSuccess) {
        std::fprintf(stderr, "HIP free failed: %s\n",
                     hipGetErrorString(error));
        return 1;
    }

    std::printf("HIP_FREE_NATIVE_PASSED\n");
    std::fflush(stdout);
    m5_exit(0);
    return 2;
}
```

- [ ] **Step 4: Add Makefile target**

Add variables:

```make
FREE_NATIVE_SMOKE_TARGET := hip_free_native_smoke
FREE_NATIVE_SMOKE_SRC := hip_free_native_smoke.hip
```

Add to `all` and `clean`, and add:

```make
$(FREE_NATIVE_SMOKE_TARGET): $(FREE_NATIVE_SMOKE_SRC) $(M5OP_LIB)
	$(HIPCC) $(HIPCCFLAGS) -O2 --offload-arch=gfx900 \
		-I$(GEM5_ROOT)/include -o $@ $< -L. -lm5op_x86
```

- [ ] **Step 5: Run pyunit and build**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest test_se_viper_multigpu.SEViperMultiGPUTest.test_hip_free_native_smoke_contract
make -C tests/test-progs/gpu/hip-api-smoke hip_free_native_smoke ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 HIPCC=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/bin/hipcc
strings tests/test-progs/gpu/hip-api-smoke/hip_free_native_smoke | rg "HIP_FREE_NATIVE_PASSED"
git diff --check
```

Expected:

```text
OK
HIP_FREE_NATIVE_PASSED
```

## Task 7: Update status handoff after each user-owned run

**Files:**
- Modify: `docs/debug/se-multigpu-status.md`

- [ ] **Step 1: After each run, inspect output only**

Use the appropriate directory and run:

```bash
OUT=m5out-hip-api-malloc-native
tail -120 "$OUT/simout.txt"
tail -120 "$OUT/simerr.txt"
rg -n "HIP_.*PASSED|API_STAGE_PASSED|panic|fatal|assert|Exiting|simulate\\(\\) limit|user interrupt|hip_api_smoke|se_hip_compat|SESDMA|doorbell route|HSA AQL Kernel Complete" "$OUT/simout.txt" "$OUT/simerr.txt" "$OUT"/*.trace
```

Change `OUT` to the directory from the user-owned run before executing the
commands.

Expected for pass:

```text
HIP_*_PASSED
Exiting @ tick ... because m5_exit instruction encountered.
```

- [ ] **Step 2: Record the run in the status doc**

Append a section with:

```markdown
## 2026-07-01 HIP API malloc native result

Commands run by Codex:

```bash
OUT=m5out-hip-api-malloc-native
tail -120 "$OUT/simout.txt"
tail -120 "$OUT/simerr.txt"
rg -n "HIP_.*PASSED|panic|fatal|assert|Exiting" "$OUT/simout.txt" "$OUT/simerr.txt" "$OUT"/*.trace
```

First relevant result:

- `HIP_API_MALLOC_PASSED`

Facts confirmed:

- Native ROCm path was used.
- `hipMalloc` returned a non-null device pointer.
- The program exited through `m5_exit`.

Hypotheses accepted or rejected:

- Accepted: native `hipMalloc` is sufficient for this smoke.
- Rejected: this run proves `hipFree` teardown behavior.

Files changed:

- `docs/debug/se-multigpu-status.md`

Verification completed:

- stdout, stderr, and trace were inspected.

Single next diagnostic step: run the next planned API smoke.
```

Replace the API name, markers, facts, and next step with the observed run
before appending the section.

- [ ] **Step 3: Run diff check**

Run:

```bash
git diff --check
```

Expected: no output.

## Task 8: ROCm source root-cause analysis for blocked APIs

**Files:**
- Read: ROCm source tree under `.deps/rocm-src-4.0.1`
- Modify: `docs/debug/se-multigpu-status.md`

Run this task only after a user-owned API smoke blocks, exceeds its tick budget,
panics, or reaches an unexpected runtime path. Do not add a new preload shim or
gem5 workaround until this task identifies the likely internal ROCm path.

- [ ] **Step 1: Record the blocked public API boundary**

Use the relevant output directory:

```bash
OUT=m5out-hip-api-memcpy-preload
tail -160 "$OUT/simout.txt"
tail -160 "$OUT/simerr.txt"
rg -n "hipMalloc|hipMemset|hipMemcpy|hipFree|hipLaunch|hipDeviceSynchronize|begin|end|panic|fatal|assert|simulate\\(\\) limit|Exiting|SESDMA|doorbell route|HSA AQL Kernel Complete|COMGR|comgr|futex|mprotect|ioctl" "$OUT/simout.txt" "$OUT/simerr.txt" "$OUT"/*.trace
```

Change `OUT` to the blocked output directory before executing the commands.

Expected output:

```text
[hip_api_smoke] begin hipMemcpy(...)
Exiting @ tick 100000000000 because simulate() limit reached.
```

- [ ] **Step 2: Locate or download the matching ROCm source tree**

Check for a local ROCm source checkout:

```bash
find .deps/rocm-src-4.0.1 -maxdepth 3 -type d | head -80
```

Expected:

```text
.deps/rocm-src-4.0.1/HIP
.deps/rocm-src-4.0.1/ROCclr
.deps/rocm-src-4.0.1/ROCR-Runtime
```

If the directory does not exist, download the source into `.deps`:

```bash
mkdir -p .deps/rocm-src-4.0.1
git clone --depth 1 --branch rocm-4.0.1 https://github.com/ROCm/HIP.git .deps/rocm-src-4.0.1/HIP
git clone --depth 1 --branch rocm-4.0.1 https://github.com/ROCm/ROCclr.git .deps/rocm-src-4.0.1/ROCclr
git clone --depth 1 --branch rocm-4.0.1 https://github.com/RadeonOpenCompute/ROCR-Runtime.git .deps/rocm-src-4.0.1/ROCR-Runtime
```

If any repository does not have a `rocm-4.0.1` branch, run:

```bash
git ls-remote --heads https://github.com/ROCm/HIP.git | rg "rocm-4.0|roc-4.0|release"
git ls-remote --heads https://github.com/ROCm/ROCclr.git | rg "rocm-4.0|roc-4.0|release"
git ls-remote --heads https://github.com/RadeonOpenCompute/ROCR-Runtime.git | rg "rocm-4.0|roc-4.0|release"
```

Then select the nearest 4.0.x branch and record the exact branch in
`docs/debug/se-multigpu-status.md`.

- [ ] **Step 3: Search from the public API symbol**

For `hipMemset`, run:

```bash
rg -n "hipMemset|ihipMemset|memsetAsync|fill|Blit|blit|SDMA|hsa_amd_memory_fill|hsa_amd_memory_async_copy|comgr" .deps/rocm-src-4.0.1/HIP .deps/rocm-src-4.0.1/ROCclr .deps/rocm-src-4.0.1/ROCR-Runtime 2>/dev/null
```

For `hipMemcpy`, run:

```bash
rg -n "hipMemcpy|ihipMemcpy|memcpyAsync|copy|Blit|blit|SDMA|hsa_amd_memory_async_copy|hipMemcpyDefault" .deps/rocm-src-4.0.1/HIP .deps/rocm-src-4.0.1/ROCclr .deps/rocm-src-4.0.1/ROCR-Runtime 2>/dev/null
```

For `hipFree`, run:

```bash
rg -n "hipFree|ihipFree|free\\(|Memory|release|destroy|hsa_amd_memory_pool_free|hsa_memory_free|hsa_queue_destroy|hsa_executable_destroy|futex" .deps/rocm-src-4.0.1/HIP .deps/rocm-src-4.0.1/ROCclr .deps/rocm-src-4.0.1/ROCR-Runtime 2>/dev/null
```

For `hipLaunchKernel`, run:

```bash
rg -n "hipLaunchKernel|hipModuleLaunchKernel|ihipLaunchKernel|hsa_queue_add_write_index|doorbell|kernel_dispatch|completion_signal" .deps/rocm-src-4.0.1/HIP .deps/rocm-src-4.0.1/ROCclr .deps/rocm-src-4.0.1/ROCR-Runtime 2>/dev/null
```

Expected:

```text
.deps/rocm-src-4.0.1/HIP/.../hip_runtime_api.cpp:...
.deps/rocm-src-4.0.1/ROCclr/.../rocvirtual.cpp:...
```

- [ ] **Step 4: Trace the selected runtime branch**

Open the candidate files around each hit:

```bash
FILE=.deps/rocm-src-4.0.1/HIP/example/path/to/source.cpp
LINE=240
START=$((LINE > 100 ? LINE - 100 : 1))
END=$((LINE + 100))
sed -n "${START},${END}p" "$FILE"
```

Change `FILE` and `LINE` to the actual hit reported by `rg`.
Record the exact function chain in this format:

```text
public API:
  hipMemcpy
native internal path:
  hipMemcpy -> ihipMemcpy -> device copy backend selection
selected backend:
  SDMA
blocking wait object:
  HSA signal wait
gem5 trace correlation:
  SESDMAEngine queue drained at tick 123456789
```

- [ ] **Step 5: Decide fix layer from evidence**

Use this decision table:

| Evidence | Fix layer |
| --- | --- |
| ROCm selected SDMA and trace shows unhandled SDMA packet | `src/dev/hsa/se_sdma_engine.*` |
| ROCm waits on HSA signal that gem5 never updates | `src/dev/hsa/hsa_signal.*` or `HSAPacketProcessor` |
| ROCm uses compute blit and gem5 panics in GPU VMA/mtype | GPU VMA/mtype handling |
| ROCm spends time in COMGR/internal kernel compile | preload precompiled HSACO path |
| ROCm blocks in unsupported syscall/ioctl | gem5 syscall/KFD ioctl emulation |
| ROCm blocks in teardown after result known | test early `m5_exit` or gated fake-free design review |
| Source path remains ambiguous | add targeted runtime trace before changing behavior |

- [ ] **Step 6: Update status doc with source evidence**

Append:

```markdown
## 2026-07-01 HIP API memcpy ROCm source root-cause

Commands run by Codex:

```bash
OUT=m5out-hip-api-memcpy-preload
tail -160 "$OUT/simout.txt"
tail -160 "$OUT/simerr.txt"
rg -n "hipMemcpy|SESDMA|panic|fatal|assert|simulate\\(\\) limit" "$OUT/simout.txt" "$OUT/simerr.txt" "$OUT"/*.trace
rg -n "hipMemcpy|ihipMemcpy|hsa_amd_memory_async_copy" .deps/rocm-src-4.0.1/HIP .deps/rocm-src-4.0.1/ROCclr .deps/rocm-src-4.0.1/ROCR-Runtime 2>/dev/null
```

First relevant result:

- ROCm selected the SDMA async-copy path and waited on an HSA completion signal.

Facts confirmed:

- Public API: `hipMemcpy`
- ROCm function chain: `hipMemcpy -> ihipMemcpy -> hsa_amd_memory_async_copy`
- Backend selected: SDMA async copy
- gem5 trace correlation: `SESDMAEngine queue drained at tick ...`

Hypotheses accepted or rejected:

- Accepted: the blocked point is after SDMA submission.
- Rejected: the failure is caused by kernel-launch coherence behavior.

Files changed:

- `docs/debug/se-multigpu-status.md`

Verification completed:

- ROCm source inspected read-only.
- No gem5 source fix was applied in this step.

Single next diagnostic step: inspect HSA signal completion handling.
```

Replace the example API, function chain, backend, trace marker, and next step
with the observed evidence before appending the section.

- [ ] **Step 7: Run diff check**

Run:

```bash
git diff --check
```

Expected: no output.

## Execution order

Run tasks in this order:

1. Task 1: native `hipMalloc` standalone smoke.
2. Task 2: confirm existing preload `hipMemset` baseline.
3. Task 3: add preload `hipMemcpy` smoke.
4. Task 4: implement preload `hipMemcpy`.
5. Task 5: native `hipLaunchKernel` smoke using direct CPU validation.
6. Task 6: native `hipFree` diagnostic smoke.
7. Task 7: status updates after each user-owned simulation.
8. Task 8: ROCm source root-cause analysis after any blocked API run.

Do not implement `hipFree` interposition until the native free diagnostic has
identified the first blocking point. If `hipFree` hangs, record it as an SE
ROCm teardown blocker, run Task 8 to identify the source-level teardown path,
and stop for design review before adding any fake-free behavior.

## Self-review

- Spec coverage: all five target APIs have an isolated diagnostic path.
- ROCm source-analysis coverage: blocked native API paths now have a required
  source-level root-cause task before adding new compatibility behavior.
- Placeholder scan: no task uses TBD/TODO/fill-in language.
- Type consistency: all new smoke tests use HIP runtime APIs and gem5
  `m5_exit(0)`.
- Scope check: this plan intentionally avoids full ROCm runtime completeness
  and focuses on functional SE compatibility for common APIs.
