# SE HIP Memset Preload Compatibility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a preload library that executes public synchronous `hipMemset` with a precompiled gfx900 kernel instead of ROCclr's runtime-compiled blit program.

**Architecture:** A code-object-v2 HSACO contains a byte-fill kernel. An `LD_PRELOAD` shared library exports `hipMemset`, loads the HSACO with the HSA executable API, submits it through a private HSA queue, and waits for the AQL completion signal before returning.

**Tech Stack:** C++17, HIP runtime/module API, hipcc, clang-offload-bundler, GNU Make, Python unittest, gem5 AtomicSimpleCPU.

---

### Task 1: Add the source and build contract test

**Files:**
- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- Create later: `tests/test-progs/gpu/hip-api-smoke/se_hip_compat/se_hip_memset.hip`
- Create later: `tests/test-progs/gpu/hip-api-smoke/se_hip_compat/se_hip_compat.cpp`
- Create later: `tests/test-progs/gpu/hip-api-smoke/se_hip_compat/Makefile`

- [ ] **Step 1: Write the failing test**

Add a test that requires:

```python
def test_se_hip_memset_compatibility_layer_contract(self):
    root = Path("tests/test-progs/gpu/hip-api-smoke/se_hip_compat")
    kernel = (root / "se_hip_memset.hip").read_text()
    wrapper = (root / "se_hip_compat.cpp").read_text()
    makefile = (root / "Makefile").read_text()

    self.assertIn('extern "C" __global__ void', kernel)
    self.assertIn("seHipMemsetKernel", kernel)
    self.assertIn('extern "C" hipError_t hipMemset', wrapper)
    self.assertIn("RTLD_NEXT", wrapper)
    self.assertIn("hsa_executable_load_agent_code_object", wrapper)
    self.assertIn("hsa_executable_get_symbol_by_name", wrapper)
    self.assertIn("hsa_queue_create", wrapper)
    self.assertIn("hsa_kernel_dispatch_packet_t", wrapper)
    self.assertIn("hsa_signal_wait_scacquire", wrapper)
    self.assertIn("SE_HIP_MEMSET_HSACO", wrapper)
    self.assertIn("--offload-arch=gfx900", makefile)
    self.assertIn("-mno-code-object-v3", makefile)
    self.assertIn("-shared", makefile)
```

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
build/VEGA_X86/gem5.opt -m unittest discover \
  -s tests/pyunit/stdlib -p test_se_viper_multigpu.py
```

Expected: failure opening the missing `se_hip_compat` source files.

### Task 2: Build the precompiled memset HSACO

**Files:**
- Create: `tests/test-progs/gpu/hip-api-smoke/se_hip_compat/se_hip_memset.hip`
- Create: `tests/test-progs/gpu/hip-api-smoke/se_hip_compat/Makefile`

- [ ] **Step 1: Add the byte-fill kernel**

Implement:

```cpp
#include <hip/hip_runtime.h>

#include <cstddef>

extern "C" __global__ void
seHipMemsetKernel(unsigned char *dst, unsigned char value, size_t size)
{
    const size_t index =
        static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index < size) {
        dst[index] = value;
    }
}
```

- [ ] **Step 2: Add HSACO build rules**

The Makefile must build a bundled object with:

```make
$(HIPCC) --rocm-path=$(ROCM_PATH) -mno-code-object-v3 -O2 \
  --genco --offload-arch=gfx900 -o se_hip_memset.bundle se_hip_memset.hip
```

Then extract:

```make
$(ROCM_PATH)/llvm/bin/clang-offload-bundler -unbundle -type=o \
  -targets=host-x86_64-unknown-linux,hip-amdgcn-amd-amdhsa-gfx900 \
  -inputs=se_hip_memset.bundle -outputs=/dev/null,se_hip_memset.hsaco
```

- [ ] **Step 3: Build the HSACO**

Run:

```bash
make -C tests/test-progs/gpu/hip-api-smoke/se_hip_compat \
  ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  se_hip_memset.hsaco
```

Expected: `se_hip_memset.hsaco` is created successfully.

### Task 3: Implement the HSA-backed preload wrapper

**Files:**
- Create: `tests/test-progs/gpu/hip-api-smoke/se_hip_compat/se_hip_compat.cpp`
- Modify: `tests/test-progs/gpu/hip-api-smoke/se_hip_compat/Makefile`

- [ ] **Step 1: Implement one-time HSA initialization**

Resolve `hipGetDevice` with `RTLD_NEXT`, select the corresponding HSA GPU
agent, find a kernarg region, load `SE_HIP_MEMSET_HSACO` with
`hsa_executable_load_agent_code_object`, resolve `seHipMemsetKernel` with an
`@kd` fallback, and create a private HSA queue.

- [ ] **Step 2: Implement synchronous AQL dispatch**

Allocate and zero the complete kernarg segment, pack the three explicit
arguments at their required alignments, submit an
`hsa_kernel_dispatch_packet_t`, ring the queue doorbell, and wait for the
completion signal to reach zero.

- [ ] **Step 3: Implement `hipMemset`**

Required behavior:

```cpp
extern "C" hipError_t
hipMemset(void *dst, int value, size_t sizeBytes)
{
    if (sizeBytes == 0) {
        return hipSuccess;
    }
    if (dst == nullptr) {
        return hipErrorInvalidDevicePointer;
    }

    // initialize HSA executable and queue once
    // launch ceil(sizeBytes / 256) workgroups
    // pass dst, low eight bits of value, and sizeBytes
    // wait for completion and return the first error
}
```

- [ ] **Step 4: Build the shared library**

Use:

```make
$(CXX) -std=c++17 -O2 -fPIC -shared \
  -I$(ROCM_PATH)/hip/include \
  -L$(ROCM_PATH)/lib -o libse_hip_compat.so \
  se_hip_compat.cpp -ldl -lhsa-runtime64
```

- [ ] **Step 5: Verify GREEN**

Run the pyunit command from Task 1. Expected: all tests pass.

### Task 4: Verify binary contracts

**Files:**
- No source changes.

- [ ] **Step 1: Build all compatibility artifacts**

Run:

```bash
make -C tests/test-progs/gpu/hip-api-smoke/se_hip_compat \
  ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1
```

- [ ] **Step 2: Verify the exported interposition symbol**

Run:

```bash
nm -D --defined-only \
  tests/test-progs/gpu/hip-api-smoke/se_hip_compat/libse_hip_compat.so \
  | grep ' T hipMemset$'
```

Expected: exactly one exported `hipMemset`.

- [ ] **Step 3: Verify the HSACO symbol**

Run:

```bash
readelf -sW \
  tests/test-progs/gpu/hip-api-smoke/se_hip_compat/se_hip_memset.hsaco \
  | grep seHipMemsetKernel
```

Expected: the kernel symbol is present.

### Task 5: Run bounded Atomic gem5 validation

**Files:**
- Modify after result: `docs/debug/se-multigpu-status.md`

- [ ] **Step 1: Run the public HIP memset smoke stage**

Run:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hip-memset-preload-atomic \
  --listener-mode=off \
  --debug-flags=GPUDriver,HSAPacketProcessor,GPUCommandProc,GPUDisp,SyscallAll \
  --debug-file=hip-memset-preload.trace \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type atomic \
  --max-ticks 100000000000 \
  --app tests/test-progs/gpu/hip-api-smoke/hip_memset_preload_smoke \
  --rocm-path \
  /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env LD_PRELOAD=/home/zhangds/gem5/tests/test-progs/gpu/hip-api-smoke/se_hip_compat/libse_hip_compat.so \
  --env SE_HIP_MEMSET_HSACO=/home/zhangds/gem5/tests/test-progs/gpu/hip-api-smoke/se_hip_compat/se_hip_memset.hsaco
```

- [ ] **Step 2: Verify behavior**

Require all:

```text
HIP_MEMSET_PRELOAD_PASSED
no /tmp/comgr-* path in the syscall trace
at least one completed GPU workgroup
at least one executed GPU instruction
```

- [ ] **Step 3: Record the result**

Append the exact command, first relevant result, confirmed facts, changed
files, verification, and one next diagnostic step to
`docs/debug/se-multigpu-status.md`.

### Task 6: Run bounded Timing Ruby-path validation

**Files:**
- Modify after result: `docs/debug/se-multigpu-status.md`

- [ ] **Step 1: Repeat the public HIP memset stage with Timing CPU**

Use the Task 5 command with:

```text
-d m5out-hip-memset-preload-timing
--cpu-type timing
--debug-file=hip-memset-preload-timing.trace
```

Keep the 100,000,000,000 absolute tick limit.

- [ ] **Step 2: Verify Ruby-path behavior**

Require all:

```text
API_STAGE_PASSED stage=memset
no /tmp/comgr-* path in the syscall trace
nonzero GPU instructions and completed workgroups
nonzero Ruby request/activity counters
```

If the run reaches its bound before completion, inspect the first relevant
Ruby/GPU observable and reduce the conclusion accordingly; do not report an
Atomic pass as Ruby verification.

### Task 7: Final regression verification

**Files:**
- No source changes.

- [ ] **Step 1: Run pyunit**

```bash
build/VEGA_X86/gem5.opt -m unittest discover \
  -s tests/pyunit/stdlib -p test_se_viper_multigpu.py
```

- [ ] **Step 2: Check patch formatting**

```bash
git diff --check
```

- [ ] **Step 3: Review scoped changes**

```bash
git status --short
git diff -- \
  tests/test-progs/gpu/hip-api-smoke/se_hip_compat \
  tests/pyunit/stdlib/test_se_viper_multigpu.py \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  docs/debug/se-multigpu-status.md
```

Do not commit without explicit user authorization.
