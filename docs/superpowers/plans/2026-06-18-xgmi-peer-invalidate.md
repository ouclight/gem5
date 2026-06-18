# XGMI Peer Invalidation Diagnostic Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an independent diagnostic that determines whether GPU0 observes GPU1's updated value after GPU0 has previously read and potentially cached the same GPU1-VRAM line.

**Architecture:** Reuse the proven raw-HSA dispatch and ROCm-teardown avoidance patterns from `xgmi-peer-vram`, but place all new code in `xgmi-peer-invalidate`. The host serializes GPU1-write-A, GPU0-read-A, GPU1-write-B, and GPU0-read/classify phases, then directly reads mapped results and exits through `m5_exit` with a classification-specific code.

**Tech Stack:** HIP/ROCm 4 gfx900 HSACO, HSA runtime queues and AQL packets, gem5 x86 pseudo operations, Python `unittest`.

---

## File Structure

- Create: `tests/test-progs/gpu/xgmi-peer-invalidate/invalidate_kernels.hip`
  - Minimal write, read, and classify kernels.
- Create: `tests/test-progs/gpu/xgmi-peer-invalidate/invalidate_hip.cpp`
  - HSA setup, four host-sequenced dispatches, CPU result collection, and
    classification-specific `m5_exit`.
- Create: `tests/test-progs/gpu/xgmi-peer-invalidate/Makefile`
  - ROCm 4 gfx900 HSACO and x86 m5op build rules.
- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`
  - Source-level regression for sequencing, classification, teardown avoidance,
    build wiring, and preservation of the existing peer-VRAM baseline.
- Modify: `docs/debug/se-multigpu-status.md`
  - Exact lightweight verification and user-owned build/run handoff.
- Modify: `configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py`
  - Opt-in suppression of GPU0's implicit kernel-launch cache invalidation.

### Task 1: Add Failing Diagnostic Source Regression

**Files:**

- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- Test: `tests/pyunit/stdlib/test_se_viper_multigpu.py`

- [ ] **Step 1: Add a source-level behavior test**

Add this test method to `SEViperMultiGPUTest`:

```python
    def test_peer_invalidate_diagnostic_has_host_sequenced_phases(self):
        root = Path("tests/test-progs/gpu/xgmi-peer-invalidate")
        host = (root / "invalidate_hip.cpp").read_text()
        kernels = (root / "invalidate_kernels.hip").read_text()
        makefile = (root / "Makefile").read_text()
        config = Path(
            "configs/example/gem5_library/"
            "x86-vega-xgmi-multigpu-se.py"
        ).read_text()
        peer_baseline = Path(
            "tests/test-progs/gpu/xgmi-peer-vram/peer_vram_hip.cpp"
        ).read_text()

        phase_markers = [
            'MARK("phase 1: GPU1 write A")',
            'MARK("phase 2: GPU0 read A")',
            'MARK("phase 3: GPU1 write B")',
            'MARK("phase 4: GPU0 read and classify")',
        ]
        phase_positions = [host.index(marker) for marker in phase_markers]
        self.assertEqual(phase_positions, sorted(phase_positions))

        self.assertIn('"write_value"', host)
        self.assertIn('"read_value"', host)
        self.assertIn('"classify_value"', host)
        self.assertIn("write_value(", kernels)
        self.assertIn("read_value(", kernels)
        self.assertIn("classify_value(", kernels)

        self.assertIn("invalidation observed", host)
        self.assertIn("stale cache line observed", host)
        self.assertIn("unexpected value observed", host)
        self.assertIn("setup read failed", host)
        self.assertIn("m5_exit(0)", host)
        self.assertIn("m5_fail(0, 1)", host)
        self.assertIn("m5_fail(0, 2)", host)
        self.assertIn("std::fflush(stdout)", host)

        first_read = host.index('MARK("phase 2: GPU0 read A")')
        second_write = host.index('MARK("phase 3: GPU1 write B")')
        second_read = host.index(
            'MARK("phase 4: GPU0 read and classify")'
        )
        result_read = host.index("volatile const uint32_t *cpu_results")
        first_exit = min(
            host.index("m5_exit(0)"),
            host.index("m5_fail(0, 1)"),
            host.index("m5_fail(0, 2)"),
        )
        self.assertLess(first_read, second_write)
        self.assertLess(second_write, second_read)
        self.assertLess(second_read, result_read)
        self.assertLess(result_read, first_exit)

        self.assertNotIn("hsa_queue_destroy(", host)
        self.assertNotIn("hsa_executable_destroy(", host)
        self.assertNotIn("hsa_code_object_reader_destroy(", host)
        self.assertNotIn("hipFree(", host)

        self.assertIn("util/m5/src/abi/x86/m5op.S", makefile)
        self.assertIn("-I$(GEM5_ROOT)/include", makefile)
        self.assertIn("-lm5op_x86", makefile)
        self.assertIn("--offload-arch=gfx900", makefile)
        self.assertIn(
            "--disable-gpu0-kernel-launch-acquire",
            config,
        )
        self.assertIn(
            "gpus[0].impl_kern_launch_acq = False",
            config,
        )
        self.assertIn(
            'cause == "m5_fail instruction encountered"',
            config,
        )

        self.assertIn("peer VRAM verification passed", peer_baseline)
        self.assertNotIn("phase 1: GPU1 write A", peer_baseline)
```

- [ ] **Step 2: Run the new test and verify RED**

Run:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_peer_invalidate_diagnostic_has_host_sequenced_phases
```

Expected result: ERROR with `FileNotFoundError` for
`tests/test-progs/gpu/xgmi-peer-invalidate/invalidate_hip.cpp`.

### Task 2: Implement Kernels and Build Rules

**Files:**

- Create: `tests/test-progs/gpu/xgmi-peer-invalidate/invalidate_kernels.hip`
- Create: `tests/test-progs/gpu/xgmi-peer-invalidate/Makefile`

- [ ] **Step 1: Add the minimal kernels**

Create `invalidate_kernels.hip` with three `extern "C" __global__` kernels:

```cpp
#include <hip/hip_runtime.h>

#include <cstdint>

extern "C" __global__ void
write_value(volatile uint32_t *target, uint32_t value)
{
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        *target = value;
    }
}

extern "C" __global__ void
read_value(const volatile uint32_t *target, uint32_t *observed)
{
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        observed[0] = *target;
    }
}

extern "C" __global__ void
classify_value(const volatile uint32_t *target, uint32_t *observed,
               uint32_t value_a, uint32_t value_b)
{
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        const uint32_t value = *target;
        observed[1] = value;
        observed[2] = value == value_b ? 0 : (value == value_a ? 1 : 2);
    }
}
```

The volatile target access prevents the compiler from eliminating or folding
the diagnostic loads and stores.

- [ ] **Step 2: Add the standalone Makefile**

Create a Makefile following the existing ROCm 4 pattern, with:

```make
HIPCC ?= hipcc
CC ?= gcc
AR ?= ar
GEM5_ROOT ?= $(abspath $(CURDIR)/../../../..)
TARGET ?= invalidate_hip
SRC := invalidate_hip.cpp
M5OP_SRC := $(GEM5_ROOT)/util/m5/src/abi/x86/m5op.S
M5OP_OBJ := m5op_x86.o
M5OP_LIB := libm5op_x86.a
HSACO_TARGET ?= invalidate_kernels.hsaco
HSACO_SRC := invalidate_kernels.hip
HSACO_BUNDLE_TARGET ?= invalidate_kernels.bundle
HIPCCFLAGS ?=
HSACO_HIPCCFLAGS ?= -mno-code-object-v3
LLVM_BIN ?= $(ROCM_PATH)/llvm/bin

ifneq ($(ROCM_PATH),)
HIPCCFLAGS += --rocm-path=$(ROCM_PATH)
HIP_LDFLAGS += -L$(ROCM_PATH)/lib
endif

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC) $(HSACO_TARGET) $(M5OP_LIB)
	$(HIPCC) $(HIPCCFLAGS) -O2 -I$(GEM5_ROOT)/include -DINVALIDATE_HSACO_PATH=\"$(abspath $(HSACO_TARGET))\" -o $@ $(SRC) -L. -lm5op_x86 $(HIP_LDFLAGS) -lhsa-runtime64 -lhsakmt

$(M5OP_OBJ): $(M5OP_SRC)
	$(CC) -I$(GEM5_ROOT)/include -c -o $@ $<

$(M5OP_LIB): $(M5OP_OBJ)
	$(AR) rcs $@ $<

$(HSACO_TARGET): $(HSACO_BUNDLE_TARGET)
	$(LLVM_BIN)/clang-offload-bundler -unbundle -type=o -targets=host-x86_64-unknown-linux,hip-amdgcn-amd-amdhsa-gfx900 -inputs=$< -outputs=/dev/null,$@

$(HSACO_BUNDLE_TARGET): $(HSACO_SRC)
	$(HIPCC) $(HIPCCFLAGS) $(HSACO_HIPCCFLAGS) -O2 --genco --offload-arch=gfx900 -o $@ $<

clean:
	$(RM) $(TARGET) $(M5OP_OBJ) $(M5OP_LIB) $(HSACO_TARGET) $(HSACO_BUNDLE_TARGET)
```

Do not run this Makefile. The user owns the ROCm build.

### Task 3: Add the Diagnostic Cache-Persistence Option

**Files:**

- Modify: `configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py`

- [ ] **Step 1: Add the opt-in argument**

Add:

```python
parser.add_argument(
    "--disable-gpu0-kernel-launch-acquire",
    action="store_true",
    help=(
        "Disable GPU0's implicit cache invalidation at kernel launch. "
        "Required only for cache-persistence diagnostics."
    ),
)
```

- [ ] **Step 2: Apply it only to GPU0**

After constructing the GPU list:

```python
if args.disable_gpu0_kernel_launch_acquire:
    gpus[0].impl_kern_launch_acq = False
```

The default remains unchanged. Do not disable launch acquire on GPU1.

- [ ] **Step 3: Treat diagnostic failures as terminal events**

Add this condition beside the existing `m5_exit` condition:

```python
cause == "m5_fail instruction encountered"
```

The existing final `sys.exit(exit_event.getCode())` propagates code 1 or 2.

### Task 4: Implement the Host-sequenced Diagnostic

**Files:**

- Create: `tests/test-progs/gpu/xgmi-peer-invalidate/invalidate_hip.cpp`
- Reference: `tests/test-progs/gpu/xgmi-peer-vram/peer_vram_hip.cpp`

- [ ] **Step 1: Copy only the proven HSA support structure**

Implement the same local support types and functions used by the peer-VRAM
program:

- `MARK`, `HIP_CHECK`, and `HSA_CHECK`;
- `read_file`;
- aligned `append_arg`;
- HSA GPU-agent and kernarg-region discovery;
- `HsaKernel` and `load_hsa_kernel`;
- `dispatch_hsa_kernel` with system-scope acquire/release fences and completion
  waiting.

Change all log prefixes and the HSACO macro to:

```cpp
#ifndef INVALIDATE_HSACO_PATH
#define INVALIDATE_HSACO_PATH "invalidate_kernels.hsaco"
#endif
```

- [ ] **Step 2: Add allocations and HSA setup**

In `main()`:

```cpp
constexpr uint32_t value_a = 0x13579bdf;
constexpr uint32_t value_b = 0x2468ace0;
constexpr size_t cache_line_bytes = 64;
constexpr size_t result_bytes = 3 * sizeof(uint32_t);
```

On HIP device 1, allocate a 64-byte target and a three-word result buffer. Do
not initialize either with HIP copy/memset paths. Enable device 1 peer access
from device 0, initialize HSA, load the same HSACO for both GPU agents, find
`write_value` for GPU1 and `read_value` plus `classify_value` for GPU0, and
create one HSA queue per GPU.

- [ ] **Step 3: Dispatch the four serialized phases**

Use exact markers and dispatch one work-item per phase:

```cpp
MARK("phase 1: GPU1 write A");
// GPU1: write_value(target, value_a)

MARK("phase 2: GPU0 read A");
// GPU0: read_value(target, results)

MARK("phase 3: GPU1 write B");
// GPU1: write_value(target, value_b)

MARK("phase 4: GPU0 read and classify");
// GPU0: classify_value(target, results, value_a, value_b)
```

Each call to `dispatch_hsa_kernel` must return successfully before issuing the
next marker and dispatch.

- [ ] **Step 4: Read and classify results on the CPU**

Read the mapped result buffer directly:

```cpp
uint32_t host_results[3] = {};
volatile const uint32_t *cpu_results = results;
for (size_t i = 0; i < 3; ++i) {
    host_results[i] = cpu_results[i];
}
```

Print `value_a`, `value_b`, the first GPU0 observation, the second GPU0
observation, and the kernel classification code.

Use this terminal classification:

```cpp
if (host_results[0] != value_a) {
    std::printf("setup read failed\n");
    std::fflush(stdout);
    m5_fail(0, 2);
} else if (host_results[1] == value_b && host_results[2] == 0) {
    std::printf("invalidation observed\n");
    std::fflush(stdout);
    m5_exit(0);
} else if (host_results[1] == value_a && host_results[2] == 1) {
    std::printf("stale cache line observed\n");
    std::fflush(stdout);
    m5_fail(0, 1);
} else {
    std::printf("unexpected value observed\n");
    std::fflush(stdout);
    m5_fail(0, 2);
}

std::fprintf(stderr, "m5_exit returned unexpectedly\n");
return 3;
```

Do not add ROCm teardown calls before any terminal exit.

- [ ] **Step 5: Run focused and complete lightweight tests**

Run:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_peer_invalidate_diagnostic_has_host_sequenced_phases
```

Expected result: one test passes.

Run:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Expected result: all lightweight tests pass.

Run:

```bash
git diff --check
```

Expected result: no output and exit status 0.

### Task 5: Record the Build and Runtime Handoff

**Files:**

- Modify: `docs/debug/se-multigpu-status.md`

- [ ] **Step 1: Record lightweight verification**

Append the exact focused/full commands and results. Record that source creation
and source-level regression completed without building the binary or running a
full simulation.

- [ ] **Step 2: Record the user-owned build command**

Record, but do not run:

```bash
make -C tests/test-progs/gpu/xgmi-peer-invalidate \
  ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  HIPCC=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/bin/hipcc
```

- [ ] **Step 3: Record the user-owned full simulation command**

Record, but do not run:

```bash
build/VEGA_X86/gem5.opt \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type timing \
  --disable-gpu0-kernel-launch-acquire \
  --app tests/test-progs/gpu/xgmi-peer-invalidate/invalidate_hip \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1
```

The single next step is for the user to build the diagnostic binary. Only after
that succeeds should the full simulation and bounded Ruby trace flags be
selected.

No commit is included because the repository guidance forbids committing the
user's uncommitted work without explicit permission.
