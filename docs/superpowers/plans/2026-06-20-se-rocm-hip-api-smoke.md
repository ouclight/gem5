# SE ROCm HIP API Smoke Test Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a public-HIP-only single-GPU smoke program that identifies the first unsupported ROCm API layer across allocation, memset, copies, runtime kernel launch, synchronization, free, and normal process exit.

**Architecture:** One `.hip` translation unit contains host code and a compiled gfx900 kernel. A command-line stage selects a cumulative prefix of the lifecycle, while exact begin/end markers and full host-side data validation distinguish API completion from later teardown or process-exit failures.

**Tech Stack:** ROCm 4 `hipcc`, HIP runtime API, HIP kernel launch syntax, Python `unittest`, gem5 SE Timing CPU.

---

## File Structure

- Create: `tests/test-progs/gpu/hip-api-smoke/hip_api_smoke.hip`
  - Stage parsing, HIP API calls, device kernel, data validation, and normal
    return.
- Create: `tests/test-progs/gpu/hip-api-smoke/Makefile`
  - Direct ROCm 4 gfx900 host/device compilation without m5ops.
- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`
  - Source-level contract regression for stages, cumulative ordering, HIP-only
    execution, validation, and lifecycle ordering.
- Modify: `docs/debug/se-multigpu-status.md`
  - RED/GREEN evidence, user-owned build command, stage run matrix, and the
    single next diagnostic step.

### Task 1: Add the Failing HIP Smoke Source Contract

**Files:**

- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- Test: `tests/pyunit/stdlib/test_se_viper_multigpu.py`

- [ ] **Step 1: Add a source-level regression**

Add this method to `SEViperMultiGPUTest`:

```python
    def test_hip_api_smoke_uses_public_hip_cumulative_stages(self):
        root = Path("tests/test-progs/gpu/hip-api-smoke")
        source = (root / "hip_api_smoke.hip").read_text()
        makefile = (root / "Makefile").read_text()

        for stage in ("malloc", "memset", "memcpy", "launch", "lifecycle"):
            self.assertIn(f'"{stage}"', source)

        required_calls = (
            "hipGetDeviceCount",
            "hipSetDevice(0)",
            "hipMalloc",
            "hipMemset",
            "hipMemcpyHostToDevice",
            "hipMemcpyDeviceToHost",
            "hipGetLastError",
            "hipDeviceSynchronize",
            "hipFree",
        )
        for call in required_calls:
            self.assertIn(call, source)

        self.assertIn("transform_kernel<<<", source)
        self.assertIn("validate_memset", source)
        self.assertIn("validate_round_trip", source)
        self.assertIn("validate_kernel", source)
        self.assertIn("API_STAGE_PASSED stage=%s", source)
        self.assertIn("LIFECYCLE_PASSED", source)

        malloc_pos = source.index("run_malloc_stage")
        memset_pos = source.index("run_memset_stage")
        memcpy_pos = source.index("run_memcpy_stage")
        launch_pos = source.index("run_launch_stage")
        free_pos = source.index("HIP_CHECK(hipFree")
        lifecycle_pos = source.index('std::printf("LIFECYCLE_PASSED')
        return_pos = source.index("return 0;", lifecycle_pos)
        self.assertLess(malloc_pos, memset_pos)
        self.assertLess(memset_pos, memcpy_pos)
        self.assertLess(memcpy_pos, launch_pos)
        self.assertLess(launch_pos, free_pos)
        self.assertLess(free_pos, lifecycle_pos)
        self.assertLess(lifecycle_pos, return_pos)

        self.assertNotIn("#include <hsa/", source)
        self.assertNotIn("hsa_", source)
        self.assertNotIn("m5_exit", source)
        self.assertNotIn("m5_fail", source)
        self.assertNotIn("volatile const uint32_t *", source)

        self.assertIn("--offload-arch=gfx900", makefile)
        self.assertIn("-mno-code-object-v3", makefile)
        self.assertNotIn("m5op", makefile)
        self.assertNotIn("-lhsa-runtime64", makefile)
        self.assertNotIn("-lhsakmt", makefile)
```

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_hip_api_smoke_uses_public_hip_cumulative_stages
```

Expected result: ERROR with `FileNotFoundError` for
`tests/test-progs/gpu/hip-api-smoke/hip_api_smoke.hip`.

### Task 2: Implement the Public-HIP Smoke Program

**Files:**

- Create: `tests/test-progs/gpu/hip-api-smoke/hip_api_smoke.hip`

- [ ] **Step 1: Add includes, constants, stage model, and HIP logging**

Start the file with:

```cpp
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
```

Add:

```cpp
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
```

The macro is used only in `main`, where `return 1` has the intended type.

- [ ] **Step 2: Add strict stage parsing**

Implement:

```cpp
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
```

Invalid input prints:

```text
usage: <program> --stage malloc|memset|memcpy|launch|lifecycle
```

and returns 2 without initializing HIP.

- [ ] **Step 3: Add the normal HIP kernel**

Add:

```cpp
__global__ void
transform_kernel(uint32_t *data, uint32_t size, uint32_t seed)
{
    const uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index < size) {
        data[index] ^= seed;
    }
}
```

No separately loaded HSACO or HSA API is permitted.

- [ ] **Step 4: Add reusable data validators**

Implement these exact functions:

```cpp
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
```

- [ ] **Step 5: Implement cumulative stage helpers**

Use these helper signatures:

```cpp
static bool run_malloc_stage(uint32_t **device_data);
static bool run_memset_stage(uint32_t *device_data);
static bool run_memcpy_stage(uint32_t *device_data,
                             std::vector<uint32_t> *input);
static bool run_launch_stage(uint32_t *device_data,
                             const std::vector<uint32_t> &input);
```

`run_malloc_stage` performs:

```cpp
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
```

Because helpers return `bool`, define a second macro:

```cpp
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
```

`run_memset_stage` performs, in order:

```cpp
HIP_STAGE_CHECK(hipMemset(device_data, memset_byte, bytes));
HIP_STAGE_CHECK(hipDeviceSynchronize());
std::vector<uint32_t> output(count);
HIP_STAGE_CHECK(hipMemcpy(
    output.data(), device_data, bytes, hipMemcpyDeviceToHost));
return validate_memset(output);
```

`run_memcpy_stage` initializes:

```cpp
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
```

then performs H2D, D2H, and `validate_round_trip`.

`run_launch_stage` performs:

```cpp
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
```

- [ ] **Step 6: Implement cumulative control flow and pass markers**

`main` must:

1. parse the stage;
2. call `run_malloc_stage`;
3. stop after the requested cumulative boundary;
4. never call `hipFree` for non-lifecycle stages;
5. perform `hipFree` only after launch validation in lifecycle.

The control flow is:

```cpp
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
```

Do not add cleanup on earlier error or stage-return paths. The purpose is to
isolate API completion before testing teardown.

### Task 3: Add the ROCm 4 Build Rule

**Files:**

- Create: `tests/test-progs/gpu/hip-api-smoke/Makefile`

- [ ] **Step 1: Add the direct hipcc Makefile**

Create:

```make
HIPCC ?= hipcc
TARGET ?= hip_api_smoke
SRC := hip_api_smoke.hip
HIPCCFLAGS ?= -mno-code-object-v3

ifneq ($(ROCM_PATH),)
HIPCCFLAGS += --rocm-path=$(ROCM_PATH)
endif

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(HIPCC) $(HIPCCFLAGS) -O2 --offload-arch=gfx900 -o $@ $<

clean:
	$(RM) $(TARGET)
```

The Makefile must not include gem5 headers, m5ops, HSA runtime libraries, or
separate HSACO extraction.

- [ ] **Step 2: Run the focused source test and verify GREEN**

Run:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_hip_api_smoke_uses_public_hip_cumulative_stages
```

Expected result: one test passes.

- [ ] **Step 3: Dry-run the build**

Run:

```bash
make -n -C tests/test-progs/gpu/hip-api-smoke \
  ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  HIPCC=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/bin/hipcc
```

Expected command contains separate `-mno-code-object-v3`, `-O2`, and
`--offload-arch=gfx900` arguments, with no HSA or m5 libraries.

### Task 4: Run Complete Lightweight Verification

**Files:**

- Test: `tests/pyunit/stdlib/test_se_viper_multigpu.py`

- [ ] **Step 1: Run the complete SE multi-GPU suite**

Run:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Expected result: all tests pass.

- [ ] **Step 2: Check worktree formatting**

Run:

```bash
git diff --check
```

Expected result: no output and exit status 0.

- [ ] **Step 3: Confirm no build output was created**

Run:

```bash
find tests/test-progs/gpu/hip-api-smoke \
  -maxdepth 1 -type f -printf '%f\n' | sort
```

Expected output:

```text
Makefile
hip_api_smoke.hip
```

### Task 5: Record User-Owned Build and Runtime Matrix

**Files:**

- Modify: `docs/debug/se-multigpu-status.md`

- [ ] **Step 1: Record implementation verification**

Append the exact RED result, focused GREEN result, full-suite count,
`git diff --check`, and Makefile dry-run result. State explicitly that Codex
did not build the HIP binary or run a simulation.

- [ ] **Step 2: Record the user-owned build command**

Record, but do not run:

```bash
make -C tests/test-progs/gpu/hip-api-smoke \
  ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  HIPCC=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/bin/hipcc
```

- [ ] **Step 3: Record the ordered stage commands**

For each stage in `malloc memset memcpy launch lifecycle`, use a separate
output directory:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hip-<stage> \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type timing \
  --app tests/test-progs/gpu/hip-api-smoke/hip_api_smoke \
  --opts="--stage <stage>" \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1
```

Do not pass `--disable-gpu0-kernel-launch-acquire`; this milestone tests normal
HIP runtime behavior.

Stop at the first stage without its pass marker. The single next diagnostic
step is for the user to build the HIP binary, then run only `--stage malloc`.

No commit is included because repository changes are committed or pushed only
when the user explicitly requests that action.
