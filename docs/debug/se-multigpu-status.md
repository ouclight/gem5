# SE Multi-GPU Debugging Status

Last updated: 2026-06-18

## Goal

Build a gem5 SE-mode multi-GPU platform for validating cache-coherence
protocol behavior between GPUs. The current end-to-end case has GPU0 write a
buffer allocated in GPU1 VRAM, then has GPU1 verify the contents.

## Current Ownership

- The user owns gem5 compilation and full simulation runs.
- Codex should inspect results and make targeted source changes only after
  identifying the root cause.
- Do not launch a long build or simulation without explicit delegation.

## Primary Test

Source:

- `tests/test-progs/gpu/xgmi-peer-vram/peer_vram_hip.cpp`
- `tests/test-progs/gpu/xgmi-peer-vram/peer_vram_kernels.hip`

Binary:

- `tests/test-progs/gpu/xgmi-peer-vram/peer_vram_hip`

Configuration:

- `configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py`

Reference run command:

```bash
build/VEGA_X86/gem5.opt \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --app tests/test-progs/gpu/xgmi-peer-vram/peer_vram_hip \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --cpu-type timing
```

`--listener-mode=off` is required in restricted environments where gem5
cannot create the remote GDB listening socket.

## Confirmed Behavior

- ROCm enumerates two HIP and HSA GPU agents.
- KFD returns two process aperture entries with GPU IDs `22124` and `22125`.
- GPU1 VRAM allocation uses pool 2 and was observed at physical address
  `0x140000000`.
- `hipDeviceEnablePeerAccess(1, 0)` maps the GPU1 allocation for GPU0.
- The GPU0 `write_peer_vram` kernel completes.
- The GPU1 `verify_peer_vram` kernel completes.
- No Ruby deadlock, protocol fatal, assertion, or GPU kernel failure was
  observed before result collection.

## Previous Failure Point

The previous run stalled after:

```text
[peer_vram_hip] begin hipMemcpy(host_errors.data(), errors, bytes,
                                hipMemcpyDeviceToHost)
```

The Timing CPU then encountered an unimplemented x87 `frndint` instruction
and repeatedly issued ignored `mprotect` syscalls. The run executed about
29.6 million instructions over 128 seconds and was terminated without a Ruby
or GPU protocol error.

Current hypothesis: the stall is in the ROCm CPU-side device-to-host copy
path under the SE Timing CPU, not in the peer-VRAM coherence request path.

## Current Source Change

The final `hipMemcpy(DeviceToHost)` in `peer_vram_hip.cpp` has been replaced
with direct CPU reads through the mapped GPU1 allocation:

```cpp
volatile const uint32_t *cpu_errors = errors;
for (uint32_t i = 0; i < count; ++i) {
    host_errors[i] = cpu_errors[i];
}
```

This intentionally bypasses the ROCm copy path while preserving:

- GPU1 VRAM allocation;
- GPU0 peer access and writes;
- GPU1 verification kernel;
- CPU validation of the verification flags.

## Verification Completed

The focused regression test first failed against the old `hipMemcpy` code,
then passed after the direct-read change.

Commands already completed successfully:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_peer_vram_result_uses_cpu_mapping_instead_of_hip_memcpy
```

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 9 tests passed.

The test program was also rebuilt successfully with:

```bash
make -C tests/test-progs/gpu/xgmi-peer-vram \
  peer_vram_hip \
  ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  HIPCC=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/bin/hipcc
```

## Incomplete Verification

The user-owned run wrote its output to:

- `m5out/simout.txt`
- `m5out/simerr.txt`

The run confirmed that both GPU kernels and the direct CPU read complete:

```text
[peer_vram_hip] begin CPU read of mapped GPU1 errors
[peer_vram_hip] end CPU read of mapped GPU1 errors
```

It then stalls at:

```text
[peer_vram_hip] begin hipFree(errors)
```

No `hsaKmtFreeMemory` or `AMDKFD_IOC_FREE_MEMORY_OF_GPU` for `errors`
follows this marker. Instead, the Timing CPU encounters the unimplemented
x87 `frndint` instruction and repeatedly issues ignored `mprotect` syscalls.
This is the same ROCm CPU-runtime failure pattern previously seen in
`hipMemcpy`, now reached through `hipFree`.

The direct CPU read therefore removed the result-copy blocker. The remaining
blocker is ROCm teardown under the SE Timing CPU, after the peer-VRAM data
path has completed.

## Next Step

The source now calculates `error_count` and prints the pass/fail result
immediately after the direct CPU read. It also skips `hipFree(errors)` and
`hipFree(gpu1_mem)` after the HSA queue and executable teardown, because
those HIP frees enter the unsupported ROCm teardown path under the SE Timing
CPU. Process termination will reclaim the allocations.

The focused regression test and all nine tests in
`test_se_viper_multigpu.py` pass. The modified test binary was rebuilt and
run as described below.

## Latest End-to-End Result

The rebuilt binary was run on 2026-06-18. The peer-VRAM test itself passed:

```text
[peer_vram_hip] begin CPU read of mapped GPU1 errors
[peer_vram_hip] end CPU read of mapped GPU1 errors
peer VRAM verification passed
```

Both HSA queues and the executable/code-object handles were then destroyed,
and the test skipped the two known-broken HIP frees.

No Ruby deadlock, coherence-protocol fatal, or data mismatch was reported.
This confirms the current GPU0-to-GPU1 peer-VRAM data path for this test.

After `main` returned, the process issued `exit_group`. gem5 then aborted in
an independent SE CPU-exit issue:

```text
gem5.opt: src/cpu/simple/timing.cc:246:
TimingSimpleCPU::suspendContext(...):
Assertion `_status == BaseSimpleCPU::Running' failed.
```

The stack is `exit_group` -> `ThreadContext::halt()` ->
`BaseSimpleCPU::haltContext()` -> `TimingSimpleCPU::suspendContext()`.
The abort occurs after the benchmark result is known and is not evidence of
a GPU coherence failure. The local `Process::deallocateMem` alias change is
not on this call path.

Next diagnostic task: determine why the exiting TimingSimpleCPU context is
not in `Running` state when `exit_group` calls `halt()`. Capture the
ThreadContext status, TimingSimpleCPU `_status`, CPU ID, and context ID
immediately before the failing halt, then compare against a simple
multi-threaded SE workload using the standard board setup.

## Exit Handling Workaround

The assertion is caused by generic multi-threaded SE `exit_group` attempting
to asynchronously halt a TimingSimpleCPU while that CPU may be in a timing
wait state. Changing `TimingSimpleCPU::suspendContext()` to accept every wait
state would leave outstanding I-cache or D-cache transactions and is not a
safe localized fix.

For this focused coherence microbenchmark, the success path now uses gem5's
x86 instruction-based `m5_exit(0)` after:

- printing `peer VRAM verification passed`;
- destroying both HSA queues;
- destroying the HSA executable and code-object reader;
- skipping the two unsupported HIP frees.

The Makefile now compiles and links
`util/m5/src/abi/x86/m5op.S` and includes `include/gem5/m5ops.h`.
This terminates simulation before the process enters the problematic
multi-threaded `exit_group` path.

The focused regression test and all nine tests in
`test_se_viper_multigpu.py` pass. Because ROCm 4 `hipcc` attempts to device
compile explicit assembly and object inputs, the Makefile first builds
`m5op_x86.o`, archives it as `libm5op_x86.a`, and links it with
`-L. -lm5op_x86`.

The binary was rebuilt successfully. `nm` confirms that it defines
`m5_exit`, and its strings contain the expected success and m5-exit markers.
The full simulation was rerun but manually terminated after it stalled.

The benchmark again completed the peer-VRAM validation:

```text
[peer_vram_hip] begin CPU read of mapped GPU1 errors
[peer_vram_hip] end CPU read of mapped GPU1 errors
peer VRAM verification passed
```

It then stalled at:

```text
[peer_vram_hip] begin hsa_queue_destroy(gpu1_queue)
```

There is no matching `end hsa_queue_destroy(gpu1_queue)` and no
`exit simulation with m5_exit` marker. The final gem5 exit cause is
`user interrupt received`, confirming that `m5_exit` was never executed.

This run expands the known unsupported/nondeterministic ROCm teardown region:
it can begin at HSA queue destruction, not only at `hipFree`. The peer-VRAM
data result is already known before teardown starts.

Next code change: invoke `m5_exit(0)` immediately after flushing
`peer VRAM verification passed`, before destroying HSA queues, executable
objects, or HIP allocations. Keep failure paths returning nonzero so only a
verified success terminates through the pseudo instruction.

This change is now implemented. The success path performs no ROCm teardown:
after printing and flushing the passing result, it prints the skip/exit
markers and invokes `m5_exit(0)`. Failure paths still return nonzero before
the pseudo instruction.

The focused test and all nine multi-GPU Python tests pass. The
`peer_vram_hip` binary was rebuilt successfully at 2026-06-18 14:07:41;
`nm` confirms the `m5_exit` symbol and `strings` confirms the new markers.
The next step is a full simulation rerun.

## Final Full-Simulation Verification

The user-owned full simulation completed successfully on 2026-06-18. The
output files are:

- `m5out/simout.txt`
- `m5out/simerr.txt`

The command recorded in `m5out/simout.txt` was:

```bash
./build/VEGA_X86/gem5.opt \
  -d m5out -r -e \
  --stdout-file=simout.txt \
  --stderr-file=simerr.txt \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type timing \
  --app tests/test-progs/gpu/xgmi-peer-vram/peer_vram_hip \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1
```

The first relevant final result was:

```text
[peer_vram_hip] begin CPU read of mapped GPU1 errors
[peer_vram_hip] end CPU read of mapped GPU1 errors
peer VRAM verification passed
[peer_vram_hip] skip ROCm teardown: unsupported in SE timing mode
[peer_vram_hip] exit simulation with m5_exit
Exiting @ tick 48859602489 because m5_exit instruction encountered.
```

Facts confirmed by this run:

- both GPU kernels completed;
- GPU0's peer writes to GPU1 VRAM were visible to GPU1's verification kernel;
- the CPU-observed verification flags contained no mismatch;
- the success path skipped the unsupported ROCm teardown region;
- the x86 `m5_exit(0)` pseudo instruction executed and terminated gem5;
- no Ruby deadlock, coherence-protocol fatal, data mismatch, or
  `TimingSimpleCPU::suspendContext()` assertion occurred.

The hypothesis that moving `m5_exit(0)` immediately after the flushed success
result avoids both ROCm teardown stalls and the later multi-threaded
`exit_group` assertion is accepted for this focused microbenchmark.

No source file was changed as part of this log inspection. This status
document was updated with the verification result.

Verification completed: the focused SE multi-GPU peer-VRAM end-to-end test
passes and exits through `m5_exit`.

Next diagnostic step: preserve this run as the passing baseline and rerun the
focused Python regression tests after any cleanup or integration changes.

## Diff Classification Review

The uncommitted work was moved from `stable` to a new local branch:

```bash
git switch -c se_multigpu
```

Result:

```text
Switched to a new branch 'se_multigpu'
```

The initial diff review classifies the work into these independent groups:

1. SE multi-GPU core implementation:
   - multi-device KFD ownership, GPU IDs, queues, apertures, VRAM pools, and
     peer mappings in `src/gpu-compute/`;
   - SE Vega GPU, board, cache hierarchy, KFD topology, and XGMI network
     Python components;
   - `x86-vega-xgmi-multigpu-se.py`;
   - TCC/TCCdir cluster-aware Ruby routing.
2. Focused validation:
   - `test_se_viper_multigpu.py`;
   - `tests/test-progs/gpu/xgmi-peer-vram/` source and Makefile;
   - the debug status document.
3. Independent fixes that should be reviewed and committed separately:
   - gfx900 kernarg preload handling in `gpu_command_processor.cc`;
   - the `BL2_M, L3Hit` Ruby transition and its regression test;
   - the generic `SimplePt2Pt` link-ID correction;
   - the generic `Process::deallocateMem` alias handling.
4. Full-system multi-GPU work outside the focused SE change:
   - `x86-mi300x-multigpu.py`;
   - multi-GPU VBIOS command changes in `amdgpu.py`, `board.py`, and related
     full-system network changes.
5. Diagnostic or generated content that should not enter the functional
   commit without explicit justification:
   - unconditional KFD/render-driver `warn()` tracing;
   - `.deps/`, `.venv/`, Python bytecode, `m5out/`;
   - generated peer test binaries, objects, archives, HSACO, and bundle files.

The focused SE regression suite was rerun:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 9 tests passed.

The additional Ruby/GPU regression file was then run:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit \
  -m unittest test_gpu_viper_wb_l2_transitions
```

The first relevant error was:

```text
FAIL: test_peer_vram_result_uses_cpu_visible_hsa_memory
AssertionError: 'hsa_memory_allocate(kernarg_region.region, bytes'
not found
```

Facts confirmed:

- three of the four tests in that file pass;
- the failing assertion expects `errors` to use HSA kernarg memory and rejects
  `hipMalloc(&errors, ...)`;
- the current benchmark source intentionally uses GPU1 `hipMalloc` VRAM for
  `errors`, and the successful full simulation used that source;
- source timestamp `14:07:18`, binary timestamp `14:07:41`, and the simulation
  log agree, so this is a stale test expectation rather than a stale binary.

Hypothesis accepted: the last test in
`test_gpu_viper_wb_l2_transitions.py` belongs to an abandoned intermediate
allocation experiment and must be reconciled with the verified peer-VRAM test
semantics before integration.

Files changed in this review: only this status document. No production or test
source was modified.

Next diagnostic step: remove or rewrite the stale HSA-allocation assertion,
then rerun both lightweight Python test files before continuing the remaining
diff cleanup.

### Stale Peer-VRAM Test Reconciled

The stale assertion in
`tests/pyunit/test_gpu_viper_wb_l2_transitions.py` was replaced with checks
for the behavior used by the passing end-to-end run:

- `errors` is allocated with `hipMalloc` while GPU1 is selected;
- allocation completes before switching back to GPU0;
- result flags are read through the CPU mapping;
- the unsupported `hipMemcpy(DeviceToHost)` path remains absent.

The previously failing command was rerun:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit \
  -m unittest test_gpu_viper_wb_l2_transitions
```

Result: all 4 tests passed.

The focused SE multi-GPU suite was also rerun:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 9 tests passed.

Facts confirmed:

- the additional regression file now describes the verified GPU1-VRAM
  allocation and CPU-read behavior;
- both lightweight test files are green;
- no production source was changed.

Files changed:

- `tests/pyunit/test_gpu_viper_wb_l2_transitions.py`
- `docs/debug/se-multigpu-status.md`

Next diagnostic step: review and remove or downgrade unconditional diagnostic
`warn()` calls in the KFD and render-driver changes, retaining only warnings
that report unsupported behavior or invalid state.

### KFD and Render-Driver Warning Cleanup

The approved logging cleanup converted normal KFD and render-driver operation
messages from unconditional `warn()` calls to `DPRINTF(GPUDriver, ...)`.
Covered operations include:

- opening KFD and render devices;
- creating queues;
- returning legacy and new aperture information;
- creating events;
- acquiring a VM;
- allocating GPU memory;
- mapping allocations to peer GPUs.

Warnings remain unconditional for unsupported or ignored behavior, conflicting
or unrecognized mmap requests, unsupported event types, duplicate waiters, and
incomplete page-table cleanup.

A focused source-classification test was added first and run with:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kfd_normal_operations_use_debug_tracing
```

The first relevant failure was:

```text
AssertionError: 'warn("Opened KFD driver' unexpectedly found
```

This confirmed that the test detected the intended pre-cleanup behavior. After
the logging changes, the focused test passed.

During the first full-suite run, the new test reported:

```text
NameError: name 're' is not defined
```

The missing standard-library import was added, and both lightweight suites were
rerun:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 10 tests passed.

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit \
  -m unittest test_gpu_viper_wb_l2_transitions
```

Result: all 4 tests passed.

`git diff --check` passed for the changed source, tests, design, and plan
documents. A targeted search found no remaining normal-operation `warn()` call
matching the converted messages.

Files changed:

- `src/gpu-compute/gpu_compute_driver.cc`
- `src/gpu-compute/gpu_render_driver.cc`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- `docs/superpowers/specs/2026-06-18-kfd-warning-cleanup-design.md`
- `docs/superpowers/plans/2026-06-18-kfd-warning-cleanup.md`
- `docs/debug/se-multigpu-status.md`

No gem5 build or full simulation was started.

Next review step: separate the generic fixes
(`gpu_command_processor.cc`, `MOESI_AMD_Base-dir.sm`,
`viper_network.py`, and `process.cc`) from the focused SE multi-GPU change and
decide which require independent commits.

### Generic-Change Split Review

The four requested files were reviewed without modifying their source:

```bash
git diff -U20 -- src/gpu-compute/gpu_command_processor.cc
git diff -U30 -- src/mem/ruby/protocol/MOESI_AMD_Base-dir.sm
git diff -U25 -- src/python/gem5/prebuilt/viper/viper_network.py
git diff -U30 -- src/sim/process.cc
```

Relevant call sites, state transitions, similar network implementations, page
table behavior, and existing tests were also inspected with targeted `rg` and
`sed` commands.

#### `gpu_command_processor.cc`

This file contains two independent changes:

1. clearing kernarg-preload descriptor fields for older code objects;
2. reading preload arguments through the SE process page table.

The gfx-version condition is currently too broad:

```cpp
if (gfxVersion != GfxVersion::gfx942 &&
    gfxVersion != GfxVersion::gfx950)
```

It also disables preload for `gfx90a`, while other command-processor and
wavefront code explicitly treats `gfx90a` as a newer descriptor/preload-capable
target. This is a regression risk. The compatibility workaround should use an
explicit list of affected legacy targets, beginning with the verified ROCm 4
`gfx900` case, instead of treating every non-`gfx942`/`gfx950` target as legacy.

The SE `readPreload` path is not exercised by the current `gfx900` peer-VRAM
case after the legacy descriptor fields are cleared. It should therefore be a
separate generic feature with a real preload-capable SE test, not bundled as a
requirement of the focused multi-GPU change.

Classification:

- legacy `gfx900` descriptor compatibility: required prerequisite, but revise
  the version guard before integration;
- SE preload reading: separate generic commit, currently under-tested.

#### `MOESI_AMD_Base-dir.sm`

This file mixes two unrelated changes:

1. `TCC_select_cluster_id` routing, which is part of the SE multi-GPU
   implementation and must remain with the corresponding changes in the other
   Ruby protocol files;
2. the generic `transition(BL2_M, L3Hit, U)` race fix.

The new transition is coherent with the state machine. `qdr_queueDmaRdReq`
copies L3 data into the TBE and schedules a delayed `L3Hit`; probe completion
can move `BL2` to `BL2_M` before that trigger arrives. In that case no memory
read was issued, so the transition correctly returns the TBE data and completes
the DMA request. Its actions match the completion paths for
`BL2_Pm/ProbeAcksComplete` and `BL2_M/MemData`, excluding the memory-queue pop.

The current regression only checks the SLICC source text. This fix should be an
independent protocol commit and ideally gain a protocol-level reproducer when
one is practical.

Classification:

- cluster routing hunks: core SE multi-GPU commit;
- `BL2_M/L3Hit`: independent Ruby protocol race-fix commit.

#### `viper_network.py`

This file also mixes two unrelated changes:

1. `ClusteredXGMINetwork`, required by the new SE multi-GPU hierarchy;
2. the generic `SimplePt2Pt` link-ID correction.

The `SimplePt2Pt` implementation currently reuses IDs between external and
internal links. Starting internal IDs after the external-link range is a valid
generic correction, though the current increment-before-assignment leaves one
unused ID. The gap is harmless, but a dedicated uniqueness test and direct
zero-based continuation would make the change clearer.

`ClusteredXGMINetwork` imports a pure helper from `se_xgmi_network.py`, causing
the existing general Viper network module to depend on an SE-specific module.
For cleaner commit isolation, the class should move into
`se_xgmi_network.py`, and `se_gpu_cache_hierarchy.py` should import it directly.
That would leave `viper_network.py` changed only by the generic link-ID fix.

Classification:

- clustered XGMI class/helper: core SE multi-GPU commit, preferably contained
  entirely in `se_xgmi_network.py`;
- `SimplePt2Pt` link IDs: independent generic network-fix commit with a test.

#### `process.cc`

The alias-aware deallocation patch is unsafe in its current form.
`Process::zeroPages` defaults to true, and the code zeroes the physical page
before unmapping and checking for aliases. When another virtual mapping still
references the page, that live alias is preserved but its contents have already
been destroyed.

The scan also only sees aliases in the current page table. It cannot account
for mappings of the same physical page in another process/page table, and it
does not track the physical memory pool needed by `deallocPhysPage`.

The successful peer-VRAM path exits through `m5_exit` before ROCm teardown, and
the earlier `exit_group` assertion was not on this call path. No evidence from
the final passing run requires this global process-memory change.

Classification:

- exclude `process.cc` from the SE multi-GPU integration;
- redesign separately using explicit physical-page ownership/reference
  accounting, or apply a localized GPU-driver solution if a concrete teardown
  reproducer still requires it.

#### Recommended Commit Boundaries

1. Core SE multi-GPU platform:
   - clustered XGMI network;
   - cluster-aware Ruby routing;
   - multi-device KFD and SE platform/configuration changes.
2. ROCm 4 `gfx900` kernel-descriptor compatibility:
   - revise to an explicit legacy-version guard;
   - keep its focused regression.
3. Ruby WB-L2 delayed-L3-hit race fix:
   - `BL2_M/L3Hit` transition and focused regression.
4. Generic `SimplePt2Pt` link-ID uniqueness fix:
   - isolated network hunk and a dedicated test.
5. SE kernarg preload support:
   - defer until a preload-capable SE workload verifies it.
6. Generic process physical-page alias handling:
   - do not integrate in current form.

Files changed by this review:

- `docs/debug/se-multigpu-status.md`

No source, test, build, or simulation command was executed.

Next code step: isolate `ClusteredXGMINetwork` into
`se_xgmi_network.py`, leaving the generic `SimplePt2Pt` fix independently
reviewable.

### Clustered XGMI Network Isolation

`ClusteredXGMINetwork` was moved completely from the general
`viper_network.py` module into `se_xgmi_network.py`. The destination module now
owns:

- the `SimpleNetwork`, link, and switch imports;
- `_cluster_int_link_specs`;
- `ClusterIntLinkSpec`;
- `ClusteredXGMINetwork`.

`se_gpu_cache_hierarchy.py` now imports the class directly from
`se_xgmi_network.py`. The topology and link-ID behavior were not changed.
`viper_network.py` no longer imports any SE-specific module, leaving its
`SimplePt2Pt` link-ID change independently reviewable.

A source-boundary regression was added first and run with:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_clustered_xgmi_network_is_owned_by_se_module
```

The first relevant failure was:

```text
AssertionError: 'class ClusteredXGMINetwork' not found
```

After the migration, the focused test passed.

The current pre-migration `gem5.opt` was also asked to import the moved class:

```bash
build/VEGA_X86/gem5.opt \
  -c 'from gem5.prebuilt.viper.se_xgmi_network import ClusteredXGMINetwork'
```

It reported:

```text
ImportError: cannot import name 'ClusteredXGMINetwork'
```

This binary was built before the Python source migration. gem5's embedded
Python importer still resolves its compiled-in module even when
`-p src/python` is supplied, so a rebuilt binary is required for the runtime
import check. A direct syntax check of the updated source succeeded:

```bash
python3 -m py_compile \
  src/python/gem5/prebuilt/viper/se_xgmi_network.py \
  src/python/gem5/prebuilt/viper/viper_network.py \
  src/python/gem5/prebuilt/viper/se_gpu_cache_hierarchy.py \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
```

Lightweight regressions:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 11 tests passed.

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit \
  -m unittest test_gpu_viper_wb_l2_transitions
```

Result: all 4 tests passed.

`git diff --check` passed.

Files changed:

- `src/python/gem5/prebuilt/viper/se_xgmi_network.py`
- `src/python/gem5/prebuilt/viper/se_gpu_cache_hierarchy.py`
- `src/python/gem5/prebuilt/viper/viper_network.py`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- `docs/debug/se-multigpu-status.md`

No build or full simulation was started.

Next code step: isolate and test the generic `SimplePt2Pt` link-ID uniqueness
fix, using contiguous internal IDs after the external-link range.

### SimplePt2Pt Link-ID Fix Isolated

The generic `SimplePt2Pt` change is now isolated in
`src/python/gem5/prebuilt/viper/viper_network.py`. Relative to the repository
baseline, the implementation has only two semantic changes:

```python
link_count = len(self.ext_links)
```

and incrementing `link_count` after constructing each internal link. External
links therefore use IDs `0..N-1`, and internal links continue contiguously from
`N` without collisions or a skipped ID.

A focused source-structure test was added. The current `gem5.opt` embeds its
Python modules at build time, so a source-level test is used until the binary is
rebuilt.

The first test version produced an error because it searched for the increment
only after the append:

```text
ValueError: substring not found
```

The test was corrected to compare both positions directly. The required RED
result was then:

```text
AssertionError: 1333 not less than 1263
```

This confirmed that the existing implementation incremented the ID before
constructing the internal link. After moving the increment, the focused test
passed:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_simple_pt2pt_link_ids_are_unique_and_contiguous
```

The complete lightweight suites were rerun:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 12 tests passed.

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit \
  -m unittest test_gpu_viper_wb_l2_transitions
```

Result: all 4 tests passed.

Python syntax checking and `git diff --check` passed.

Files changed:

- `src/python/gem5/prebuilt/viper/viper_network.py`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- `docs/debug/se-multigpu-status.md`

No build or full simulation was started.

Next code step: narrow the ROCm 4 kernel-descriptor compatibility workaround
in `gpu_command_processor.cc` to explicitly affected legacy gfx targets and
separate the unverified SE preload-reading path.

### gfx900 Descriptor Compatibility Narrowed

The two changes previously mixed in `gpu_command_processor.cc` were separated.

The ROCm 4 compatibility workaround is now explicitly limited to the verified
target:

```cpp
if (gfxVersion == GfxVersion::gfx900) {
    akc->kernarg_preload_spec_length = 0;
    akc->kernarg_preload_spec_offset = 0;
}
```

This avoids disabling preload metadata for `gfx902`, `gfx908`, `gfx90a`,
`gfx942`, or `gfx950`. The `GfxVersion` lookup remains earlier in
`dispatchKernelObject()` so the same value can be used by both compatibility
handling and `HSAQueueEntry`.

The unverified `if (!FullSystem)` preload-reading branch was removed from
`readPreload()`. The focused gfx900 peer-VRAM workload does not exercise that
path because its descriptor fields are cleared. SE preload support remains
deferred until a preload-capable SE workload can validate it.

Two source-level regressions were updated first:

- `test_unverified_se_kernarg_preload_path_is_not_enabled`;
- `test_only_gfx900_ignores_new_descriptor_preload_fields`.

The RED run produced the expected two failures:

```text
AssertionError: 'if (!FullSystem)' unexpectedly found
```

and:

```text
'gfxVersion != GfxVersion::gfx942 && gfxVersion != GfxVersion::gfx950'
!= 'gfxVersion == GfxVersion::gfx900'
```

After the source change, both focused tests passed.

The complete lightweight suites were rerun:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit \
  -m unittest test_gpu_viper_wb_l2_transitions
```

Result: all 4 tests passed.

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 12 tests passed.

`git diff --check` passed. Relative to the repository baseline,
`gpu_command_processor.cc` now contains only the gfx900 descriptor
compatibility change; the unverified SE preload feature is no longer present.

Files changed:

- `src/gpu-compute/gpu_command_processor.cc`
- `tests/pyunit/test_gpu_viper_wb_l2_transitions.py`
- `docs/debug/se-multigpu-status.md`

No build or full simulation was started. Because this is a C++ source change,
the next user-owned build must precede final runtime verification.

### SE Kernarg Preload Path Restored

The previous cleanup incorrectly removed the SE-mode branch from
`GPUCommandProcessor::readPreload()`. The branch is required because SE kernarg
virtual addresses must be read through the process page table; without it, SE
execution falls through to the Full System GPU VM walker path.

The original branch was recovered from the earlier workspace diff and restored:

```cpp
if (!FullSystem) {
    auto *tc = sys->threads[0];
    SETranslatingPortProxy virt_proxy(tc);
    virt_proxy.readBlob(
        preload_addr,
        reinterpret_cast<uint8_t *>(task->preloadArgs()),
        sizeof(uint32_t) * akc->kernarg_preload_spec_length);
    initPreload(akc, task);
    return;
}
```

The source-level regression was changed first to require this behavior. Its RED
run was:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit \
  -m unittest \
  test_gpu_viper_wb_l2_transitions.TestGpuViperWbL2Transitions.test_se_kernarg_preload_uses_process_address_space
```

The first relevant failure was:

```text
AssertionError: 'if (!FullSystem)' not found
```

After restoring the branch, the same focused test passed. Both lightweight
suites were then run:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit \
  -m unittest test_gpu_viper_wb_l2_transitions
```

Result: all 4 tests passed.

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 12 tests passed.

```bash
git diff --check -- \
  src/gpu-compute/gpu_command_processor.cc \
  tests/pyunit/test_gpu_viper_wb_l2_transitions.py
```

Result: passed.

Facts confirmed:

- SE kernarg preload again uses `SETranslatingPortProxy`;
- the Full System walker path remains unchanged;
- the narrowed `gfx900` descriptor compatibility guard remains in place;
- the prior hypothesis that the SE preload branch was unnecessary is rejected.

Files changed:

- `src/gpu-compute/gpu_command_processor.cc`
- `tests/pyunit/test_gpu_viper_wb_l2_transitions.py`
- `docs/debug/se-multigpu-status.md`

No build or full simulation was started.

Next diagnostic step: the user-owned gem5 rebuild must compile the restored C++
path before runtime verification or further source cleanup.

### Rebuild After SE Preload Restoration

The user reported that gem5 was rebuilt successfully with no errors. The exact
build command was not provided, so it is not reconstructed here.

The rebuilt binary reports:

```text
gem5 compiled Jun 18 2026 16:29:22
```

Its filesystem timestamp was checked with:

```bash
stat -c '%y %s %n' build/VEGA_X86/gem5.opt
```

Result:

```text
2026-06-18 16:32:40.897806243 +0800 1203684752 build/VEGA_X86/gem5.opt
```

Both lightweight suites were rerun using this rebuilt binary:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit \
  -m unittest test_gpu_viper_wb_l2_transitions
```

Result: all 4 tests passed.

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 12 tests passed.

`git diff --check` also passed for the restored source, its regression test,
and this status document.

Facts confirmed:

- the restored C++ source compiles into `build/VEGA_X86/gem5.opt`;
- both lightweight regression suites pass with the rebuilt binary;
- no full simulation was started by Codex.

Files changed by this step:

- `docs/debug/se-multigpu-status.md`

### Process Alias-Handling Removal Reconsidered

The planned removal of the `Process::deallocateMem()` alias check is withdrawn.
The user confirmed that this change was introduced to fix an earlier bug, and
the current code and runtime logs provide a concrete reason for the behavior:
SE GPU and shared-memory mappings can map one physical page at multiple virtual
addresses, while `MemState::unmapRegion()` eventually calls
`Process::deallocateMem()`.

Inspection commands included:

```bash
rg -n -i -C 12 \
  "deallocateMem|alias|aliased|deallocPhysPage|physical page|process.cc" \
  docs/debug/se-multigpu-status.md m5out tests src/gpu-compute \
  src/sim/process.cc
```

```bash
rg -n \
  "pTable->map\\(|mapRegion\\(|allocateMem\\(|deallocateMem\\(" \
  src/gpu-compute/gpu_compute_driver.cc \
  src/gpu-compute/gpu_render_driver.cc src/sim
```

Relevant facts:

- KFD allocations install explicit physical mappings with
  `process->pTable->map()`;
- normal SE unmap processing reaches `Process::deallocateMem()`;
- freeing one VA alias without checking remaining mappings can return a still
  referenced physical page to the allocator;
- the current alias scan prevents that premature physical-page recycle.

The previous safety concern remains valid but changes the required action:
`zeroPages` currently clears the page before the alias check, so a surviving
alias can retain its mapping while losing its contents. This means the patch
should not be deleted; its last-alias decision should also guard page
zeroing and physical-page deallocation.

Hypothesis rejected: the alias-handling change is unnecessary and should be
removed solely because the focused peer-VRAM success path bypasses teardown.

No source or test was changed during this review.

Next diagnostic step: recover or construct the smallest SE regression with two
virtual mappings to one physical page, verify that unmapping the first mapping
preserves the second mapping and its contents, then move zeroing/deallocation
under the last-alias condition.

### Real SE Page-Alias Regression

A real x86 SE microbenchmark and lightweight AtomicSimpleCPU configuration were
added under:

- `tests/test-progs/se/page-alias/page_alias.c`
- `tests/test-progs/se/page-alias/Makefile`
- `tests/test-progs/se/page-alias/run.py`

The program reserves two fixed anonymous VMAs without touching them and issues
`m5_work_begin(0, 0)`. At that event, the Python configuration uses the existing
`Process.map()` method to map both VMAs to physical page `0x1fff0000`. The
program writes a deterministic pattern, unmaps the first VA, and verifies the
second VA.

The binary was built with:

```bash
make -C tests/test-progs/se/page-alias
```

Result: the static x86 `page_alias` binary and x86 m5ops archive were built
successfully.

The first run exposed an unrelated embedded-Python compatibility error in the
stdlib `Simulator` annotation:

```text
TypeError: unsupported operand type(s) for |: 'type' and 'type'
```

The harness was changed to use the existing low-level
`board._pre_instantiate()`, `m5.instantiate()`, and `m5.simulate()` loop. The
next run reached `workbegin` but found that the stdlib core stores its workload
as a `SimObjectVector`; indexing its first process fixed the harness.

The valid RED command was:

```bash
build/VEGA_X86/gem5.opt \
  -d /tmp/gem5-se-page-alias-red \
  tests/test-progs/se/page-alias/run.py
```

The first relevant behavioral failure was:

```text
surviving alias changed after munmap at byte 0: expected 0xb got 0
```

This confirms that the existing alias scan prevents premature physical-page
recycling but the earlier `zeroPages` write still destroys the contents visible
through a surviving VA alias.

`Process::deallocateMem()` was changed to:

1. save the physical address and unmap the requested VA;
2. scan the remaining process mappings for the same physical page;
3. leave the page and its contents untouched while an alias remains;
4. only for the final alias, zero through `system->physProxy` using the saved
   physical address and then call `deallocPhysPage()`.

Local checks completed:

```bash
python3 -m py_compile tests/test-progs/se/page-alias/run.py
```

```bash
git diff --check -- \
  src/sim/process.cc \
  tests/test-progs/se/page-alias \
  docs/superpowers/specs/2026-06-18-se-page-alias-deallocation-design.md \
  docs/superpowers/plans/2026-06-18-se-page-alias-deallocation.md \
  docs/debug/se-multigpu-status.md
```

Both commands passed.

Files changed:

- `src/sim/process.cc`
- `tests/test-progs/se/page-alias/page_alias.c`
- `tests/test-progs/se/page-alias/Makefile`
- `tests/test-progs/se/page-alias/run.py`
- `docs/superpowers/specs/2026-06-18-se-page-alias-deallocation-design.md`
- `docs/superpowers/plans/2026-06-18-se-page-alias-deallocation.md`
- `docs/debug/se-multigpu-status.md`

No gem5 build or GPU simulation was started by Codex.

Next verification step: the user must rebuild `build/VEGA_X86/gem5.opt` so the
`process.cc` change is present, then rerun the page-alias command for GREEN
verification.

### SE Page-Alias GREEN Verification

The user reported that gem5 rebuilt successfully. The exact build command was
not provided. The rebuilt binary reports:

```text
gem5 compiled Jun 18 2026 16:52:56
```

Its filesystem metadata was checked with:

```bash
stat -c '%y %s %n' build/VEGA_X86/gem5.opt
```

Result:

```text
2026-06-18 16:56:32.707830382 +0800 1203561056 build/VEGA_X86/gem5.opt
```

The real SE regression was rerun:

```bash
build/VEGA_X86/gem5.opt \
  -d /tmp/gem5-se-page-alias-green \
  tests/test-progs/se/page-alias/run.py
```

The relevant result was:

```text
Mapping SE aliases 0x600000000000 and 0x600000001000 to physical page 0x1fff0000
SE page alias preservation passed
Exiting @ tick 213977000 because exiting with last active thread context.
```

The process and gem5 command exited with status 0. This verifies that unmapping
the first VA no longer clears the physical page while the second alias remains.

Both existing lightweight suites were run with the rebuilt binary:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit \
  -m unittest test_gpu_viper_wb_l2_transitions
```

Result: all 4 tests passed.

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 12 tests passed.

Final checks:

```bash
python3 -m py_compile tests/test-progs/se/page-alias/run.py
git diff --check
```

Result: both passed.

Facts confirmed:

- the original alias check is necessary to prevent premature physical-page
  recycling;
- page zeroing must also wait until the final VA alias is gone;
- physical-address zeroing preserves the normal zero-on-reuse behavior;
- the focused multi-GPU regressions remain green.

Files changed by this verification step:

- `docs/debug/se-multigpu-status.md`

No full GPU simulation was started by Codex.

Next review step: classify the corrected `process.cc` alias fix and its real SE
regression as an independent generic change, then continue reviewing the
remaining uncommitted diff boundaries.

### Process Alias Fix Integrated into Formal Tests

The corrected generic `Process::deallocateMem()` alias fix is now paired with a
formal gem5 test:

- `tests/gem5/se_mode/page_alias/test_page_alias.py`
- `tests/test-progs/se/page-alias/page_alias.c`
- `tests/test-progs/se/page-alias/Makefile`
- `tests/test-progs/se/page-alias/run.py`

The test definition uses a focused fixture to run:

```bash
make -C tests/test-progs/se/page-alias page_alias
```

before launching the SE configuration. It verifies the marker:

```text
SE page alias preservation passed
```

The repository's existing `MakeFixture`/`MakeTarget` implementation was first
attempted, but it is incompatible with the current `ext/testlib` API:

```text
AttributeError: 'MakeTarget' object has no attribute 'require'
```

A local fixture with the current `Fixture.setup(testitem)` interface was used
instead. The first formal runner attempt also selected the nonexistent
`build/X86/gem5.opt`; tagging the test as `VEGA_X86` selected the verified
binary in this worktree.

A local `.gitignore` now excludes only generated files:

```text
/libm5op_x86.a
/m5op_x86.o
/page_alias
```

The final formal test command was:

```bash
cd tests
./main.py run --skip-build gem5/se_mode/page_alias
```

Result:

```text
Test: se-page-alias-preservation-VEGA_X86-x86_64-opt Passed
Test: se-page-alias-preservation-VEGA_X86-x86_64-opt-MatchRegex Passed
Results: 2 Passed
```

Existing lightweight suites were rerun:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit \
  -m unittest test_gpu_viper_wb_l2_transitions
```

Result: all 4 tests passed.

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 12 tests passed.

Final checks:

```bash
python3 -m py_compile \
  tests/test-progs/se/page-alias/run.py \
  tests/gem5/se_mode/page_alias/test_page_alias.py
git diff --check
```

Result: both passed.

Files changed:

- `tests/gem5/se_mode/page_alias/test_page_alias.py`
- `tests/test-progs/se/page-alias/.gitignore`
- `docs/superpowers/specs/2026-06-18-se-page-alias-test-integration-design.md`
- `docs/superpowers/plans/2026-06-18-se-page-alias-test-integration.md`
- `docs/debug/se-multigpu-status.md`

No gem5 build or full GPU simulation was started by Codex.

Next review step: treat the `process.cc` fix and page-alias test as a complete
independent generic change, then review the isolated `SimplePt2Pt` link-ID fix
and decide whether its current source-level test should become a direct
topology behavior test.

### Generic Alias and SimplePt2Pt Review

The `process.cc` alias fix and its page-alias regression were reviewed as an
independent generic change. The page table implementation was inspected with:

```bash
rg -n "getMappings" src -g '*'
sed -n '70,120p' src/mem/page_table.cc
sed -n '300,420p' src/sim/process.cc
sed -n '1,190p' src/sim/mem_pool.cc
```

The first relevant result is that `EmulationPageTable::getMappings()` emits one
entry per mapped virtual page, including its physical page address. After
`Process::deallocateMem()` removes one mapping, scanning the remaining entries
therefore detects another alias to the same physical page before zeroing or
returning that page to the default SE memory pool.

Facts confirmed:

- the implementation matches the focused regression's same-process,
  same-page-table alias case;
- zeroing and physical-page recycling are delayed until the final mapping in
  that page table is removed;
- the change is not a cross-process physical-page reference-counting scheme;
- the change should be described and reviewed with that scope rather than as a
  complete solution for aliases shared by independent page tables.

The `SimplePt2Pt` implementation and its source-structure regression were then
reviewed. A direct topology probe loaded the current worktree source instead of
the Python module embedded in the existing `gem5.opt`:

```bash
build/VEGA_X86/gem5.opt -c \
  'import importlib.util; from pathlib import Path; \
from m5.objects import RubyController, RubySystem; \
p=Path("src/python/gem5/prebuilt/viper/viper_network.py"); \
s=importlib.util.spec_from_file_location("viper_network_source", p); \
m=importlib.util.module_from_spec(s); s.loader.exec_module(m); \
n=m.SimplePt2Pt(RubySystem()); \
n.connect([RubyController(), RubyController(), RubyController()]); \
print([int(x.link_id) for x in n.ext_links]); \
print([int(x.link_id) for x in n.int_links]); \
print([(int(x.src_node.router_id), int(x.dst_node.router_id)) \
for x in n.int_links])'
```

The observable result was:

```text
[0, 1, 2]
[3, 4, 5, 6, 7, 8]
[(0, 1), (0, 2), (1, 0), (1, 2), (2, 0), (2, 1)]
```

This confirms that a direct behavior test is practical without rebuilding
gem5. The current test only searches source text for assignment and increment
positions, so it can pass without proving the constructed topology.

Lightweight checks run during the review:

```bash
git diff --check
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_simple_pt2pt_link_ids_are_unique_and_contiguous
```

Result: both passed. No source file, build, or full simulation was changed or
started during this review.

Accepted review conclusion: keep the `process.cc` change and page-alias test as
an independent generic same-page-table fix. Replace the `SimplePt2Pt`
source-structure assertion with a direct topology behavior test that loads the
worktree module, constructs three controllers, and verifies external IDs
`0..2`, internal IDs `3..8`, uniqueness, contiguity, and all six directed
router pairs.

Files changed:

- `docs/debug/se-multigpu-status.md`

Next code step: after test-design approval, replace only
`test_simple_pt2pt_link_ids_are_unique_and_contiguous` with the direct topology
behavior test. Demonstrate RED against the old internal-ID algorithm, restore
the current implementation, then run the focused and complete lightweight
suites.

### Cluster Topology Test Direction Corrected

The topology design was clarified: the SE multi-GPU Ruby network is not a
`SimplePt2Pt` topology. It consists of one CPU cluster and one cluster per GPU.
Every cluster's Ruby controllers connect to one local router. The CPU router
has bidirectional links to every GPU router, and the GPU routers form a
bidirectional XGMI full mesh.

The accepted test direction is therefore a direct behavior test of
`ClusteredXGMINetwork`. It will construct a CPU plus three-GPU topology and
verify controller-to-router ownership, all required directed inter-router
links, PCIe versus XGMI latency and weight, and globally unique contiguous link
IDs.

The generic `SimplePt2Pt` link-ID test is not the next core SE multi-GPU task.
Its implementation change remains an independent generic fix.

Files changed:

- `docs/superpowers/specs/2026-06-18-se-xgmi-cluster-topology-test-design.md`
- `docs/debug/se-multigpu-status.md`

Verification completed:

- design checked against `se_xgmi_network.py` and
  `se_gpu_cache_hierarchy.py`;
- no source implementation, build, or simulation was changed or started.

Next step: user review of the written topology-test specification, followed by
an implementation plan for the direct `ClusteredXGMINetwork` behavior test.

### Cluster Topology Test Plan Prepared

A lightweight probe was used to confirm how the constructed network SimObjects
expose controller ownership, router IDs, link IDs, latency, and weight.

The first command attempted to load the worktree module with
`importlib.util.module_from_spec()` and `exec_module()` but did not register the
module in `sys.modules`. The first relevant error was:

```text
AttributeError: 'NoneType' object has no attribute '__dict__'
```

The stack ended in Python 3.8 `dataclasses.py` while processing
`ClusterIntLinkSpec`. This confirmed that direct worktree loading of a module
containing `@dataclass` must register the module name in `sys.modules` before
execution.

The corrected probe registered the module and constructed a topology with two
CPU controllers and GPU controller counts `[1, 2, 3]`. The observable result
was:

```text
routers [0, 1, 2, 3]
ext [(0, 0, 0), (1, 0, 1), (2, 1, 2), (3, 2, 3), (4, 2, 4),
     (5, 3, 5), (6, 3, 6), (7, 3, 7)]
int [(0, 1, 31, 7, 8), (1, 0, 31, 7, 9),
     (0, 2, 31, 7, 10), (2, 0, 31, 7, 11),
     (0, 3, 31, 7, 12), (3, 0, 31, 7, 13),
     (1, 2, 9, 2, 14), (1, 3, 9, 2, 15),
     (2, 1, 9, 2, 16), (2, 3, 9, 2, 17),
     (3, 1, 9, 2, 18), (3, 2, 9, 2, 19)]
```

Facts confirmed:

- direct inspection can identify each controller's cluster router;
- router, latency, weight, and link-ID parameters convert directly to integers;
- the current implementation matches the accepted CPU plus three-GPU topology;
- the test can load current worktree Python without rebuilding gem5.

Files changed:

- `docs/superpowers/plans/2026-06-18-se-xgmi-cluster-topology-test.md`
- `docs/debug/se-multigpu-status.md`

Verification completed:

- lightweight construction probe only;
- no build, full simulation, or production source modification.

Next step: execute the written plan to add the direct topology behavior test
and run the focused and complete lightweight suites.

## Relevant Uncommitted Areas

The working tree contains substantial pre-existing, uncommitted work in:

- `src/gpu-compute/`
- `src/mem/ruby/protocol/`
- `src/python/gem5/prebuilt/viper/`
- `src/python/gem5/components/devices/gpus/`
- `configs/example/gem5_library/`
- `tests/pyunit/`
- `tests/test-progs/gpu/`

These changes belong to the user. Do not discard or broadly reformat them.

### Direct ClusteredXGMINetwork Behavior Test

The direct topology regression constructs two CPU controllers and three GPU
clusters containing one, two, and three controllers. It loads
`se_xgmi_network.py` from the worktree, calls `ClusteredXGMINetwork.connect()`,
and inspects the resulting SimObjects.

Commands:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest test_se_viper_multigpu.SEViperMultiGPUTest.test_clustered_xgmi_network_constructs_cluster_topology
```

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest test_se_viper_multigpu
```

```bash
git diff --check
```

First relevant result:

```text
Ran 1 test in 0.004s

OK
```

The complete lightweight suite reported:

```text
Ran 13 tests in 0.012s

OK
```

Facts confirmed:

- router 0 owns every CPU controller;
- router `i + 1` owns every controller for GPU `i`;
- CPU and GPU routers have both directed PCIe links;
- distinct GPU routers form a complete directed XGMI graph;
- PCIe and XGMI links retain their configured latency and weight;
- external and internal link IDs are globally unique and contiguous.

Accepted hypothesis: the constructed `ClusteredXGMINetwork` matches the
established CPU-cluster plus per-GPU-cluster topology. No topology hypothesis
was rejected by these passing checks.

Files changed:

- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- `docs/debug/se-multigpu-status.md`

Verification completed:

- focused direct topology test: 1 test passed;
- complete lightweight SE multi-GPU Python suite: 13 tests passed;
- `git diff --check`: exit status 0 with no output.

Next diagnostic step: 该既定 cluster topology 无后续诊断；在增加新的端到端
workload 前选择下一项 protocol behavior validation。

### Next Protocol Diagnostic Selected

The next experiment is an independent `xgmi-peer-invalidate` diagnostic. It
will not modify the established `xgmi-peer-vram` baseline.

The accepted host-sequenced experiment is:

1. GPU1 writes value A to a target in GPU1 VRAM.
2. GPU0 reads the target and records A, establishing the remote read/cache
   state.
3. GPU1 writes value B to the same target.
4. GPU0 reads again and records whether it observes B, stale A, or another
   value.

Each HSA dispatch completes before the next starts, so GPU-to-GPU signaling is
not part of the experiment. The program will directly read mapped result
buffers from the CPU, print and flush the classification, skip unsupported ROCm
teardown, and terminate with `m5_exit`.

Stale A is an expected diagnostic result because inter-GPU cache coherence is
not assumed to be implemented. It must be reported distinctly from both
coherence success and malformed/unexpected data.

Files changed:

- `docs/superpowers/specs/2026-06-18-xgmi-peer-invalidate-design.md`
- `docs/debug/se-multigpu-status.md`

Verification completed:

- design checked against the existing HSA dispatch, direct CPU result-read,
  ROCm teardown avoidance, and x86 `m5_exit` patterns in
  `xgmi-peer-vram`;
- no test-program source, build, or simulation was changed or started.

Next step: user review of the written `xgmi-peer-invalidate` design, followed
by an implementation plan.

### Peer Invalidation Experiment Validity Correction

Read-only inspection found that `SEVegaGPU` sets
`impl_kern_launch_acq = True`. `GPUDispatcher::exec()` consequently calls
`Shader::prepareInvalidate()` before every kernel dispatch, and the compute
units issue L1/L2 invalidation traffic before launching the kernel.

This means the original four-kernel experiment would be inconclusive: GPU0's
second read could observe B because its next kernel launch invalidated GPU0's
cache, even if GPU1 never generated a cross-GPU coherence invalidation.

Accepted correction: add an opt-in
`--disable-gpu0-kernel-launch-acquire` configuration argument. The diagnostic
uses it to set only `gpus[0].impl_kern_launch_acq = False`; default behavior and
GPU1 launch acquire remain unchanged.

Files changed:

- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- `docs/superpowers/specs/2026-06-18-xgmi-peer-invalidate-design.md`
- `docs/superpowers/plans/2026-06-18-xgmi-peer-invalidate.md`
- `docs/debug/se-multigpu-status.md`

Verification completed:

- traced kernel launch through `GPUDispatcher::exec()`,
  `Shader::prepareInvalidate()`, and `ComputeUnit` invalidation requests;
- no build or full simulation was started.

Next step: verify the strengthened regression fails without the diagnostic
configuration option, then implement the opt-in GPU0 setting.

### Diagnostic Exit-Code Semantics Corrected

Repository inspection confirmed:

```text
void m5_exit(uint64_t ns_delay);
void m5_fail(uint64_t ns_delay, uint64_t code);
```

Therefore `m5_exit(1)` and `m5_exit(2)` would specify delayed exits rather than
classification codes. The diagnostic contract is corrected to use
`m5_exit(0)` for observed invalidation, `m5_fail(0, 1)` for stale data, and
`m5_fail(0, 2)` for setup or unexpected values.

Next step: strengthen the source regression for `m5_fail`, verify RED against
the current host source, then correct the terminal pseudo instructions.

The configuration event loop must also treat
`m5_fail instruction encountered` as terminal. Otherwise it reports an unknown
event and resumes simulation after the diagnostic has already classified a
stale or malformed result.

Next step: add this event-loop requirement to the regression, verify RED, and
add the terminal cause beside the existing `m5_exit` handling.

### XGMI Peer Invalidation Diagnostic Implemented

An independent diagnostic was added under:

- `tests/test-progs/gpu/xgmi-peer-invalidate/invalidate_hip.cpp`
- `tests/test-progs/gpu/xgmi-peer-invalidate/invalidate_kernels.hip`
- `tests/test-progs/gpu/xgmi-peer-invalidate/Makefile`

The host serializes:

1. GPU1 writes A to a GPU1-VRAM target.
2. GPU0 reads and records A.
3. GPU1 writes B to the same target.
4. GPU0 reads again and classifies B, stale A, or an unexpected value.

The configuration now provides the opt-in
`--disable-gpu0-kernel-launch-acquire` flag. The diagnostic requires this flag
so GPU0's implicit per-kernel cache invalidation cannot create a false
coherence success. The default configuration remains unchanged.

Terminal results are:

- `invalidation observed`: `m5_exit(0)`;
- `stale cache line observed`: `m5_fail(0, 1)`;
- setup or unexpected value: `m5_fail(0, 2)`.

The simulation event loop now treats `m5_fail instruction encountered` as a
terminal cause and propagates its event code.

Commands run:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_peer_invalidate_diagnostic_has_host_sequenced_phases
```

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

```bash
git diff --check
```

```bash
make -n -C tests/test-progs/gpu/xgmi-peer-invalidate \
  ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  HIPCC=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/bin/hipcc
```

First relevant final result:

```text
Ran 1 test in 0.001s

OK
```

The final complete lightweight suite reported:

```text
Ran 14 tests in 0.013s

OK
```

Facts confirmed:

- the source-level regression first failed because the independent diagnostic
  did not exist, then passed after implementation;
- additional RED/GREEN checks caught and corrected implicit GPU0 launch
  invalidation and incorrect `m5_exit` failure-code assumptions;
- the host phases are serialized by HSA completion waits;
- GPU0 launch acquire is disabled only when explicitly requested;
- stale and malformed results terminate with distinct `m5_fail` codes;
- no ROCm teardown is performed after classification;
- `make -n` expands the expected gfx900 HSACO, x86 m5op archive, and host link
  commands without writing build outputs;
- the existing `xgmi-peer-vram` baseline remains separate.

Files changed:

- `configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- `tests/test-progs/gpu/xgmi-peer-invalidate/Makefile`
- `tests/test-progs/gpu/xgmi-peer-invalidate/invalidate_hip.cpp`
- `tests/test-progs/gpu/xgmi-peer-invalidate/invalidate_kernels.hip`
- `docs/superpowers/specs/2026-06-18-xgmi-peer-invalidate-design.md`
- `docs/superpowers/plans/2026-06-18-xgmi-peer-invalidate.md`
- `docs/debug/se-multigpu-status.md`

Verification completed:

- focused source regression: 1 test passed;
- complete lightweight SE multi-GPU suite: 14 tests passed;
- `git diff --check`: exit status 0;
- Makefile dry run only.

The diagnostic binary was not compiled and no full simulation was launched.

User-owned build command:

```bash
make -C tests/test-progs/gpu/xgmi-peer-invalidate \
  ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  HIPCC=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/bin/hipcc
```

After a successful build, the full simulation command is:

```bash
build/VEGA_X86/gem5.opt \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type timing \
  --disable-gpu0-kernel-launch-acquire \
  --app tests/test-progs/gpu/xgmi-peer-invalidate/invalidate_hip \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1
```

Next diagnostic step: the user builds the new diagnostic binary. Do not select
Ruby trace flags or start the full simulation until that build succeeds.

### XGMI Peer Invalidation Binary Built

The user-owned build completed successfully. The displayed kernel command
appeared to contain `-mno-code-object-v3-O2`, so the generated Makefile and
artifacts were inspected before simulation.

The Makefile actually contains separate arguments:

```make
$(HIPCC) $(HIPCCFLAGS) $(HSACO_HIPCCFLAGS) -O2 --genco ...
```

Artifact inspection confirmed:

```text
invalidate_kernels.hsaco: ELF 64-bit LSB shared object, AMDGPU architecture
invalidate_hip: x86-64 ELF executable
```

`nm` confirmed both required pseudo-operation symbols:

```text
m5_exit
m5_fail
```

`strings` confirmed all four phase markers and all result classifications.
Therefore the apparent option concatenation was a copied/displayed command
artifact rather than the current Makefile contents, and no rebuild is required.

Files changed during inspection:

- `docs/debug/se-multigpu-status.md`

Verification completed:

- build artifacts exist with current timestamps;
- HSACO and host binary formats are valid;
- host binary defines both `m5_exit` and `m5_fail`;
- diagnostic phase and result strings are present.

Next diagnostic step: run the first full simulation without broad Ruby debug
flags and collect `m5out/simout.txt` plus `m5out/simerr.txt`. Add bounded Ruby
tracing only if the run reaches a protocol failure, stale result, or other
ambiguous classification.

### First Invalidation Run Blocked by clock_nanosleep

The first user-owned simulation did not reach any diagnostic kernel phase. It
stopped during:

```text
[peer_invalidate] begin hsa_executable_freeze(executable, nullptr)
```

The first fatal result was:

```text
src/sim/syscall_emul.cc:79: fatal:
syscall clock_nanosleep (#230) unimplemented.
```

The call occurred in a ROCm helper thread while freezing the executable, before
`phase 1: GPU1 write A`. Therefore this run provides no GPU coherence result.

Root-cause inspection found that the x86-64 syscall table maps
`nanosleep(35)` to `ignoreWarnOnceFunc`, while `clock_nanosleep(230)` had no
handler and therefore caused the fatal.

The focused regression first failed against the missing handler. The x86-64
table now applies the existing SE sleep policy:

```cpp
{230, "clock_nanosleep", ignoreWarnOnceFunc},
```

Commands:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_x86_clock_nanosleep_matches_se_nanosleep_policy
```

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

```bash
git diff --check
```

Results:

```text
Ran 1 test in 0.000s
OK

Ran 15 tests in 0.013s
OK
```

Facts confirmed:

- the failure is an SE syscall-emulation blocker, not a Ruby or coherence
  result;
- the new behavior matches the existing x86-64 `nanosleep` policy;
- no GPU, Ruby protocol, or diagnostic-program source change was required.

Files changed:

- `src/arch/x86/linux/syscall_tbl64.cc`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- `docs/debug/se-multigpu-status.md`

Verification completed:

- focused source regression passed;
- complete lightweight suite passed: 15 tests;
- `git diff --check` passed.

Next diagnostic step: the user rebuilds `build/VEGA_X86/gem5.opt`, then reruns
the same invalidation simulation command without broad Ruby debug flags.
