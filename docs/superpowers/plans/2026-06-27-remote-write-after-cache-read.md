# Remote Write After Cache Read Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans for inline implementation. Do not use subagents for this repository unless the user explicitly asks for them. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a diagnostic smoke that classifies whether GPU1 observes updated or stale data when GPU1 reads a GPU1 VRAM location, GPU0 overwrites it through SDMA_XGMI, and GPU1 reads it again inside the same kernel dispatch.

**Architecture:** Add a small HIP kernel bundle with a long-running GPU1 reader kernel and a host HSA program that launches the reader, performs GPU0-to-GPU1 `hsa_amd_memory_async_copy`, releases the reader, and prints a classification. Keep the first version diagnostic-only: updated and stale are both successful observations, while setup failures, timeouts, and unexpected values fail.

**Tech Stack:** gem5 SE multi-GPU, ROCm 4.0.1 HSA runtime, HIP offline code object generation, existing xgmi-peer-vram Makefile patterns, `SESDMAEngine`, `HSAPacketProcessor`, pyunit source-structure tests.

---

## File Structure

- `tests/test-progs/gpu/xgmi-peer-vram/remote_cache_kernels.hip`: GPU kernels for the diagnostic. It contains one long-running reader kernel and no host code.
- `tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_cache_read.cpp`: host HSA diagnostic program. It loads the HSACO, creates queues, allocates memory, launches the GPU1 reader, performs SDMA_XGMI overwrite, and prints classification.
- `tests/test-progs/gpu/xgmi-peer-vram/Makefile`: build target for `hsa_remote_cache_read` and `remote_cache_kernels.hsaco`.
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`: structural tests proving the smoke is wired, uses same-dispatch read-after-remote-write semantics, and does not depend on HIP memcpy/memset.
- `docs/debug/se-multigpu-status.md`: handoff record after implementation and after full simulation.

## Task 1: Add structural pyunit coverage for the diagnostic shape

**Files:**
- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`

- [ ] **Step 1: Add the failing source-structure test**

Add this test near the existing xgmi-peer-vram and SDMA smoke tests:

```python
    def test_remote_write_after_cache_read_smoke_has_same_dispatch_reader(self):
        root = Path("tests/test-progs/gpu/xgmi-peer-vram")
        host = (root / "hsa_remote_cache_read.cpp").read_text()
        kernels = (root / "remote_cache_kernels.hip").read_text()
        makefile = (root / "Makefile").read_text()

        self.assertIn("HSA_REMOTE_CACHE_READ_TARGET", makefile)
        self.assertIn("hsa_remote_cache_read.cpp", makefile)
        self.assertIn("remote_cache_kernels.hip", makefile)
        self.assertIn("remote_cache_kernels.hsaco", makefile)

        self.assertIn("REMOTE_CACHE_READ_HSACO_PATH", host)
        self.assertIn("RemoteCacheControl", host)
        self.assertIn("REMOTE_CACHE_READ_RESULT", host)
        self.assertIn("REMOTE_CACHE_READ_PASSED_UPDATED", host)
        self.assertIn("REMOTE_CACHE_READ_OBSERVED_STALE", host)
        self.assertIn("REMOTE_CACHE_READ_FAILED_UNEXPECTED", host)
        self.assertIn('"GPU0_TO_GPU1_OVERWRITE"', host)
        self.assertIn("hsa_amd_memory_async_copy", host)
        self.assertNotIn("hipMemcpy", host)
        self.assertNotIn("hipMemset", host)

        self.assertIn("remote_cache_reader", kernels)
        self.assertIn("first_read_done", kernels)
        self.assertIn("allow_second_read", kernels)
        self.assertIn("reader_done", kernels)
        self.assertIn("classification", kernels)

        first_read = kernels.index("control->first_value = first")
        wait_loop = kernels.index("control->allow_second_read", first_read)
        second_read = kernels.index("const uint32_t second = target[0]", wait_loop)
        classify = kernels.index("control->classification", second_read)
        self.assertLess(first_read, wait_loop)
        self.assertLess(wait_loop, second_read)
        self.assertLess(second_read, classify)

        self.assertIn("const uint32_t *target", kernels)
        self.assertNotIn("volatile uint32_t *target", kernels)
        self.assertIn("volatile RemoteCacheControl *control", kernels)
```

- [ ] **Step 2: Run the new test and verify RED**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_remote_write_after_cache_read_smoke_has_same_dispatch_reader
```

Expected result:

```text
FileNotFoundError: ... hsa_remote_cache_read.cpp
```

The failure is correct because the smoke files do not exist yet.

## Task 2: Add the GPU reader kernel bundle

**Files:**
- Create: `tests/test-progs/gpu/xgmi-peer-vram/remote_cache_kernels.hip`

- [ ] **Step 1: Create the kernel source**

Create `remote_cache_kernels.hip` with this content:

```cpp
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
};

enum RemoteCacheClassification : uint32_t
{
    RemoteCacheUnset = 0,
    RemoteCacheUpdated = 1,
    RemoteCacheStale = 2,
    RemoteCacheUnexpected = 3,
};

extern "C" __global__ void
remote_cache_reader(const uint32_t *target,
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
    if (second == value_b) {
        control->classification = RemoteCacheUpdated;
    } else if (second == value_a) {
        control->classification = RemoteCacheStale;
    } else {
        control->classification = RemoteCacheUnexpected;
    }
    __threadfence_system();
    control->reader_done = 1;
}
```

- [ ] **Step 2: Run the pyunit test and verify the failure moves**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_remote_write_after_cache_read_smoke_has_same_dispatch_reader
```

Expected result:

```text
FileNotFoundError: ... hsa_remote_cache_read.cpp
```

The kernel file is now present; the host file remains missing.

## Task 3: Add the host HSA diagnostic program

**Files:**
- Create: `tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_cache_read.cpp`

- [ ] **Step 1: Start from the existing HSA helper pattern**

Use `tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_peer_async_copy.cpp` for:

- HSA status printing;
- agent enumeration;
- allocable GPU pool selection;
- `hsa_amd_agents_allow_access`;
- `hsa_amd_memory_async_copy` plus signal wait;
- host-side byte validation style.

Use `tests/test-progs/gpu/xgmi-peer-vram/peer_vram_hip.cpp` for:

- reading an HSACO into memory;
- creating a code object reader;
- creating an executable;
- loading kernel symbols for each agent;
- dispatching an HSA kernel packet manually.

- [ ] **Step 2: Define constants and result strings**

Include these definitions in `hsa_remote_cache_read.cpp`:

```cpp
#ifndef REMOTE_CACHE_READ_HSACO_PATH
#define REMOTE_CACHE_READ_HSACO_PATH "remote_cache_kernels.hsaco"
#endif

static constexpr uint32_t ValueA = 0x11111111u;
static constexpr uint32_t ValueB = 0x22222222u;
static constexpr uint32_t SpinLimit = 100000000u;
static constexpr uint64_t HostPollLimit = 100000000ull;

enum RemoteCacheClassification : uint32_t
{
    RemoteCacheUnset = 0,
    RemoteCacheUpdated = 1,
    RemoteCacheStale = 2,
    RemoteCacheUnexpected = 3,
};

struct RemoteCacheControl
{
    uint32_t first_read_done;
    uint32_t allow_second_read;
    uint32_t reader_done;
    uint32_t first_value;
    uint32_t second_value;
    uint32_t classification;
};
```

- [ ] **Step 3: Implement bounded host polling**

Add this helper:

```cpp
static bool
wait_for_flag(const char *label, volatile uint32_t *flag)
{
    for (uint64_t i = 0; i < HostPollLimit; ++i) {
        if (*flag != 0) {
            return true;
        }
    }
    std::printf("REMOTE_CACHE_READ_FAILED reason=%s_timeout\n", label);
    std::fflush(stdout);
    return false;
}
```

- [ ] **Step 4: Implement the main phase ordering**

The program must print and execute these phases in order:

```cpp
std::printf("[hsa_remote_cache_read] phase 0 setup\n");
std::printf("[hsa_remote_cache_read] phase 1 launch GPU1 reader\n");
std::printf("[hsa_remote_cache_read] phase 2 wait first read\n");
std::printf("[hsa_remote_cache_read] phase 3 GPU0_TO_GPU1_OVERWRITE\n");
std::printf("[hsa_remote_cache_read] phase 4 release second read\n");
std::printf("[hsa_remote_cache_read] phase 5 read classification\n");
```

The actual ordering is:

1. allocate GPU1 `target`;
2. allocate GPU0 `source`;
3. allocate `RemoteCacheControl` in host/system-visible memory;
4. initialize `target` to `ValueA`;
5. initialize `source` to `ValueB`;
6. launch `remote_cache_reader` on GPU1;
7. wait for `control.first_read_done`;
8. run `hsa_amd_memory_async_copy(target, gpu1, source, gpu0, sizeof(uint32_t), ...)` with label `GPU0_TO_GPU1_OVERWRITE`;
9. set `control.allow_second_read = 1`;
10. wait for `control.reader_done`;
11. print classification.

- [ ] **Step 5: Print diagnostic classification**

Use this output logic:

```cpp
const char *classification_name = "unexpected";
if (control->classification == RemoteCacheUpdated) {
    classification_name = "updated";
} else if (control->classification == RemoteCacheStale) {
    classification_name = "stale";
}

std::printf("REMOTE_CACHE_READ_RESULT first=0x%08x second=0x%08x "
            "classification=%s\n",
            control->first_value, control->second_value,
            classification_name);

if (control->classification == RemoteCacheUpdated) {
    std::printf("REMOTE_CACHE_READ_PASSED_UPDATED\n");
    return 0;
}
if (control->classification == RemoteCacheStale) {
    std::printf("REMOTE_CACHE_READ_OBSERVED_STALE\n");
    return 0;
}

std::printf("REMOTE_CACHE_READ_FAILED_UNEXPECTED\n");
return 1;
```

- [ ] **Step 6: Run pyunit and verify RED shifts to Makefile**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_remote_write_after_cache_read_smoke_has_same_dispatch_reader
```

Expected result:

```text
AssertionError: 'HSA_REMOTE_CACHE_READ_TARGET' not found
```

The source files now exist; the Makefile target is still missing.

## Task 4: Wire the smoke into the xgmi-peer-vram Makefile

**Files:**
- Modify: `tests/test-progs/gpu/xgmi-peer-vram/Makefile`

- [ ] **Step 1: Add target variables**

Add near the other HSA SDMA target variables:

```make
HSA_REMOTE_CACHE_READ_TARGET ?= hsa_remote_cache_read
HSA_REMOTE_CACHE_READ_SRC := hsa_remote_cache_read.cpp
REMOTE_CACHE_HSACO_TARGET ?= remote_cache_kernels.hsaco
REMOTE_CACHE_HSACO_SRC := remote_cache_kernels.hip
REMOTE_CACHE_BUNDLE_TARGET ?= remote_cache_kernels.bundle
```

- [ ] **Step 2: Add target to `all`**

Change `all` so it includes:

```make
$(HSA_REMOTE_CACHE_READ_TARGET)
```

- [ ] **Step 3: Add HSACO build rules**

Add:

```make
$(REMOTE_CACHE_BUNDLE_TARGET): $(REMOTE_CACHE_HSACO_SRC)
	$(HIPCC) $(HSACO_HIPCCFLAGS) --genco --offload-arch=gfx900 -o $@ $(REMOTE_CACHE_HSACO_SRC)

$(REMOTE_CACHE_HSACO_TARGET): $(REMOTE_CACHE_BUNDLE_TARGET)
	$(CLANG_OFFLOAD_BUNDLER) -unbundle -type=o -targets=host-x86_64-unknown-linux,hip-amdgcn-amd-amdhsa-gfx900 -inputs=$(REMOTE_CACHE_BUNDLE_TARGET) -outputs=/dev/null,$@
```

- [ ] **Step 4: Add host build rule**

Add:

```make
$(HSA_REMOTE_CACHE_READ_TARGET): $(HSA_REMOTE_CACHE_READ_SRC) $(REMOTE_CACHE_HSACO_TARGET)
	$(CXX) $(HSA_CXXFLAGS) -DREMOTE_CACHE_READ_HSACO_PATH=\"$(abspath $(REMOTE_CACHE_HSACO_TARGET))\" -o $@ $(HSA_REMOTE_CACHE_READ_SRC) -lhsa-runtime64 -lhsakmt
```

- [ ] **Step 5: Add clean entries**

Extend `clean` to remove:

```make
$(HSA_REMOTE_CACHE_READ_TARGET) $(REMOTE_CACHE_HSACO_TARGET) $(REMOTE_CACHE_BUNDLE_TARGET)
```

- [ ] **Step 6: Run pyunit and verify GREEN**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_remote_write_after_cache_read_smoke_has_same_dispatch_reader
```

Expected result:

```text
Ran 1 test
OK
```

## Task 5: Build the smoke program

**Files:**
- Build artifact: `tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_cache_read`
- Build artifact: `tests/test-progs/gpu/xgmi-peer-vram/remote_cache_kernels.hsaco`

- [ ] **Step 1: Build only the new smoke**

Run:

```bash
make -C tests/test-progs/gpu/xgmi-peer-vram \
  hsa_remote_cache_read \
  ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  CXX=g++
```

Expected result:

```text
remote_cache_kernels.bundle
remote_cache_kernels.hsaco
hsa_remote_cache_read
```

No `hipMemset` or `hipMemcpy` should be needed.

- [ ] **Step 2: If link fails on ROCm library paths**

Run the same build with explicit library path environment:

```bash
LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu \
make -C tests/test-progs/gpu/xgmi-peer-vram \
  hsa_remote_cache_read \
  ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  CXX=g++
```

Expected result: same artifacts as Step 1.

## Task 6: Run the diagnostic under SE

**Files:**
- Generated logs: `m5out-hsa-remote-cache-read/`

- [ ] **Step 1: Run the full diagnostic**

Run:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hsa-remote-cache-read \
  -r -e \
  --stdout-file=simout.txt \
  --stderr-file=simerr.txt \
  --debug-flags=GPUDriver,HSAPacketProcessor,SESDMAEngine \
  --debug-file=remote-cache.trace \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type atomic \
  --app tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_cache_read \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu \
  --env HSA_ENABLE_SDMA=1
```

Expected valid terminal outputs include exactly one of:

```text
REMOTE_CACHE_READ_PASSED_UPDATED
```

```text
REMOTE_CACHE_READ_OBSERVED_STALE
```

Unexpected terminal outputs are:

```text
REMOTE_CACHE_READ_FAILED_UNEXPECTED
REMOTE_CACHE_READ_FAILED reason=first_read_timeout
REMOTE_CACHE_READ_FAILED reason=reader_done_timeout
```

- [ ] **Step 2: Inspect the result**

Run:

```bash
tail -160 m5out-hsa-remote-cache-read/simout.txt
rg -n "REMOTE_CACHE_READ|queue_type 3|backend sdma_xgmi|to SESDMAEngine|SDMA queue|POLL_REGMEM|SDMA atomic|fatal|panic|unsupported|mismatch" \
  m5out-hsa-remote-cache-read/simout.txt \
  m5out-hsa-remote-cache-read/simerr.txt \
  m5out-hsa-remote-cache-read/remote-cache.trace
```

Expected trace evidence:

```text
queue_type 3
backend sdma_xgmi
doorbell route ... to SESDMAEngine
SDMA queue ... drained
```

- [ ] **Step 3: Interpret the observation**

If output is:

```text
REMOTE_CACHE_READ_OBSERVED_STALE
```

Record it as evidence that the current SE multi-GPU path lacks a visible
remote-write invalidation for this access pattern.

If output is:

```text
REMOTE_CACHE_READ_PASSED_UPDATED
```

Do not record it as proof of multi-GPU coherence. Record it as an observation
requiring follow-up cache-residency checks.

If output is:

```text
REMOTE_CACHE_READ_FAILED_UNEXPECTED
```

Inspect `first`, `second`, allocation addresses, and packet trace before
changing coherence code.

## Task 7: Update handoff and run final checks

**Files:**
- Modify: `docs/debug/se-multigpu-status.md`

- [ ] **Step 1: Append run record**

Append a section using the concrete result from the run. Use the exact command
lines from Task 5, Task 6, and the inspection command, then fill the result
fields with the values printed by `REMOTE_CACHE_READ_RESULT`.

Use this shape for an `updated` observation:

````markdown
## 2026-06-27 remote-write-after-cache-read diagnostic

Commands run:

```bash
make -C tests/test-progs/gpu/xgmi-peer-vram hsa_remote_cache_read ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 CXX=g++
build/VEGA_X86/gem5.opt -d m5out-hsa-remote-cache-read -r -e --stdout-file=simout.txt --stderr-file=simerr.txt --debug-flags=GPUDriver,HSAPacketProcessor,SESDMAEngine --debug-file=remote-cache.trace --listener-mode=off configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py --cpu-type atomic --app tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_cache_read --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 --env LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu --env HSA_ENABLE_SDMA=1
tail -160 m5out-hsa-remote-cache-read/simout.txt
rg -n "REMOTE_CACHE_READ|queue_type 3|backend sdma_xgmi|to SESDMAEngine|SDMA queue|POLL_REGMEM|SDMA atomic|fatal|panic|unsupported|mismatch" m5out-hsa-remote-cache-read/simout.txt m5out-hsa-remote-cache-read/simerr.txt m5out-hsa-remote-cache-read/remote-cache.trace
```

First relevant observable result:

```text
REMOTE_CACHE_READ_RESULT first=0x11111111 second=0x22222222 classification=updated
REMOTE_CACHE_READ_PASSED_UPDATED
```

Facts confirmed:

- GPU1 first read value: 0x11111111.
- SDMA_XGMI overwrite queue: queue_type 3, backend sdma_xgmi, doorbell routed to SESDMAEngine.
- GPU1 second read value: 0x22222222.
- Classification: updated.

Hypotheses accepted:

- The diagnostic ran to completion and GPU1 observed the remote write in this run.

Hypotheses rejected:

- The run did not expose a stale cached value for this access pattern.

Files changed:

- tests/test-progs/gpu/xgmi-peer-vram/remote_cache_kernels.hip
- tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_cache_read.cpp
- tests/test-progs/gpu/xgmi-peer-vram/Makefile
- tests/pyunit/stdlib/test_se_viper_multigpu.py
- docs/debug/se-multigpu-status.md

Single next diagnostic step:

- Strengthen cache-residency evidence before treating the updated result as coherence proof.
```
````

Use this shape for a `stale` observation:

````markdown
## 2026-06-27 remote-write-after-cache-read diagnostic

Commands run:

```bash
make -C tests/test-progs/gpu/xgmi-peer-vram hsa_remote_cache_read ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 CXX=g++
build/VEGA_X86/gem5.opt -d m5out-hsa-remote-cache-read -r -e --stdout-file=simout.txt --stderr-file=simerr.txt --debug-flags=GPUDriver,HSAPacketProcessor,SESDMAEngine --debug-file=remote-cache.trace --listener-mode=off configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py --cpu-type atomic --app tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_cache_read --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 --env LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu --env HSA_ENABLE_SDMA=1
tail -160 m5out-hsa-remote-cache-read/simout.txt
rg -n "REMOTE_CACHE_READ|queue_type 3|backend sdma_xgmi|to SESDMAEngine|SDMA queue|POLL_REGMEM|SDMA atomic|fatal|panic|unsupported|mismatch" m5out-hsa-remote-cache-read/simout.txt m5out-hsa-remote-cache-read/simerr.txt m5out-hsa-remote-cache-read/remote-cache.trace
```

First relevant observable result:

```text
REMOTE_CACHE_READ_RESULT first=0x11111111 second=0x11111111 classification=stale
REMOTE_CACHE_READ_OBSERVED_STALE
```

Facts confirmed:

- GPU1 first read value: 0x11111111.
- SDMA_XGMI overwrite queue: queue_type 3, backend sdma_xgmi, doorbell routed to SESDMAEngine.
- GPU1 second read value: 0x11111111.
- Classification: stale.

Hypotheses accepted:

- The current SE multi-GPU path lacks visible remote-write invalidation for this access pattern.

Hypotheses rejected:

- The remote write did not make GPU1's same-dispatch second read observe the updated value.

Files changed:

- tests/test-progs/gpu/xgmi-peer-vram/remote_cache_kernels.hip
- tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_cache_read.cpp
- tests/test-progs/gpu/xgmi-peer-vram/Makefile
- tests/pyunit/stdlib/test_se_viper_multigpu.py
- docs/debug/se-multigpu-status.md

Single next diagnostic step:

- Locate the cache level retaining the old GPU1 line and identify the smallest invalidation hook.
```
````

- [ ] **Step 2: Run source and formatting checks**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_remote_write_after_cache_read_smoke_has_same_dispatch_reader
python3 -m py_compile tests/pyunit/stdlib/test_se_viper_multigpu.py
git diff --check -- \
  docs/debug/se-multigpu-status.md \
  tests/pyunit/stdlib/test_se_viper_multigpu.py \
  tests/test-progs/gpu/xgmi-peer-vram/Makefile \
  tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_cache_read.cpp \
  tests/test-progs/gpu/xgmi-peer-vram/remote_cache_kernels.hip
```

Expected result:

```text
Ran 1 test
OK
```

`py_compile` and `git diff --check` should produce no output.

- [ ] **Step 3: Keep generated artifacts untracked**

Do not stage:

```text
m5out-hsa-remote-cache-read/
tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_cache_read
tests/test-progs/gpu/xgmi-peer-vram/remote_cache_kernels.bundle
tests/test-progs/gpu/xgmi-peer-vram/remote_cache_kernels.hsaco
```

Only source, test, and documentation files are candidates for commit.

## Self-Review

- Spec coverage: the plan covers diagnostic structure, same-dispatch reader,
  SDMA_XGMI remote write, result classification, trace validation, and handoff
  update.
- Placeholder scan: no plan step depends on unspecified files or undefined
  result strings.
- Scope check: the plan intentionally excludes coherence fixes and ROCm API
  expansion; it only builds and runs the diagnostic baseline.
