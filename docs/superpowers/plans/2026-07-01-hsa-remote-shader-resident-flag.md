# HSA Remote Shader Resident-Flag Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans for inline implementation. Do not use subagents for this repository unless the user explicitly asks for them. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an independent HSA smoke test where a resident reader GPU observes a writer GPU's shader-store update after the writer publishes a shader-store flag.

**Architecture:** Add a new `hsa_remote_shader_resident_flag` binary and a focused `resident_flag_kernels.hip` bundle. The reader stays in one kernel dispatch, polls a GPU-written flag in reader VRAM, then rereads the value and classifies updated, stale, timeout, or unexpected behavior.

**Tech Stack:** gem5 SE multi-GPU, ROCm 4.0.1 HSA runtime, HIP offline HSACO generation, existing xgmi-peer-vram Makefile patterns, pyunit structural tests, user-owned full gem5 Timing runs.

---

## File Structure

- `tests/test-progs/gpu/xgmi-peer-vram/resident_flag_kernels.hip`: new GPU kernels only. It contains one resident reader kernel and one writer kernel.
- `tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_shader_resident_flag.cpp`: new host HSA diagnostic program. It owns argument parsing, HSA setup, memory allocation, queue creation, kernel dispatch, result printing, and early `m5_exit` on diagnostic completion.
- `tests/test-progs/gpu/xgmi-peer-vram/Makefile`: add `hsa_remote_shader_resident_flag`, `resident_flag_kernels.bundle`, and `resident_flag_kernels.hsaco` targets.
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`: add structural tests for the new binary, kernel ordering, layouts, sync modes, and result markers.
- `docs/debug/se-multigpu-status.md`: record implementation commands, verification results, changed files, and next run command.

## Task 1: Add structural pyunit coverage

**Files:**
- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`

- [ ] **Step 1: Add a failing test for the new standalone diagnostic**

Add this method near the existing remote-cache structural tests:

```python
    def test_remote_shader_resident_flag_smoke_is_standalone(self):
        root = Path("tests/test-progs/gpu/xgmi-peer-vram")
        host = (root / "hsa_remote_shader_resident_flag.cpp").read_text()
        kernels = (root / "resident_flag_kernels.hip").read_text()
        makefile = (root / "Makefile").read_text()

        self.assertIn("hsa_remote_shader_resident_flag", makefile)
        self.assertIn("hsa_remote_shader_resident_flag.cpp", makefile)
        self.assertIn("resident_flag_kernels.hip", makefile)
        self.assertIn("resident_flag_kernels.hsaco", makefile)
        self.assertIn("RESIDENT_FLAG_HSACO_PATH", host)

        self.assertIn("ResidentFlagControl", host)
        self.assertIn("LayoutSameLine", host)
        self.assertIn("LayoutSplitLine", host)
        self.assertIn("SyncStrict", host)
        self.assertIn("SyncWriterFenceOnly", host)
        self.assertIn("SyncNoFence", host)
        self.assertIn('"--reverse"', host)
        self.assertIn('"--layout"', host)
        self.assertIn('"--sync"', host)
        self.assertIn("RESIDENT_FLAG_RESULT", host)
        self.assertIn("RESIDENT_FLAG_PASSED_UPDATED", host)
        self.assertIn("RESIDENT_FLAG_OBSERVED_STALE_VALUE", host)
        self.assertIn("RESIDENT_FLAG_FLAG_TIMEOUT", host)
        self.assertIn("RESIDENT_FLAG_FAILED_UNEXPECTED", host)
        self.assertNotIn("hipMemcpy", host)
        self.assertNotIn("hipMemset", host)

        self.assertIn("resident_flag_reader", kernels)
        self.assertIn("resident_flag_writer", kernels)
        self.assertIn("__threadfence_system", kernels)
        self.assertIn("value_dword", kernels)
        self.assertIn("flag_dword", kernels)

        value_store = kernels.index("target[value_dword] = updated_value")
        first_fence = kernels.index("__threadfence_system", value_store)
        flag_store = kernels.index("target[flag_dword] = 1", first_fence)
        self.assertLess(value_store, first_fence)
        self.assertLess(first_fence, flag_store)

        first_read = kernels.index("const uint32_t first = target[value_dword]")
        flag_poll = kernels.index("target[flag_dword]", first_read)
        second_read = kernels.index("const uint32_t second = target[value_dword]", flag_poll)
        self.assertLess(first_read, flag_poll)
        self.assertLess(flag_poll, second_read)
```

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_remote_shader_resident_flag_smoke_is_standalone
```

Expected result:

```text
FileNotFoundError: ... hsa_remote_shader_resident_flag.cpp
```

This is the expected failure before adding the new diagnostic.

## Task 2: Add the resident flag kernels

**Files:**
- Create: `tests/test-progs/gpu/xgmi-peer-vram/resident_flag_kernels.hip`

- [ ] **Step 1: Create the kernel source**

Create `resident_flag_kernels.hip`:

```cpp
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
        flag = target[flag_dword];
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
```

- [ ] **Step 2: Run the focused test and confirm the expected host-file failure remains**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_remote_shader_resident_flag_smoke_is_standalone
```

Expected result:

```text
FileNotFoundError: ... hsa_remote_shader_resident_flag.cpp
```

## Task 3: Add the host HSA diagnostic

**Files:**
- Create: `tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_shader_resident_flag.cpp`

- [ ] **Step 1: Reuse local helper patterns from the existing HSA smoke**

Copy the following helper categories from
`tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_cache_read.cpp` into the new
file, renaming only diagnostic labels where needed:

```text
print_status
append_arg
find CPU/GPU agents
find allocable memory pools
allocate/map/free memory helpers
load code object from RESIDENT_FLAG_HSACO_PATH
create executable and resolve kernel symbols
create queues
launch_reader_async / PendingDispatch / cleanup_dispatch
wait_for_signal_lt_one
wait_for_control_field
```

Keep the new program standalone. Do not add dependencies on
`hsa_remote_cache_read.cpp`.

- [ ] **Step 2: Add resident-flag-specific constants and enums**

Add these declarations in the new host file:

```cpp
static constexpr uint32_t OldValue = 0x11112222u;
static constexpr uint32_t UpdatedValue = 0x33334444u;
static constexpr uint32_t DefaultSpinLimit = 100000000u;
static constexpr uint32_t SameLineValueDword = 0;
static constexpr uint32_t SameLineFlagDword = 1;
static constexpr uint32_t SplitLineValueDword = 0;
static constexpr uint32_t SplitLineFlagDword = 32;
static constexpr size_t TargetDwords = 64;
static constexpr size_t TargetBytes = TargetDwords * sizeof(uint32_t);

enum ResidentFlagLayout
{
    LayoutSameLine,
    LayoutSplitLine,
};

enum ResidentFlagSync
{
    SyncStrict = 0,
    SyncWriterFenceOnly = 1,
    SyncNoFence = 2,
};

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
```

- [ ] **Step 3: Add CLI parsing**

Implement these options:

```text
--reverse
--layout same-line
--layout split-line
--sync strict
--sync writer-fence-only
--sync no-fence
--spin-limit N
```

Reject invalid values with one-line usage text:

```text
usage: hsa_remote_shader_resident_flag [--reverse] [--layout same-line|split-line] [--sync strict|writer-fence-only|no-fence] [--spin-limit N]
```

- [ ] **Step 4: Implement the core run flow**

The core flow must be:

```cpp
const bool reverse = options.reverse;
hsa_agent_t reader_agent = reverse ? gpu0_agent : gpu1_agent;
hsa_agent_t writer_agent = reverse ? gpu1_agent : gpu0_agent;
const uint32_t value_dword =
    options.layout == LayoutSameLine ? SameLineValueDword : SplitLineValueDword;
const uint32_t flag_dword =
    options.layout == LayoutSameLine ? SameLineFlagDword : SplitLineFlagDword;

std::fill(target, target + TargetDwords, 0);
target[value_dword] = OldValue;
target[flag_dword] = 0;
std::memset(control, 0, sizeof(*control));
__sync_synchronize();

launch resident_flag_reader on reader_queue;
wait_for_control_field("first_read_done", &control->first_read_done, 1);
launch resident_flag_writer on writer_queue;
wait_for_signal_lt_one("resident flag writer", writer_pending.completion);
wait_for_control_field("reader_done", &control->reader_done, 1);
wait_for_signal_lt_one("resident flag reader", reader_pending.completion);
```

The writer kernel arguments must be:

```text
target, value_dword, flag_dword, UpdatedValue, sync_mode
```

The reader kernel arguments must be:

```text
target, control, value_dword, flag_dword, OldValue, UpdatedValue, spin_limit, sync_mode
```

- [ ] **Step 5: Print result markers**

Print exactly one result line:

```cpp
std::printf("RESIDENT_FLAG_RESULT direction=%s layout=%s sync=%s "
            "first=0x%08x second=0x%08x flag=%u spins=%u "
            "classification=%u\n",
            direction_name, layout_name, sync_name,
            control->first_value, control->second_value,
            control->seen_flag, control->spins, control->classification);
```

Then print exactly one final marker:

```cpp
switch (control->classification) {
  case 1:
    std::printf("RESIDENT_FLAG_PASSED_UPDATED\n");
    break;
  case 2:
    std::printf("RESIDENT_FLAG_OBSERVED_STALE_VALUE\n");
    break;
  case 3:
    std::printf("RESIDENT_FLAG_FLAG_TIMEOUT\n");
    break;
  default:
    std::printf("RESIDENT_FLAG_FAILED_UNEXPECTED\n");
    break;
}
```

Exit status rules:

```text
UPDATED: 0
STALE_VALUE: 0
FLAG_TIMEOUT: 1
UNEXPECTED: 1
setup/launch/wait error: 1
```

- [ ] **Step 6: Add early m5 exit on diagnostic completion**

Follow the existing xgmi-peer-vram pattern: include `gem5/m5ops.h`, link with
the local m5op archive, flush stdout, and call `m5_exit(0)` after printing
`UPDATED` or `STALE_VALUE`. For timeout/unexpected/setup failures, return a
nonzero process code instead of calling success exit.

## Task 4: Wire the Makefile

**Files:**
- Modify: `tests/test-progs/gpu/xgmi-peer-vram/Makefile`

- [ ] **Step 1: Add targets**

Add a new target following the existing HSACO/binary pattern:

```make
HSA_REMOTE_SHADER_RESIDENT_FLAG_TARGET := hsa_remote_shader_resident_flag
RESIDENT_FLAG_HSACO := resident_flag_kernels.hsaco
RESIDENT_FLAG_BUNDLE := resident_flag_kernels.bundle

$(RESIDENT_FLAG_BUNDLE): resident_flag_kernels.hip
	$(HIPCC) $(HIPCC_FLAGS) --genco --offload-arch=gfx900 -o $@ $<

$(RESIDENT_FLAG_HSACO): $(RESIDENT_FLAG_BUNDLE)
	$(ROCM_PATH)/llvm/bin/clang-offload-bundler -unbundle -type=o \
	  -targets=host-x86_64-unknown-linux,hip-amdgcn-amd-amdhsa-gfx900 \
	  -inputs=$< -outputs=/dev/null,$@

$(HSA_REMOTE_SHADER_RESIDENT_FLAG_TARGET): hsa_remote_shader_resident_flag.cpp $(RESIDENT_FLAG_HSACO) $(M5OP_LIB)
	$(CXX) $(CXXFLAGS) -I$(GEM5_ROOT)/include \
	  -DRESIDENT_FLAG_HSACO_PATH=\"$(abspath $(RESIDENT_FLAG_HSACO))\" \
	  -o $@ hsa_remote_shader_resident_flag.cpp \
	  -L. -lm5op_x86 -L$(ROCM_PATH)/lib -lhsa-runtime64 -lhsakmt
```

Adjust variable names to match the existing Makefile style if the file already
defines equivalent `HIPCC_FLAGS`, `CXXFLAGS`, `GEM5_ROOT`, or `M5OP_LIB`
variables.

- [ ] **Step 2: Include the target in clean/all if existing patterns require it**

If the Makefile has explicit `all` or `clean` lists, add:

```make
$(HSA_REMOTE_SHADER_RESIDENT_FLAG_TARGET)
$(RESIDENT_FLAG_HSACO)
$(RESIDENT_FLAG_BUNDLE)
```

to the appropriate lists.

## Task 5: Verify source structure and build

**Files:**
- Test: `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- Build target: `tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_shader_resident_flag`

- [ ] **Step 1: Run the focused structural pyunit**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_remote_shader_resident_flag_smoke_is_standalone
```

Expected result:

```text
Ran 1 test
OK
```

- [ ] **Step 2: Run related structural tests**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest test_se_viper_multigpu
```

Expected result:

```text
OK
```

- [ ] **Step 3: Build the new binary**

Run:

```bash
make -C tests/test-progs/gpu/xgmi-peer-vram \
  hsa_remote_shader_resident_flag \
  ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  HIPCC=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/bin/hipcc \
  CXX=g++
```

Expected result:

```text
hsa_remote_shader_resident_flag
resident_flag_kernels.hsaco
```

are produced without compile or link errors.

- [ ] **Step 4: Check marker strings**

Run:

```bash
strings tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_shader_resident_flag | \
  rg "RESIDENT_FLAG_RESULT|RESIDENT_FLAG_PASSED_UPDATED|RESIDENT_FLAG_OBSERVED_STALE_VALUE|RESIDENT_FLAG_FLAG_TIMEOUT|RESIDENT_FLAG_FAILED_UNEXPECTED"
```

Expected result: all five markers are present.

- [ ] **Step 5: Run whitespace check**

Run:

```bash
git diff --check
```

Expected result: no output.

## Task 6: User-owned full simulation commands

**Files:**
- Generated output directories only.

- [ ] **Step 1: Forward same-line strict**

User run:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hsa-remote-shader-resident-flag-forward-same-strict \
  -r -e --stdout-file=simout.txt --stderr-file=simerr.txt \
  --listener-mode=off \
  --debug-flags=GPUDriver,HSAPacketProcessor,GPUDisp,GPUAgentDisp \
  --debug-file=resident-flag.trace \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type timing \
  --app tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_shader_resident_flag \
  '--opts=--layout same-line --sync strict' \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu \
  --env HSA_ENABLE_SDMA=1
```

Expected diagnostic completion:

```text
RESIDENT_FLAG_RESULT direction=forward layout=same-line sync=strict ...
RESIDENT_FLAG_PASSED_UPDATED
```

`RESIDENT_FLAG_OBSERVED_STALE_VALUE` is also a valid diagnostic completion,
but it would require coherence interpretation.

- [ ] **Step 2: Reverse same-line strict**

User run:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hsa-remote-shader-resident-flag-reverse-same-strict \
  -r -e --stdout-file=simout.txt --stderr-file=simerr.txt \
  --listener-mode=off \
  --debug-flags=GPUDriver,HSAPacketProcessor,GPUDisp,GPUAgentDisp \
  --debug-file=resident-flag.trace \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type timing \
  --app tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_shader_resident_flag \
  '--opts=--reverse --layout same-line --sync strict' \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu \
  --env HSA_ENABLE_SDMA=1
```

Expected diagnostic completion: same marker rules as the forward run.

- [ ] **Step 3: Forward split-line strict**

User run:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hsa-remote-shader-resident-flag-forward-split-strict \
  -r -e --stdout-file=simout.txt --stderr-file=simerr.txt \
  --listener-mode=off \
  --debug-flags=GPUDriver,HSAPacketProcessor,GPUDisp,GPUAgentDisp \
  --debug-file=resident-flag.trace \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type timing \
  --app tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_shader_resident_flag \
  '--opts=--layout split-line --sync strict' \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu \
  --env HSA_ENABLE_SDMA=1
```

Expected diagnostic completion: same marker rules as the forward run.

- [ ] **Step 4: Reverse split-line strict**

User run:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hsa-remote-shader-resident-flag-reverse-split-strict \
  -r -e --stdout-file=simout.txt --stderr-file=simerr.txt \
  --listener-mode=off \
  --debug-flags=GPUDriver,HSAPacketProcessor,GPUDisp,GPUAgentDisp \
  --debug-file=resident-flag.trace \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type timing \
  --app tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_shader_resident_flag \
  '--opts=--reverse --layout split-line --sync strict' \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu \
  --env HSA_ENABLE_SDMA=1
```

Expected diagnostic completion: same marker rules as the forward run.

## Task 7: Interpret and record results

**Files:**
- Modify: `docs/debug/se-multigpu-status.md`

- [ ] **Step 1: Inspect each completed run**

For each output directory, run:

```bash
tail -120 <m5out-dir>/simout.txt
tail -120 <m5out-dir>/simerr.txt
rg -n "RESIDENT_FLAG_|panic|fatal|assert|Invalid transition|Page fault" \
  <m5out-dir>/simout.txt <m5out-dir>/simerr.txt <m5out-dir>/resident-flag.trace
```

Expected result for a valid diagnostic completion:

```text
RESIDENT_FLAG_RESULT ...
RESIDENT_FLAG_PASSED_UPDATED
```

or:

```text
RESIDENT_FLAG_RESULT ...
RESIDENT_FLAG_OBSERVED_STALE_VALUE
```

No `panic`, `fatal`, `assert`, `Invalid transition`, or `Page fault` should be
present.

- [ ] **Step 2: Record the interpretation**

Append a section to `docs/debug/se-multigpu-status.md` containing:

```text
Command run:
<exact command>

First relevant result:
<first RESIDENT_FLAG_* line or first error>

Facts confirmed:
- <direction/layout/sync>
- <updated/stale/timeout/unexpected>
- <whether writer and reader dispatches completed>

Hypotheses accepted or rejected:
- <visibility interpretation>

Files changed:
- <source/doc paths>

Verification completed:
- <pyunit/build/full-run inspection>

Single next diagnostic step:
- <next run or trace/protocol inspection>
```

## Self-Review

- Spec coverage: the plan covers the standalone binary, separate kernel file,
  same-line and split-line layout, strict and weaker sync modes, output
  markers, structural tests, build verification, and user-owned full run
  commands.
- Placeholder scan: no task depends on an unspecified file, command, result
  marker, or mode name.
- Type consistency: host and kernel structures use the same field names;
  sync values are consistent between host and kernel; result classifications
  match the design.
