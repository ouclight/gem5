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

### Invalidation Rerun Reaches Ignored clock_nanosleep

After rebuilding gem5, the rerun passed the previous fatal point. The first
relevant changed result was:

```text
src/sim/syscall_emul.cc:97:
warn: ignoring syscall clock_nanosleep(...)
```

The output still stopped inside:

```text
[peer_invalidate] begin hsa_executable_freeze(executable, nullptr)
```

No `phase 1: GPU1 write A`, Ruby failure, result classification, or gem5 exit
cause was observed. The process was no longer running when inspected, and
`m5out-invalidate/stats.txt` was empty, so this capture cannot establish
whether the runtime was permanently deadlocked or manually stopped while
making slow progress.

The hypothesis that the one-line ignore handler is sufficient is not yet
accepted. Ignoring a sleep used as a ROCm helper-thread polling backoff may
alter thread scheduling under TimingSimpleCPU.

No source file was changed during this result inspection.

Next diagnostic step: run the already validated `xgmi-peer-vram` binary with
the same rebuilt `gem5.opt` and normal kernel-launch acquire settings. Compare
whether it completes `hsa_executable_freeze`. Do not add Ruby tracing yet,
because the run has not reached a GPU request phase.

### Peer-VRAM Control Run Passed

The user reran the established `xgmi-peer-vram` workload with the same rebuilt
`gem5.opt`, and it completed normally. This rejects the hypothesis that the
new `clock_nanosleep` ignore handler generally prevents ROCm executable freeze
or kernel execution.

Read-only HSACO comparison confirmed that both code objects are valid gfx900
AMDGPU ELF files using metadata version `[1, 0]`. Their sizes and ABI structure
are nearly identical. The main structural difference is:

- passing peer-VRAM HSACO: two kernel symbols;
- blocked invalidation HSACO: three kernel symbols (`write_value`,
  `read_value`, and `classify_value`).

The ROCm 4 `llvm-objdump` binary cannot disassemble either gfx900 object and
aborts with `Disassembly not yet supported for subtarget`; this tool limitation
does not distinguish the two HSACOs. `llvm-readelf` successfully read both
objects and found no malformed metadata.

Accepted facts:

- the block remains before any GPU kernel dispatch or Ruby request;
- it is specific to the new code object or its loader path;
- broad Ruby tracing would not provide useful evidence yet.

Next diagnostic step: reduce the invalidation HSACO from three kernel symbols
to two by replacing `read_value` and `classify_value` with one parameterized
`observe_value` kernel. Preserve the four host-sequenced phases and result
classification, rebuild only the diagnostic binary, and rerun without Ruby
debug flags.

### Project Priority Shift: ROCm HIP API Compatibility

The immediate project priority has changed from the peer-invalidation
microbenchmark to reusable ROCm API support for diverse applications on the
SE multi-GPU platform.

The first milestone is a single-GPU, public-HIP-only smoke program with
cumulative stages:

```text
malloc -> memset -> memcpy -> launch -> lifecycle
```

The program must not use raw HSA dispatch, direct mapped-device reads, or m5
pseudo-operation exits. Every API has begin/end markers and every data
operation has host-side validation.

Acceptance is split into:

- API completion: the target API returns and its data validation passes;
- lifecycle completion: `hipFree` returns and the program exits normally.

This separation prevents an SE `exit_group` issue from being mistaken for a
HIP API failure.

Files changed:

- `docs/superpowers/specs/2026-06-20-se-rocm-hip-api-smoke-design.md`
- `docs/debug/se-multigpu-status.md`

Verification completed:

- design checked against the existing peer-VRAM workarounds and known
  `hipMemcpy`, `hipFree`, ROCm teardown, and process-exit blockers;
- no test program, gem5 source, build, or simulation was changed or started.

Next step: user review of the HIP API smoke-test specification, followed by an
implementation plan.

### HIP API Smoke Implementation Plan Prepared

The approved design is now decomposed into:

1. a failing source contract;
2. one public-HIP `.hip` program with five cumulative stages;
3. a direct ROCm 4 gfx900 Makefile without HSA or m5ops;
4. focused and complete lightweight verification;
5. a user-owned build and ordered runtime matrix.

The later stages strictly rerun earlier API validations before adding the next
layer. Non-lifecycle stages intentionally do not call `hipFree`, so allocation,
data operations, and launch completion can be distinguished from teardown.

Files changed:

- `docs/superpowers/plans/2026-06-20-se-rocm-hip-api-smoke.md`
- `docs/debug/se-multigpu-status.md`

Verification completed:

- plan checked against every design requirement;
- no placeholders or unresolved implementation decisions remain;
- no test source, gem5 source, build, or simulation was changed or started.

Next step: execute the written plan, beginning with the focused RED source
regression.

### Public HIP API Smoke Program Implemented

The single-GPU public-HIP compatibility program was added under:

- `tests/test-progs/gpu/hip-api-smoke/hip_api_smoke.hip`
- `tests/test-progs/gpu/hip-api-smoke/Makefile`

It accepts cumulative stages:

```text
malloc
memset
memcpy
launch
lifecycle
```

The program uses normal HIP APIs and `<<<...>>>` kernel launch syntax. It does
not include raw HSA APIs, direct mapped-device reads, or gem5 pseudo
instructions. Every HIP call emits begin/end markers, and every data-producing
stage validates all 256 output words.

The focused source contract first failed with:

```text
FileNotFoundError:
tests/test-progs/gpu/hip-api-smoke/hip_api_smoke.hip
```

After implementation, commands run were:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_hip_api_smoke_uses_public_hip_cumulative_stages
```

```bash
make -n -C tests/test-progs/gpu/hip-api-smoke \
  ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  HIPCC=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/bin/hipcc
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

Ran 16 tests in 0.014s
OK
```

The Makefile dry run expanded to one direct command with separate
`-mno-code-object-v3`, `-O2`, and `--offload-arch=gfx900` arguments. It did not
link HSA runtime libraries or m5ops.

Facts confirmed:

- stages are cumulative and ordered;
- non-lifecycle stages intentionally omit `hipFree`;
- the lifecycle stage runs the launch workload, then calls `hipFree`, prints
  `LIFECYCLE_PASSED`, and returns zero;
- memset, round-trip copy, and kernel transformation have full host-side data
  validation;
- no build artifacts were generated by Codex.

Files changed:

- `tests/test-progs/gpu/hip-api-smoke/hip_api_smoke.hip`
- `tests/test-progs/gpu/hip-api-smoke/Makefile`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- `docs/superpowers/specs/2026-06-20-se-rocm-hip-api-smoke-design.md`
- `docs/superpowers/plans/2026-06-20-se-rocm-hip-api-smoke.md`
- `docs/debug/se-multigpu-status.md`

Verification completed:

- focused source contract passed;
- complete lightweight SE multi-GPU suite passed: 16 tests;
- `git diff --check` passed;
- Makefile dry run passed;
- HIP binary was not built and no simulation was started.

User-owned build command:

```bash
make -C tests/test-progs/gpu/hip-api-smoke \
  ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  HIPCC=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/bin/hipcc
```

Ordered runtime matrix:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hip-malloc \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type timing \
  --app tests/test-progs/gpu/hip-api-smoke/hip_api_smoke \
  --opts="--stage malloc" \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1
```

After `API_STAGE_PASSED stage=malloc`, repeat with the stage and output
directory changed in order to:

```text
memset
memcpy
launch
lifecycle
```

Do not pass `--disable-gpu0-kernel-launch-acquire`; this matrix tests normal
HIP runtime behavior. Stop at the first stage without its expected pass marker.

Next diagnostic step: the user builds `hip_api_smoke`, then runs only the
`malloc` stage.

### HIP API Smoke Binary Built

The user-owned `hip_api_smoke` build completed successfully. Read-only artifact
inspection confirmed:

- the binary timestamp is newer than `hip_api_smoke.hip`;
- it is a valid x86-64 ELF executable;
- it contains all five stage names and both pass markers;
- it contains the registered `transform_kernel` device symbol and host stub;
- it does not define gem5 m5ops or raw HSA symbols.

No source file was changed during artifact inspection.

Next diagnostic step: run only `--stage malloc` with normal kernel-launch
acquire settings and no broad Ruby debug flags. Do not run later stages until
`API_STAGE_PASSED stage=malloc` is observed.

### Public HIP malloc Stage Passed

The user-owned `malloc` stage completed successfully.

The command was:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hip-malloc \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type timing \
  --app tests/test-progs/gpu/hip-api-smoke/hip_api_smoke \
  --opts="--stage malloc" \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1
```

The first relevant final result was:

```text
[hip_api_smoke] begin hipMalloc(device_data, bytes)
[hsaKmtAllocMemory] node 1
[hsaKmtMapMemoryToGPUNodes] address 0x7ffdee800000 number of nodes 1
[hip_api_smoke] end hipMalloc(device_data, bytes)
API_STAGE_PASSED stage=malloc
Exiting @ tick 45711472770 because exiting with last active thread context.
```

Facts confirmed:

- public HIP runtime initialization completed with two enumerated GPU nodes;
- `hipSetDevice(0)` completed;
- `hipMalloc` allocated and mapped memory on KFD node 1;
- the API pass marker was printed;
- the process returned normally without `m5_exit`, ROCm teardown stalls, or a
  `TimingSimpleCPU::suspendContext()` assertion.

The warnings for `AMDKFD_IOC_SET_SCRATCH_BACKING_VA`,
`AMDKFD_IOC_SET_TRAP_HANDLER`, and `AMDKFD_IOC_GET_TILE_CONFIG` did not block
this stage. They remain compatibility gaps to revisit only if a later API
requires their semantics.

No source file was changed during result inspection.

Next diagnostic step: run only the cumulative `memset` stage without broad Ruby
debug flags. Stop at the first unmatched HIP begin marker or data-validation
failure.

### Public HIP memset Stage Blocked by FRNDINT

The cumulative `memset` stage reached:

```text
[hip_api_smoke] begin hipMemset(device_data, memset_byte, bytes)
```

but did not print the matching `end` marker. The first relevant unsupported
operation was:

```text
build/VEGA_X86/arch/x86/generated/exec-ns.cc.inc:
warn: instruction 'frndint' unimplemented
```

No `hipDeviceSynchronize`, D2H copy, data validation, GPU kernel completion, or
Ruby protocol failure was reached. Therefore the current blocker is in the
ROCm CPU-side memset/runtime preparation path, before the public HIP call
returns.

Root-cause inspection found:

- x86 decoding already routes opcode `FRNDINT`;
- `src/arch/x86/isa/insts/x87/arithmetic/round.py` contained only an empty
  comment placeholder;
- generated gem5 code consequently used `WarnUnimplemented`, effectively
  treating the required x87 operation as a no-op.

A `roundfp` micro-op and `FRNDINT` macroop were added. The implementation reads
x87 FCW bits `[11:10]` and applies:

- nearest-even with `std::nearbyint`;
- down with `std::floor`;
- up with `std::ceil`;
- toward zero with `std::trunc`.

The focused regression first failed against the empty placeholder, then passed
after implementation.

Commands:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_x86_frndint_uses_fcw_rounding_mode
```

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

```bash
python3 -m py_compile \
  src/arch/x86/isa/insts/x87/arithmetic/round.py
```

```bash
git diff --check
```

Results:

```text
Ran 1 test in 0.000s
OK

Ran 17 tests in 0.014s
OK
```

Files changed:

- `src/arch/x86/isa/microops/fpop.isa`
- `src/arch/x86/isa/insts/x87/arithmetic/round.py`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- `docs/debug/se-multigpu-status.md`

Verification completed:

- focused source regression passed;
- complete lightweight suite passed: 17 tests;
- Python syntax and `git diff --check` passed;
- gem5 was not rebuilt and the instruction has not yet been executed.

Next diagnostic step: the user rebuilds `build/VEGA_X86/gem5.opt`, then reruns
the same `--stage memset` command without Ruby debug flags.

### FRNDINT Decoder Wiring Corrected

After the first FRNDINT implementation build, the `memset` rerun showed no
behavior change and still printed:

```text
warn: instruction 'frndint' unimplemented
```

Generated-source inspection showed that `Roundfp` classes were present, but
the decoder still returned `WarnUnimplemented("frndint", ...)`.

The x87 decoder is enclosed in `format WarnUnimpl`; implemented instructions
must explicitly use the `Inst::` format. FRNDINT still used:

```text
0x4: frndint();
```

and therefore never selected the generated macroop. The decoder is corrected
to:

```text
0x4: Inst::FRNDINT();
```

The strengthened regression first failed against the missing decoder
connection and then passed.

Files changed:

- `src/arch/x86/isa/decoder/x87.isa`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- `docs/debug/se-multigpu-status.md`

Verification completed:

- focused FRNDINT source regression passed;
- complete lightweight suite passed: 17 tests;
- Python syntax and `git diff --check` passed;
- the current `gem5.opt` still contains the old generated decoder until the
  next rebuild.

Next diagnostic step: the user rebuilds `build/VEGA_X86/gem5.opt` again, checks
that generated decode code no longer contains
`WarnUnimplemented("frndint", ...)`, and reruns `--stage memset`.

### FRNDINT Executes but HIP memset Still Does Not Return

After rebuilding with the decoder correction, generated code now contains:

```text
return new x86_macroop::FRNDINT(...)
```

and no longer contains `WarnUnimplemented("frndint", ...)`. The rerun also
contains no `frndint unimplemented` warning, confirming that the instruction
implementation is active.

The run still stopped after:

```text
[hip_api_smoke] begin hipMemset(device_data, memset_byte, bytes)
```

without reaching the matching end marker. The remaining output consists mainly
of ignored `mprotect` calls, with one `MOVNTDQ` non-temporal-hint warning. No
`hipDeviceSynchronize`, D2H copy, data validation, GPU completion event, Ruby
failure, or exit cause was observed.

Accepted conclusions:

- FRNDINT was a real missing instruction, but it was not sufficient to make
  `hipMemset` complete;
- the first blocked public API remains `hipMemset`;
- repeated `mprotect` warnings alone do not prove that ignored protection
  semantics are the cause, because ignored `mprotect` also occurs in passing
  ROCm paths;
- this Timing CPU run may be executing a very slow ROCm CPU-side internal
  kernel/runtime initialization path.

No source file was changed during this result inspection.

Next diagnostic step: run the same `--stage memset` workload with
`--cpu-type kvm` in a new output directory. If KVM passes, classify the blocker
as Timing CPU runtime compatibility/performance before changing KFD or GPU
logic. If KVM also fails at the same marker, investigate ROCm runtime/KFD
semantics next. Do not modify `mprotect` based only on warning frequency.

### HIP memset Has Not Reached GPU Memory Execution

The interrupted `memset` run was inspected using:

```bash
wc -l m5out-hip-memset/stdout.txt m5out-hip-memset/stderr.txt
tail -n 120 m5out-hip-memset/stdout.txt
tail -n 160 m5out-hip-memset/stderr.txt
```

```bash
rg -n \
  '^board\.gpus0\..*(num.*(Inst|Wg|Wave|Kernel|Dispatch)|executed|completed)' \
  m5out-hip-memset/stats.txt
```

```bash
rg -n -i \
  'sdma|gpu.*(kernel|dispatch|queue|request|response|read|write)|simTicks|simInsts' \
  m5out-hip-malloc/stats.txt m5out-hip-memset/stats.txt
```

```bash
strings \
  .deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/lib/libamdhip64.so.4.0.40001 \
  | rg -i 'memset|fill|blit|HSA_ENABLE_SDMA'
```

The first relevant observable result is:

```text
Exiting @ tick 78056531667 because user interrupt received.
```

The run retired 69,256,091 CPU instructions before interruption, but all GPU
execution and memory-operation counters remained zero:

```text
board.gpus0.CUs0.numInstrExecuted 0
board.gpus0.CUs0.completedWGs 0
board.gpus0.CUs0.globalMemInsts 0
board.gpus0.CUs0.vectorMemWrites 0
```

The same zero result applies to every CU of both GPUs. GPU VRAM controller read
and write bursts are also zero.

The SE environment explicitly sets:

```text
HSA_ENABLE_SDMA=0
```

ROCm library inspection shows the non-SDMA fallback components
`KernelBlitManager` and internal kernel `__amd_rocclr_fillBuffer`. Therefore
this `hipMemset` is expected to create and dispatch an internal fill kernel,
not issue an SDMA constant-fill packet.

Facts confirmed:

- no fill-kernel instruction or GPU memory write executed before interruption;
- this is not currently evidence of a GPU memory request being sent and then
  losing its Ruby or VRAM response;
- the remaining boundary is before CU execution: either ROCm is still creating
  the internal blit kernel, or it has submitted AQL work that the doorbell/HSA
  packet processor has not consumed;
- repeated CPU-side `mprotect` calls and the substantial CPU instruction count
  are consistent with internal blit-kernel creation, but do not yet prove that
  this is the exact stopping point.

No source file was changed during this inspection.

Next diagnostic step: rerun the Timing CPU `memset` stage with only
`GPUDriver,HSAPacketProcessor,GPUCommandProc,GPUDisp,AMDGPUMem` tracing enabled
from tick 45,000,000,000. Do not enable broad Ruby tracing. The last observed
boundary will distinguish internal-kernel creation from an unconsumed AQL
packet or a dispatched kernel waiting on memory.

### Focused memset Trace Ends Before AQL Submission

The focused trace run used:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hip-memset-trace \
  --debug-start=45000000000 \
  --debug-flags=GPUDriver,HSAPacketProcessor,GPUCommandProc,GPUDisp,AMDGPUMem \
  --debug-file=hip-memset-gpu.trace \
  --stdout-file=stdout.txt \
  --stderr-file=stderr.txt \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type timing \
  --app tests/test-progs/gpu/hip-api-smoke/hip_api_smoke \
  --opts="--stage memset" \
  --rocm-path \
  /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1
```

Online inspection commands:

```bash
tail -n 80 m5out-hip-memset-trace/stdout.txt
tail -n 120 m5out-hip-memset-trace/stderr.txt
tail -n 160 m5out-hip-memset-trace/hip-memset-gpu.trace
```

```bash
rg -n \
  'submitting kernel dispatch pkt|launching kernel|Kernel.*completed|doorbell|AQL processing|Received Response' \
  m5out-hip-memset-trace/hip-memset-gpu.trace
```

```bash
test -d /proc/727225
pgrep -a gem5.opt
```

The trace contains only 11 lines. Its last operation is:

```text
45374128119: ... ioctl: AMDKFD_IOC_MAP_MEMORY_TO_GPU
45374128119: ... map target gpu_id 22124
45374128119: ... map target gpu_id 22125
```

There is no compute-queue creation, doorbell activity, AQL processing, kernel
dispatch, GPU command-processor activity, GPU memory request, or response.

An initial process check incorrectly concluded that PID 727225 had exited.
That check ran inside Codex's isolated PID namespace and cannot observe the
user's host-side gem5 process. A subsequent five-second file-size check showed:

```text
stderr.txt: 29100 -> 29295 bytes
hip-memset-gpu.trace: 1320 -> 1320 bytes
```

Therefore the simulation was still running and retiring CPU-side work.
`stderr.txt` continued to add ignored `mprotect` warnings, while the focused
GPU trace did not add any new KFD queue, AQL, dispatch, or memory activity.

Facts confirmed:

- the current failure remains before AQL/GPU submission;
- the hypothesis of a GPU or Ruby memory request waiting for its response is
  rejected for this capture;
- the observed CPU-side work is in ROCm internal blit-kernel preparation and
  memory setup;
- the focused GPU trace cannot identify the exact CPU-side function where
  progress is being spent;
- host-process liveness must be inferred from user observation or output-file
  growth, not from process inspection inside Codex's PID namespace.

No source file was changed during inspection.

Next diagnostic step: let the current run continue. Periodically compare the
sizes of `stderr.txt` and `hip-memset-gpu.trace`. Stop only when the GPU trace
records compute-queue/AQL activity, `hipMemset` returns, or CPU-side output
ceases to advance for a sustained interval.

### Long memset Run Confirms CPU-Side Activity Without GPU Submission

The focused run was stopped normally with Ctrl-C. Post-run inspection used:

```bash
rg -n '^(simTicks|simInsts|hostSeconds|hostInstRate|hostTickRate)' \
  m5out-hip-memset-trace/stats.txt
```

```bash
rg -n \
  '^board\.gpus[01]\.CUs[0-3]\.(numInstrExecuted|completedWGs|globalMemInsts|vectorMemWrites)' \
  m5out-hip-memset-trace/stats.txt
```

```bash
rg -n \
  '^board\.gpu_memories[01]\.mem_ctrl\.(bytesReadSys|bytesWrittenSys|dram\.(readBursts|writeBursts))' \
  m5out-hip-memset-trace/stats.txt
```

The exit and aggregate result were:

```text
Exiting @ tick 125420884569 because user interrupt received.
hostSeconds 588.91
simInsts 146562427
```

All GPU CU execution, workgroup completion, global-memory instructions, GPU
VRAM bytes, and DRAM burst counters remained zero on both GPUs.

Facts confirmed:

- gem5 made substantial CPU-side progress for almost ten host minutes;
- the recurring `mprotect` warnings are associated with continuing CPU work,
  not a blocked GPU memory response;
- no ROCm fill kernel was submitted or executed during 146 million simulated
  CPU instructions;
- waiting longer without CPU execution tracing is unlikely to add a new
  diagnostic boundary.

No source file was changed during result inspection.

Next diagnostic step: rerun with a bounded `ExecEnable,ExecUser,ExecSymbol,
ExecThread` sample from tick 120,000,000,000 through 120,010,000,000. Keep the
existing focused GPU flags. Use a separate output directory and compressed
debug file. The sample will identify the CPU symbol or address consuming time
without generating an unbounded instruction trace.

### First Bounded Exec Sample Stopped Before Its Window

Post-run inspection command:

```bash
ls -lh m5out-hip-memset-exec
rg -n '^(simTicks|simInsts|hostSeconds|hostInstRate)' \
  m5out-hip-memset-exec/stats.txt
gzip -cd m5out-hip-memset-exec/hip-memset-exec.trace.gz
```

The run ended with:

```text
Exiting @ tick 68665889709 because user interrupt received.
```

The requested debug window began at tick 120,000,000,000, so it was never
reached. `hip-memset-exec.trace.gz` is a 20-byte empty gzip stream and contains
no execution samples. This run does not provide new CPU-symbol evidence.

No source file was changed during inspection.

Next diagnostic step: repeat the bounded execution sample using tick
60,000,000,000 through 60,010,000,000, which is below the previously observed
termination point. Stop after the debug file has grown and then remained
unchanged beyond the debug-end tick.

### SE-Mode SDMA Feasibility Inspection

Read-only inspection commands:

```bash
rg -n \
  'SDMA|sdma|HSA_ENABLE_SDMA|CREATE_QUEUE|KFD_IOC_QUEUE_TYPE' \
  src/python/gem5/prebuilt/viper/se_board.py \
  src/python/gem5/components/devices/gpus/se_viper_gpu.py \
  src/gpu-compute/gpu_compute_driver.cc \
  src/dev/hsa src/dev/amdgpu
```

```bash
sed -n '190,255p' src/gpu-compute/gpu_compute_driver.cc
sed -n '145,240p' \
  src/python/gem5/components/devices/gpus/se_viper_gpu.py
sed -n '340,575p' src/dev/amdgpu/sdma_engine.cc
```

Facts confirmed:

- gem5 already has an `SDMAEngine` model supporting important Vega packets,
  including linear copy, write, fence, trap, atomic, and constant fill;
- the current SE board explicitly exports `HSA_ENABLE_SDMA=0`;
- `SEVegaGPU` instantiates only the HSA packet processor, GPU command
  processor, dispatcher, and compute units; it does not instantiate an
  `AMDGPUDevice`, `SDMAEngine`, PM4 processor, SDMA walker, or SDMA doorbell;
- `GPUComputeDriver::allocateQueue()` logs `queue_type` but does not branch on
  it. Every `AMDKFD_IOC_CREATE_QUEUE` is currently registered as an HSA compute
  queue through `HSAPacketProcessor::setDeviceQueueDesc()`;
- therefore changing only `HSA_ENABLE_SDMA=1` would expose a runtime path for
  which the SE model has no correct queue ownership or doorbell routing.

Accepted conclusion:

- SDMA in SE mode is implementable by reusing packet execution logic from
  `SDMAEngine`, but requires explicit SE integration rather than a
  configuration switch.

A minimum useful implementation would require:

1. per-GPU SE SDMA engine instances and DMA/TLB connections;
2. queue-type dispatch for `KFD_IOC_QUEUE_TYPE_SDMA` and
   `KFD_IOC_QUEUE_TYPE_SDMA_XGMI`;
3. SDMA queue descriptor and doorbell registration separate from the HSA
   compute packet processor;
4. SE-compatible virtual-address translation for host memory, local VRAM, and
   peer VRAM;
5. completion/fence signaling and queue destruction;
6. focused packet tests for constant fill and linear copy before enabling
   `HSA_ENABLE_SDMA=1`.

No source file was changed during this inspection.

The current HIP memset diagnosis remains independent: first complete the
bounded CPU execution sample at ticks 60,000,000,000 through 60,010,000,000.
SDMA support should be treated as a separate platform feature, not assumed to
be the fix for the current CPU-side internal-kernel preparation issue.

### 120B Exec Window Reached but Macro/Micro Trace Was Incomplete

The reused `m5out-hip-memset-exec` directory now contains a later run that
reached the requested trace window. Inspection commands:

```bash
nl -ba m5out-hip-memset-exec/hip-memset-exec.trace
rg -n '^(simTicks|simInsts|hostSeconds|hostInstRate)' \
  m5out-hip-memset-exec/stats.txt
```

```bash
rg -n \
  '^board\.processor\.cores[0-9]+\.core\.(commitStats0\.numInsts|numCycles)' \
  m5out-hip-memset-exec/stats.txt
```

```bash
rg -n \
  '^board\.gpus[01]\.CUs[0-3]\.(numInstrExecuted|completedWGs|globalMemInsts|vectorMemWrites)' \
  m5out-hip-memset-exec/stats.txt
```

The run completed by user interrupt at:

```text
simTicks 800385732748
hostSeconds 4797.17
simInsts 1249117409
```

After approximately 80 host minutes and 1.249 billion simulated CPU
instructions, `hipMemset` still had not returned. Every GPU execution and VRAM
request counter remained zero.

The bounded trace contains only six lines, all on CPU core 1:

```text
0x7ffff7fcc3ef ... NOP
0x7ffff7fcc427 ... NOP
0x7ffff7fcdb6d ... NOP
```

The three PCs repeat. Symbolization reports them only as large offsets from
`_end`, so the owning shared object is not identified. The trace flags included
`ExecEnable`, `ExecUser`, `ExecSymbol`, and `ExecThread`, but omitted
`ExecMacro` and `ExecMicro`. On x86 this omitted most macroop/microop execution,
so the six NOP entries are not a representative instruction profile.

CPU statistics show two runtime worker threads consuming nearly all execution:

```text
core 1: 583671014 instructions, 94.5 percent non-idle
core 2: 640796468 instructions, 94.5 percent non-idle
core 0:  24650767 instructions,  5.5 percent non-idle
core 3:         0 instructions
```

There were 816 ignored `mprotect` calls. `stderr.txt` stopped growing well
before the run ended, so the long final phase is dominated by two active CPU
worker threads rather than continuing `mprotect` calls.

Facts confirmed:

- the runtime is not blocked waiting for a GPU/Ruby response;
- two CPU-side worker threads are actively looping or performing work;
- the current trace cannot identify their functions because x86 macroops and
  microops were not enabled;
- another long untraced wait is not useful.

No source file was changed during inspection.

Next diagnostic step: repeat only a short reachable window with
`ExecEnable,ExecUser,ExecSymbol,ExecThread,ExecMacro,ExecMicro`. A
10,000,000-tick window is sufficient; use an uncompressed trace and terminate
after the window closes.

### Full Exec Window Identifies COMGR/Clang Blit-Kernel Compilation

The corrected bounded run used:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hip-memset-exec \
  --debug-start=60000000000 \
  --debug-end=60010000000 \
  --debug-flags=ExecEnable,ExecUser,ExecSymbol,ExecThread,ExecMacro,ExecMicro \
  --debug-file=hip-memset-exec-full.trace \
  --stdout-file=stdout.txt \
  --stderr-file=stderr.txt \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type timing \
  --app tests/test-progs/gpu/hip-api-smoke/hip_api_smoke \
  --opts="--stage memset" \
  --rocm-path \
  /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1
```

Inspection commands:

```bash
nl -ba m5out-hip-memset-exec/hip-memset-exec-full.trace
```

```bash
awk '$5 ~ /^0x/ && $5 !~ /\./ {print $5}' \
  m5out-hip-memset-exec/hip-memset-exec-full.trace \
  | sort | uniq -c | sort -nr
```

```bash
addr2line -C -f -p \
  -e .deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib/libamd_comgr.so.1.9.40001 \
  0x2ae58ee 0x2ae1b87
```

The trace is 5.7 MiB and correctly covers ticks 60,000,000,000 through
60,010,000,000. The overall run ended later at:

```text
simTicks 158417267157
hostSeconds 793.32
simInsts 198519912
```

The trace contains active execution on cores 1 and 2. Addresses in the
`0x7ffff2ae...` range map, with load base `0x7ffff0000000`, into
`libamd_comgr.so.1.9.40001`. `addr2line` identifies Clang recursive AST visitor
functions, including:

```text
clang::RecursiveASTVisitor<...>::TraverseOMPTargetUpdateDirective(...)
clang::RecursiveASTVisitor<...>::TraverseObjCProtocolExpr(...)
```

Core 1 repeatedly calls many distinct functions from this AST traversal rather
than spinning on one instruction. Core 2 repeatedly scans object structures
and performs an indirect virtual call. This is consistent with COMGR/Clang
internal-kernel compilation and its helper work, not GPU request processing.

Facts confirmed:

- ROCm's first non-SDMA `hipMemset` is synchronously creating/compiling its
  internal blit/fill kernel through `libamd_comgr`;
- the long delay occurs before compute-queue/AQL submission;
- no GPU kernel, Ruby request, or VRAM request is waiting for a response;
- ignored `mprotect` is associated with compiler/runtime setup, but the trace
  does not show it as the loop condition;
- the compilation is making CPU-side progress through AST traversal, although
  it is prohibitively expensive under detailed Timing CPU simulation.

No source file was changed during inspection.

Next diagnostic step: inspect ROCm 4's blit-kernel creation controls and cache
path to determine whether the internal `__amd_rocclr_fillBuffer` code object can
be precompiled or reused, avoiding in-simulation COMGR compilation. Do not
continue longer Timing CPU runs of the same unmodified `hipMemset` path.

### ROCclr 4.0 Source Inspection and Bounded APU Control

The matching official ROCclr source was inspected from the `rocm-4.0.x`
branch:

```bash
git clone --depth 1 --branch rocm-4.0.x \
  https://github.com/ROCm/ROCclr.git /tmp/ROCclr-rocm-4.0.x
```

Relevant source searches and inspections:

```bash
rg -n \
  'createBlitProgram|BlitProgram|KernelBlitManager|HostBlitManager|fillBuffer' \
  /tmp/ROCclr-rocm-4.0.x/device
```

```bash
rg -n \
  'OCL_CODE_CACHE_ENABLE|OCL_CODE_CACHE_RESET|GPU_DUMP_BLIT_KERNELS|GPU_BLIT_ENGINE_TYPE' \
  /tmp/ROCclr-rocm-4.0.x
```

Source facts:

- `device/device.cpp`, `Device::BlitProgram::create()`, concatenates ROCclr's
  built-in blit OpenCL sources and calls `program_->build(...)`;
- `device/rocm/rocvirtual.cpp`, `roc::VirtualGPU::create()`,
  unconditionally creates a `KernelBlitManager` for the ROCr backend;
- `device/rocm/rocblit.cpp`, `KernelBlitManager::createProgram()`, invokes
  `device.createBlitProgram()` and creates the complete set of blit kernels
  before `fillBuffer()` can dispatch;
- `GPU_BLIT_ENGINE_TYPE` does not select `HostBlitManager` in this ROCr
  backend, so setting it to host mode cannot bypass compilation here;
- `OCL_CODE_CACHE_ENABLE` and `OCL_CODE_CACHE_RESET` both default to false in
  `utils/flags.hpp`;
- the compiler option named `kernel-cache` is not evidence that ROCclr's
  persistent runtime cache is enabled; the runtime cache switch remains
  disabled by default.

This confirms that the observed COMGR/Clang work is expected first-use ROCclr
behavior: with SDMA disabled, `hipMemset` reaches a kernel blit manager whose
initialization builds the whole internal blit program synchronously.

The APU configuration was also inspected. `configs/example/apu_se.py` sets
`HSA_ENABLE_SDMA=0`, so it uses the same non-SDMA premise. Unlike the stdlib
XGMI script's current environment, its normal environment retains the user's
`HOME`. Existing APU tests primarily launch precompiled code objects and
therefore do not establish that first-use `hipMemset` avoids ROCclr
compilation.

Two rejected APU invocations failed during configuration, before simulation:

```text
--cpu-type=TimingSimpleCPU
fatal: Valid CPU types are X86TimingSimpleCPU and X86O3CPU
```

```text
--gfx-version=gfx900
AssertionError: Incorrect gfx version for APU
```

The corrected, foreground, bounded APU control was:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-apu-hip-memset-control \
  --listener-mode=off \
  --debug-start=45000000000 \
  --debug-flags=GPUDriver,HSAPacketProcessor,GPUCommandProc,GPUDisp \
  configs/example/apu_se.py \
  -n 3 \
  --reg-alloc-policy=dynamic \
  --gfx-version=gfx902 \
  --cpu-type=X86TimingSimpleCPU \
  -m 70000000000 \
  -c tests/test-progs/gpu/hip-api-smoke/hip_api_smoke \
  -o '--stage memset'
```

Observable result:

```text
simTicks 70000000000
simInsts 17614029
hostSeconds 86.79
```

The run stopped at the configured tick limit before the program reached its
first `hipGetDeviceCount` marker. Only CPU core 0 was active and no GPU kernel
was submitted. Therefore this bounded APU run neither reproduces nor rejects
the `hipMemset` compilation delay. Extending it blindly is not useful because
the APU setup is substantially slower before the test reaches the HIP API.

Facts confirmed:

- both SE configurations disable SDMA and consequently depend on ROCclr's
  kernel-based blit path;
- the XGMI `hipMemset` delay is explained by ROCclr source behavior and the
  COMGR execution trace, not by a missing GPU/Ruby response;
- `GPU_BLIT_ENGINE_TYPE` cannot select host fill in the ROCr backend;
- persistent ROCclr code caching is disabled by default;
- the bounded APU control did not run far enough to compare `hipMemset`.

Hypotheses rejected:

- selecting host blit solely through `GPU_BLIT_ENGINE_TYPE`;
- treating passing APU programs with precompiled kernels as evidence that
  first-use `hipMemset` does not compile internal kernels;
- using another long APU TimingSimpleCPU run as the next diagnostic.

No gem5 or test source was changed during this step. The only changed file is
this status document. A temporary official source checkout exists at
`/tmp/ROCclr-rocm-4.0.x`; bounded APU output is in
`m5out-apu-hip-memset-control/`.

Next diagnostic step: enable `OCL_CODE_CACHE_ENABLE=1` in a bounded run with a
writable, persistent `HOME`, and trace file-related syscalls to identify the
actual cache path and determine whether ROCclr creates a reusable blit program
cache. This first run may still require compilation; the important question is
whether a second run can consume the generated cache without entering COMGR.

### ROCclr Persistent-Cache Probe Does Not Avoid First-Run Compilation

The XGMI config was given two diagnostic-only CLI capabilities:

- repeatable `--env KEY=VALUE`, which replaces a matching default ROCm
  environment variable or appends a new one;
- absolute `--max-ticks`, which bounds every simulation run.

The config also accepts `--cpu-type atomic` so CPU-only runtime/compiler work
can be investigated when KVM is unavailable. No change to the compiled gem5
binary is required because the config passes the resulting list through the
existing `SEViperBoard.set_se_gpu_binary_workload(env_list=...)` interface.

Configuration probes:

```bash
build/VEGA_X86/gem5.opt \
  -d /tmp/m5out-env-probe \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type timing \
  --app /bin/true \
  --rocm-path \
  /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env HOME=/tmp/rocclr-home \
  --env OCL_CODE_CACHE_ENABLE=1
```

This completed normally. A separate 1,000,000-tick probe confirmed that
`--max-ticks` exits with `simulate() limit reached`.

KVM was attempted first:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hip-memset-cache-first \
  --listener-mode=off \
  --debug-flags=SyscallAll \
  --debug-file=hip-memset-cache-syscalls.trace \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type kvm \
  --max-ticks 100000000000 \
  --app tests/test-progs/gpu/hip-api-smoke/hip_api_smoke \
  --opts='--stage memset' \
  --rocm-path \
  /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env HOME=/tmp/rocclr-home \
  --env OCL_CODE_CACHE_ENABLE=1
```

It failed before simulation:

```text
KVM is required but is unavailable on this system
```

The bounded Timing run used the same command with `--cpu-type timing`. It
ended automatically at:

```text
simTicks 100000000000
simInsts 104823050
hostSeconds 445.56
```

`config.ini` confirms that the guest process received:

```text
HOME=/tmp/rocclr-home
OCL_CODE_CACHE_ENABLE=1
HSA_ENABLE_SDMA=0
```

The syscall trace is an uncompressed 24 MiB file at:

```text
m5out-hip-memset-cache-first/hip-memset-cache-syscalls.trace
```

At approximately 56.9 billion ticks, ROCclr created only:

```text
/tmp/comgr-2a605a/include/opencl1.2-c.pch
/tmp/comgr-2a605a/input/CompileSource
/tmp/comgr-2a605a/output/CompileSource-3de2c865.bc.tmp
```

There was no syscall containing `/tmp/rocclr-home`, no `.cache` or
kernel-cache directory, and no persistent file outside the COMGR temporary
tree. The run was still compiling when it reached its limit. All GPU
instruction and completed-workgroup counters were zero.

An AtomicSimpleCPU configuration probe then completed normally:

```bash
build/VEGA_X86/gem5.opt \
  -d /tmp/m5out-atomic-probe \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type atomic \
  --max-ticks 1000000000 \
  --app /bin/true \
  --rocm-path \
  /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env HOME=/tmp/rocclr-home \
  --env OCL_CODE_CACHE_ENABLE=1
```

Ruby reports `atomic_noncaching` memory mode for this CPU. This is unsuitable
for cache-timing conclusions but valid for accelerating the CPU-only compiler
and filesystem investigation.

The bounded atomic cache-generation run was:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hip-memset-cache-atomic-first \
  --listener-mode=off \
  --debug-flags=SyscallAll \
  --debug-file=hip-memset-cache-syscalls.trace \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type atomic \
  --max-ticks 100000000000 \
  --app tests/test-progs/gpu/hip-api-smoke/hip_api_smoke \
  --opts='--stage memset' \
  --rocm-path \
  /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env HOME=/tmp/rocclr-home \
  --env OCL_CODE_CACHE_ENABLE=1
```

It also ended automatically:

```text
simTicks 100000000000
simInsts 240320934
hostSeconds 371.14
```

Atomic execution advanced farther: it completed the first `CompileSource`
temporary phase, removed that COMGR tree, and entered a second
`/tmp/comgr-e59861` phase. It still did not return from `hipMemset`, create a
persistent cache file, submit a GPU kernel, or execute a GPU instruction.

Facts confirmed:

- the cache environment variables are correctly propagated into the simulated
  process;
- `OCL_CODE_CACHE_ENABLE=1` does not avoid ROCclr's expensive first-run
  internal blit compilation;
- no persistent-cache lookup or write occurs during the observed frontend
  compilation phases;
- AtomicSimpleCPU makes more CPU/compiler progress but still cannot complete
  the complete blit build within the tested bound;
- a second-run cache-hit experiment cannot yet be performed because the first
  run produced no persistent cache artifact;
- the original `hipMemset` diagnosis remains CPU-side COMGR compilation before
  AQL/GPU submission.

Hypotheses rejected:

- the original `HOME=/` setting alone explains why compilation starts;
- simply enabling `OCL_CODE_CACHE_ENABLE` makes the first simulated run
  practical;
- a cache artifact is created before ROCclr finishes its internal blit
  program build.

Files changed:

- `configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py`
- `docs/debug/se-multigpu-status.md`

Verification completed:

- `/bin/true` completed through the environment-override configuration;
- a 1,000,000-tick run stopped at the exact configured bound;
- the atomic configuration completed a `/bin/true` probe;
- both HIP cache probes stopped automatically at 100,000,000,000 ticks;
- both `stats.txt` files report zero GPU instructions/workgroups;
- syscall and filesystem inspection found only COMGR temporary artifacts.

Next diagnostic step: generate the ROCclr gfx900 blit program outside detailed
simulation, then determine whether ROCclr 4.0.1 can import that artifact as its
internal blit program. Continuing first-run COMGR compilation for hundreds of
billions of simulated ticks is not the next step.

### Precompiled HSA Kernel Compatibility Layer Implements `hipMemset`

The first SE compatibility implementation was added under:

```text
tests/test-progs/gpu/hip-api-smoke/se_hip_compat/
```

It exports the public synchronous `hipMemset` symbol through an
`LD_PRELOAD` library and uses a precompiled gfx900 code-object-v2 byte-fill
kernel.

The first implementation used `hipModuleLoad` and `hipModuleGetFunction`.
Atomic runs in:

```text
m5out-hip-memset-preload-atomic/
m5out-hip-memset-preload-atomic-kd/
```

confirmed that `hipModuleLoad` accepted the HSACO but
`hipModuleGetFunction` returned `hipErrorNotFound` for both:

```text
seHipMemsetKernel
seHipMemsetKernel@kd
```

`readelf -n` identified the code-object-v2 metadata symbol as
`seHipMemsetKernel@kd`. Existing peer-VRAM tests successfully load the same
format through the HSA executable API. The HIP module path was therefore
rejected for this ROCm 4.0.1 code-object-v2 use case.

The compatibility library now:

1. resolves the current device through `dlsym(RTLD_NEXT, "hipGetDevice")`;
2. selects the corresponding HSA GPU agent;
3. loads the HSACO with `hsa_executable_load_agent_code_object`;
4. resolves the kernel with the normal-name/`@kd` fallback;
5. creates a private HSA queue;
6. submits an `hsa_kernel_dispatch_packet_t`;
7. waits for the completion signal before returning from `hipMemset`.

Build commands:

```bash
make -C tests/test-progs/gpu/hip-api-smoke/se_hip_compat \
  ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  HIPCC=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/bin/hipcc
```

```bash
make -C tests/test-progs/gpu/hip-api-smoke \
  ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  HIPCC=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/hip/bin/hipcc \
  hip_memset_preload_smoke
```

Binary verification:

```bash
nm -D --defined-only \
  tests/test-progs/gpu/hip-api-smoke/se_hip_compat/libse_hip_compat.so
```

shows one exported `hipMemset`. `readelf -sW` on
`se_hip_memset.hsaco` shows `seHipMemsetKernel`. A host-only boundary probe
confirmed:

```text
hipMemset(nullptr, value, 0) = hipSuccess
hipMemset(nullptr, value, 1) = hipErrorInvalidDevicePointer
```

The cumulative `--stage memset` smoke proved that the wrapper's `hipMemset`
returned, but its following explicit `hipDeviceSynchronize()` caused ROCclr to
create its first `VirtualGPU`, which unconditionally creates the
`KernelBlitManager` and restarts the known COMGR build. This is a separate API
initialization issue, so a focused diagnostic
`hip_memset_preload_smoke` was added. It performs:

```text
hipSetDevice -> hipMalloc -> hipMemset -> direct SE validation
```

It does not call `hipDeviceSynchronize` or `hipMemcpy`. The synchronous HSA
completion in the wrapper makes the direct validation ordered.

The successful bounded Atomic command was:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hip-memset-preload-isolated-atomic \
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

It printed:

```text
HIP_MEMSET_PRELOAD_PASSED
Exiting @ tick 23745191040 because exiting with last active thread context.
```

GPU0 executed nonzero instructions, global memory operations, and completed
workgroups. No `/tmp/comgr-*` directory or `CompileSource` syscall occurred.
Loading `libamd_comgr.so` as a normal ROCm dependency still occurs and is not
evidence of compilation.

Because AtomicSimpleCPU uses Ruby `atomic_noncaching`, the final validation
used Timing CPU:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hip-memset-preload-timing-m5exit \
  --listener-mode=off \
  --debug-flags=GPUDriver,HSAPacketProcessor,GPUCommandProc,GPUDisp,SyscallAll \
  --debug-file=hip-memset-preload-timing.trace \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type timing \
  --max-ticks 100000000000 \
  --app tests/test-progs/gpu/hip-api-smoke/hip_memset_preload_smoke \
  --rocm-path \
  /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env LD_PRELOAD=/home/zhangds/gem5/tests/test-progs/gpu/hip-api-smoke/se_hip_compat/libse_hip_compat.so \
  --env SE_HIP_MEMSET_HSACO=/home/zhangds/gem5/tests/test-progs/gpu/hip-api-smoke/se_hip_compat/se_hip_memset.hsaco
```

The first Timing run printed the pass marker but then hit an unrelated
`TimingSimpleCPU::suspendContext` assertion during process `exit_group`.
The focused diagnostic now calls `m5_exit` after successful validation. The
repeated Timing run completed cleanly:

```text
HIP_MEMSET_PRELOAD_PASSED
Exiting @ tick 45928600758 because m5_exit instruction encountered.
```

Timing statistics confirm:

- nonzero GPU instructions, global memory operations, and completed
  workgroups on all four GPU0 CUs;
- nonzero TCP, SQC, TCC, directory, internal-link, and router Ruby message
  counts;
- no `/tmp/comgr-*` or `CompileSource` activity.

Facts confirmed:

- the public synchronous `hipMemset` API can be supported in SE mode without
  SDMA and without in-simulation ROCclr blit compilation;
- code-object-v2 loading must use the HSA executable API with this ROCm 4.0.1
  stack;
- Atomic CPU is suitable for fast functional iteration;
- Timing CPU confirms the normal Ruby request path;
- the original cumulative smoke proceeds past `hipMemset`, and its next
  blocker is first-use `hipDeviceSynchronize`/`VirtualGPU` initialization.

Files changed:

- `configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py`
- `tests/test-progs/gpu/hip-api-smoke/Makefile`
- `tests/test-progs/gpu/hip-api-smoke/hip_memset_preload_smoke.hip`
- `tests/test-progs/gpu/hip-api-smoke/se_hip_compat/Makefile`
- `tests/test-progs/gpu/hip-api-smoke/se_hip_compat/se_hip_compat.cpp`
- `tests/test-progs/gpu/hip-api-smoke/se_hip_compat/se_hip_memset.hip`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- design and implementation-plan documents for this compatibility layer;
- this status document.

Next diagnostic step: inspect and prototype lazy creation of ROCclr's
`KernelBlitManager` so that `hipDeviceSynchronize` and ordinary HIP kernel
launch do not compile the complete blit program unless a blit API is actually
used.

## KVM Startup Failure on External Host

On 2026-06-22, the user ran the malloc stage on a KVM-capable external host:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hip-malloc \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type kvm \
  --app tests/test-progs/gpu/hip-api-smoke/hip_api_smoke \
  --opts='--stage malloc' \
  --rocm-path /home/deson/rocm-4.0.1
```

The run aborted at tick 0 while `KvmVM::delayedStartup()` registered the
first CPU-memory KVM memslot:

```text
Failed to setup KVM memory region:
    Guest Address: 0x0
    Size:          0xc0000000
Program aborted at tick 0
```

Facts confirmed:

- gem5 opened KVM and created all four KVM CPUs;
- the failure precedes process execution, ROCm initialization, and
  `hipMalloc`;
- `0xc0000000` is the configured default 3 GiB CPU-memory range;
- the current panic in `src/cpu/kvm/vm.cc` does not print the failed ioctl's
  `errno`, so the evidence does not yet distinguish `EINVAL`, `ENOMEM`, or a
  host-specific KVM restriction;
- the DRAM-capacity and legacy-stat warnings are not the fatal condition.

No source files were changed for this failure. No memory-size workaround has
been accepted without first identifying the ioctl error.

The requested `strace` run showed:

```text
KVM_SET_USER_MEMORY_REGION {
  slot=0, guest_phys_addr=0, memory_size=6442450944
} = 0
KVM_SET_USER_MEMORY_REGION {
  slot=1, guest_phys_addr=0, memory_size=3221225472
} = -1 E...
```

The second errno name was truncated in the pasted output, but the decisive
fact is independent of its spelling: slot 0 maps `[0, 6 GiB)`, then slot 1
tries to map the overlapping `[0, 3 GiB)` range.

Root cause accepted:

- `SEViperXGMICacheHierarchy._configure_backing_store()` creates a unified
  Ruby backing store spanning CPU memory, the address hole, and both VRAM
  ranges: `[0, 6 GiB)`;
- `SEViperBoard` leaves `System.memories` at its `Self.all` default, so the
  CPU and GPU DRAM interfaces are also collected as physical backing stores;
- all those interfaces retain the default `kvm_map=True`;
- KVM therefore receives overlapping guest-physical memslots and rejects
  the second registration.

The standard Ruby setup in `configs/ruby/Ruby.py` avoids this condition by
setting each DRAM interface's `kvm_map=False` when
`access_backing_store=True`, leaving the Ruby reference backing store as the
KVM mapping.

The KVM mapping fix was implemented with a focused red-green regression:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_viper_ruby_backing_store_is_only_kvm_mapped_memory
```

Before the production change, the test failed because the CPU and both GPU
memory interfaces retained `kvm_map=True`. After the change, it passed.

Implementation:

- `SEViperBoard.get_all_mem_interfaces()` returns the CPU memory interfaces
  followed by every GPU memory interface;
- `SEViperXGMICacheHierarchy._configure_backing_store()` sets
  `kvm_map=False` on each of those interfaces before creating the unified
  Ruby backing store;
- `ruby_system.phys_mem` retains its default `kvm_map=True`, so KVM receives
  only the single non-overlapping Ruby backing range.

Verification completed:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 19 tests passed.

```bash
python3 -m py_compile \
  src/python/gem5/prebuilt/viper/se_board.py \
  src/python/gem5/prebuilt/viper/se_gpu_cache_hierarchy.py \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
git diff --check
```

Result: both commands completed successfully with no output.

Files changed for this fix:

- `src/python/gem5/prebuilt/viper/se_board.py`
- `src/python/gem5/prebuilt/viper/se_gpu_cache_hierarchy.py`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document.

No gem5 build or full simulation was started by Codex.

Next diagnostic step: the user should rebuild `build/VEGA_X86/gem5.opt` on
the KVM host and repeat the same `--stage malloc` command. If startup still
fails, capture all `KVM_SET_USER_MEMORY_REGION` lines; the expected result is
one successful 6 GiB mapping and no overlapping 3 GiB or 1 GiB mappings.

The rebuilt KVM run no longer reports a failed
`KVM_SET_USER_MEMORY_REGION`, confirming that the overlapping-memslot fix
removed the original tick-0 startup failure. KVM entered guest user
execution, but later aborted at tick `237280149`:

```text
KVM register state:
  rax: 0x3c
  rdi: 0x7f
  rip: 0x7ffff801e2b6
...
KVM: Unexpected exit (exit_reason: 8)
```

`KVM_EXIT_SHUTDOWN` is exit reason 8. The register values are more useful
than the shutdown itself:

- x86-64 syscall 60 (`rax=0x3c`) is `exit`;
- its status argument is 127 (`rdi=0x7f`);
- no `[hip_api_smoke] begin ...` marker appears in the supplied output.

The process therefore failed before entering the smoke-test stages and tried
to exit with status 127. The subsequent KVM shutdown occurs on gem5's SE KVM
syscall-return path after the non-returning exit syscall; it is a secondary
failure, not evidence of a GPU or HIP API fault. The repeated unsupported-MSR
warnings and coalesced-MMIO notices are also not the primary failure.

Current hypothesis: the ELF interpreter or dynamic loader failed to resolve
one of `hip_api_smoke`'s runtime dependencies on the external host. The
binary is dynamically linked and requires `libamdhip64.so.4` among its
dependencies. The binary built in the original workspace also contains a
RUNPATH under `/home/zhangds/...`; the KVM-host command relies on
`LD_LIBRARY_PATH` generated from `/home/deson/rocm-4.0.1` to override it.

No source code was changed for this new failure.

Next diagnostic step: on the KVM host, inspect `m5out-hip-malloc/simout.txt`
and `simerr.txt` for the dynamic-loader message, then run host-side dependency
resolution with the same ROCm library search path. Do not rerun the full
simulation until all `ldd` entries resolve.

The user ran on the KVM host:

```bash
ldd tests/test-progs/gpu/hip-api-smoke/hip_api_smoke
```

All dependencies resolved. In particular:

```text
libamdhip64.so.4 => /home/deson/rocm-4.0.1/lib/libamdhip64.so.4
libhsa-runtime64.so.1 => /home/deson/rocm-4.0.1/lib/libhsa-runtime64.so.1
libamd_comgr.so.1 => /home/deson/rocm-4.0.1/lib/libamd_comgr.so.1
libhsakmt.so.1 => /home/deson/rocm-4.0.1/lib/libhsakmt.so.1
```

This rejects the narrow hypothesis that a required shared object is absent
from the KVM host's configured library paths. It does not yet prove that the
same ELF interpreter operations succeed through gem5 SE syscall emulation.

No files were changed.

Next diagnostic step: inspect the existing run's `simout.txt` and
`simerr.txt`. If they do not contain the loader's reason for `exit(127)`,
repeat the short malloc-stage KVM run with `--env LD_DEBUG=libs,files` and
capture the last loader operation before the exit.

The ioctl trace from the same run ends with:

```text
KVM_GET_MSR_INDEX_LIST ... = -1 E2BIG
...
KVM_GET_MSR_INDEX_LIST ... = 0
KVM_SET_MSRS ... = 13
...
KVM_RUN ... = 0
KVM_GET_REGS {rax=0x3c, ..., rdi=0x7f, ...} = 0
...
SIGABRT
```

Interpretation:

- the repeated `E2BIG` results are the normal KVM variable-length-list
  discovery protocol: KVM reports the required count and gem5 retries with a
  larger buffer until the request succeeds;
- `KVM_SET_MSRS = 13` means that 13 MSR entries were accepted, not failure
  with errno 13;
- `KVM_RUN = 0` means control returned normally to gem5; the reason is read
  from the shared `kvm_run` mapping and was already reported as
  `KVM_EXIT_SHUTDOWN` (8);
- the final `SIGABRT` is gem5 executing its panic path after treating reason
  8 as unexpected.

This trace therefore supplies no new host-KVM ioctl failure and does not
change the current first-failure target: determine why guest execution
reaches an exit-status-127 path, or whether the SE KVM syscall trampoline
triple-faults while trying to service that exit. The existing stdout/stderr
and loader trace are still required to distinguish those cases.

The supplied `simerr.txt` from a repeat with
`--env LD_DEBUG=libs,files` contains no loader diagnostics and no
`[hip_api_smoke]` marker. It contains only gem5 warnings and the same KVM
shutdown dump. `AT_SECURE` suppression is not expected because the x86 SE
process builder explicitly emits `AT_SECURE=0`.

The smoke source returns only statuses 0, 1, or 2, so status 127 originates
before `main()`, most likely from the ELF interpreter. The KVM dump has user
CS/SS and user-space RIP/RCX in the interpreter mapping, not the privileged
SE syscall trampoline. The #GP/triple-fault shutdown is therefore the
secondary KVM behavior after the non-returning `exit(127)` syscall, which
currently masks the clean SE exit event.

No files were changed.

Next diagnostic step: repeat only the short malloc-stage run with
`--debug-flags=SyscallAll,KvmRun,KvmIO` and a separate uncompressed debug
file. Inspect the last 100 syscall lines before syscall 60 (`exit`) to find
the loader operation that caused status 127.

The requested trace contained:

```text
0: board.processor.cores0.core: Entering KVM...
0: board.processor.cores0.core: KVM: Executing for 1000000000 ticks
0: board.processor.cores0.core: KVM: Executed 0 instructions ...
486536310: board.processor.cores0.core:
  handleKvmExit (exit_reason: 8)
```

There was no `KVM_EXIT_MMIO` and therefore no `SyscallAll` entry. This
rejects the dynamic-loader-error hypothesis: gem5 never trapped the first
guest syscall, so the observed `rax=60`/`rdi=127` values were simply the
register state at the eventual triple fault, not proof that gem5 emulated
`exit(127)`.

Root cause accepted:

- the x86 KVM-in-SE syscall trampoline writes to the physical m5ops window
  to generate `KVM_EXIT_MMIO`;
- `SEViperBoard` currently places that window at `0xffff0000`;
- the unified Ruby backing store is `[0, 0x180000000)`, covering CPU memory,
  the address hole, and both GPU VRAM ranges;
- after the KVM mapping fix, that complete 6 GiB range is registered as RAM,
  so `0xffff0000` is inside a KVM RAM memslot;
- the trampoline write is handled as RAM instead of MMIO, then its privileged
  continuation faults and ultimately produces `KVM_EXIT_SHUTDOWN`.

`AddrRange.end` is exclusive. For the default layout, the first safe m5ops
base after all backing memory is therefore `0x180000000` (6 GiB).

No source code was changed for this result.

Next code change: calculate `m5ops_base` as the maximum of `0xffff0000` and
the exclusive end of every CPU/GPU memory range. Add a regression asserting
that the 64 KiB m5ops range does not overlap the unified Ruby/KVM backing
range. Then rebuild on the KVM host and verify that the first guest syscall
produces `KVM_EXIT_MMIO` and `SyscallAll` output.

The m5ops placement fix was implemented with a focused red-green test:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_viper_m5ops_range_follows_all_cpu_and_gpu_memory
```

Before the production change, the test failed because
`_m5ops_base_after_ranges` did not exist. The implementation now computes:

```python
max(
    [0xFFFF0000]
    + [int(addr_range.end) for addr_range in memory_ranges]
)
```

`SEViperBoard.set_se_gpu_binary_workload()` applies this calculation to
`self.mem_ranges`, which contains CPU memory and every GPU VRAM range. For
the default two-GPU layout the result is `0x180000000`, immediately after
the exclusive end of the 6 GiB unified Ruby/KVM backing range.

Verification completed:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 20 tests passed.

```bash
python3 -m py_compile \
  src/python/gem5/prebuilt/viper/se_board.py \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
git diff --check
```

Result: both commands completed successfully with no output.

Files changed for this step:

- `src/python/gem5/prebuilt/viper/se_board.py`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document.

No gem5 build or full simulation was started by Codex.

Next diagnostic step: rebuild gem5 on the KVM host and repeat the malloc-stage
run with `SyscallAll,KvmRun,KvmIO`. The first syscall must now produce
`KVM_EXIT_MMIO` and syscall trace entries instead of immediately reaching
`KVM_EXIT_SHUTDOWN`.

The configurability of `m5ops_base` was verified against the implementation:

- `System.m5ops_base` is a `Param.Addr`, not an x86 architectural constant;
- `System` constructs a 64 KiB m5ops range at the configured physical base;
- x86 KVM-in-SE uses the fixed virtual address
  `MMIORegionVirtAddr = 0xffffc90000000000`;
- `X86_64Process` maps that virtual range to
  `system->m5opRange().start()`, so the physical base is intentionally
  relocatable;
- x86 TLB handling recognizes the configured range and decodes an operation
  using `paddr - m5opRange.start()`;
- generic SE stdlib code already relocates `m5ops_base` above configured
  memory with `max(0xffff0000, memory_size)`.

Therefore `0xffff0000` is a conventional/default physical location used when
it does not overlap memory, not a required fixed address. Moving this SE
multi-GPU board's physical m5ops window to `0x180000000` for the default
layout is consistent with gem5's existing SE design. Full-system x86 boards
that use `0xffff0000` have a different fixed platform memory map and are not
the reference for this SE multi-GPU backing layout.

## KVM HIP Malloc Validation

After rebuilding gem5 on the KVM host with both the KVM memslot and m5ops
placement fixes, the user repeated the malloc-stage smoke test with:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hip-malloc-syscall \
  --listener-mode=off \
  --debug-flags=SyscallAll,KvmRun,KvmIO \
  --debug-file=kvm-loader.trace \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type kvm \
  --app tests/test-progs/gpu/hip-api-smoke/hip_api_smoke \
  --opts='--stage malloc' \
  --rocm-path /home/deson/rocm-4.0.1
```

The run completed successfully:

```text
[hip_api_smoke] end hipGetDeviceCount(&device_count)
[hip_api_smoke] begin hipSetDevice(0)
[hip_api_smoke] end hipSetDevice(0)
[hip_api_smoke] begin hipMalloc(device_data, bytes)
[hsaKmtAllocMemory] node 1
[hsaKmtMapMemoryToGPUNodes] address 0x7ffdeea00000 number of nodes 1
[hip_api_smoke] end hipMalloc(device_data, bytes)
API_STAGE_PASSED stage=malloc
Exiting @ tick 46721275957 because exiting with last active thread context.
```

Facts confirmed:

- the unified Ruby backing store is accepted by KVM without overlapping
  memslots;
- relocating the physical m5ops window restores KVM-in-SE syscall trapping;
- the dynamic loader and ROCm runtime initialize under KVM;
- HIP/HSA enumerate the platform and select GPU0;
- `hipMalloc` allocates and maps GPU memory successfully;
- process exit is handled normally without `KVM_EXIT_SHUTDOWN`.

The `madvise` warning is an existing ignored SE syscall and did not prevent
the successful stage result. The render-driver mmap address warning is
diagnostic output, not a failure.

No files were changed in response to this validation result.

Next diagnostic step: run the cumulative native KVM memset stage with
`--opts='--stage memset'`, without `LD_PRELOAD`, to determine whether KVM
makes ROCclr's internal blit-kernel compilation and execution practical.

## Native KVM HIP Memset Event-Queue Failure

The native KVM memset stage progressed through ROCm initialization and
`hipMalloc`, then entered `hipMemset`:

```text
[hip_api_smoke] begin hipMemset(device_data, memset_byte, bytes)
src/sim/syscall_emul.cc:86: warn: ignoring syscall mprotect(...)
gem5.opt: src/sim/eventq.hh:759:
EventQueue::schedule(...): Assertion `when >= getCurTick()' failed.
Program aborted at tick 44043271641
```

Facts confirmed:

- KVM makes the CPU-side path fast enough to reach native `hipMemset`;
- this failure is not the prior ROCclr compilation stall;
- the failure is a gem5 event-queue timestamp violation, before a
  `hipMemset` completion marker;
- stdlib assigns each KVM core to a separate event queue by default;
- this run uses four KVM cores;
- the existing VIPER board source explicitly warns that multiple KVM event
  queues break GPU model event ordering, m5 commands, and simulation exits;
- `SEViperBoard` currently does not override the per-core event queues.

Current hypothesis: ROCclr's multi-threaded work during `hipMemset` exposes a
cross-event-queue race between a KVM CPU queue and GPU/Ruby queue 0, causing
an event to be scheduled at a tick already passed by the destination queue.
The abbreviated backtrace does not yet identify the event owner, so this
hypothesis is not accepted as root cause yet.

No source files were changed for this failure.

Next diagnostic step: inspect the existing `simerr.txt` after the assertion
and capture the complete libc backtrace, especially the first gem5 symbol
above `EventQueue::schedule`. Do not rerun the simulation yet.

The complete backtrace was supplied:

```text
EventQueue::schedule(...)
BaseKvmCPU::activateContext(...)
Process::initState()
X86_64Process::initState()
doClone<X86Linux64>(...)
cloneFunc<X86Linux64>(...)
SyscallDesc::doSyscall(...)
BaseKvmCPU::doMMIOAccess(...)
BaseKvmCPU::handleKvmExit()
BaseKvmCPU::tick()
```

Root cause accepted:

- native `hipMemset` causes ROCclr to create a host worker thread with
  `clone`;
- gem5's SE clone path initializes a new process/thread context and activates
  an idle KVM CPU;
- stdlib assigned the four KVM CPUs to separate event queues 1 through 4;
- `BaseKvmCPU::activateContext()` schedules the new CPU tick on its queue at
  `clockEdge(0)`;
- the target queue has already advanced beyond that requested tick, causing
  `EventQueue::schedule()` to assert `when >= getCurTick()`.

This confirms the cross-event-queue hypothesis. The ignored `mprotect`
warning is merely the final syscall before ROCclr's clone and is not the
cause. No GPU memory request had an opportunity to fail.

No source files were changed for this diagnostic result.

Next code change: in `SEViperBoard`, override stdlib's per-core KVM event
queue assignment and place every KVM core on event queue 0, matching the
GPU/Ruby/device queue. Preserve all four CPU ThreadContexts for ROCclr
pthread support; only host-parallel event-queue execution is disabled. Add a
focused configuration regression, then rebuild and repeat the native KVM
memset stage.

The event-queue fix was implemented with a focused red-green regression:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_viper_places_all_kvm_cores_on_device_event_queue
```

Before the production change, the test failed because no SE VIPER helper
overrode stdlib's per-core KVM event queues. The implementation now calls,
after `super()._connect_things()`:

```python
for core in cores:
    if core.is_kvm_core():
        core.get_simobject().eventq_index = 0
```

This placement occurs after `BaseCPUProcessor.incorporate_processor()` has
assigned event queues 1 through N, so it deliberately restores every KVM
core to the GPU/Ruby/device event queue 0. It does not remove cores or
ThreadContexts; the default four contexts remain available to ROCclr worker
threads.

Verification completed:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 21 tests passed.

```bash
python3 -m py_compile \
  src/python/gem5/prebuilt/viper/se_board.py \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
git diff --check
```

Result: both commands completed successfully.

Files changed for this step:

- `src/python/gem5/prebuilt/viper/se_board.py`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document.

No gem5 build or full simulation was started by Codex.

Next diagnostic step: rebuild gem5 on the KVM host and repeat the same native
`--stage memset` run. Confirm that ROCclr's `clone` passes
`BaseKvmCPU::activateContext()` without the past-tick assertion, then record
the next API marker or first new error.

## Native KVM Clone Page-Table Synchronization Failure

After rebuilding with all KVM cores on event queue 0, native `hipMemset`
passed the previous clone-time event scheduling point. The next failure was:

```text
src/arch/x86/linux/se_workload.cc:166:
panic: Page fault at addr 0x3d151c
    ss: 0x1b
    rsp: 0x7ffdee859dc0
    rflags: 0x10046
    cs: 0x23
    rip: 0x7ffff7fbd573
    err_code: 0x6
```

Facts confirmed:

- the event-queue fix removed the prior
  `EventQueue::schedule(when < getCurTick())` assertion;
- ROCclr's worker-thread `clone` now completes far enough for the new KVM
  context to execute;
- the new context takes a user-mode write page fault (`err_code=0x6`) on an
  address that is mapped in the parent process;
- `Process::clone()` correctly replaces the child page-table pointer with
  the parent's pointer for `CLONE_VM`;
- x86 `archClone()` copies the parent's architectural registers, including
  CR3, into the child gem5 ThreadContext.

Root cause accepted:

- idle KVM CPUs synchronize their initial ThreadContext into KVM during
  startup and clear `threadContextDirty`;
- `clone()` later rewrites the idle child ThreadContext with the parent's
  registers and shared page-table CR3;
- `BaseKvmCPU::activateContext()` schedules the CPU but does not mark the
  modified ThreadContext dirty;
- the next KVM entry therefore skips `updateKvmState()` and executes with the
  stale initial CR3, whose page table lacks the parent's user mappings.

The ignored `mprotect` warning is not the direct cause, and changing the page
fault handler to allocate the address would mask the stale-KVM-state bug.

No source files were changed for this diagnostic result.

Next code change: set `threadContextDirty=true` in
`BaseKvmCPU::activateContext()` before scheduling an idle KVM CPU. Add a
focused KVM CPU state regression if practical, then rebuild and repeat native
`--stage memset`.

The KVM activation-state fix was implemented with a focused red-green
regression:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kvm_context_activation_resynchronizes_thread_state
```

The test first failed because `BaseKvmCPU::activateContext()` did not
resynchronize modified ThreadContext state. The implementation now performs,
before scheduling the next KVM tick:

```cpp
syncThreadContext();
threadContextDirty = true;
```

The order is intentional:

- `syncThreadContext()` first consumes any outstanding KVM state, preserving
  the invariant that `kvmStateDirty` and `threadContextDirty` are not both
  true; this covers wakeup from states such as KVM `HLT`;
- `threadContextDirty=true` then forces the current gem5 ThreadContext,
  including clone-copied registers and CR3, into KVM on the next entry.

Verification completed:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 22 tests passed.

```bash
python3 -m py_compile \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
git diff --check
```

Result: both commands completed successfully.

Files changed for this step:

- `src/cpu/kvm/base.cc`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document.

No gem5 build or full simulation was started by Codex.

Next diagnostic step: rebuild gem5 on the KVM host and repeat native
`--stage memset`. Confirm that the ROCclr worker thread no longer faults at
`0x3d151c`, then record the next API marker or first new error.

The rebuilt native KVM run no longer reported the clone page fault, but it
remained inside:

```text
[hip_api_smoke] begin hipMemset(device_data, memset_byte, bytes)
```

with no matching end marker at the time of observation.

This confirms that the KVM activation-state fix passed the previous failure
point. It does not yet distinguish active COMGR compilation, futex/thread
waiting, or a submitted GPU operation waiting for completion.

No files were changed for this observation. The running simulation should
not be restarted or terminated until its existing stdout/stderr growth,
redirected COMGR files, and last syscall activity are inspected.

Next diagnostic step: inspect the live run once for output growth, COMGR
temporary files, process CPU utilization, and the last syscall/KVM trace
entries. Use those observations to classify the stall before making another
code change.

The bounded KVM diagnostic was run with:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hip-memset-kvm-debug \
  --listener-mode=off \
  --debug-flags=SyscallAll,GPUDriver,HSAPacketProcessor,GPUCommandProc,GPUDisp \
  --debug-file=hip-memset-kvm.trace \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type kvm \
  --max-ticks 100000000000 \
  --app tests/test-progs/gpu/hip-api-smoke/hip_api_smoke \
  --opts='--stage memset' \
  --rocm-path /home/deson/rocm-4.0.1
```

The trace confirms:

- `hipMalloc` completes;
- native `hipMemset` enters ROCclr's CPU-side path;
- ROCclr successfully creates worker threads with `clone`;
- the event-queue assertion and stale-CR3 page fault do not recur;
- one short-lived worker exits normally, and another worker starts;
- there is no AQL packet, GPU kernel dispatch, or GPU completion event after
  entering `hipMemset`;
- no COMGR temporary file appears under the redirected `/tmp`, consistent
  with an in-memory compilation path;
- the host gem5 process remains runnable at approximately 99-100% CPU.

Two process samples showed CPU time increasing from approximately `2:11` to
`2:49` while the trace remained in user-space computation after the worker
thread setup. This rules out an unchanged futex wait or GPU completion wait.

Accepted classification: the run is actively executing ROCclr/COMGR/LLVM
CPU-side compilation before any GPU blit dispatch. KVM removed the Timing
CPU instruction-emulation bottleneck, but the compilation itself remains
substantial and, because all KVM CPUs must share event queue 0 for GPU model
correctness, gem5 uses only one host CPU at a time.

No source files were changed for this diagnostic result.

Next diagnostic step: allow this bounded run to continue until either
`hipMemset` returns or the `100000000000` tick limit is reached. Record the
final tick, host elapsed time, final API marker, and whether the first AQL/GPU
dispatch appears. Do not make another code change before that result.

The bounded run reached its configured limit:

```text
src/sim/process.cc:334:
warn: Process::allocateMem: addr 0x7ffff6acd000 already mapped
Exiting @ tick 100000000000 because simulate() limit reached.
```

There was still no matching `end hipMemset` and no recorded AQL/GPU dispatch.
The run did not panic or deadlock; gem5 stopped normally because
`--max-ticks=100000000000` is an absolute simulated-tick limit.

The `allocateMem already mapped` warning is not currently treated as the
blocker. `Process::allocateMem()` explicitly checks for the case where
multiple SE threads fault on the same page at nearby times and returns if
another thread has already installed the mapping. This is consistent with
ROCclr's active worker threads and is not a failed allocation.

Facts confirmed:

- all KVM startup, syscall trap, event queue, clone, and child-CR3 fixes
  remain effective through 100 billion ticks;
- native ROCclr `hipMemset` still does not reach GPU dispatch within that
  bound;
- the remaining time is CPU-side work before AQL submission.

No source files were changed for this result.

Next diagnostic step: repeat with a larger bound and only GPU submission
debug flags, omitting `SyscallAll` to reduce trace volume. Use
`--max-ticks=500000000000` and stop at the first `end hipMemset`, AQL/GPU
dispatch, new fatal error, or the new limit.

The longer repeat reproduced the same cloned-thread page fault:

```text
panic: Page fault at addr 0x3d151c
    rsp: 0x7ffdee859dc0
    rip: 0x7ffff7fbd573
    err_code: 0x6
Program aborted at tick 42988279356
```

This invalidates the previous claim that the KVM activation-state change
fully fixed clone state synchronization. The current implementation calls:

```cpp
syncThreadContext();
threadContextDirty = true;
```

inside `BaseKvmCPU::activateContext()`. For a newly cloned context this order
is incorrect:

- `archClone()` has already copied the parent's registers and CR3 into the
  idle child's gem5 ThreadContext;
- the idle vCPU may still have `kvmStateDirty=true` for its old state;
- `syncThreadContext()` copies that stale KVM state back into the
  ThreadContext, overwriting the clone state;
- marking the ThreadContext dirty then writes the stale CR3 back into KVM.

The earlier bounded run that passed this point was scheduling-dependent and
does not establish correctness.

Revised root cause: activation has two distinct cases that require different
state ownership:

- a cloned context has an authoritative gem5 ThreadContext and must discard
  stale KVM state before forcing a KVM update;
- a normally suspended KVM context, such as one waking from `HLT`, has
  authoritative KVM state and must synchronize it before re-entry.

No additional source change was made for this result.

Next code change: make `BaseKvmCPU::activateContext()` branch on
`tc->getUseForClone()`. For a clone, clear stale `kvmStateDirty` without
calling `syncThreadContext()`, then set `threadContextDirty=true`. For normal
reactivation, retain `syncThreadContext()` before marking the context dirty.
Extend the regression to enforce both paths.

The clone-state activation fix was revised and implemented with a red-green
regression:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kvm_context_activation_preserves_clone_state
```

`BaseKvmCPU::activateContext()` now distinguishes state ownership:

```cpp
if (tc->getUseForClone()) {
    kvmStateDirty = false;
} else {
    syncThreadContext();
}
tc->setUseForClone(false);
threadContextDirty = true;
```

For the first activation after an SE clone, the parent-derived gem5
ThreadContext is authoritative, so stale startup KVM state is discarded
without copying it back. The clone marker is then cleared so later futex,
signal, or `HLT` wakeups use the normal KVM-state synchronization path.
Every path forces the resulting ThreadContext into KVM before the scheduled
tick.

This replaces the earlier unconditional `syncThreadContext()` implementation,
which was disproved by the repeated `0x3d151c` page fault.

Verification completed:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 22 tests passed.

```bash
python3 -m py_compile \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
git diff --check
```

Result: both commands completed successfully.

Files changed for this revision:

- `src/cpu/kvm/base.cc`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document.

No gem5 build or full simulation was started by Codex.

Next diagnostic step: rebuild gem5 on the KVM host and repeat the bounded
native `--stage memset` run. The cloned worker must pass the former
`0x3d151c` fault consistently across repeated runs before increasing the
simulation bound.

The revised clone-state fix still reproduced the identical page fault on the
external KVM host. This means the fix is not validated and must not be
treated as complete. Before changing the implementation again, verify that
the KVM host contains the intended `activateContext()` source and that its
`gem5.opt` was rebuilt after that source change.

Do not replace the complete `src/cpu/kvm/base.cc`: it is common KVM
infrastructure and a whole-file replacement can silently mix source versions.

Next diagnostic step: capture the KVM host's focused `git diff` for
`src/cpu/kvm/base.cc`, the exact `activateContext()` body, source/binary
timestamps, and the final build output. If the intended branch is present in
the rebuilt binary, add temporary activation diagnostics for clone flag,
dirty flags, and CR3 rather than making another behavioral change.

The KVM host confirmed that the intended `BaseKvmCPU::activateContext()`
branch was present and that `build/VEGA_X86/gem5.opt` was newer than
`src/cpu/kvm/base.cc`. Therefore, the repeated page fault was not caused by
editing the wrong source tree or running a stale binary.

Source inspection exposed an earlier ordering error in `doClone()`:

- `ctc->setUseForClone(true)` is followed by `cp->initState()`;
- `Process::initState()` immediately called `tc->activate()`;
- only after that activation did `p->clone()` and `OS::archClone()` install
  the parent's address space, registers, and CR3;
- the final `ctc->activate()` was a no-op because the context was already
  active.

This rejects the previous assumption that the clone-aware branch in
`BaseKvmCPU::activateContext()` observed the post-`archClone()` state. It was
executing before that state existed.

A red-green source regression was added and run:

```bash
PYTHONPATH=src/python python3 -m unittest \
  tests.pyunit.stdlib.test_se_viper_multigpu.SEViperMultiGPUTest.test_process_init_defers_clone_context_activation
```

Before the implementation change, the test failed because
`Process::initState()` had no clone guard. `Process::initState()` now skips
the early `tc->activate()` when `tc->getUseForClone()` is true. `doClone()`'s
existing final activation therefore occurs only after `archClone()` and
`returnInto()` have installed the authoritative child state. The existing
KVM clone activation branch then discards stale startup KVM state and marks
the copied ThreadContext state for synchronization into KVM.

Verification completed:

```bash
PYTHONPATH=src/python python3 -m unittest \
  tests.pyunit.stdlib.test_se_viper_multigpu.SEViperMultiGPUTest.test_process_init_defers_clone_context_activation \
  tests.pyunit.stdlib.test_se_viper_multigpu.SEViperMultiGPUTest.test_kvm_context_activation_preserves_clone_state
```

Result: 2 tests passed.

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 23 tests passed.

```bash
python3 -m py_compile tests/pyunit/stdlib/test_se_viper_multigpu.py
git diff --check -- \
  src/sim/process.cc \
  src/cpu/kvm/base.cc \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
```

Result: both commands completed successfully.

Files changed in this debugging step:

- `src/sim/process.cc`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document.

No gem5 build or full simulation was started by Codex.

Single next diagnostic step: rebuild gem5 on the KVM host, then repeat the
bounded `hip_api_smoke --stage memset` run. Confirm that repeated runs pass
the former cloned-worker fault at `0x3d151c` before investigating any later
stall.

After rebuilding with the deferred clone activation fix, the user repeated
the bounded native KVM command twice:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hip-memset-kvm-debug \
  --listener-mode=off \
  --debug-flags=SyscallAll,GPUDriver,HSAPacketProcessor,GPUCommandProc,GPUDisp \
  --debug-file=hip-memset-kvm.trace \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type kvm \
  --max-ticks 100000000000 \
  --app tests/test-progs/gpu/hip-api-smoke/hip_api_smoke \
  --opts='--stage memset' \
  --rocm-path /home/deson/rocm-4.0.1
```

Observable result: neither run reproduced the page fault at `0x3d151c`.

Confirmed fact: deferring clone activation until after `archClone()` removes
the previously repeatable cloned-worker address-space fault across two
consecutive runs. The clone activation ordering hypothesis is accepted for
that failure. This does not yet establish that native `hipMemset` completes.

No files other than this status document were changed for this validation,
and Codex did not run a gem5 build or simulation.

Single next diagnostic step: compare the final application marker and exit
reason from the two runs. Determine whether both reached
`[hip_api_smoke] end hipMemset(...)`, hit the configured tick limit, or remain
inside ROCclr/COMGR.

Both post-fix validation runs had the same bounded outcome:

```text
src/sim/syscall_emul.cc:97: warn: ignoring syscall sched_yield(...)
src/sim/process.cc:348:
warn: Process::allocateMem: addr 0x7ffff6acd000 already mapped
Exiting @ tick 100000000000 because simulate() limit reached.
```

Neither run printed `[hip_api_smoke] end hipMemset(...)`. There was no panic,
and the former cloned-worker page fault did not recur.

This restores the classification established by the earlier syscall and GPU
trace: after clone correctness is fixed, the native ROCclr path still spends
the 100-billion-tick window in CPU-side ROCclr/COMGR/LLVM work before the
first observed AQL/GPU dispatch. The ignored `sched_yield` warning and
`allocateMem already mapped` warning are not treated as the first blocker:
the former is an SE syscall approximation, and the latter is the existing
multi-thread same-page race handling path.

No source files were changed for this result other than this status document.
Codex did not run a gem5 build or simulation.

Single next diagnostic step: repeat the now-corrected KVM run with
`--max-ticks=500000000000` and only the GPU submission debug flags, omitting
`SyscallAll`. Stop at the first `end hipMemset`, AQL/GPU dispatch, new fatal
error, or the new limit. The previous attempted longer run was terminated by
the now-fixed clone page fault and therefore did not complete this
experiment.

The post-fix 500-billion-tick run again reproduced the same fault:

```text
src/arch/x86/linux/se_workload.cc:166:
panic: Page fault at addr 0x3d151c
    rsp: 0x7ffdee859dc0
    rip: 0x7ffff7fbd573
    err_code: 0x6
Program aborted at tick 42788162007
```

This invalidates the conclusion that deferred clone activation fully fixes
the fault. Two 100-billion-tick passes only showed that the failure is
scheduling-dependent.

No further behavioral fix has been made. The page-fault panic now records:

- context ID and process ID;
- whether the process page table already contained the faulting address
  before `fixupFault()`;
- CR3 and FS base;
- RDI, RSI, and RDX at the fault.

This distinguishes two remaining classes without another speculative change:

- `pte_mapped_before_fixup=1` indicates that KVM faulted on an address gem5
  already considers mapped, pointing to KVM TLB/page-table visibility;
- an unexpected CR3 or FS base, or a bad pointer argument register, points
  back to clone architectural/TLS state.

Files changed for this diagnostic step:

- `src/arch/x86/linux/se_workload.cc`
- this status document.

Verification completed:

```bash
git diff --check -- src/arch/x86/linux/se_workload.cc
```

Result: completed successfully. Codex did not build gem5 or run a simulation.

Single next diagnostic step: rebuild gem5 and rerun the same native KVM
`--stage memset` command until either the enriched page-fault panic appears
or `hipMemset` completes. Record the new diagnostic fields; do not change
page-fault handling before that evidence is available.

The first diagnostic build reproduced the page fault, but the added panic
message was malformed:

```text
Page fault at addr        context_id: %d
...
<bad format><bad format>...
```

The displayed stack values were shifted into the wrong labels and cannot be
used as evidence. This was a diagnostics-only bug caused by passing many
heterogeneous arguments through gem5's `cprintf`-based panic formatter.
It does not indicate a new guest failure.

The diagnostic was rewritten to construct the complete message with
`std::ostringstream` and pass only the resulting string through
`panic("%s", ...)`. Page-fault handling behavior remains unchanged.

Verification completed:

```bash
git diff --check -- src/arch/x86/linux/se_workload.cc
```

Result: completed successfully. Files changed:

- `src/arch/x86/linux/se_workload.cc`
- this status document.

Codex did not build gem5 or run a simulation.

Single next diagnostic step: rebuild gem5 and rerun the same native KVM
`--stage memset` command until the corrected diagnostic appears. Capture the
fields from `Page fault at addr` through `rdx`; the malformed run must not be
used to classify the fault.

The corrected diagnostic reproduced the fault with valid fields:

```text
Page fault at addr 0x3d151c
    rsp: 0x7ffdee859dc0
    rip: 0x7ffff7fbd573
    err_code: 0x6
    context_id: 1
    pid: 101
    pte_mapped_before_fixup: 0
    cr3: 0
    fs_base: 0x7ffdee85a700
    rdi: 0x7ffff6b668c0
    rsi: 0x7ffff6b672c0
    rdx: 0xfffffffffffffe98
```

Confirmed facts:

- the write target `0x3d151c` is neither a page-table mapping nor a lazy VMA,
  so this is not the previously considered same-page allocation/TLB race;
- the cloned worker's FS base matches its clone TLS address, so basic TLS
  installation succeeded;
- `cr3=0` is suspicious under the KVM configuration, which sets
  `useArchPT=true`, but the physical root page can in principle be allocated
  at address zero; it is not sufficient by itself to prove corruption;
- the fault is a user-mode non-present write (`err_code=0x6`), so the next
  task is to decode the faulting instruction and identify which register or
  displacement produced `0x3d151c`.

The page-fault diagnostic now additionally records:

- GS base and every general-purpose register;
- 16 bytes beginning at the faulting RIP;
- the process VMA list, including the object containing the RIP.

This remains diagnostics-only; no page-fault or clone behavior changed.

Verification completed:

```bash
git diff --check -- src/arch/x86/linux/se_workload.cc
```

Result: completed successfully. Files changed:

- `src/arch/x86/linux/se_workload.cc`
- this status document.

Codex did not build gem5 or run a simulation.

Single next diagnostic step: rebuild gem5 and reproduce once more. Capture
the complete panic from `Page fault at addr` through the VMA entry containing
the reported RIP. Decode `instruction_bytes` before making another clone or
page-table change.

The expanded diagnostic identified the exact failing instruction:

```text
rip: 0x7ffff7fbd573
rbx: 0x3d0f00
instruction_bytes: 87 83 1c 06 00 00 ...
```

The RIP belongs to `/usr/lib/x86_64-linux-gnu/libpthread.so.0`. With its
mapping base and ELF load offset, this is `start_thread+0x43`:

```asm
853f: mov    %rdi,%rbx
...
856c: callq  __ctype_init@plt
8571: xor    %eax,%eax
8573: xchg   %eax,0x61c(%rbx)
```

The effective address is:

```text
rbx + 0x61c = 0x3d0f00 + 0x61c = 0x3d151c
```

`0x3d0f00` is the clone flags value. The fault is therefore caused by RBX
holding clone flags instead of the pthread descriptor when `start_thread`
accesses `pd->cancelhandling`. This is not an unmapped-page or GPU request
problem.

`start_thread` explicitly saves its pthread descriptor in RBX before calling
`__ctype_init@plt`; `__ctype_init` itself does not use RBX. The remaining
state transition is the PLT path, including possible first-use lazy symbol
resolution. That path is scheduling-dependent and is the current focused
hypothesis.

No source code was changed for this analysis.

Single next diagnostic step: run the same KVM `--stage memset` workload with
one additional process environment setting:

```text
--env LD_BIND_NOW=1
```

This forces PLT resolution before worker creation. Repeat enough times to
cover the formerly intermittent fault. If RBX corruption disappears, isolate
KVM SE lazy-binding register preservation; if it remains, instrument RBX
before and after `__ctype_init@plt`.

The run with `--env LD_BIND_NOW=1` reproduced the identical failure:

```text
rip: 0x7ffff7fbd573
rbx: 0x3d0f00
instruction_bytes: 87 83 1c 06 00 00 ...
```

This rejects PLT lazy binding as the cause. The faulting effective address and
register corruption are unchanged.

The important control-flow fact remains:

```asm
start_thread:
    mov %rdi,%rbx
    ...
    call __ctype_init@plt
    ...
    xchg %eax,0x61c(%rbx)
```

The pthread descriptor is installed in RBX before the call, but the KVM state
observed at the fatal page fault contains the earlier clone flags value.
Therefore the next investigation must identify the first KVM SE page-fault
round trip after which RBX has regressed.

A compact diagnostic line is now emitted for every KVM SE page fault on a
nonzero context:

```text
KVM SE page fault: context=... pid=... addr=... rip=... rbx=... rsp=...
fs_base=...
```

This is diagnostics-only. It does not change clone, page-fault, or KVM state
handling. The line is assembled with `std::ostringstream` and passed as one
string to avoid the earlier formatter problem.

Verification completed:

```bash
git diff --check -- src/arch/x86/linux/se_workload.cc
```

Result: completed successfully. Files changed:

- `src/arch/x86/linux/se_workload.cc`
- this status document.

Codex did not build gem5 or run a simulation.

Single next diagnostic step: rebuild gem5, rerun the same `--stage memset`
test, and preserve all `KVM SE page fault:` lines before the final panic. The
first line where RBX changes from a pthread/TLS-region pointer to `0x3d0f00`
will identify the responsible KVM-to-gem5 state transition.

The page-fault trace contained only the final fatal page fault:

```text
KVM SE page fault: context=1 pid=101 addr=0x3d151c
rip=0x7ffff7fbd573 rbx=0x3d0f00 ...
```

There was no earlier child page fault. This rejects the hypothesis that a
successful KVM SE page-fault return restored stale RBX state.

The glibc clone wrapper explains why the stale value is specifically the
clone flags:

```asm
__clone:
    mov %rdx,%rdi       # flags become syscall argument 1
    ...
    syscall
    ...
    pop %rdi            # child start_thread argument
    call *%rax          # call start_thread
```

The child ThreadContext initially and legitimately contains the clone-time
register set, including `rbx=0x3d0f00`. `start_thread` later replaces RBX with
the pthread descriptor. The current hypothesis is that gem5 pushes the cached
initial ThreadContext back into KVM after KVM has executed that replacement.

Diagnostics were added at both register synchronization boundaries for
nonzero contexts:

```text
KVM register sync TC->KVM: context=... tc_rip=... tc_rbx=...
    kvm_rip=... kvm_rbx=...
KVM register sync KVM->TC: context=... kvm_rip=... kvm_rbx=...
    tc_rip=... tc_rbx=...
```

This directly reveals whether a stale TC-to-KVM write overwrites a newer RBX,
or whether KVM itself never observes the `start_thread` assignment.
No synchronization behavior was changed.

Verification completed:

```bash
git diff --check -- \
  src/arch/x86/kvm/x86_cpu.cc \
  src/arch/x86/linux/se_workload.cc
```

Result: completed successfully. Files changed:

- `src/arch/x86/kvm/x86_cpu.cc`
- this status document.

Codex did not build gem5 or run a simulation.

Single next diagnostic step: rebuild and rerun once, then preserve every
`KVM register sync` line for context 1 plus the final page-fault line. The
first TC-to-KVM line where `tc_rbx=0x3d0f00` but `kvm_rbx` is a pthread
pointer proves the stale-state overwrite.

The register synchronization trace rejected the stale-TC overwrite
hypothesis. Before the failing worker entered KVM, gem5 pushed:

```text
TC->KVM:
tc_rbx=0x7ffde6059700
kvm_rbx=0x7ffdee85a700
```

The TC value is the new worker's TLS/pthread region, while the old KVM value
belongs to the previous worker. After KVM execution, the next synchronization
reported:

```text
KVM->TC:
kvm_rbx=0x3d0f00
```

Therefore KVM execution, not a later gem5 TC-to-KVM overwrite, produced the
clone-flags value observed at `start_thread+0x43`.

The same run also showed several successful lazy page faults for the earlier
worker, proving that the compact page-fault diagnostics work and that the
fatal worker is a later clone reusing context 1.

Two additional diagnostics were added:

- `KVM SE clone entry` records `flags`, `new_stack`, the two 64-bit words at
  that stack address, and TLS. For glibc `__clone`, those words must be
  `start_thread` and the pthread descriptor.
- both KVM register-sync directions now include RSP and RDI in addition to
  RIP and RBX.

These fields distinguish an incorrect child stack/newStack handoff from
register corruption during KVM execution. No behavior was changed.

Verification completed:

```bash
git diff --check -- \
  src/arch/x86/kvm/x86_cpu.cc \
  src/sim/syscall_emul.hh
```

Result: completed successfully. Files changed:

- `src/arch/x86/kvm/x86_cpu.cc`
- `src/sim/syscall_emul.hh`
- this status document.

Codex did not build gem5 or run a simulation.

Single next diagnostic step: rebuild and rerun once. Capture every
`KVM SE clone entry` line and the final context-1 `TC->KVM`, `KVM->TC`, and
page-fault lines. Verify whether `child_argument` equals the new TLS/pthread
pointer and whether initial RSP points at `new_stack`.

The clone-entry trace confirmed a correct glibc child stack:

```text
new_stack=0x7ffdee859e70
child_function=0x7ffff7fbd530
child_argument=0x7ffdee85a700
tls=0x7ffdee85a700
```

The initial KVM register push also confirmed:

```text
tc_rsp=0x7ffdee859e70
tc_rdi=0x3d0f00
```

This is the expected state immediately after clone: RSP points to the
function/argument pair and RDI still contains the clone flags until glibc's
child trampoline executes its two pops.

After KVM execution, the page fault showed `rbx=0x3d0f00`. Since
`start_thread` copies its incoming RDI to RBX, the evidence implies that the
child trampoline entered `start_thread` with RDI still equal to the clone
flags despite the correct `child_argument` in gem5's virtual-memory view.

The clone diagnostic now additionally:

- translates `new_stack` to its guest-physical address;
- reads the two words directly through `System::physProxy`;
- reports `physical_function` and `physical_argument`.

This tests whether the architectural guest page table and physical memory
contain the same stack values as `SETranslatingPortProxy`, before considering
KVM backing-store registration or visibility defects.

No behavioral change was made.

Verification completed:

```bash
git diff --check -- \
  src/sim/syscall_emul.hh \
  src/arch/x86/kvm/x86_cpu.cc
```

Result: completed successfully. Files changed:

- `src/sim/syscall_emul.hh`
- this status document.

Codex did not build gem5 or run a simulation.

Single next diagnostic step: rebuild and rerun once, then capture the
`KVM SE clone entry` line. Compare `child_argument` with
`physical_argument`. If both equal TLS, inspect KVM's userspace backing
address for `stack_paddr`; if they differ, fix the gem5 physical-memory
visibility path first.

The clone entry reported:

```text
stack_paddr=0x1598e70
child_argument=0x7ffdee85a700
physical_argument=0x7ffdee85a700
```

Thus the software page-table translation and the value read through
`System::physProxy` agree. This still does not prove that KVM sees the same
bytes: `physProxy` is an address-routed functional access, while KVM directly
maps the `BackingStoreEntry::pmem` pointer selected by `kvmMap`.

The clone diagnostic now searches the physical-memory backing stores for the
KVM-mapped range containing `stack_paddr` and reads the two stack words
directly from that host pointer. It reports:

```text
kvm_stack_found
kvm_backing_start
kvm_function
kvm_argument
```

This is the exact host memory source supplied to
`KVM_SET_USER_MEMORY_REGION`, not another gem5 port access. No behavior was
changed.

Verification completed:

```bash
git diff --check -- src/sim/syscall_emul.hh
```

Result: completed successfully. Files changed:

- `src/sim/syscall_emul.hh`
- this status document.

Codex did not build gem5 or run a simulation.

Single next diagnostic step: rebuild and rerun once, then capture only the
`KVM SE clone entry` line. Compare `kvm_argument` with `physical_argument`.
If KVM backing contains `0x3d0f00` or another stale value, fix the Ruby/KVM
backing-store split; if it contains TLS, inspect the hardware PTE walk rooted
at the child's CR3.

The direct KVM backing-store read also matched:

```text
kvm_stack_found=1
kvm_backing_start=0
kvm_function=0x7ffff7fbd530
kvm_argument=0x7ffdee85a700
```

This rejects a Ruby/functional-memory versus KVM-host-backing split. The same
physical bytes passed to `KVM_SET_USER_MEMORY_REGION` contain the correct
function and pthread argument.

The remaining memory-path hypothesis is that the hardware page-table entries
rooted at the child's CR3 translate `new_stack` to a different physical
address than `pTable->translate()`. A raw four-level x86 page-table walk was
added after `archClone()` using the actual child CR3 and physical PTE bytes.
It reports:

```text
KVM SE hardware page walk:
cr3=...
l4_addr=... l4_entry=...
l3_addr=... l3_entry=...
l2_addr=... l2_entry=...
l1_addr=... l1_entry=...
walked_paddr=...
walked_function=...
walked_argument=...
```

This is diagnostics-only. No clone, KVM, or page-table behavior changed.

Verification completed:

```bash
git diff --check -- \
  src/arch/x86/linux/linux.hh \
  src/sim/syscall_emul.hh
```

Result: completed successfully. Files changed:

- `src/arch/x86/linux/linux.hh`
- this status document.

Codex did not build gem5 or run a simulation.

Single next diagnostic step: rebuild and rerun once, then capture only the
`KVM SE hardware page walk` and `KVM SE clone entry` lines. Compare
`walked_paddr` with `stack_paddr`, and `walked_argument` with TLS.

The external KVM host initially failed to compile the added page-walk
diagnostic:

```text
src/arch/x86/linux/linux.hh:138:
error: expected unqualified-id before 'namespace'
```

The current workspace source was checked around the class/function boundary
and compiled as a single generated object:

```bash
scons build/VEGA_X86/python/_m5/param_X86EmuLinux.o -j1
```

Result: the object compiled successfully. Therefore the diagnostic source in
the workspace is syntactically valid; the external error indicates that the
copied `linux.hh` lost a closing brace or otherwise differs around the end of
`archClone()`.

Immediately before `namespace guest_abi`, the external source must contain:

```cpp
        }
    }

    class SyscallABI {};
};

namespace guest_abi
```

No source behavior was changed for this result.

Single next diagnostic step: correct the external `linux.hh` closing braces,
rebuild, and rerun once to capture `KVM SE hardware page walk` and
`KVM SE clone entry`.

The corrected run showed that the raw hardware page-table walk also matches
all other memory views:

```text
cr3=0
valid=1
walked_paddr=0x1598e70
walked_function=0x7ffff7fbd530
walked_argument=0x7ffdee85a700
```

This rejects every examined stack-memory explanation:

- the clone ABI stack words are correct;
- the software page table translates to the expected physical page;
- `physProxy` reads the correct words;
- the KVM host backing contains the correct words;
- the actual hardware PTE walk rooted at CR3 reaches the same page and words.

The remaining ambiguity is inside `start_thread` itself. Its prologue:

```asm
push %r12
push %rbp
push %rbx
mov  %rdi,%rbx
sub  $0xa0,%rsp
mov  %rdi,0x8(%rsp)
```

leaves two decisive values in the interrupted user stack:

- `rsp+0x8` contains the RDI value received by `start_thread`;
- `rsp+0x98` contains the RBX value inherited before `mov %rdi,%rbx`.

The fatal diagnostic now records these as
`start_thread_saved_rdi` and `start_thread_saved_rbx`. This determines whether
the function entered with a bad RDI or whether RBX was corrupted after the
prologue.

Verification completed:

```bash
git diff --check -- \
  src/arch/x86/linux/se_workload.cc \
  src/arch/x86/linux/linux.hh \
  src/sim/syscall_emul.hh
scons build/VEGA_X86/python/_m5/param_X86EmuLinux.o -j1
```

Result: formatting checks passed and the generated X86EmuLinux object is up
to date/compilable. Codex did not run a simulation.

Single next diagnostic step: rebuild and reproduce once, then report only
`start_thread_saved_rdi` and `start_thread_saved_rbx` from the final panic.

The stack snapshot reported:

```text
start_thread_saved_rdi: 0x3d0f00
start_thread_saved_rbx: 0
```

`start_thread_saved_rdi` is decisive: `start_thread` received the clone flags
as its argument instead of the pthread descriptor. Therefore the faulting
`mov %rdi,%rbx` legitimately installed `0x3d0f00` in RBX; the corruption
occurred before or at entry to `start_thread`, not in `__ctype_init`.

The original saved-RBX offset was incorrect. Given the exact prologue and
the interrupted RSP, the pushed pre-entry RBX is at `rsp+0xa0`, not
`rsp+0x98`. The diagnostic was corrected; the earlier saved-RBX value must
not be used.

The next relevant state is RCX at clone time because the KVM SE syscall
handler returns with `sysret`, which takes its user return RIP from RCX. If
the child RCX targets `start_thread` directly, it bypasses glibc `__clone`'s
child trampoline:

```asm
pop %rax
pop %rdi
call *%rax
```

and enters `start_thread` with RDI still containing clone flags, exactly
matching the observed state.

Diagnostics now include RCX and RAX in both KVM synchronization directions,
and parent/child PC, RCX, RAX, and RDI in the hardware-page-walk line.

Verification completed:

```bash
git diff --check -- \
  src/arch/x86/linux/se_workload.cc \
  src/arch/x86/kvm/x86_cpu.cc \
  src/arch/x86/linux/linux.hh \
  src/sim/syscall_emul.hh
scons build/VEGA_X86/python/_m5/param_X86EmuLinux.o -j1
```

Result: formatting checks and the generated X86EmuLinux object compilation
passed. Codex did not run a simulation.

Single next diagnostic step: rebuild and rerun once, then capture the
`KVM SE hardware page walk` line and the first context-1 `TC->KVM` line.
Compare `child_rcx`/`tc_rcx` with the expected return address immediately
after glibc `__clone`'s syscall, rather than with `start_thread`.

The return-state diagnostic showed:

```text
parent_rcx=0x7ffff6aea345
child_rcx=0x7ffff6aea345
tc_rcx=0x7ffff6aea345
tc_rax=0
tc_rsp=0x7ffdee859e70
```

`0x7ffff6aea345` is exactly the instruction after the syscall in glibc
`__clone`. The child therefore returns to the expected trampoline with
`RAX=0` and the correct stack pointer; it does not jump directly to
`start_thread`.

Combined with the later user-stack snapshot, the full sequence is now:

- the child resumes at `__clone+0x35`;
- the child branch executes;
- stack depth at the fatal instruction proves that the two pops and indirect
  call completed;
- `start_thread_saved_rdi=0x3d0f00`, so `pop %rdi` supplied clone flags;
- the software page table, raw hardware PTE walk, physical-memory port, and
  KVM host backing all show TLS at `new_stack+8`.

The remaining coherent explanation is a stale KVM vCPU TLB translation for
the reused child stack virtual page. The vCPU reads another physical page
even though the current page tables and backing store are correct.

No source behavior was changed for this conclusion.

## 2026-06-24 hipMemset SDMA Selection Investigation

Question: under what conditions does ROCm 4.0.1 `hipMemset` request SDMA,
and does the current `hip_api_smoke --stage memset` run exercise the new
`SESDMAEngine`?

Commands run:

```bash
readelf -Ws /tmp/hip-rocclr/opt/rocm-4.0.1/hip/lib/libamdhip64.so.4.0.40001 \
  | rg "hipMemset|hsa_amd_memory_fill|hipMemcpy|hipMalloc|hipFree"

objdump -R /tmp/hip-rocclr/opt/rocm-4.0.1/hip/lib/libamdhip64.so.4.0.40001 \
  | c++filt \
  | rg "hsa_amd_memory_fill|hsa_memory_copy|hsa_amd_memory_async_copy|hsa_queue_create|hsa_signal"

objdump -Cd --start-address=0xb0020 --stop-address=0xb06f0 \
  /tmp/hip-rocclr/opt/rocm-4.0.1/hip/lib/libamdhip64.so.4.0.40001 \
  | c++filt

objdump -Cd --start-address=0x9adf0 --stop-address=0x9c200 \
  /tmp/hip-rocclr/opt/rocm-4.0.1/hip/lib/libamdhip64.so.4.0.40001 \
  | c++filt

rg "CREATE_QUEUE|type 1|type 2|Register SDMA|SDMA doorbell|SESDMAEngine|GPU_BLIT_ENGINE_TYPE|HSA_FORCE_SDMA" \
  m5out-hip-memset-sdma-debug m5out-hip-memset-sdma-force -n

rg "HSA_ENABLE_SDMA|HSA_FORCE_SDMA|GPU_BLIT_ENGINE_TYPE|PAL_DISABLE_SDMA" \
  m5out-hip-memset-sdma-debug/config.ini \
  m5out-hip-memset-sdma-force/config.ini -n
```

First relevant observable result:

```text
libamdhip64.so.4.0.40001 imports hsa_memory_copy,
hsa_amd_memory_async_copy, hsa_amd_memory_async_copy_rect, hsa_queue_create,
and hsa_signal APIs, but it does not import hsa_amd_memory_fill.
```

The disassembly of exported `hipMemset` at `0xb0020` shows that the normal
path calls the internal helper at `0x9adf0` with a 1-byte fill element and
the requested byte count. The helper constructs and submits a ROCclr memory
command object rather than directly calling ROCr `hsa_amd_memory_fill`.

The `m5out-hip-memset-sdma-force` run confirmed that the forced environment
was present:

```text
HSA_ENABLE_SDMA=1
HSA_FORCE_SDMA=1
```

but the KFD queue trace still showed only type-2 queues:

```text
AMDKFD_IOC_CREATE_QUEUE gpu_id 22124 type 2 ...
AMDKFD_IOC_CREATE_QUEUE gpu_id 22124 type 2 ...
```

No `type 1`, `Register SDMA`, or `SDMA doorbell` line appeared.

Facts confirmed:

- The current unmodified HIP `hipMemset` path does not exercise gem5's
  `SESDMAEngine`, because it never creates a KFD SDMA queue.
- Setting `HSA_FORCE_SDMA=1` in this run did not change queue creation from
  type 2 to type 1.
- The observed type-2 queues are consistent with ROCclr kernel/AQL blit setup,
  not the KFD SDMA queue path implemented by `SESDMAEngine`.
- ROCr still contains a separate `hsa_amd_memory_fill -> Runtime::FillMemory
  -> GpuAgent::DmaFill -> BlitSdma/BlitKernel` path, but HIP 4.0.1
  `hipMemset` is not directly using the exported `hsa_amd_memory_fill` symbol
  in the inspected library.

Hypotheses accepted:

- Current `hipMemset` stall is not caused by an `SESDMAEngine` packet decode
  bug; the engine is not reached.
- `HSA_FORCE_SDMA=1` is insufficient, at least through the observed HIP
  `hipMemset` path.

Hypotheses rejected:

- Rejected: `hipMemset` with `HSA_ENABLE_SDMA=1` necessarily creates a KFD
  SDMA queue.
- Rejected: the absence of SDMA doorbells is due to only missing
  `HSA_FORCE_SDMA=1`; the force run still created only type-2 queues.

Files changed:

- this status document only.

Verification completed:

- Read-only binary inspection of local ROCm 4.0.1 HIP/ROCr libraries.
- Read-only inspection of existing `m5out-hip-memset-sdma-debug` and
  `m5out-hip-memset-sdma-force` traces.
- No gem5 build or simulation was started by Codex.

Single next diagnostic step: add or run a direct HSA smoke test that calls
`hsa_amd_memory_fill` on a GPU allocation with `HSA_ENABLE_SDMA=1` and
`HSA_FORCE_SDMA=1`, then inspect whether it creates KFD queue type 1. If it
does, validate `SESDMAEngine` with that direct HSA path. If it still creates
only type 2, instrument ROCr `GpuAgent::InitDma/CreateBlitSdma` selection
conditions or use an ROCm interposition path to force direct SDMA queue
creation for the SE MVP.

## 2026-06-24 SE SDMA MVP Phase 1 Start

Decision accepted: add an independent SE-only `SESDMAEngine`, rather than
trying to reuse the full-system `src/dev/amdgpu/sdma_engine.*` object. The
full-system engine remains a reference for packet definitions and basic
semantics, but it is coupled to `AMDGPUDevice`, GPUVM/GART, MMIO register
state, PM4, and interrupt paths that are not present in SE mode.

Design and execution plan were recorded in:

- `docs/superpowers/specs/2026-06-24-se-sdma-mvp-design.md`
- `docs/superpowers/plans/2026-06-24-se-sdma-mvp.md`

Meaningful commands run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_viper_gpu_exposes_sdma_dma_port
```

First result: failed because `SESDMAEngine` was not present in
`src/dev/hsa/HSADevice.py`. This was the expected RED test.

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kfd_sdma_queue_type_is_not_registered_as_compute
```

First result: failed because `GPUComputeDriver` had no queue-backend split and
all queue types still flowed through `setDeviceQueueDesc()`. This was the
expected RED test.

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_hsa_packet_processor_routes_sdma_doorbells
```

Result after implementation: passed. The HSA packet processor now keeps an
`sdmaQueues` map and routes matching doorbells to `SESDMAEngine::writeDoorbell`
before falling back to the compute scheduler.

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_sdma_engine_has_phase1_packet_handlers
```

Result after implementation: passed. `SESDMAEngine` now has Phase 1 packet
handler entries for `NOP`, `FENCE`, `TRAP`, `CONST_FILL`, and linear `COPY`,
with unsupported opcodes/subopcodes fataling explicitly.

```bash
scons build/VEGA_X86/dev/hsa/se_sdma_engine.o -j1
```

First result: failed. `sdma_packets.hh` required `Addr` to be visible before
including the packet structs, and `DmaVirtCallback<T>` for packet structs
needed explicit zero-value struct initialization rather than the default
integer `0`.

Accepted fix: include `base/types.hh` before `sdma_packets.hh` in
`se_sdma_engine.hh`, and pass `sdmaFence{}`, `sdmaTrap{}`,
`sdmaConstFill{}`, and `sdmaCopy{}` to the callbacks.

```bash
scons build/VEGA_X86/dev/hsa/se_sdma_engine.o -j1
```

Result after fix: passed.

```bash
scons build/VEGA_X86/dev/hsa/hsa_packet_processor.o \
      build/VEGA_X86/gpu-compute/gpu_compute_driver.o -j1
```

Result: both modified objects compiled. Only existing host/toolchain warnings
were reported, plus an unrelated `mxfp_convert.hh` unused-parameter warning.

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu
```

Result: all 29 tests passed.

```bash
git diff --check
```

Result: passed.

Facts confirmed:

- Each SE GPU now instantiates a `SESDMAEngine`.
- The SDMA engine exposes a DMA port through `SEVegaGPU.get_cpu_dma_ports()`,
  so the existing SE Viper Ruby hierarchy will attach it to the same GPU
  cluster as the GPU command processor and HSA packet processor DMA ports.
- `AMDKFD_IOC_CREATE_QUEUE` now distinguishes compute/AQL queues from
  `KFD_IOC_QUEUE_TYPE_SDMA`.
- `KFD_IOC_QUEUE_TYPE_SDMA_XGMI` is explicitly marked as Phase 2 and fails
  loudly instead of being misregistered as a compute queue.
- SDMA doorbells are routed by queue ID through the existing SE doorbell page.
- `SESDMAEngine` uses the SE process page table through
  `translateRange()` and issues memory operations through `dmaReadVirt()` and
  `dmaWriteVirt()`.

Hypotheses accepted:

- The correct MVP integration point is a new SE HSA-side DMA engine attached
  to each SE GPU's existing Ruby DMA-port grouping.
- Queue type dispatch must be fixed before any `hipMemset` validation; without
  this, ROCm SDMA queues would still be interpreted as compute/AQL queues.

Hypotheses rejected:

- Reusing the full-system `SDMAEngine` directly is not the MVP path because it
  would require importing unrelated FS GPU device state into SE mode.

Files changed for this step:

- `docs/superpowers/specs/2026-06-24-se-sdma-mvp-design.md`
- `docs/superpowers/plans/2026-06-24-se-sdma-mvp.md`
- `src/dev/hsa/HSADevice.py`
- `src/dev/hsa/SConscript`
- `src/dev/hsa/se_sdma_engine.hh`
- `src/dev/hsa/se_sdma_engine.cc`
- `src/dev/hsa/hsa_packet_processor.hh`
- `src/dev/hsa/hsa_packet_processor.cc`
- `src/gpu-compute/gpu_compute_driver.hh`
- `src/gpu-compute/gpu_compute_driver.cc`
- `src/python/gem5/components/devices/gpus/se_viper_gpu.py`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`

Verification completed:

- source-level SE multi-GPU pyunit regression: passed;
- focused compile of new and modified objects: passed;
- whitespace/conflict check with `git diff --check`: passed.

Important limitation:

- This is not yet an end-to-end `hipMemset` pass. The engine has Phase 1
  packet execution scaffolding and simple DMA-based `CONST_FILL`/linear
  `COPY`, but it has not yet been validated against ROCm's actual SDMA ring
  packets in a rebuilt full gem5 binary.
- The current implementation intentionally keeps `HSA_ENABLE_SDMA=0` as the
  default. Use `--env HSA_ENABLE_SDMA=1` only for targeted SDMA testing after
  rebuilding gem5.

Single next diagnostic step:

The user should rebuild `build/VEGA_X86/gem5.opt`, then run the focused KVM
memset test with SDMA enabled:

```bash
build/VEGA_X86/gem5.opt -d m5out-hip-memset-sdma \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type kvm \
  --app tests/test-progs/gpu/hip-api-smoke/hip_api_smoke \
  '--opts=--stage memset' \
  --rocm-path /home/deson/rocm-4.0.1 \
  --env HSA_ENABLE_SDMA=1
```

Expected first observable result if queue creation and doorbell routing work:

```text
KFD create queue ... type 1
SDMA doorbell queue ...
```

If it fails, collect the first fatal or panic from `simerr.txt`; do not rerun
with longer ticks until the first unsupported SDMA opcode or packet-format
issue is identified.

## 2026-06-24 SE SDMA Build Fix: Params create

The user's full gem5 link failed with:

```text
undefined reference to `gem5::SESDMAEngineParams::create() const'
```

Root cause:

- `HSADevice.py` generated `SESDMAEngineParams` with a declared
  `create() const` method;
- `src/dev/hsa/se_sdma_engine.cc` defined the `SESDMAEngine` constructor but
  did not define `SESDMAEngineParams::create() const`;
- once `create()` was added, compilation exposed the next required SimObject
  condition: `DmaVirtDevice` derives from `PioDevice`, so `SESDMAEngine` also
  had to implement `getAddrRanges()`, `read()`, and `write()`.

Files changed:

- `src/dev/hsa/se_sdma_engine.hh`
- `src/dev/hsa/se_sdma_engine.cc`

Implemented fix:

- `SESDMAEngineParams::create() const` now returns `new SESDMAEngine(*this)`;
- `SESDMAEngine` now implements empty PIO address ranges and returns
  bad-address responses for unexpected PIO reads/writes.

Verification completed:

```bash
scons build/VEGA_X86/gem5py_m5 build/VEGA_X86/dev/hsa/se_sdma_engine.o -j1
```

Result: passed. This specifically verifies the previously missing
`SESDMAEngineParams::create()` link path.

```bash
git diff --check
```

Result: passed.

Attempted but not completed in this workspace:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_sdma_engine_has_phase1_packet_handlers \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_viper_gpu_exposes_sdma_dma_port
```

Result: not run because `build/VEGA_X86/gem5.opt` is not present in this
workspace at the time of this note. Only `build/VEGA_X86/gem5py_m5` exists.

Single next diagnostic step:

The user should resume the full `scons build/VEGA_X86/gem5.opt` build. If a
new error appears, report the first compile or link error only.

Follow-up link result from the user's full build:

```text
multiple definition of `gem5::SESDMAEngineParams::create() const';
build/VEGA_X86/python/_m5/param_SESDMAEngine.o: ... first defined here
```

Root cause:

- This gem5 build auto-generates `SESDMAEngineParams::create() const` in
  `param_SESDMAEngine.o`;
- the manually added `SESDMAEngineParams::create() const` in
  `src/dev/hsa/se_sdma_engine.cc` therefore caused a duplicate symbol during
  full `gem5.opt` linking.

Accepted correction:

- remove the manual `SESDMAEngineParams::create() const` definition;
- keep the `getAddrRanges()`, `read()`, and `write()` implementations because
  they are required to make `SESDMAEngine` a concrete `DmaVirtDevice`.

Verification completed:

```bash
scons build/VEGA_X86/dev/hsa/se_sdma_engine.o -j1
```

Result: passed.

```bash
git diff --check
```

Result: passed.

Single next diagnostic step:

Resume the full `scons build/VEGA_X86/gem5.opt` build. If it fails again,
report the first new compile/link error.

## 2026-06-24 SE SDMA Build Fix: PIO port connection

The first SE SDMA runtime configuration attempt failed with:

```text
src/dev/io_device.cc:62: panic: Pio port of board.gpus0.se_sdma_engine not connected to anything!
```

Root cause:

- `SESDMAEngine` inherits from `DmaVirtDevice`;
- `DmaVirtDevice` also owns a `PioDevice` response port;
- even though `SESDMAEngine::getAddrRanges()` returns an empty range list,
  gem5 still requires the PIO port to be connected;
- `SEVegaGPU.connect_iobus()` connected `gpu_cmd_proc.pio` and
  `gpu_cmd_proc.hsapp.pio`, but did not connect `se_sdma_engine.pio`.

Accepted fix:

- connect `self.se_sdma_engine.pio = iobus.mem_side_ports` in
  `src/python/gem5/components/devices/gpus/se_viper_gpu.py`.

Regression test added:

- `SEViperMultiGPUTest.test_se_sdma_engine_pio_port_is_connected_to_iobus`

Verification completed:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_sdma_engine_pio_port_is_connected_to_iobus
```

Result: passed.

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu
```

Result: all 30 tests passed.

```bash
git diff --check
```

Result: passed.

Single next diagnostic step:

Rerun the same `hipMemset` SDMA test. If it fails again, report the first new
panic/fatal line; the previous unconnected PIO port panic should not recur.

## 2026-06-24 hipMemset SDMA debug: no SDMA queue requested

The user reran the `hipMemset` SDMA debug test in:

- `m5out-hip-memset-sdma-debug`

The previous PIO port panic did not recur. The run reached:

```text
[hip_api_smoke] begin hipMemset(device_data, memset_byte, bytes)
```

and then repeatedly emitted ignored `mprotect` warnings.

Inspection command:

```bash
rg -n "CREATE_QUEUE|AMDKFD_IOC_CREATE_QUEUE|queue|type 1|type 3|SDMA|doorbell|hipMemset|HSA_ENABLE_SDMA" \
  m5out-hip-memset-sdma-debug -S
```

Relevant result:

```text
AMDKFD_IOC_CREATE_QUEUE gpu_id 22124 type 2 ...
AMDKFD_IOC_CREATE_QUEUE queue_id 0 ...
AMDKFD_IOC_CREATE_QUEUE gpu_id 22124 type 2 ...
AMDKFD_IOC_CREATE_QUEUE queue_id 1 ...
```

Facts confirmed:

- `HSA_ENABLE_SDMA=1` is present in `config.ini`;
- both `SESDMAEngine` instances are present in `config.ini/config.json`;
- topology properties expose:
  - `num_sdma_engines 2`
  - `num_sdma_xgmi_engines 1`
  - `num_sdma_queues_per_engine 8`
- no `KFD_IOC_QUEUE_TYPE_SDMA` (`type 1`) queue was created;
- no `SDMA doorbell` reached `SESDMAEngine`;
- HIP/ROCr created only `KFD_IOC_QUEUE_TYPE_COMPUTE_AQL` (`type 2`) queues
  before the `hipMemset` stall.

Accepted hypothesis:

- the current `hipMemset` path is not exercising `SESDMAEngine` yet. It is
  still using the ROCm compute/AQL blit path and/or CPU-side runtime path.

Rejected hypothesis:

- this is not an SDMA packet decode or SDMA DMA-completion bug, because no
  SDMA queue or doorbell reached the engine.

Additional local evidence:

```bash
strings -a -t x \
  /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib/libhsa-runtime64.so.1.2.40001 \
  | rg -i "HSA_.*SDMA|SDMA_"
```

The ROCr library contains SDMA blit symbols and environment strings including
`HSA_ENABLE_SDMA` and a `HSA_FORCE_SDMA...` prefix. This suggests that merely
enabling SDMA may not force `hipMemset` to choose SDMA.

Single next diagnostic step:

Rerun the same focused test with both SDMA env knobs:

```bash
build/VEGA_X86/gem5.opt -d m5out-hip-memset-sdma-force \
  --debug-flags=GPUDriver,SESDMAEngine \
  --debug-file=sdma.trace \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type atomic \
  --app tests/test-progs/gpu/hip-api-smoke/hip_api_smoke \
  '--opts=--stage memset' \
  --rocm-path /home/deson/rocm-4.0.1 \
  --env HSA_ENABLE_SDMA=1 \
  --env HSA_FORCE_SDMA=1
```

Then inspect:

```bash
rg "CREATE_QUEUE|type 1|type 2|SDMA|doorbell|Register SDMA" \
  m5out-hip-memset-sdma-force/sdma.trace
```

If `type 1` still does not appear, the SE SDMA MVP cannot validate `hipMemset`
through unmodified HIP `hipMemset`; the next implementation target should be
either a direct HSA SDMA smoke test or a higher-level ROCm/HIP interception
strategy for memset/memcpy.

## 2026-06-23: bounded KVM clone single-step trace implemented

The recommended diagnostic approach was implemented without changing clone
semantics. A clone child is recognized when its ThreadContext is about to
return through the SE syscall trampoline with:

```text
rip == syscallCodeVirtAddr + 10
rax == 0
stack[0] != 0
stack[1] == fs_base
```

At that point, `KVM_SET_GUEST_DEBUG` enables
`KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP`. `KVM_EXIT_DEBUG` is now
dispatched by `BaseKvmCPU` and handled by `X86KvmCPU`.

Each debug exit records:

```text
KVM clone single-step:
context step rip rsp rax rdi rbx
stack_translated stack_paddr stack0 stack1
kvm_stack_found kvm_stack0 kvm_stack1
```

The trace stops when RIP reaches the child function (`start_thread`) or after
32 instructions, whichever occurs first. Guest debugging is explicitly
disabled at that point. This bounds both runtime overhead and log volume.

A source regression was first run before the x86 implementation and failed
because `KVM_GUESTDBG_SINGLESTEP` and `X86KvmCPU::handleKvmExitDebug()` were
absent. After implementation, the exact focused command was:

```bash
PYTHONPATH=build/VEGA_X86/python:src/python python3 -m unittest \
  tests.pyunit.stdlib.test_se_viper_multigpu.SEViperMultiGPUTest.test_x86_kvm_supports_bounded_clone_single_step_trace
```

Result: one test passed.

The affected C++ objects were compiled with:

```bash
scons build/VEGA_X86/cpu/kvm/base.o -j1
scons build/VEGA_X86/arch/x86/kvm/x86_cpu.o -j1
```

Result: both objects compiled successfully. An initial attempt to run both
SCons commands concurrently raced while generating parameter headers and
reported:

```text
build/VEGA_X86/params/BaseInterrupts.hh: No such file or directory
```

Sequential execution rejected that as a source-code failure.

The complete existing regression file was run with the built gem5 Python
environment:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 24 tests passed.

Formatting verification:

```bash
git diff --check
```

Result: completed successfully.

Files changed in this diagnostic step:

- `src/cpu/kvm/base.hh`
- `src/cpu/kvm/base.cc`
- `src/arch/x86/kvm/x86_cpu.hh`
- `src/arch/x86/kvm/x86_cpu.cc`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- `docs/debug/se-multigpu-status.md`

Facts confirmed:

- the generic KVM run loop can now service `KVM_EXIT_DEBUG`;
- the x86 implementation compiles against the available host KVM headers;
- clone tracing is armed only for a validated child-return stack shape;
- tracing is bounded and automatically disabled;
- no full gem5 simulation was run locally.

Single next diagnostic step: on the KVM host, rebuild `gem5.opt`, reproduce
the `hipMemset` case once, and save all lines matching:

```text
KVM clone single-step
```

Those lines will identify the first instruction after which RAX, RDI, RSP, or
the KVM-backed stack diverges.

## 2026-06-23: clone page-fault root cause found and scratch race fixed

The KVM host produced the first bounded single-step trace:

```text
KVM clone single-step armed: context=1 target=0x7ffff7fbd530 remaining=32
step=1 rip=0xffff800000000014 rax=0
step=2 rip=0xffff800000000017 rax=0
step=3 rip=0xffff80000000001a rax=0
step=4 rip=0xffff800000000024 rax=0x65
```

Mapping those addresses to the syscall handler established:

```text
+0x0a..+0x14  mov %rax,(syscallDataBuf)
+0x14..+0x17  mov %cr3,%rax
+0x17..+0x1a  mov %rax,%cr3
+0x1a..+0x24  mov (syscallDataBuf),%rax
+0x24         sysret
```

The first incorrect transition is therefore proven: RAX is the correct clone
child return value `0` through the CR3 reload, then restoring it from the
fixed scratch address changes it to `0x65` (decimal 101, the clone child PID
returned in the parent).

Accepted root cause:

- `syscallDataBuf = syscallCodeVirtAddr + 0x100` is shared by every KVM CPU;
- one CPU can overwrite the saved syscall return value while another CPU is
  reloading CR3;
- the child then returns to glibc with the parent's nonzero clone result and
  follows the parent path with the child stack/register state;
- this explains the intermittent `0x3d151c` fault, the sensitivity to CPU
  scheduling and `sim_quantum`, and why repeated runs were required.

The single-step trace stopped after step 4 because `sysret` restores RFLAGS
from R11 and removes the trap flag used by KVM guest single-step. Extending
the trace across `sysret` is no longer necessary because the first bad state
transition occurred before it.

A failing regression was added and run before changing the handler:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kvm_se_syscall_return_uses_vcpu_local_cr2_scratch
```

First relevant result:

```text
FAIL: 'syscallDataBuf' unexpectedly found in the system call handler
```

A stack-based `push %rax` / `pop %rax` replacement was considered and
rejected before finalization because it leaves a write in the x86-64 user
red zone below RSP and could corrupt live user data.

The implemented handler uses the vCPU-local CR2 register as temporary
storage:

```text
mov %rax,%cr2
mov %cr3,%rax
mov %rax,%cr3
mov %cr2,%rax
sysret
```

CR2 is private to each vCPU and hardware replaces it with the faulting
address before the SE page-fault handler consumes it. The handler no longer
uses shared memory or modifies the user stack.

Verification completed:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kvm_se_syscall_return_uses_vcpu_local_cr2_scratch \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_x86_kvm_supports_bounded_clone_single_step_trace
```

Result: both focused tests passed.

```bash
scons build/VEGA_X86/arch/x86/process.o -j1
```

Result: `process.o` compiled successfully.

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 25 tests passed.

```bash
git diff --check
```

Result: completed successfully.

An optional `ndisasm` check could not run because `ndisasm` is not installed
on the local host. This does not affect the object compilation or regression
results.

Files changed in this step:

- `src/arch/x86/process.cc`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- `docs/debug/se-multigpu-status.md`

Single next diagnostic step: rebuild `gem5.opt` on the KVM host and rerun the
same `hipMemset` stage repeatedly. The single-step trace should retain
`rax=0` after the final `mov %cr2,%rax`, and the run should reach
`end hipMemset` without the `0x3d151c` page fault.

The KVM host then ran the `hipMemset` stage four consecutive times.

Observed result:

```text
RAX remained 0 in every run.
No 0x3d151c page fault occurred.
No run reached end hipMemset.
```

This confirms the CR2 scratch change fixed the shared-scratch corruption and
the intermittent clone page fault. It does not yet establish the cause of the
remaining `hipMemset` wait.

The test binary still contains the temporary `KVM_SET_GUEST_DEBUG`
single-step diagnostic. The observed trace stops at the instruction before
`sysret` and never prints:

```text
KVM clone single-step disabled
```

Therefore guest-debug mode remains armed without completing its intended
lifecycle. The current hang result is not a clean validation of normal KVM
execution.

No source files were changed for this observation.

Single next diagnostic step: remove only the temporary bounded KVM
single-step instrumentation, retain the CR2 scratch fix and existing passive
diagnostics, rebuild `gem5.opt`, and rerun `hipMemset`. If it still fails to
reach `end hipMemset`, inspect the final syscall/futex activity from that
clean run.

## 2026-06-23: temporary KVM single-step diagnostic removed

The temporary guest-debug instrumentation was removed as recommended. The
cleanup includes:

- the `KVM_EXIT_DEBUG` dispatch and default handler in `BaseKvmCPU`;
- the x86 `KVM_SET_GUEST_DEBUG` enable/disable operations;
- the automatic clone single-step state fields;
- the per-instruction `KVM clone single-step` logger;
- the regression that required the diagnostic to be present.

The CR2 syscall-return scratch fix, clone activation fixes, and passive
register/memory diagnostics remain unchanged.

A cleanup regression was first changed to require absence of all guest-debug
code and run with:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_x86_kvm_clone_single_step_diagnostic_is_removed
```

First relevant result before cleanup:

```text
FAIL: 'case KVM_EXIT_DEBUG:' unexpectedly found
```

After cleanup, the focused validation was:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_x86_kvm_clone_single_step_diagnostic_is_removed \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kvm_se_syscall_return_uses_vcpu_local_cr2_scratch
```

Result: both tests passed.

The affected objects were compiled sequentially:

```bash
scons build/VEGA_X86/cpu/kvm/base.o -j1
scons build/VEGA_X86/arch/x86/kvm/x86_cpu.o -j1
scons build/VEGA_X86/arch/x86/process.o -j1
```

Result: `base.o` and `x86_cpu.o` compiled successfully; `process.o` was
reported up to date after its previous successful CR2-fix compilation.

Complete regression:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 25 tests passed.

```bash
git diff --check
```

Result: completed successfully.

Files changed in this cleanup:

- `src/cpu/kvm/base.hh`
- `src/cpu/kvm/base.cc`
- `src/arch/x86/kvm/x86_cpu.hh`
- `src/arch/x86/kvm/x86_cpu.cc`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- `docs/debug/se-multigpu-status.md`

Single next diagnostic step: rebuild `gem5.opt` on the KVM host and rerun the
same `hipMemset` stage without guest-debug instrumentation. If it still does
not print `end hipMemset`, capture the last 100 syscall trace lines and the
final `simerr.txt` messages from that clean run.

## 2026-06-23: clean post-single-step run reaches CPU compilation phase

The clean KVM run remained active without printing `end hipMemset`. The
reported last 100 syscall lines end with:

```text
clone(... stack=0x7ffde6058e70)
Returned 102
set_robust_list(...)
Returned 0
sched_setaffinity(...)
Returned 0
```

Earlier in the same trace:

- `hipMalloc` allocates and maps GPU0 VRAM successfully;
- the first ROCclr worker thread initializes and exits normally;
- the second worker thread is created successfully;
- no futex call remains blocked at the trace boundary;
- no AQL packet, GPU command, dispatch, or completion is recorded.

The accompanying `simerr.txt` contains several demand page faults from the
worker. They return to execution and are not fatal panics. The previous
`0x3d151c` clone corruption does not recur.

This clean run reaches the same previously classified boundary as the
earlier KVM compilation experiment: after `sched_setaffinity`, the ROCclr
worker enters user-space COMGR/LLVM work that emits no syscalls for a long
period. Therefore an unchanged `SyscallAll` trace alone is not evidence of a
deadlock, and repeating another instruction trace would duplicate an
experiment already completed.

No source files were changed for this observation.

Exact non-invasive live check requested:

```bash
ps -C gem5.opt -o pid,etime,time,pcpu,stat
sleep 30
ps -C gem5.opt -o pid,etime,time,pcpu,stat
```

Single next diagnostic step: compare the two process samples. If CPU time
increases and utilization remains near one host CPU, continue the existing
run until `end hipMemset` or its configured max-ticks. If CPU time does not
increase or the process is persistently non-runnable, inspect host thread
states before changing code.

The requested live samples were:

```text
PID 75831  ELAPSED 05:13  TIME 00:05:12  CPU 99.8  STAT R+
PID 75831  ELAPSED 06:00  TIME 00:05:58  CPU 99.6  STAT R+
```

Across 47 seconds of wall time, gem5 accumulated 46 seconds of CPU time and
remained runnable at approximately 100 percent utilization. This confirms
active single-host-core computation, not a futex wait, HLT state, deadlock,
or missing GPU response. The classification remains ROCclr/COMGR/LLVM
CPU-side work before GPU submission.

No source files were changed.

Single next diagnostic step: leave the current run active until it prints
`end hipMemset`, emits the first AQL/GPU dispatch, reaches its configured
max-ticks, or reports a new fatal error. Do not rerun or add tracing while
this instance is making progress.

The clean run reached its configured 100-billion-tick limit without printing
`end hipMemset`. During the run, two process samples had already confirmed
that gem5 remained runnable at approximately 100 percent host CPU and
accumulated CPU time at wall-clock rate.

Final classification:

- the CR2 scratch fix remains effective; no `0x3d151c` page fault occurred;
- no deadlock, futex wait, HLT stall, or GPU-response wait was observed;
- native ROCclr `hipMemset` still spends the complete 100-billion-tick
  window in CPU-side initialization/COMGR work before the first recorded GPU
  dispatch.

This does not duplicate a completed post-fix 500-billion-tick experiment.
The earlier long-window attempts were terminated by the clone return-value
corruption that is now fixed, so no clean 500-billion-tick result exists.

No source files were changed for this result.

Single next diagnostic step: run one clean 500-billion-tick experiment with
only GPU submission debug flags and no `SyscallAll` or temporary KVM
register diagnostics. Stop at the first `end hipMemset`, first AQL/GPU
dispatch, new fatal error, or the limit.

## 2026-06-23: SE versus FS direction assessment

Repository inspection confirmed that full-system AMD GPU support is a real
alternative rather than a hypothetical rewrite:

- `configs/example/gpufs/runfs.py` lists Vega10, MI100, MI200, MI300X, and
  MI355X as GPUFS targets;
- `configs/example/gpufs/system/system.py` instantiates modeled SDMA engines
  and connects them through the amdgpu device path;
- `configs/example/gem5_library/x86-mi300x-multigpu.py` provides a two-GPU
  full-system example using the real Linux/amdgpu software stack.

However, the existing FS paths do not prove equivalence to the current SE
research platform:

- the checked-in multi-GPU FS example targets two MI300X devices, not the
  current Vega/gfx900 platform;
- its board, device, memory, and interconnect construction differ from the
  custom CPU/GPU cluster and XGMI Ruby topology;
- switching wholesale would require revalidating topology, address maps,
  memory routing, coherence assumptions, and experimental observability.

Recommended project split:

1. preserve the current SE branch as the topology/Ruby/XGMI research
   platform;
2. perform a bounded FS feasibility spike as the ROCm software-compatibility
   baseline;
3. do not begin a full migration unless FS first passes the required API and
   multi-GPU gates;
4. if FS cannot represent the target topology without major reconstruction,
   retain SE and implement the minimal SE SDMA feature.

Suggested FS gates:

- boot the supported GPU FS image and load amdgpu;
- pass `hipMalloc`, `hipMemset`, `hipMemcpy`, kernel launch, synchronization,
  and `hipFree`;
- enumerate and use two GPUs;
- demonstrate peer memory or another minimal cross-GPU operation;
- determine whether the target clustered Ruby/XGMI topology can replace the
  stock FS interconnect without changing the intended experiment.

No source files were changed for this assessment.

Single next project decision: approve or reject a bounded FS feasibility
spike before committing engineering effort to either a complete FS migration
or SE SDMA implementation.

The user confirmed that the primary research objective is multi-GPU
interconnect and cache-coherence behavior.

Architecture decision:

- keep SE mode as the primary research platform;
- preserve the explicit CPU/GPU cluster routers and XGMI Ruby topology;
- treat ROCm API support as enabling infrastructure for experiments, not as
  the primary fidelity target;
- do not migrate the main platform to FS merely to obtain broader ROCm API
  compatibility;
- use FS only as an optional behavioral reference for SDMA packets, driver
  interactions, or API outcomes.

This decision prioritizes direct control and observability of Ruby coherence,
remote cache invalidation, peer memory traffic, and inter-GPU routing. A
full-system migration would introduce Linux/amdgpu initialization and device
model complexity without by itself implementing the desired cross-GPU Ruby
coherence protocol.

Single next engineering direction: retain the SE topology and define a
minimal SE SDMA integration sufficient for `hipMemset` and `hipMemcpy`, then
return to explicit inter-GPU coherence experiments. Do not spend additional
long runs on the non-SDMA ROCclr blit-compilation path.

The possible influence of `sim_quantum` was reviewed. Stdlib sets
`root.sim_quantum` to 1 ms whenever KVM cores are present, but gem5 only
creates and uses global quantum synchronization when
`numMainEventQueues > 1`:

```cpp
if (numMainEventQueues > 1) {
    fatal_if(simQuantum == 0, ...);
    new GlobalSyncEvent(...);
    inParallelMode = true;
}
```

This SE Viper configuration deliberately overrides stdlib's per-KVM-core
event queues and places every KVM CPU on event queue 0 before
instantiation. Existing generated configuration files show
`eventq_index=0` throughout the inspected objects.

Conclusion: `sim_quantum` can affect timing and race probability in a true
multi-event-queue KVM run, but it should be inactive in the corrected
single-event-queue SE Viper configuration. It is not currently a supported
root-cause hypothesis for the intermittent clone fault.

No source files were changed for this result.

Single verification step on the external KVM run: inspect its generated
`config.ini` and confirm that all `board.processor.cores*.core` sections have
`eventq_index=0`. If any core has a nonzero value, then that run differs from
the intended configuration and `sim_quantum` must be reconsidered.

Single next diagnostic step: add a minimal x86 KVM SE validation that executes
`invlpg (%rsp)` in the syscall-return stub before `sysret`. If the repeated
clone fault disappears, the stale child-stack TLB hypothesis is confirmed;
then replace the diagnostic invalidation with the correctly scoped clone/vCPU
TLB invalidation mechanism.

The complete fatal stack snapshot additionally showed:

```text
start_thread_saved_rdi: 0x3d0f00
start_thread_saved_rbx: 0x7ffdee85a700
```

The corrected saved-RBX slot confirms that RBX was the expected pthread/TLS
pointer before `start_thread` assigned `RBX = RDI`. This independently proves
that the only bad input at function entry was RDI, loaded by glibc's
`pop %rdi` from the child stack.

The final KVM state is consistent with the page-fault handler:

```text
kvm_rsp=0xffff800000004fd0
kvm_rip=0xffff80000000500a
kvm_rbx=0x3d0f00
```

No hypothesis changes: the current page tables, raw hardware walk, physical
memory, and KVM backing all contain TLS at the stack argument slot, while the
vCPU load observes the old clone-flags value. A stale vCPU TLB translation is
the focused hypothesis.

Before changing behavior, the proposed validation design is:

- insert the four-byte `invlpg (%rsp)` instruction immediately before
  `sysret` in the KVM SE syscall-return blob;
- leave clone and page-table code unchanged;
- run repeated native KVM `--stage memset` attempts;
- treat disappearance of the deterministic `start_thread` bad-RDI fault as
  confirmation only, not as the final fix;
- remove or replace the inline invalidation after selecting a correctly
  scoped vCPU TLB flush mechanism.

No source behavior was changed for this update.

The user approved the focused stale-TLB validation. It was implemented with a
red-green source regression:

```bash
PYTHONPATH=src/python python3 -m unittest \
  tests.pyunit.stdlib.test_se_viper_multigpu.SEViperMultiGPUTest.test_kvm_se_syscall_return_invalidates_stack_tlb
```

Before the implementation, the test failed because the syscall-return blob
did not contain `invlpg (%rsp)`.

The KVM SE syscall-return stub in `src/arch/x86/process.cc` now executes:

```asm
mov %cr3,%rax
mov %rax,%cr3
mov syscallDataBuf,%rax
invlpg (%rsp)
sysret
```

Machine bytes added immediately before `sysret`:

```text
0f 01 3c 24
```

The existing absolute-address patch offsets in `syscallBlob` remain unchanged
because the new bytes were appended after both patched operands.

Verification completed:

```bash
PYTHONPATH=src/python python3 -m unittest \
  tests.pyunit.stdlib.test_se_viper_multigpu.SEViperMultiGPUTest.test_kvm_se_syscall_return_invalidates_stack_tlb \
  tests.pyunit.stdlib.test_se_viper_multigpu.SEViperMultiGPUTest.test_process_init_defers_clone_context_activation \
  tests.pyunit.stdlib.test_se_viper_multigpu.SEViperMultiGPUTest.test_kvm_context_activation_preserves_clone_state
```

Result: all 3 focused tests passed.

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Result: all 24 tests passed.

```bash
python3 -m py_compile tests/pyunit/stdlib/test_se_viper_multigpu.py
git diff --check
```

Result: completed successfully.

Files changed for this validation:

- `src/arch/x86/process.cc`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document.

Codex did not run a full gem5 build or simulation.

Single next diagnostic step: rebuild gem5 on the KVM host and repeat native
`hip_api_smoke --stage memset` enough times to cover the intermittent fault.
Record whether `start_thread_saved_rdi` remains `0x3d0f00`, whether the former
page fault disappears, and the final API marker or exit reason. Treat this
inline `invlpg` as hypothesis validation, not yet as the final fix.

The run with `invlpg (%rsp)` reproduced the same page fault. This rejects a
simple stale leaf-TLB translation for the child stack page.

The temporary `invlpg` bytes and their source regression were removed. They
must not be carried into a final fix.

The next diagnostic moves the stack-content check to the final TC-to-KVM
register synchronization immediately before the child vCPU first executes.
For a nonzero context at the KVM SE syscall handler, the synchronization line
now records:

- `stack0` and `stack1` through `SETranslatingPortProxy`;
- `physical_stack0` and `physical_stack1`;
- `kvm_stack0` and `kvm_stack1` read directly from the KVM-mapped backing;
- the stack physical address and translation/found flags.

This determines whether the parent or runtime rewrites the child stack after
clone setup but before the child enters KVM.

Verification completed:

```bash
git diff --check -- \
  src/arch/x86/kvm/x86_cpu.cc \
  src/arch/x86/process.cc \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
scons build/VEGA_X86/arch/x86/kvm/x86_cpu.o -j1
```

Result: formatting checks passed and `x86_cpu.o` compiled successfully.

```bash
PYTHONPATH=src/python python3 -m unittest \
  tests.pyunit.stdlib.test_se_viper_multigpu.SEViperMultiGPUTest.test_process_init_defers_clone_context_activation \
  tests.pyunit.stdlib.test_se_viper_multigpu.SEViperMultiGPUTest.test_kvm_context_activation_preserves_clone_state
```

Result: both focused regressions passed.

Files changed:

- `src/arch/x86/kvm/x86_cpu.cc`
- `src/arch/x86/process.cc` (temporary validation removed)
- `tests/pyunit/stdlib/test_se_viper_multigpu.py` (temporary validation removed)
- this status document.

Codex did not run a full gem5 build or simulation.

Single next diagnostic step: rebuild and rerun once, then capture the first
context-1 `KVM register sync TC->KVM` line. Compare `stack1`,
`physical_stack1`, and `kvm_stack1` with TLS at the actual execution boundary.

At the final TC-to-KVM boundary, all three stack views still contained the
correct pthread argument:

```text
stack1=0x7ffdee85a700
physical_stack1=0x7ffdee85a700
kvm_stack1=0x7ffdee85a700
```

Thus the child stack is not rewritten before KVM execution. Together with the
failed `invlpg` validation, the next distinction is whether the original
child stack changes during execution or whether the vCPU follows unexpected
trampoline instruction bytes.

The fatal diagnostic now reconstructs the original child stack address from
the `start_thread` prologue:

```text
original_new_stack = interrupted_user_rsp + 0xb0
```

and records:

```text
original_child_function
original_child_argument
```

This compares the stack words after the bad RDI has already reached
`start_thread` with their values immediately before KVM execution.

Verification completed:

```bash
git diff --check -- \
  src/arch/x86/linux/se_workload.cc \
  src/arch/x86/kvm/x86_cpu.cc
scons build/VEGA_X86/python/_m5/param_X86EmuLinux.o -j1
```

Result: formatting checks passed and the X86EmuLinux generated object remains
compilable. Codex did not run a simulation.

Single next diagnostic step: rebuild and reproduce once, then report only
`original_child_function` and `original_child_argument` from the final panic.

The final stack reconstruction reported:

```text
original_child_function: 0x7fffffffe5fe
original_child_argument: 0x7ffdee85a700
```

These labels assumed a normal indirect call. Re-evaluating the exact stack
arithmetic:

- the `start_thread` prologue consumes `0xb8` bytes;
- therefore its entry RSP is `fault_rsp + 0xb8`;
- with a normal `call *%rax`, that entry slot must contain return address
  `__clone+0x43`;
- the observed entry slot instead still contains TLS.

The first reported word is also explained by this alternative stack shape:
`push %r12` overwrote it with the current R12 value. The evidence therefore
indicates that `start_thread` was entered without the expected call return
address, i.e. the observed behavior resembles a jump rather than the
`call *%rax` in the host library disassembly.

To check the actual guest instruction image, the hardware-page-walk
diagnostic now records 24 bytes beginning at the child return RCX as
`clone_return_bytes`. The expected sequence includes:

```text
... 31 ed 58 5f ff d0 ...
```

where `ff d0` is `call *%rax`.

Verification completed:

```bash
git diff --check -- \
  src/arch/x86/linux/linux.hh \
  src/arch/x86/linux/se_workload.cc
scons build/VEGA_X86/python/_m5/param_X86EmuLinux.o -j1
```

Result: checks passed and the generated object compiled successfully.

Single next diagnostic step: rebuild and rerun once, then report only
`clone_return_bytes` from the `KVM SE hardware page walk` line.

The guest trampoline bytes were exactly correct:

```text
clone_return_bytes=
4885c07c137401c331ed585fffd04889c7b83c0000000f05
```

This includes the expected child path:

```text
31 ed       xor %ebp,%ebp
58          pop %rax
5f          pop %rdi
ff d0       call *%rax
```

The earlier final-stack values were read through gem5's functional memory
path. KVM writes during `pop`, `call`, and the `start_thread` prologue go
directly to the host backing mapped by KVM. The two views were compared before
execution, but not after KVM had modified the stack. Therefore those values
cannot yet establish that `call` was skipped or that `pop %rdi` read flags.

The final panic now reads the same four stack slots directly from the
KVM-mapped `BackingStoreEntry::pmem` and reports:

```text
kvm_start_thread_saved_rdi
kvm_start_thread_saved_rbx
kvm_original_child_function
kvm_original_child_argument
```

These must be compared with the existing functional-memory fields. No
behavior was changed.

Verification completed:

```bash
git diff --check -- \
  src/arch/x86/linux/se_workload.cc \
  src/arch/x86/linux/linux.hh
scons build/VEGA_X86/python/_m5/param_X86EmuLinux.o -j1
```

Result: checks passed and the generated object remains compilable.

Single next diagnostic step: rebuild and reproduce once, then report the four
new `kvm_*` stack fields from the final panic.

The direct KVM-backing values at the fatal page fault were:

```text
kvm_start_thread_saved_rdi: 0x3d0f00
kvm_start_thread_saved_rbx: 0x7ffde6059700
kvm_original_child_function: 0x7fffffffe6ae
kvm_original_child_argument: 0x7ffde6059700
```

They match the functional-memory view, so no execution-time split exists
between gem5 memory accesses and KVM host backing.

The exact stack arithmetic is:

- glibc child trampoline starts with `RSP = new_stack`;
- two pops leave `RSP = new_stack + 0x10`;
- `call *%rax` should push its return address and enter `start_thread` with
  `RSP = new_stack + 0x8`;
- the `start_thread` prologue consumes `0xb8`, matching the observed fatal
  RSP and proving the function entry RSP was `new_stack + 0x8`.

However, the backing word at `new_stack+8` remains TLS rather than the call
return address, while `start_thread` saved RDI as clone flags. The trampoline
guest bytes, input stack, return RCX, and RAX are all correct. Static
state inspection cannot explain which instruction first diverges.

The next diagnostic must observe real KVM execution instruction by
instruction from `__clone+0x35` through `start_thread` entry. Candidate
approaches:

1. use temporary `KVM_SET_GUEST_DEBUG` single-step support and log RIP, RSP,
   RAX, RDI, RBX, and the top two stack words after each exit;
2. patch temporary INT3 breakpoints into the guest trampoline and add
   breakpoint continuation logic;
3. bypass the glibc trampoline by constructing a direct child entry, which
   would mask the cause and is not recommended.

Recommended next step: approach 1. It is diagnostic-only, does not alter
clone semantics, and will identify the first incorrect transition among
`test`, `pop %rax`, `pop %rdi`, and `call *%rax`.

No source behavior was changed for this conclusion.

## 2026-06-26 KVM long-run hipMemset result

The user reported a KVM run of `hip_api_smoke --stage memset` with a
`500000000000` tick limit.

Exact command: not supplied in full in this turn. It is the same KVM
`hip_api_smoke --stage memset` experiment family used above, with the
simulation limit raised to 500 billion ticks.

First relevant observable result:

```text
No "[hip_api_smoke] end hipMemset(device_data, memset_byte, bytes)" marker
appeared before the 500 billion tick limit.
```

Facts confirmed:

- Raising the KVM simulation limit from 100 billion ticks to 500 billion
  ticks still does not complete `hipMemset`.
- The previous "CPU model is too slow" explanation is insufficient for the
  current HIP `hipMemset` failure. KVM also fails to reach completion within
  an already impractical tick budget.
- This strengthens the previous SDMA-selection conclusion: the current
  unmodified HIP/ROCclr `hipMemset` path is not a usable validation path for
  the new `SESDMAEngine`, because existing traces showed only KFD type-2
  queues and no type-1 SDMA queue or SDMA doorbell.

Hypotheses accepted:

- The practical blocker is a missing or non-completing HIP/ROCclr blit
  execution path in SE mode, not simply slow CPU-side execution.
- Continuing to extend the same unmodified HIP `hipMemset` run to larger
  tick limits is not a productive diagnostic.

Hypotheses rejected:

- Rejected: KVM plus a larger tick budget is sufficient to make the current
  `hipMemset` path complete.

Files changed:

- this status document only.

Verification completed:

- User-run simulation result recorded.
- Codex did not start a build or simulation.

Single next diagnostic step: stop using unmodified HIP `hipMemset` as the
first SDMA validation target. Build or run a direct HSA SDMA/fill smoke that
forces `hsa_amd_memory_fill` or direct KFD SDMA queue creation, then inspect
whether the run produces `AMDKFD_IOC_CREATE_QUEUE ... type 1`,
`Register SDMA`, and `SDMA doorbell`.

## 2026-06-26 Direct HSA SDMA fill smoke

Goal: verify whether a direct HSA path can request a KFD SDMA queue type 1,
without going through HIP `hipMemset` or ROCclr's kernel blit path.

Implementation:

- Added `tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_fill.cpp`.
- Added `HSA_SDMA_FILL_TARGET` / `HSA_SDMA_FILL_SRC` and an
  `hsa_sdma_fill` build target to
  `tests/test-progs/gpu/xgmi-peer-vram/Makefile`.
- Added a source-level regression in
  `tests/pyunit/stdlib/test_se_viper_multigpu.py`.

The smoke program:

1. calls `hsa_init`;
2. finds the first GPU agent;
3. finds the first allocable global AMD memory pool on that agent;
4. allocates 4 KiB with `hsa_amd_memory_pool_allocate`;
5. calls `hsa_amd_memory_fill(ptr, 0x5a6b7c8d, 1024)`;
6. frees the allocation and prints `HSA_SDMA_FILL_PASSED` on success.

Commands run:

```bash
PYTHONPATH=src/python python3 -m unittest \
  tests.pyunit.stdlib.test_se_viper_multigpu.SEViperMultiGPUTest.test_direct_hsa_sdma_fill_smoke_uses_hsa_amd_memory_fill
```

Initial RED result:

```text
FileNotFoundError: hsa_sdma_fill.cpp
```

After implementation:

```text
Ran 1 test in 0.000s
OK
```

```bash
make -C tests/test-progs/gpu/xgmi-peer-vram hsa_sdma_fill \
  ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1
```

Result:

```text
g++ ... -o hsa_sdma_fill hsa_sdma_fill.cpp -lhsa-runtime64 -lhsakmt
```

```bash
LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib \
  ldd tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_fill
```

Result: `libhsa-runtime64.so.1` and `libhsakmt.so.1` resolve from the local
ROCm dependency tree.

```bash
git diff --check -- \
  tests/test-progs/gpu/xgmi-peer-vram/Makefile \
  tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_fill.cpp \
  tests/pyunit/stdlib/test_se_viper_multigpu.py \
  docs/debug/se-multigpu-status.md
```

Result: passed with no output.

Facts confirmed:

- The direct HSA smoke is buildable against ROCm 4.0.1.
- It calls `hsa_amd_memory_fill` directly; it does not call `hipMemset`.
- Codex did not run gem5 simulation.

Hypotheses accepted:

- A direct HSA fill smoke is now available to distinguish ROCr SDMA selection
  from HIP/ROCclr `hipMemset` behavior.

Hypotheses rejected:

- None yet; the simulation result is still needed to determine whether ROCr
  creates KFD queue type 1.

Files changed:

- `tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_fill.cpp`
- `tests/test-progs/gpu/xgmi-peer-vram/Makefile`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document.

Verification completed:

- Focused pyunit regression passed.
- `hsa_sdma_fill` compiled successfully.
- Dynamic-library resolution succeeds when `LD_LIBRARY_PATH` points at the
  local ROCm dependency tree.
- `git diff --check` passed for the touched files.

Single next diagnostic step: run `hsa_sdma_fill` under gem5 SE with
`--debug-flags=GPUDriver,SESDMAEngine`, `HSA_ENABLE_SDMA=1`, and
`HSA_FORCE_SDMA=1`, then inspect the trace for
`AMDKFD_IOC_CREATE_QUEUE ... type 1`, `Register SDMA`, and `SDMA doorbell`.

## 2026-06-26 Direct HSA SDMA fill simulation result

The user ran the direct HSA fill smoke:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hsa-sdma-fill \
  -r -e \
  --stdout-file=simout.txt \
  --stderr-file=simerr.txt \
  --debug-flags=GPUDriver,SESDMAEngine \
  --debug-file=sdma.trace \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type atomic \
  --app tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_fill \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu \
  --env HSA_ENABLE_SDMA=1 \
  --env HSA_FORCE_SDMA=1
```

Commands run by Codex to inspect the result:

```bash
sed -n '1,220p' m5out-hsa-sdma-fill/simout.txt
sed -n '1,260p' m5out-hsa-sdma-fill/simerr.txt
rg -n "CREATE_QUEUE|type 1|type 2|Register SDMA|SDMA doorbell|SESDMA|hsa_amd_memory_fill|HSA_SDMA_FILL|hsa_sdma_fill|fatal|panic|Exiting" \
  m5out-hsa-sdma-fill/sdma.trace \
  m5out-hsa-sdma-fill/simout.txt \
  m5out-hsa-sdma-fill/simerr.txt
rg -n "Created SE SDMA|Register SDMA|Unregister SDMA|doorbell|writeDoorbell|Processing SDMA|CONST_FILL|COPY|FENCE|TRAP|AMDKFD_IOC_CREATE_QUEUE" \
  m5out-hsa-sdma-fill/sdma.trace
```

First relevant observable result:

```text
[hsa_sdma_fill] begin hsa_amd_memory_fill
GPU Blit Kernel Completed dump and reset
[hsa_sdma_fill] end hsa_amd_memory_fill: HSA_STATUS_SUCCESS...
HSA_SDMA_FILL_PASSED
Exiting @ tick 14565619467 because exiting with last active thread context.
```

The only KFD queue creation in `sdma.trace` was:

```text
AMDKFD_IOC_CREATE_QUEUE gpu_id 22124 type 2 ...
```

There was no `type 1`, `Register SDMA`, `SDMA doorbell`, or
`writeDoorbell` entry. The `SESDMAEngine` instances were constructed:

```text
board.gpus0.se_sdma_engine: Created SE SDMA engine for gpu_id 22124
board.gpus1.se_sdma_engine: Created SE SDMA engine for gpu_id 22125
```

Facts confirmed:

- Direct `hsa_amd_memory_fill` succeeds under SE atomic mode.
- The current ROCr path still chooses a kernel/AQL blit path, not KFD SDMA.
- The direct HSA fill smoke therefore does not exercise `SESDMAEngine`.
- `HSA_ENABLE_SDMA=1` and `HSA_FORCE_SDMA=1` were present in the process
  environment, but they were insufficient to force a type-1 SDMA queue.

Hypotheses accepted:

- The absence of type-1 SDMA is not specific to HIP `hipMemset`; direct ROCr
  `hsa_amd_memory_fill` also selects a type-2 queue in this configuration.
- The current `SESDMAEngine` implementation is not yet validated by this
  run because no SDMA queue or doorbell reached it.

Hypotheses rejected:

- Rejected: direct `hsa_amd_memory_fill` with `HSA_FORCE_SDMA=1` necessarily
  creates KFD queue type 1.
- Rejected: the earlier HIP `hipMemset` type-2 behavior was solely a ROCclr
  wrapper issue.

Files changed:

- this status document only.

Verification completed:

- Existing `m5out-hsa-sdma-fill` logs inspected.
- Codex did not start a build or simulation.

Single next diagnostic step: inspect ROCr SDMA-selection controls for
`GpuAgent::InitDma/CreateBlitSdma` in ROCm 4.0.1 and run one minimal
selection experiment, preferably by increasing the direct fill size or using
the exact ROCr SDMA-selection environment variable if identified. The success
criterion remains `AMDKFD_IOC_CREATE_QUEUE ... type 1`.

## 2026-06-26 ROCr 4.0 source-level SDMA queue mapping correction

Commands run by Codex:

```bash
git ls-remote --heads --tags https://github.com/ROCm/ROCR-Runtime.git '*4.0*' '*40001*' '*a5173c90*'
git clone --depth 1 --branch rocm-4.0.0 https://github.com/ROCm/ROCR-Runtime.git /tmp/ROCR-Runtime-rocm-4.0.0
rg -n "hsa_amd_memory_fill|FillMemory|DmaFill|InitDma|CreateBlitSdma|BlitSdma|BlitKernel|HSA_.*SDMA|ENABLE_SDMA|FORCE_SDMA|GPU Blit Kernel Completed|CREATE_QUEUE|QueueType" src -S
sed -n '60,140p' src/core/util/flag.h
sed -n '480,710p' src/core/runtime/amd_gpu_agent.cpp
sed -n '500,550p' src/core/runtime/runtime.cpp
sed -n '120,210p' src/core/runtime/amd_blit_sdma.cpp
sed -n '620,650p' .deps/rocm-4.0.1/root/opt/rocm-4.0.1/include/hsakmttypes.h
sed -n '40,70p' src/dev/hsa/kfd_ioctl.h
```

First relevant observable result:

```text
ceae52f3e9d58ff71bfff0956ea8d53686eb09b5 refs/tags/rocm-4.0.0
```

No public `rocm-4.0.1`, `40001`, or `a5173c90` ref was found in the official
ROCR-Runtime repository query. The source inspected is therefore the closest
public ROCr 4.0 tag, `rocm-4.0.0`, not an exact source match for the local
`1.2.40001.0-rocm-rel-4.0-26-a5173c90` package.

Facts confirmed from ROCr source and local ROCm headers:

- `hsa_amd_memory_fill` calls `Runtime::FillMemory`, which selects a GPU
  agent from pointer info and calls `GpuAgent::DmaFill`.
- `GpuAgent::DmaFill` calls `blits_[BlitDevToDev]->SubmitLinearFillCommand`.
- `GpuAgent::InitDma` initializes `BlitDevToDev` with `CreateBlitKernel`,
  while `HSA_ENABLE_SDMA` affects the H2D, D2H, PCIe P2P, and xGMI blit
  objects created by `blit_lambda`.
- The inspected ROCr tag still contains an SDMA fill implementation in
  `BlitSdma::SubmitLinearFillCommand`, but `DmaFill` does not select that
  object for normal same-device fills.
- The local ROCm 4.0.1 `hsakmttypes.h` defines `HSA_QUEUE_SDMA = 2` and
  `HSA_QUEUE_COMPUTE_AQL = 21`.
- The gem5 local `src/dev/hsa/kfd_ioctl.h` currently defines
  `KFD_IOC_QUEUE_TYPE_SDMA = 1` and `KFD_IOC_QUEUE_TYPE_COMPUTE_AQL = 2`.

Hypotheses accepted:

- The earlier interpretation of `AMDKFD_IOC_CREATE_QUEUE ... type 2` as a
  compute/AQL queue was wrong for the local ROCm 4.0.1 headers. In ROCm 4.0.1,
  type 2 means KFD SDMA.
- The direct `hsa_amd_memory_fill` run did create an SDMA queue from ROCr's
  point of view, but gem5 classified queue type 2 as `COMPUTE_AQL` because
  its local KFD queue-type constants are stale or mismatched.
- This queue-type ABI mismatch explains why `SESDMAEngine` was constructed
  but did not log `Register SDMA` or receive SDMA doorbells.

Hypotheses rejected:

- Rejected: the absence of `Register SDMA` proves ROCr did not create an SDMA
  queue. The stronger explanation is now gem5 queue-type misclassification.
- Rejected: the success criterion should be `type 1`. For the local ROCm 4.0.1
  headers, the correct PCIe SDMA queue type is `type 2`; `type 5` is SDMA_XGMI.

Files changed:

- this status document only.

Verification completed:

- Official ROCr source tag availability checked.
- ROCr 4.0.0 source inspected under `/tmp/ROCR-Runtime-rocm-4.0.0`.
- Local ROCm 4.0.1 KFD queue constants compared against gem5's
  `src/dev/hsa/kfd_ioctl.h`.
- Codex did not start a gem5 build or simulation.

Single next diagnostic step: update gem5's KFD queue-type constants and
`GPUComputeDriver::allocateQueue` handling to match ROCm 4.0.1
(`SDMA=2`, `SDMA_XGMI=5`, `COMPUTE_AQL=21`), then rerun the existing
`hsa_sdma_fill` command and require `Register SDMA` plus `SDMA doorbell` in
`m5out-hsa-sdma-fill/sdma.trace`.

## 2026-06-26 KFD queue type mapping fix

Commands run by Codex:

```bash
sed -n '1,90p' src/dev/hsa/kfd_ioctl.h
sed -n '220,265p' src/gpu-compute/gpu_compute_driver.cc
rg -n "KFD_IOC_QUEUE_TYPE_|queue_type|SDMA_XGMI|COMPUTE_AQL" \
  tests src/dev/hsa src/gpu-compute \
  tests/pyunit/stdlib/test_se_viper_multigpu.py -S
PYTHONPATH=src/python python3 -m unittest \
  tests.pyunit.stdlib.test_se_viper_multigpu.SEViperMultiGPUTest.test_kfd_sdma_queue_type_is_not_registered_as_compute
git diff --check -- \
  src/dev/hsa/kfd_ioctl.h \
  tests/pyunit/stdlib/test_se_viper_multigpu.py \
  docs/debug/se-multigpu-status.md
PYTHONPATH=src/python python3 -m unittest \
  tests.pyunit.stdlib.test_se_viper_multigpu
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kfd_sdma_queue_type_is_not_registered_as_compute
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

First relevant observable result:

```text
Ran 1 test in 0.000s
OK
```

The full file also passed through the gem5 pyunit entry:

```text
Ran 31 tests in 0.040s
OK
```

The ordinary `python3 -m unittest tests.pyunit.stdlib.test_se_viper_multigpu`
entry reported five `m5.objects` loader errors involving
`__spec__.loader_state`. The same tests pass through the gem5 pyunit entry,
so this is an invocation limitation, not a failed queue-type regression.

Facts confirmed:

- `src/dev/hsa/kfd_ioctl.h` now matches the ROCm 4.0.1 queue-type ABI for
  the queue types used by this path:
  - `KFD_IOC_QUEUE_TYPE_COMPUTE = 1`;
  - `KFD_IOC_QUEUE_TYPE_SDMA = 2`;
  - `KFD_IOC_QUEUE_TYPE_SDMA_XGMI = 5`;
  - `KFD_IOC_QUEUE_TYPE_COMPUTE_AQL = 21`.
- `GPUComputeDriver::allocateQueue` already dispatches
  `KFD_IOC_QUEUE_TYPE_SDMA` to `registerSDMAQueue`; after the constant fix,
  ROCr's `AMDKFD_IOC_CREATE_QUEUE ... type 2` will take the SDMA path instead
  of the compute path.
- The focused pyunit now asserts the ROCm 4.0.1 numeric ABI, so a future
  regression back to compact numbering will fail.

Hypotheses accepted:

- The missing `Register SDMA` in the previous `hsa_sdma_fill` trace was
  caused by gem5's stale KFD queue-type constants misclassifying type 2 as
  compute/AQL.

Hypotheses rejected:

- Rejected: `GPUComputeDriver::allocateQueue` needed a structural rewrite for
  this issue. The existing switch was sufficient once the constants matched
  ROCm 4.0.1.

Files changed:

- `src/dev/hsa/kfd_ioctl.h`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document.

Verification completed:

- Focused pyunit passed with ordinary Python.
- `git diff --check` passed for the touched files.
- Focused pyunit passed through `build/VEGA_X86/gem5.opt -p`.
- Full `test_se_viper_multigpu` pyunit passed through
  `build/VEGA_X86/gem5.opt -p`.
- Codex did not start a gem5 build or full simulation.

Single next diagnostic step: rebuild gem5, rerun the existing
`hsa_sdma_fill` command, and inspect `m5out-hsa-sdma-fill/sdma.trace` for
`AMDKFD_IOC_CREATE_QUEUE ... type 2`, followed by `Register SDMA queue` and
`SDMA doorbell`.

## 2026-06-26 Direct HSA SDMA fill after queue type fix

The user rebuilt gem5 and reran the existing direct HSA SDMA fill command.

Commands run by Codex to inspect the result:

```bash
tail -260 docs/debug/se-multigpu-status.md
git status --short
find m5out-hsa-sdma-fill -maxdepth 2 -type f | sort
sed -n '1,220p' m5out-hsa-sdma-fill/simout.txt
sed -n '1,260p' m5out-hsa-sdma-fill/simerr.txt
rg -n "AMDKFD_IOC_CREATE_QUEUE|Register SDMA|Unregister SDMA|SDMA doorbell|writeDoorbell|CONST|FILL|COPY|FENCE|TRAP|Processing|Unsupported|fatal|panic|Created SE SDMA|HSA_SDMA_FILL|hsa_amd_memory_fill|GPU Blit|Exiting" \
  m5out-hsa-sdma-fill/sdma.trace \
  m5out-hsa-sdma-fill/simout.txt \
  m5out-hsa-sdma-fill/simerr.txt -S
sed -n '140,240p' m5out-hsa-sdma-fill/sdma.trace
sed -n '1,260p' src/dev/hsa/se_sdma_engine.cc
rg -n "0x14|TRAP|FENCE|POLL|CONSTANT_FILL|SDMA_OP|opcode|SUB_OP|sub" \
  src/dev/amdgpu src/dev/hsa \
  /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/amd_blit_sdma.cpp \
  /tmp/ROCR-Runtime-rocm-4.0.0/src/core/inc/sdma_registers.h -S
sed -n '35,95p' src/dev/amdgpu/sdma_commands.hh
sed -n '450,490p' src/dev/amdgpu/sdma_engine.cc
sed -n '620,705p' src/dev/amdgpu/sdma_engine.cc
```

First relevant observable result:

```text
AMDKFD_IOC_CREATE_QUEUE gpu_id 22124 type 2 ...
board.gpus0.se_sdma_engine: Register SDMA queue 0 gpu_id 22124 ...
board.gpus0.se_sdma_engine: SDMA doorbell queue 0 gpu_id 22124 write_index 1
board.gpus0.se_sdma_engine: SDMA queue 0 header 0x11402 opcode 0x2 sub_opcode 0x14
src/dev/hsa/se_sdma_engine.cc:197: fatal: SE SDMA unsupported opcode 0x2 sub-opcode 0x14 header 0x11402
```

Facts confirmed:

- The queue-type mapping fix worked: ROCr's `type 2` queue is now registered
  with `SESDMAEngine`.
- The SDMA doorbell reaches `SESDMAEngine`.
- The next blocker is not queue creation; it is missing SDMA packet support.
- `opcode 0x2` is `SDMA_OP_WRITE`; in gem5's full-system SDMA engine,
  `SDMA_OP_WRITE` with `SDMA_SUBOP_WRITE_LINEAR` reads inline dword payload
  from the ring and writes it to the packet destination.

Hypotheses accepted:

- The SE SDMA MVP must support `SDMA_OP_WRITE` / linear write before the
  direct HSA fill can progress to the later fill/fence/trap packets.

Hypotheses rejected:

- Rejected: the queue-type mapping fix failed. It succeeded and exposed the
  next unsupported SDMA packet.

Files changed:

- `src/dev/hsa/se_sdma_engine.cc`
- `src/dev/hsa/se_sdma_engine.hh`
- this status document.

Implementation:

- Added `SDMA_OP_WRITE` handling to `SESDMAEngine::decodeHeader`.
- Added `executeWrite` and `executeWriteData` helpers.
- The SE implementation follows the full-system SDMA engine's linear-write
  model: `count + 1` dwords are read from the ring after the packet body and
  then written to `pkt.dest` through SE virtual DMA.

Verification completed:

```bash
git diff --check -- \
  src/dev/hsa/se_sdma_engine.cc \
  src/dev/hsa/se_sdma_engine.hh \
  docs/debug/se-multigpu-status.md
```

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_viper_gpu_exposes_sdma_dma_port \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kfd_sdma_queue_type_is_not_registered_as_compute \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_hsa_packet_processor_routes_sdma_doorbells
```

Result: all three focused pyunit checks passed. Codex did not start a gem5
build or full simulation.

Single next diagnostic step: rebuild gem5, rerun `hsa_sdma_fill`, and inspect
the next `SESDMAEngine` packet after `SDMA_OP_WRITE`. Expected progress is no
fatal at `opcode 0x2`; the next target is to reach and execute
`SDMA_OP_CONST_FILL`.

## 2026-06-26 Direct HSA SDMA fill after WRITE support

The user rebuilt gem5 and reran the direct HSA SDMA fill command.

Commands run by Codex to inspect the result:

```bash
tail -220 docs/debug/se-multigpu-status.md
git status --short
find m5out-hsa-sdma-fill -maxdepth 1 -type f -printf '%f %s\n' | sort
sed -n '1,260p' m5out-hsa-sdma-fill/simout.txt
sed -n '1,260p' m5out-hsa-sdma-fill/simerr.txt
rg -n "AMDKFD_IOC_CREATE_QUEUE|Register SDMA|SDMA doorbell|SDMA queue|SDMA write|CONST|Const|FILL|COPY|FENCE|TRAP|unsupported|fatal|panic|HSA_SDMA_FILL|hsa_amd_memory_fill|Exiting|drained|completed" \
  m5out-hsa-sdma-fill/sdma.trace \
  m5out-hsa-sdma-fill/simout.txt \
  m5out-hsa-sdma-fill/simerr.txt -S
rg -n "SDMA_PKT_WRITE|WRITE_UNTILED|WRITE_LINEAR|OP_WRITE|WRITE.*TAG|SDMA_SUBOP_WRITE" \
  /tmp/ROCR-Runtime-rocm-4.0.0/src/core/inc/sdma_registers.h \
  /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/amd_blit_sdma.cpp \
  src/dev/amdgpu/sdma_packets.hh \
  src/dev/amdgpu/sdma_commands.hh \
  src/dev/amdgpu/sdma_engine.cc -S
git ls-remote --heads --tags https://github.com/RadeonOpenCompute/ROCK-Kernel-Driver.git \
  'roc-4.3.x' 'roc-4.0*' 'rocm-4.0*'
git clone --depth 1 --branch rocm-4.0.1 \
  https://github.com/RadeonOpenCompute/ROCK-Kernel-Driver.git \
  /tmp/ROCK-Kernel-Driver-rocm-4.0.1
sed -n '1543,1605p' \
  /tmp/ROCK-Kernel-Driver-rocm-4.0.1/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h
sed -n '1538,1552p' \
  /tmp/ROCK-Kernel-Driver-rocm-4.0.1/drivers/gpu/drm/amd/amdgpu/sdma_v4_0.c
sed -n '1674,1688p' \
  /tmp/ROCK-Kernel-Driver-rocm-4.0.1/drivers/gpu/drm/amd/amdgpu/sdma_v4_0.c
```

First relevant observable result:

```text
board.gpus0.se_sdma_engine: SDMA doorbell queue 0 gpu_id 22124 write_index 1
board.gpus0.se_sdma_engine: SDMA queue 0 header 0x11402 opcode 0x2 sub_opcode 0x14
src/dev/hsa/se_sdma_engine.cc:178: fatal: SE SDMA unsupported WRITE sub-opcode 0x14
```

Facts confirmed:

- The run no longer fails at unsupported opcode; it reaches the newly added
  `SDMA_OP_WRITE` branch.
- The next blocker is the strict sub-opcode check inside the SE WRITE handler.
- ROCK Kernel Driver `rocm-4.0.1` exists and was downloaded to
  `/tmp/ROCK-Kernel-Driver-rocm-4.0.1`.
- In `vega10_sdma_pkt_open.h`, the WRITE_UNTILED packet layout is:
  header dword, destination low dword, destination high dword, count/swap
  dword, then inline data dwords.
- The kernel's `sdma_v4_0.c` emits WRITE packets with
  `SDMA_PKT_HEADER_OP(SDMA_OP_WRITE) |
   SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_WRITE_LINEAR)` followed by that
  WRITE_UNTILED body.

Hypotheses accepted:

- The SE SDMA WRITE implementation should not rely only on the generic
  `bits(header, 15, 8)` check before reading the WRITE_UNTILED body. The
  observed ROCr/SDMA packet stream has `opcode=WRITE` but presents
  `sub_opcode=0x14` in those generic bits.
- Doorbell value alone should not be treated as authoritative SDMA queue
  write pointer. ROCr updates `Queue_write_ptr`; the SE engine should read
  that memory when a doorbell arrives.

Hypotheses rejected:

- Rejected: the previous WRITE support completely fixed the direct fill path.
  It only moved the failure from unsupported opcode to unsupported WRITE
  sub-opcode.

Files changed:

- `src/dev/hsa/se_sdma_engine.cc`
- `src/dev/hsa/se_sdma_engine.hh`
- this status document.

Implementation:

- `SESDMAEngine::writeDoorbell` now treats the MMIO doorbell value as a
  trigger and reads the authoritative queue write pointer from
  `Queue_write_ptr`.
- Added `updateWriteIndex` helper to set the wrapped SDMA queue write index
  after that read and then process the queue.
- Relaxed `SDMA_OP_WRITE` handling to treat observed WRITE packets as linear
  untiled WRITE packets and read the existing `sdmaWrite` body.

Verification completed:

```bash
git diff --check -- \
  src/dev/hsa/se_sdma_engine.cc \
  src/dev/hsa/se_sdma_engine.hh \
  docs/debug/se-multigpu-status.md
```

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_viper_gpu_exposes_sdma_dma_port \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kfd_sdma_queue_type_is_not_registered_as_compute \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_hsa_packet_processor_routes_sdma_doorbells
```

Result: all three focused pyunit checks passed. Codex did not start a gem5
build or full simulation.

Single next diagnostic step: rebuild gem5, rerun `hsa_sdma_fill`, and check
whether `SESDMAEngine` now logs a nonzero `queue_write_index`, executes the
WRITE packet, and reaches `SDMA_OP_CONST_FILL`.

## 2026-06-26 Direct HSA SDMA fill after doorbell/write-index fix

The user rebuilt gem5 and reran the direct HSA SDMA fill command.

Commands run by Codex to inspect the result:

```bash
tail -180 docs/debug/se-multigpu-status.md
git status --short -- \
  docs/debug/se-multigpu-status.md \
  src/dev/hsa/se_sdma_engine.cc \
  src/dev/hsa/se_sdma_engine.hh \
  src/dev/hsa/kfd_ioctl.h \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
find m5out-hsa-sdma-fill -maxdepth 1 -type f -printf '%f %s\n' | sort
sed -n '1,240p' m5out-hsa-sdma-fill/simout.txt
sed -n '1,260p' m5out-hsa-sdma-fill/simerr.txt
rg -n "AMDKFD_IOC_CREATE_QUEUE|Register SDMA|SDMA doorbell|raw_write_index|queue_write_index|SDMA queue|SDMA WRITE|SDMA write|CONST|FILL|COPY|FENCE|TRAP|unsupported|fatal|panic|HSA_SDMA_FILL|hsa_amd_memory_fill|Exiting|drained|completed|BadAddress|Page fault" \
  m5out-hsa-sdma-fill/sdma.trace \
  m5out-hsa-sdma-fill/simout.txt \
  m5out-hsa-sdma-fill/simerr.txt -S
rg -n "0x100010040|0x100010000|0x100000000|0x1000|0x1002|Mapping VA|Registering \[|AMDKFD_IOC_ALLOC_MEMORY_OF_GPU|pool 1|VRAM|USERPTR" \
  m5out-hsa-sdma-fill/sdma.trace \
  m5out-hsa-sdma-fill/simerr.txt -S
sed -n '1,220p' src/dev/dma_virt_device.hh
sed -n '1,140p' src/dev/dma_virt_device.cc
sed -n '1,240p' src/dev/dma_device.hh
```

First relevant observable result:

```text
board.gpus0.se_sdma_engine: SDMA doorbell queue 0 gpu_id 22124 raw_write_index 1
board.gpus0.se_sdma_engine: SDMA queue 0 gpu_id 22124 queue_write_index 1 wrapped 1
board.gpus0.se_sdma_engine: SDMA queue 0 header 0x11402 opcode 0x2 sub_opcode 0x14
board.gpus0.se_sdma_engine: SDMA WRITE queue 0 sub_opcode 0x14, treating as linear untiled WRITE packet
board.gpus0.se_sdma_engine: SDMA write queue 0 4097 dwords to 0x100010040
src/dev/dma_virt_device.cc:66: fatal: fatal condition range.fault occurred: Failed translation: vaddr 0x100010040
```

Facts confirmed:

- The latest code reads and logs `Queue_write_ptr`, but for this first
  doorbell it is still `1`.
- The SE engine now enters the WRITE handler and attempts the WRITE packet.
- The new blocker is address-space handling for SDMA packet payload
  addresses, not queue creation or WRITE decode.
- `0x100010040` falls in the already allocated GPU/VRAM physical region. The
  trace shows a VRAM allocation at VA `0x7ffdf6a00000` mapped to framebuffer
  PA `0x100000000` with size `0x200000`, so `0x100010040` is inside that
  framebuffer range.
- `DmaVirtDevice::dmaWriteVirt` always translates through the SE process page
  table and fatals on translation faults. That is correct for queue/ring CPU
  virtual addresses, but not sufficient for SDMA payload addresses that are
  already physical framebuffer addresses.

Hypotheses accepted:

- The SE SDMA engine needs separate handling for SDMA payload source/dest
  addresses: first try process virtual translation, and if that fails, treat
  the address as a physical DMA address.

Hypotheses rejected:

- Rejected: the remaining failure is still caused by the WRITE sub-opcode
  check. That check has been bypassed; the failure is now a translation fault
  during the payload write.

Files changed:

- `src/dev/hsa/se_sdma_engine.cc`
- `src/dev/hsa/se_sdma_engine.hh`
- this status document.

Implementation:

- Added `dmaReadAddr` and `dmaWriteAddr` helpers for SDMA packet payload
  accesses.
- These helpers try SE virtual translation first. If any translated range
  faults, they issue direct physical DMA to the original address.
- Converted WRITE, CONST_FILL, COPY, and FENCE payload accesses to use these
  helpers. Queue/ring metadata accesses still use virtual DMA.

Verification completed:

```bash
git diff --check -- \
  src/dev/hsa/se_sdma_engine.cc \
  src/dev/hsa/se_sdma_engine.hh \
  docs/debug/se-multigpu-status.md
```

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_viper_gpu_exposes_sdma_dma_port \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kfd_sdma_queue_type_is_not_registered_as_compute \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_hsa_packet_processor_routes_sdma_doorbells
```

Result: all three focused pyunit checks passed. Codex did not start a gem5
build or full simulation.

Single next diagnostic step: rebuild gem5, rerun `hsa_sdma_fill`, and check
whether the WRITE payload write to `0x100010040` now completes and the engine
continues to the next SDMA packet, ideally `SDMA_OP_CONST_FILL`.

## 2026-06-26 Direct HSA SDMA fill over-processes ring after payload fix

The user reran `hsa_sdma_fill` after rebuilding with the payload physical-DMA
fallback. The run appeared to hang and was interrupted.

Commands run by Codex to inspect the result:

```bash
ls -lh m5out-hsa-sdma-fill
ps -C gem5.opt -o pid,etime,time,pcpu,stat,args
tail -80 m5out-hsa-sdma-fill/simout.txt
tail -120 m5out-hsa-sdma-fill/simerr.txt
tail -120 m5out-hsa-sdma-fill/sdma.trace
grep -m 80 -E 'Register SDMA|SDMA doorbell|queue_write_index|SDMA write|direct physical|unsupported|fatal|panic' \
  m5out-hsa-sdma-fill/sdma.trace
sed -n '1,260p' src/dev/hsa/se_sdma_engine.cc
sed -n '1,240p' src/dev/hsa/se_sdma_engine.hh
```

First relevant observable result:

```text
m5out-hsa-sdma-fill/sdma.trace size: 4.3G
[hsa_sdma_fill] begin hsa_amd_memory_fill
Exiting @ tick 22729134023 because user interrupt received.
```

The SDMA trace showed the first packet completing far enough to issue a
physical payload write:

```text
SDMA doorbell queue 0 gpu_id 22124 raw_write_index 1
SDMA queue 0 gpu_id 22124 queue_write_index 1 wrapped 1
SDMA write queue 0 4097 dwords to 0x100010040
SE SDMA direct physical write addr 0x100010040 size 16388
```

After that, the trace repeatedly showed zero headers and occasional
misdecoded `0x1` headers:

```text
SDMA queue 0 header 0 opcode 0 sub_opcode 0
...
SDMA queue 0 header 0x1 opcode 0x1 sub_opcode 0
```

Facts confirmed:

- The previous translation fatal is fixed: the WRITE packet now reaches the
  direct physical DMA path.
- The run still does not reach `end hsa_amd_memory_fill` or
  `HSA_SDMA_FILL_PASSED`.
- No gem5 process was still running when Codex inspected the directory.
- `queue_write_index` is still observed as `1`, while the engine advances
  `readIndex` in byte units.
- Because `readIndex != writeIndex` remains true, the engine over-processes
  the ring, wraps, re-executes the first WRITE packet, and consumes zero-filled
  ring space as packets. This explains the 4.3G trace and the apparent hang.

Hypothesis accepted:

- The current MVP must not decode zero-filled ring space as valid work. For
  this ROCr queue observation, a zero SDMA header after real work is empty or
  unsubmitted ring space, not useful progress.

Hypothesis rejected:

- Rejected: the new hang is CPU-side ROCm compilation time. The SDMA debug
  trace is actively growing because the new SE SDMA engine is repeatedly
  processing ring entries.

Files changed:

- `src/dev/hsa/se_sdma_engine.cc`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document.

Implementation:

- `SESDMAEngine::decodeHeader()` now treats `header == 0` as empty ring
  space before advancing the read pointer.
- On a zero header, it forces `queue.readIndex = queue.writeIndex` and calls
  `finishPacket()`, so the queue drains instead of scanning zero-filled ring
  memory indefinitely.
- Added a focused pyunit assertion that the zero-header guard exists before
  the normal header read-index advance.

Verification completed:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_sdma_engine_has_phase1_packet_handlers \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_hsa_packet_processor_routes_sdma_doorbells \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kfd_sdma_queue_type_is_not_registered_as_compute
```

Result: all three focused pyunit checks passed.

```bash
git diff --check -- \
  src/dev/hsa/se_sdma_engine.cc \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
```

Result: completed successfully with no output. Codex did not start a gem5
build or full simulation.

Single next diagnostic step: rebuild gem5 and rerun `hsa_sdma_fill`. The
expected improvement is that `sdma.trace` no longer grows without bound after
the first WRITE packet. If the program still waits in `hsa_amd_memory_fill`,
inspect the next wait condition: likely ROCr is waiting for a completion
signal packet that the MVP has not yet implemented or has skipped because
the observed write pointer is not byte-accurate.

## 2026-06-26 Direct HSA SDMA fill after zero-header drain fix

The user rebuilt gem5 and reran `hsa_sdma_fill`. The user reported that the
program still appeared to be executing, but the log stopped growing.

Commands run by Codex to inspect the result:

```bash
ps -C gem5.opt -o pid,etime,time,pcpu,stat,args
ls -lh m5out-hsa-sdma-fill
tail -80 m5out-hsa-sdma-fill/simout.txt
tail -120 m5out-hsa-sdma-fill/simerr.txt
tail -120 m5out-hsa-sdma-fill/sdma.trace
```

First relevant observable result:

```text
sdma.trace size: 28K
[hsa_sdma_fill] begin hsa_amd_memory_fill
```

The SDMA trace now ends with:

```text
SDMA doorbell queue 0 gpu_id 22124 raw_write_index 1
SDMA queue 0 gpu_id 22124 queue_write_index 1 wrapped 1
SDMA queue 0 header 0x11402 opcode 0x2 sub_opcode 0x14
SDMA WRITE queue 0 sub_opcode 0x14, treating as linear untiled WRITE packet
SDMA write queue 0 4097 dwords to 0x100010040
SE SDMA direct physical write addr 0x100010040 size 16388
SDMA queue 0 saw zero header at rptr 16404; treating as empty ring space and draining to wptr 1
SDMA queue 0 drained at rptr 1
```

Facts confirmed:

- The zero-header drain fix works: `sdma.trace` no longer grows without bound.
- The SE SDMA engine executes the observed WRITE packet once and then drains
  the queue when it reaches zero-filled ring space.
- The test still does not print `end hsa_amd_memory_fill` or
  `HSA_SDMA_FILL_PASSED`.
- No `gem5.opt` process was visible to Codex via `ps -C gem5.opt` at
  inspection time, so if a run is still active it is not the process recorded
  in the inspected output directory.

Hypothesis accepted:

- The current infinite-ring-consumption failure is fixed.
- The remaining blocker is likely ROCr waiting for an SDMA completion/signal
  update that the MVP has not produced or has skipped because the observed
  queue write pointer is not byte-accurate.

Hypothesis rejected:

- Rejected: continuing to wait is likely to make progress through SDMA packet
  execution. The SDMA engine has already drained and has no more logged work.

No files were changed for this inspection step beyond this status document.

Single next diagnostic step: inspect ROCr's SDMA completion path and the
generated ring contents/write-pointer convention. The next code target is to
make the SE SDMA MVP produce the completion signal expected by
`hsa_amd_memory_fill`, either by correctly decoding the skipped sync packet or
by handling the ROCr blocking command completion update directly.

The ROCr SDMA completion path was inspected with:

```bash
grep -R "SubmitBlockingCommand\|hsa_amd_memory_fill\|Build.*Fill\|ReleaseWriteAddress" \
  -n /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime
sed -n '200,430p' /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/amd_blit_sdma.cpp
sed -n '430,650p' /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/amd_blit_sdma.cpp
sed -n '800,875p' /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/amd_blit_sdma.cpp
grep -R "SDMA_OP_.*ATOMIC\|ATOMIC" -n src/dev src/gpu-compute | head -80
grep -R "SDMA_OP_" -n src/dev/amdgpu src/dev/hsa/se_sdma_engine.cc | head -120
sed -n '1120,1195p' src/dev/amdgpu/sdma_engine.cc
sed -n '300,350p' src/dev/amdgpu/sdma_packets.hh
```

Facts confirmed:

- ROCr `SubmitBlockingCommand()` sets the selected completion signal to `2`,
  submits the SDMA command sequence, then waits for the same signal to become
  `1`.
- `SubmitCommand()` appends a sync command after the actual fill/copy
  command. With platform atomic support, the sync command is an SDMA atomic
  decrement of the signal value. Without platform atomic support, it emits one
  or two FENCE packets writing the completion value.
- `SubmitLinearFillCommand()` submits `SDMA_PKT_CONSTANT_FILL` through
  `SubmitBlockingCommand()`, so a successful fill must eventually update the
  completion signal.
- gem5's full SDMA engine has `SDMA_OP_ATOMIC` handling for `ADD64`, but the
  current `SESDMAEngine` MVP only handles NOP, FENCE, TRAP, WRITE,
  CONST_FILL, and COPY.

Hypothesis accepted:

- The remaining hang is a missing completion-signal path, not a missing
  asynchronous notification switch. The wait in ROCr is satisfied by a memory
  update to the signal value; an interrupt/trap is only needed when the signal
  has an event mailbox.

Single next diagnostic step: implement or expose enough completion-signal
handling in `SESDMAEngine`. The preferred precise fix is to decode and execute
the sync packet appended after the fill command: `SDMA_OP_ATOMIC` ADD64 when
platform atomics are used, otherwise FENCE writes to the signal location. If
the queue write pointer remains unusable as a byte boundary, instrument the
first ring bytes around the fill command to locate the appended sync packet
before adding a broader fallback.

## 2026-06-26 SE SDMA atomic completion support

The requested `SESDMAEngine` `SDMA_OP_ATOMIC` MVP support was implemented.

Files changed:

- `src/dev/hsa/se_sdma_engine.cc`
- `src/dev/hsa/se_sdma_engine.hh`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document.

Implementation:

- `SESDMAEngine::decodeHeader()` now recognizes `SDMA_OP_ATOMIC`.
- Added `executeAtomic()` and `executeAtomicData()`.
- The MVP supports `SDMA_ATOMIC_ADD64`, matching ROCr's
  `BuildAtomicDecrementCommand()` completion path.
- The atomic target address is accessed through `dmaReadAddr()` and
  `dmaWriteAddr()`, so it can work with either an SE virtual signal address or
  a direct physical/framebuffer address.
- Unsupported atomic operations still fatal explicitly.

Verification completed:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_sdma_engine_has_phase1_packet_handlers \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_hsa_packet_processor_routes_sdma_doorbells \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kfd_sdma_queue_type_is_not_registered_as_compute
```

Result: all three focused pyunit checks passed.

```bash
git diff --check -- \
  src/dev/hsa/se_sdma_engine.cc \
  src/dev/hsa/se_sdma_engine.hh \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
```

Result: completed successfully with no output. Codex did not start a gem5
build or full simulation.

Single next diagnostic step: rebuild gem5 and rerun `hsa_sdma_fill` with the
same SDMA debug flags. Expected evidence of progress is an
`SDMA atomic ADD64` trace line after the fill packet and then
`end hsa_amd_memory_fill`. If the trace still drains immediately after the
first WRITE/zero header and never reaches ATOMIC, the queue write-pointer
boundary remains wrong and the next step is to dump the first ring bytes to
locate ROCr's appended sync packet.

## 2026-06-26 Direct HSA SDMA fill after atomic implementation

The user rebuilt gem5 and reran `hsa_sdma_fill` after the `SESDMAEngine`
atomic handler was added.

Commands run by Codex to inspect the result:

```bash
ls -lh m5out-hsa-sdma-fill
tail -100 m5out-hsa-sdma-fill/simout.txt
tail -140 m5out-hsa-sdma-fill/simerr.txt
tail -180 m5out-hsa-sdma-fill/sdma.trace
grep -n -E 'SDMA atomic|SDMA_OP_ATOMIC|ATOMIC|SDMA queue|SDMA write|CONST|FILL|FENCE|TRAP|drained|zero header|unsupported|fatal|panic|HSA_SDMA_FILL|hsa_amd_memory_fill|Exiting' \
  m5out-hsa-sdma-fill/sdma.trace \
  m5out-hsa-sdma-fill/simout.txt \
  m5out-hsa-sdma-fill/simerr.txt
```

First relevant observable result:

```text
[hsa_sdma_fill] begin hsa_amd_memory_fill
Exiting @ tick 34176051405 because user interrupt received.
```

The SDMA trace still ends with:

```text
SDMA queue 0 gpu_id 22124 queue_write_index 1 wrapped 1
SDMA queue 0 header 0x11402 opcode 0x2 sub_opcode 0x14
SDMA WRITE queue 0 sub_opcode 0x14, treating as linear untiled WRITE packet
SDMA write queue 0 4097 dwords to 0x100010040
SE SDMA direct physical write addr 0x100010040 size 16388
SDMA queue 0 saw zero header at rptr 16404; treating as empty ring space and draining to wptr 1
SDMA queue 0 drained at rptr 1
```

Facts confirmed:

- The new atomic handler did not execute in this run; there is no
  `SDMA atomic` or `SDMA atomic ADD64` trace line.
- The run still does not reach `end hsa_amd_memory_fill`.
- The previous infinite trace growth remains fixed: `sdma.trace` is small
  and stops after drain.
- The immediate blocker is still that the engine sees `queue_write_index 1`
  and then drains at the first zero header after the WRITE payload. The
  appended ROCr sync packet is not being reached.

Hypothesis accepted:

- The remaining problem is queue ring boundary/write-pointer interpretation,
  not the absence of an atomic packet executor.

Hypothesis rejected:

- Rejected: the added atomic handler was reached and failed. It was never
  reached in this run.

No files were changed for this inspection step beyond this status document.

Single next diagnostic step: add focused ring-dump instrumentation around the
first submitted SDMA command. Dump the first several dwords at the ring base,
the bytes immediately after the first decoded packet, and the raw
`Queue_write_ptr`/doorbell values. The goal is to locate the ROCr-appended
sync packet and determine whether `queue_write_index=1` is a packet count,
doorbell count, corrupted read of the write-pointer memory, or a side effect
of how gem5 maps the SDMA queue write pointer.

## 2026-06-26 SE SDMA ring dump instrumentation

Focused ring-dump instrumentation was added to locate the missing completion
sync packet.

Files changed:

- `src/dev/hsa/se_sdma_engine.cc`
- `src/dev/hsa/se_sdma_engine.hh`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document.

Implementation:

- Added `dumpRingDwords()` and `dumpRingDwordsData()` helpers to
  `SESDMAEngine`.
- On the first doorbell for a queue, the engine dumps the first 96 dwords at
  the SDMA ring base together with the queue's read/write indices and
  read/write pointer addresses.
- On the first zero header for a queue, the engine dumps 64 dwords around the
  zero-header location before draining.
- Added per-queue `dumpedDoorbellRing` and `dumpedZeroHeaderRing` guards so
  the instrumentation does not flood the trace.

Verification completed:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_sdma_engine_has_phase1_packet_handlers
```

Result: passed.

```bash
git diff --check -- \
  src/dev/hsa/se_sdma_engine.cc \
  src/dev/hsa/se_sdma_engine.hh \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
```

Result: completed successfully with no output. Codex did not start a gem5
build or full simulation.

Single next diagnostic step: rebuild gem5 and rerun `hsa_sdma_fill` with
`--debug-flags=GPUDriver,SESDMAEngine`. Inspect `sdma.trace` for
`SDMA ring dump` lines. The expected useful evidence is whether an
`SDMA_OP_ATOMIC` header or FENCE completion packet appears anywhere in the
first ring dump or just after the first WRITE payload.

## 2026-06-26 Direct HSA SDMA fill with ring dump

The user rebuilt gem5 and reran `hsa_sdma_fill` with the ring-dump
instrumentation.

Commands run by Codex to inspect the result:

```bash
tail -180 docs/debug/se-multigpu-status.md
git status --short -- \
  docs/debug/se-multigpu-status.md \
  src/dev/hsa/se_sdma_engine.cc \
  src/dev/hsa/se_sdma_engine.hh \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
ls -lh m5out-hsa-sdma-fill
tail -100 m5out-hsa-sdma-fill/simout.txt
tail -160 m5out-hsa-sdma-fill/simerr.txt
grep -n -E 'SDMA doorbell|queue_write_index|SDMA ring dump request|SDMA ring dump queue|SDMA queue 0 header|SDMA WRITE|SDMA write|direct physical write|zero header|drained|SDMA atomic|SDMA fence|unsupported|fatal|panic' \
  m5out-hsa-sdma-fill/sdma.trace
grep -R "SDMA_PKT_WRITE" -n \
  /tmp/ROCR-Runtime-rocm-4.0.0/src \
  /tmp/ROCK-Kernel-Driver-rocm-4.0.1 2>/dev/null | head -80
grep -R "BuildHdpFlushCommand\|SDMA_PKT_HDP_FLUSH" -n \
  /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime \
  /tmp/ROCR-Runtime-rocm-4.0.0/src/core/inc
sed -n '63,85p' src/dev/hsa/kfd_ioctl.h
sed -n '70,92p' /tmp/ROCK-Kernel-Driver-rocm-4.0.1/include/uapi/linux/kfd_ioctl.h
sed -n '205,255p' src/gpu-compute/gpu_compute_driver.cc
```

First relevant observable result:

```text
[hsa_sdma_fill] begin hsa_amd_memory_fill
```

The SDMA trace shows:

```text
SDMA doorbell queue 0 gpu_id 22124 raw_write_index 1
SDMA queue 0 gpu_id 22124 queue_write_index 1 wrapped 1
SDMA ring dump request queue 0 reason 'doorbell ring base' ring 0x7ffdf6940000 offset 0 dwords 96 read_index 0 write_index 1
SDMA queue 0 header 0x11402 opcode 0x2 sub_opcode 0x14
```

The first ring dwords are:

```text
offset 0   dword[0] 0x00011402
offset 4   dword[1] 0x00010040
offset 8   dword[2] 0x00000001
offset 12  dword[3] 0x00001000
offset 16  dword[4] 0x00000001
offset 20  dword[5] 0x00000001
offset 24  dword[6] 0x00000000
offset 28  dword[7] 0x00000000
offset 32  dword[8] 0xec8fc000
offset 36  dword[9] 0x00007ffd
offset 40  dword[10] 0xec940000
offset 44  dword[11] 0x00007ffd
```

The dump near the point where the current implementation reaches a zero
header shows zero-filled space, with sparse data words but no recognizable
SDMA packet header:

```text
offset 16384 dword[11] 0x00000001
offset 16400 dword[15] 0x00000000
offset 16404 dword[16] 0x00000000
```

Facts confirmed:

- No `SDMA_OP_ATOMIC` or FENCE completion packet is reached by the current
  decoder.
- The first packet in the ring is not the expected direct
  `SDMA_PKT_CONSTANT_FILL`; it is a WRITE-class packet with header
  `0x00011402`, destination `0x100010040`, and count field `0x1000`.
- Treating this first packet as the existing gem5 `sdmaWrite` format makes the
  MVP consume `4097` dwords of inline payload and advances the read pointer to
  `16404`, where it sees a zero header.
- The local `kfd_ioctl_create_queue_args` layout matches the ROCm 4.0.1 UAPI
  layout. The create-queue registration passes
  `args->write_pointer_address` to `registerSDMAQueue()`, so there is no
  obvious field-order ABI mismatch at this struct.
- `Queue_write_ptr` and the raw doorbell value are both observed as `1`, while
  the visible ring contents show a packet that the current decoder treats as
  much larger than one byte.

Hypotheses accepted:

- The immediate blocker is now packet-format and/or write-pointer semantics,
  not missing atomic execution. The atomic handler still has not been reached.
- The current `sdmaWrite` handler is unsafe for header `0x00011402`: this
  packet is not the simple linear WRITE sub-opcode supported by gem5's
  existing `sdmaWrite` structure.

Hypotheses rejected:

- Rejected: the completion packet is plainly visible immediately after the
  first decoded packet and simply lacks an executor. The ring dump did not
  show an atomic/fence header at the offsets inspected.
- Rejected: `kfd_ioctl_create_queue_args` is obviously ABI-misaligned with
  ROCm 4.0.1. The local layout matches the downloaded UAPI layout for the
  fields used here.

No source files were changed for this inspection step beyond this status
document.

Single next diagnostic step: stop treating unknown WRITE sub-opcode `0x14` as
the simple linear WRITE format. Add a narrow diagnostic/implementation path
for this observed packet: either identify its exact ROCr/SDMA packet type from
gfx9 SDMA docs/source and advance by its real packet size, or temporarily
skip this unsupported WRITE variant with a logged fixed packet size to expose
the next packet in the ring. Also instrument the current raw doorbell value
versus `Queue_write_ptr` after the CPU write path to determine why both are
`1` despite the ring containing a large command.

## 2026-06-26 SE SDMA WRITE sub-opcode 0x14 diagnostic guard

Implemented the next diagnostic step without launching a long gem5
simulation.

Commands run by Codex:

```bash
sed -n '1,220p' AGENTS.md
sed -n '1,260p' docs/debug/se-multigpu-status.md
git status --short
git diff -- \
  src/dev/hsa/se_sdma_engine.cc \
  src/dev/hsa/se_sdma_engine.hh \
  tests/pyunit/stdlib/test_se_viper_multigpu.py \
  docs/debug/se-multigpu-status.md
sed -n '1,220p' src/dev/hsa/se_sdma_engine.hh
sed -n '1,380p' src/dev/hsa/se_sdma_engine.cc
sed -n '900,980p' tests/pyunit/stdlib/test_se_viper_multigpu.py
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_sdma_engine_has_phase1_packet_handlers
git diff --check -- \
  docs/debug/se-multigpu-status.md \
  src/dev/hsa/se_sdma_engine.cc \
  src/dev/hsa/se_sdma_engine.hh \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
```

First relevant observable result:

```text
Ran 1 test in 0.000s
OK
```

Facts confirmed by this source-level step:

- The `WRITE` decoder now only accepts `SDMA_SUBOP_WRITE_LINEAR` as the
  existing `sdmaWrite` format.
- The observed `sub_opcode == 0x14` no longer executes as a large inline
  linear WRITE payload. It is handled as a diagnostic-only unsupported WRITE
  variant and advances by the five observed body dwords after the header.
- The diagnostic path dumps the ring at the new read pointer with reason
  `after unsupported WRITE variant`, so the next user run can identify
  whether a real next packet exists at offset 24 or whether the runtime has
  written a different ring/write-index encoding.
- The SDMA queue records `lastDoorbellIndex` and logs it alongside
  `queue_write_index`, wrapped write index, read index, and ring size.

Hypotheses accepted:

- Accepted for this diagnostic step: `0x00011402` must not be decoded as the
  simple linear `sdmaWrite` structure. Doing so overconsumes the ring and
  hides the real next condition.

Hypotheses rejected:

- Rejected: the previous `sdmaWrite` path is a safe fallback for all WRITE
  sub-opcodes. The observed packet proves that fallback is unsafe.

Files changed:

- `src/dev/hsa/se_sdma_engine.hh`
- `src/dev/hsa/se_sdma_engine.cc`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document

Verification completed:

- Focused pyunit test
  `test_se_sdma_engine_has_phase1_packet_handlers`: passed.
- `git diff --check` on the changed files: passed with no output.

Single next diagnostic step: user should rebuild gem5 and rerun the same
`hsa_sdma_fill` command with `--debug-flags=GPUDriver,SESDMAEngine`. Inspect
`m5out-hsa-sdma-fill/sdma.trace` for:

```text
unsupported WRITE sub-opcode 0x14 header 0x11402; diagnostic skip 5 dwords
SDMA ring dump request queue 0 reason 'after unsupported WRITE variant'
```

If the next header at offset 24 is still zero and both raw doorbell and
`Queue_write_ptr` remain `1`, the next investigation should focus on SDMA
ring/write-pointer units or ROCr's queue doorbell protocol rather than adding
more packet executors.

## 2026-06-26 Interpreting 0x11402 as AQL versus SDMA

Codex performed a read-only source inspection to identify where the observed
ring dword `0x00011402` can be authoritatively decoded.

Commands run by Codex:

```bash
rg -n \
  "0x00011402|0x11402|11402|SDMA_PKT_WRITE|SDMA_SUBOP|WRITE_LINEAR|untiled|TILED|sdma.*write|Build.*Write|Submit.*SDMA|hsa_amd_memory_fill|memory_fill" \
  /tmp/ROCR-Runtime-rocm-4.0.0 \
  /tmp/ROCK-Kernel-Driver-rocm-4.0.1 \
  src/dev/amdgpu src/dev/hsa 2>/dev/null
sed -n '1,580p' /tmp/ROCR-Runtime-rocm-4.0.0/src/core/inc/sdma_registers.h
sed -n '1,980p' /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/amd_blit_sdma.cpp
sed -n '1,340p' /tmp/ROCR-Runtime-rocm-4.0.0/src/core/inc/amd_blit_sdma.h
sed -n '2740,2925p' /tmp/ROCR-Runtime-rocm-4.0.0/src/inc/hsa.h
rg -n \
  "hsa_kernel_dispatch_packet_t|kernel_dispatch_packet|HSA_PACKET_TYPE_KERNEL_DISPATCH|HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS" \
  /tmp/ROCR-Runtime-rocm-4.0.0/src/inc \
  /tmp/ROCR-Runtime-rocm-4.0.0/src/core src
sed -n '100,220p' src/dev/hsa/hsa_packet_processor.cc
sed -n '63,92p' src/dev/hsa/kfd_ioctl.h
```

First relevant observable result:

```text
HSA_PACKET_TYPE_KERNEL_DISPATCH = 2
typedef struct hsa_kernel_dispatch_packet_s {
  uint16_t header;
  uint16_t setup;
  uint16_t workgroup_size_x;
  uint16_t workgroup_size_y;
  ...
}
```

Facts confirmed:

- ROCr's SDMA implementation constructs fill packets in
  `amd_blit_sdma.cpp::BuildFillCommand()` as `SDMA_PKT_CONSTANT_FILL`.
- `SDMA_PKT_CONSTANT_FILL` is defined in `sdma_registers.h` with `op =
  SDMA_OP_CONST_FILL`, not `op = 2`.
- ROCr's SDMA ring indices are explicitly byte-based:
  `RingIndexTy: 32/64-bit monotonic ring index, counting in bytes`.
- The ROCr SDMA code updates both `Queue_write_ptr` and `Queue_DoorBell` with
  the new byte index.
- `0x00011402` decodes naturally as the first 32 bits of an AQL kernel
  dispatch packet:
  - low 16 bits `0x1402` are the AQL `header`;
  - packet type bits are `2`, i.e. `HSA_PACKET_TYPE_KERNEL_DISPATCH`;
  - high 16 bits `0x0001` are the AQL `setup`, i.e. one-dimensional dispatch.
- The following dword `0x00010040` also matches AQL kernel dispatch layout:
  `workgroup_size_x = 64`, `workgroup_size_y = 1`.

Hypotheses accepted:

- The observed ring contents are more likely an AQL compute dispatch packet
  than a gfx9 SDMA packet.
- The correct next investigation is queue classification/doorbell routing:
  why a kernel-dispatch AQL packet is being registered or routed through the
  SE SDMA queue path.

Hypotheses rejected:

- Rejected: `0x00011402` should be decoded as an SDMA WRITE packet until a
  gfx9 SDMA sub-opcode table proves otherwise. The AQL packet layout explains
  the first two dwords directly, while ROCr's SDMA fill path does not generate
  that header.

No source files were changed for this inspection step beyond this status
document.

Single next diagnostic step: instrument queue creation and doorbell routing
to print `queue_id`, `queue_type`, `ring_base_address`,
`write_pointer_address`, `read_pointer_address`, `doorbell_offset`, and the
first ring dword at registration time. Confirm whether the queue with ring
base `0x7ffdf6940000` was created as `KFD_IOC_QUEUE_TYPE_COMPUTE_AQL` or
`KFD_IOC_QUEUE_TYPE_SDMA`, then fix the queue-type mapping/routing if the
compute queue is incorrectly entering `registerSDMAQueue()`.

## 2026-06-26 Queue creation and doorbell routing instrumentation

Implemented the queue-classification diagnostic step without launching a long
gem5 build or simulation.

Commands run by Codex:

```bash
sed -n '1,120p' /home/zhangds/.codex/plugins/cache/openai-curated/superpowers/7fd3161c/skills/systematic-debugging/SKILL.md
sed -n '1,80p' AGENTS.md
tail -180 docs/debug/se-multigpu-status.md
git status --short -- \
  docs/debug/se-multigpu-status.md \
  src/gpu-compute/gpu_compute_driver.cc \
  src/dev/hsa/hsa_packet_processor.cc \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
git diff -- \
  src/gpu-compute/gpu_compute_driver.cc \
  src/dev/hsa/hsa_packet_processor.cc \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
sed -n '180,285p' src/gpu-compute/gpu_compute_driver.cc
sed -n '100,220p' src/dev/hsa/hsa_packet_processor.cc
sed -n '780,980p' tests/pyunit/stdlib/test_se_viper_multigpu.py
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kfd_sdma_queue_type_is_not_registered_as_compute \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_hsa_packet_processor_routes_sdma_doorbells \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_sdma_engine_has_phase1_packet_handlers
git diff --check -- \
  docs/debug/se-multigpu-status.md \
  src/gpu-compute/gpu_compute_driver.cc \
  src/dev/hsa/hsa_packet_processor.cc \
  src/dev/hsa/se_sdma_engine.cc \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
```

First relevant observable result:

```text
Ran 3 tests in 0.001s
OK
```

Facts confirmed by this source-level step:

- `GPUComputeDriver::allocateQueue()` now logs the KFD create-queue request
  before routing it, including `gpu_id`, `queue_type`, `ring_base`,
  `ring_size`, `read_ptr`, `write_ptr`, EOP buffer address, and EOP size.
- After assigning a queue ID, the driver logs `queue_id`, `queue_type`,
  selected backend, `doorbell_offset`, doorbell size, GPU ID, ring base, ring
  size, read pointer, and write pointer.
- `HSAPacketProcessor::setDeviceQueueDesc()` logs compute/AQL queue
  registration fields.
- `HSAPacketProcessor::registerSDMAQueue()` logs SDMA queue registration
  fields.
- `HSAPacketProcessor::write()` logs each doorbell's MMIO offset, computed
  queue ID, raw doorbell value, adjusted value, whether it hit `sdmaQueues`,
  and whether it routes to `SESDMAEngine` or `HWScheduler`.
- `SESDMAEngine::registerQueue()` now performs a one-dword DMA read from the
  SDMA ring base and logs `first_ring_dword` at registration time.

Hypotheses accepted:

- Accepted: the next run should determine whether the suspicious AQL-looking
  packet entered the SDMA path because of queue-type classification, queue ID
  collision, or doorbell routing.

Hypotheses rejected:

- No protocol hypothesis was rejected by this source-only instrumentation
  step.

Files changed:

- `src/gpu-compute/gpu_compute_driver.cc`
- `src/dev/hsa/hsa_packet_processor.cc`
- `src/dev/hsa/se_sdma_engine.cc`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document

Verification completed:

- Focused pyunit tests for SDMA queue routing and SDMA engine diagnostics:
  passed.
- `git diff --check` on the changed files: passed with no output.

Single next diagnostic step: user should rebuild gem5 and rerun
`hsa_sdma_fill` with debug flags including `GPUDriver`, `HSAPacketProcessor`,
and `SESDMAEngine`. Inspect the trace for these lines:

```text
AMDKFD_IOC_CREATE_QUEUE request ...
AMDKFD_IOC_CREATE_QUEUE assigned ...
setDeviceQueueDesc queue_id ...
registerSDMAQueue queue_id ...
Register SDMA queue ... first_ring_dword ...
doorbell route daddr ... sdma_hit ...
doorbell route queue_id ... to SESDMAEngine
doorbell route queue_id ... to HWScheduler
```

The decisive comparison is whether the ring base containing first dword
`0x00011402` was assigned backend `compute` or `sdma`, and whether its
doorbell write incorrectly hits `sdmaQueues`.

## 2026-06-26 hsa_sdma_fill queue-type log analysis

Codex inspected the user-provided `m5out-hsa-sdma-fill` output after rerunning
`hsa_sdma_fill`. No gem5 build or full simulation was started by Codex.

Commands run by Codex:

```bash
sed -n '1,260p' AGENTS.md
sed -n '1,260p' docs/debug/se-multigpu-status.md
git status --short
git diff -- \
  src/dev/hsa/kfd_ioctl.h \
  src/gpu-compute/gpu_compute_driver.cc \
  src/dev/hsa/hsa_packet_processor.cc \
  src/dev/hsa/se_sdma_engine.cc \
  src/dev/hsa/se_sdma_engine.hh \
  docs/debug/se-multigpu-status.md
sed -n '1,220p' /home/zhangds/.codex/plugins/cache/openai-curated/superpowers/7fd3161c/skills/systematic-debugging/SKILL.md
ls -lh m5out-hsa-sdma-fill
tail -120 m5out-hsa-sdma-fill/simout.txt
tail -160 m5out-hsa-sdma-fill/simerr.txt
grep -n -E \
  "AMDKFD_IOC_CREATE_QUEUE|setDeviceQueueDesc|registerSDMAQueue|Register SDMA queue|doorbell route|SDMA doorbell|SDMA queue|0x00011402|unsupported WRITE|drained|saw zero" \
  m5out-hsa-sdma-fill/sdma.trace | tail -220
sed -n '1,120p' src/dev/hsa/kfd_ioctl.h
grep -R -n \
  "KFD_IOC_QUEUE_TYPE_COMPUTE|KFD_IOC_QUEUE_TYPE_SDMA|KFD_IOC_QUEUE_TYPE_COMPUTE_AQL|HSA_QUEUE_COMPUTE|HSA_QUEUE_SDMA|HSA_QUEUE_COMPUTE_AQL" \
  /tmp/ROCK-Kernel-Driver-rocm-4.0.1/include \
  /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/include 2>/dev/null | head -120
grep -R -n \
  "queue_type.*KFD_IOC_QUEUE_TYPE|KFD_IOC_QUEUE_TYPE.*queue_type|HSA_QUEUE_COMPUTE_AQL|HSA_QUEUE_SDMA" \
  /tmp/ROCR-Runtime-rocm-4.0.0 \
  /tmp/ROCK-Kernel-Driver-rocm-4.0.1 2>/dev/null | head -200
```

First relevant observable result:

```text
AMDKFD_IOC_CREATE_QUEUE request gpu_id 22124 queue_type 2 ring_base 0x7ffdf6940000 ...
registerSDMAQueue queue_id 0 ring_base 0x7ffdf6940000 ...
AMDKFD_IOC_CREATE_QUEUE assigned queue_id 0 queue_type 2 backend sdma ...
doorbell route queue_id 0 to SESDMAEngine
SDMA queue 0 header 0x11402 opcode 0x2 sub_opcode 0x14
```

The run entered:

```text
[hsa_sdma_fill] begin hsa_amd_memory_fill
```

and was later interrupted by the user:

```text
Exiting @ tick 44401155066 because user interrupt received.
```

Facts confirmed:

- The queue whose ring begins with `0x00011402` was created with ioctl
  `queue_type 2`.
- Current gem5 classified ioctl `queue_type 2` as `backend sdma`, registered
  it in `HSAPacketProcessor::sdmaQueues`, and routed its doorbell to
  `SESDMAEngine`.
- The ring contents are still AQL-looking, not SDMA-looking. `0x00011402`
  decodes as AQL packet type 2 plus setup 1, and the next dword
  `0x00010040` matches an AQL kernel dispatch workgroup field.
- The local `src/dev/hsa/kfd_ioctl.h` currently defines:

```text
KFD_IOC_QUEUE_TYPE_COMPUTE      1
KFD_IOC_QUEUE_TYPE_SDMA         2
KFD_IOC_QUEUE_TYPE_SDMA_XGMI    5
KFD_IOC_QUEUE_TYPE_COMPUTE_AQL  21
```

- ROCm 4.0.1's real KFD UAPI header defines:

```text
KFD_IOC_QUEUE_TYPE_COMPUTE      0x0
KFD_IOC_QUEUE_TYPE_SDMA         0x1
KFD_IOC_QUEUE_TYPE_COMPUTE_AQL  0x2
KFD_IOC_QUEUE_TYPE_SDMA_XGMI    0x3
```

- ROCm's public `hsakmttypes.h` separately defines `HSA_QUEUE_SDMA = 2` and
  `HSA_QUEUE_COMPUTE_AQL = 21`. These are hsakmt API values, not the ioctl
  values received by `kfd_ioctl_create_queue_args.queue_type`.
- ROCr source uses `hsaKmtCreateQueue(..., HSA_QUEUE_COMPUTE_AQL, ...)` for
  AQL queues and `HSA_QUEUE_SDMA` for SDMA blit queues. The kernel ioctl side
  consumes the compact `KFD_IOC_QUEUE_TYPE_*` values.
- The `registered_sdma_queues <bad format>u` trace field is an
  instrumentation format-string bug, likely from printing `size_t` with a
  format unsupported by gem5's tracing formatter. It is not the root cause.

Hypothesis accepted:

- The queue-type mapping change is wrong. gem5's ioctl-facing
  `KFD_IOC_QUEUE_TYPE_*` constants were changed to hsakmt `HSA_QUEUE_TYPE`
  values, so an actual `KFD_IOC_QUEUE_TYPE_COMPUTE_AQL` queue with numeric
  value `2` is misclassified as SDMA.

Hypotheses rejected:

- Rejected: this run proves that `hsa_amd_memory_fill` generated an SDMA
  WRITE packet with header `0x00011402`. The more direct explanation is that
  gem5 routed an AQL compute queue into the SDMA engine.
- Rejected: the current blocker is an unimplemented SDMA WRITE sub-opcode
  `0x14`. That decoder path was reached only after queue misclassification.

Files changed:

- this status document only.

Verification completed:

- Read-only log inspection of `m5out-hsa-sdma-fill/{simout.txt,simerr.txt,sdma.trace}`.
- Read-only comparison against ROCm 4.0.1 KFD UAPI and ROCr queue creation
  source snippets already present under `/tmp` and `.deps`.

Single next diagnostic step: revert `src/dev/hsa/kfd_ioctl.h` queue constants
to the KFD UAPI values (`COMPUTE=0`, `SDMA=1`, `COMPUTE_AQL=2`,
`SDMA_XGMI=3`), adjust the queue-routing unit test expectations, fix the
`registered_sdma_queues` trace format, rebuild gem5, and rerun
`hsa_sdma_fill`. The expected result after the fix is that `queue_type 2`
goes to `setDeviceQueueDesc`/`HWScheduler`, not to `registerSDMAQueue` or
`SESDMAEngine`.

## 2026-06-26 Queue-type mapping fix

Implemented the accepted queue-type mapping fix. No gem5 build or full
simulation was started by Codex.

Commands run by Codex:

```bash
sed -n '1,220p' /home/zhangds/.codex/plugins/cache/openai-curated/superpowers/7fd3161c/skills/test-driven-development/SKILL.md
tail -180 docs/debug/se-multigpu-status.md
git status --short
git diff -- \
  src/dev/hsa/kfd_ioctl.h \
  src/gpu-compute/gpu_compute_driver.cc \
  src/dev/hsa/hsa_packet_processor.cc \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kfd_queue_type_constants_match_ioctl_uapi
rg -n \
  "HSA_QUEUE_TYPE ABI|KFD_IOC_QUEUE_TYPE_COMPUTE|registered_sdma_queues|test_kfd_sdma_queue_type" \
  src/dev/hsa/kfd_ioctl.h \
  src/dev/hsa/hsa_packet_processor.cc \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kfd_queue_type_constants_match_ioctl_uapi \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_hsa_packet_processor_routes_sdma_doorbells \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_sdma_engine_has_phase1_packet_handlers
git diff -- \
  src/dev/hsa/kfd_ioctl.h \
  src/dev/hsa/hsa_packet_processor.cc \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
git diff --check -- \
  docs/debug/se-multigpu-status.md \
  src/dev/hsa/kfd_ioctl.h \
  src/dev/hsa/hsa_packet_processor.cc \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
rg -n \
  "HSA_QUEUE_TYPE ABI|registered_sdma_queues %zu|KFD_IOC_QUEUE_TYPE_SDMA         2|KFD_IOC_QUEUE_TYPE_COMPUTE_AQL  21" \
  src/dev/hsa/kfd_ioctl.h \
  src/dev/hsa/hsa_packet_processor.cc \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
```

First relevant observable result:

```text
FAIL: test_kfd_queue_type_constants_match_ioctl_uapi
AssertionError: 'KFD_IOC_QUEUE_TYPE_COMPUTE      0' not found ...
```

This was the expected RED result after changing the test expectation and
before changing production code.

Implemented changes:

- `src/dev/hsa/kfd_ioctl.h` now defines queue types with KFD ioctl UAPI
  values:

```text
KFD_IOC_QUEUE_TYPE_COMPUTE      0
KFD_IOC_QUEUE_TYPE_SDMA         1
KFD_IOC_QUEUE_TYPE_COMPUTE_AQL  2
KFD_IOC_QUEUE_TYPE_SDMA_XGMI    3
```

- The comment now explicitly says these values are KFD ioctl UAPI values,
  not hsakmt's public `HSA_QUEUE_TYPE` enum.
- `test_kfd_queue_type_constants_match_ioctl_uapi` now asserts the KFD UAPI
  constants and rejects the incorrect `HSA_QUEUE_TYPE ABI` comment.
- `HSAPacketProcessor` now prints `registered_sdma_queues` using an explicit
  `uint64_t` cast and `%lu`, avoiding the previous `<bad format>u` trace
  output.

Facts confirmed:

- The focused queue-type test failed before the production change and passed
  after it.
- With this source state, ioctl `queue_type 2` will match
  `KFD_IOC_QUEUE_TYPE_COMPUTE_AQL` and enter the compute/AQL backend path.
- ioctl `queue_type 1` is the SDMA path.

Files changed:

- `src/dev/hsa/kfd_ioctl.h`
- `src/dev/hsa/hsa_packet_processor.cc`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document

Verification completed:

```text
Ran 3 tests in 0.001s
OK
```

The passing focused tests were:

- `test_kfd_queue_type_constants_match_ioctl_uapi`
- `test_hsa_packet_processor_routes_sdma_doorbells`
- `test_se_sdma_engine_has_phase1_packet_handlers`

`git diff --check` on the changed files passed with no output. A final
search found no production occurrence of the rejected old mappings or the
`registered_sdma_queues %zu` format.

Single next diagnostic step: user should rebuild gem5 and rerun
`hsa_sdma_fill` with `GPUDriver,HSAPacketProcessor,SESDMAEngine` debug flags.
The expected trace change is:

```text
AMDKFD_IOC_CREATE_QUEUE ... queue_type 2 ... backend compute
setDeviceQueueDesc queue_id 0 ...
doorbell route queue_id 0 to HWScheduler
```

There should be no `registerSDMAQueue` for `queue_type 2` and no
`SESDMAEngine` decode of `0x00011402`. If ROCr later creates a real SDMA
queue, it should appear as ioctl `queue_type 1`.

## 2026-06-26 hsa_sdma_fill rerun after queue-type fix

Codex inspected the user rerun in `m5out-hsa-sdma-fill` after the queue-type
mapping fix. No gem5 build or full simulation was started by Codex.

Commands run by Codex:

```bash
sed -n '1,80p' AGENTS.md
tail -260 docs/debug/se-multigpu-status.md
git status --short
git diff -- \
  src/dev/hsa/kfd_ioctl.h \
  src/gpu-compute/gpu_compute_driver.cc \
  src/dev/hsa/hsa_packet_processor.cc \
  src/dev/hsa/se_sdma_engine.cc \
  src/dev/hsa/se_sdma_engine.hh \
  tests/pyunit/stdlib/test_se_viper_multigpu.py \
  docs/debug/se-multigpu-status.md
ls -lh m5out-hsa-sdma-fill
tail -160 m5out-hsa-sdma-fill/simout.txt
tail -220 m5out-hsa-sdma-fill/simerr.txt
rg -n \
  "AMDKFD_IOC_CREATE_QUEUE|setDeviceQueueDesc|registerSDMAQueue|Register SDMA queue|doorbell route|to HWScheduler|to SESDMAEngine|SDMA doorbell|SDMA queue|0x00011402|unsupported|HSA_SDMA_FILL|hsa_sdma_fill|Exiting|panic|fatal|warn:" \
  m5out-hsa-sdma-fill/sdma.trace \
  m5out-hsa-sdma-fill/simout.txt \
  m5out-hsa-sdma-fill/simerr.txt
rg -n \
  "queue_type 1|queue_type 2|backend sdma|backend compute|registerSDMAQueue|Register SDMA queue|SESDMAEngine|SDMA doorbell|SDMA queue|doorbell route|setDeviceQueueDesc|unsetDeviceQueueDesc|GPU Blit Kernel Completed|HSA_SDMA_FILL_PASSED" \
  m5out-hsa-sdma-fill/sdma.trace \
  m5out-hsa-sdma-fill/simout.txt \
  m5out-hsa-sdma-fill/simerr.txt
rg -c \
  "registerSDMAQueue|backend sdma|queue_type 1|SESDMAEngine|SDMA doorbell|SDMA queue" \
  m5out-hsa-sdma-fill/sdma.trace
rg -n \
  "GPU Blit Kernel Completed dump and reset|Blit Kernel Completed|hsa_amd_memory_fill|HSA_ENABLE_SDMA|memory_fill|Submit.*SDMA|BuildFillCommand|KernelBlit" \
  /tmp/ROCR-Runtime-rocm-4.0.0 src tests/test-progs/gpu/xgmi-peer-vram 2>/dev/null | head -200
sed -n '200,260p' /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/hsa_ext_amd.cpp
sed -n '520,565p' src/gpu-compute/shader.cc
sed -n '150,178p' tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_fill.cpp
rg -n \
  "FillMemory\\(|blit.*fill|Blit.*Fill|Sdma.*Fill|memory_fill" \
  /tmp/ROCR-Runtime-rocm-4.0.0/src/core \
  /tmp/ROCR-Runtime-rocm-4.0.0/src 2>/dev/null | head -120
sed -n '500,535p' /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/runtime.cpp
sed -n '665,700p' /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/amd_gpu_agent.cpp
rg -n \
  "BlitDevToDev|blits_\\[|BlitSdma|BlitKernel|enable_sdma" \
  /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/amd_gpu_agent.cpp \
  /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/runtime.cpp | head -160
sed -n '520,610p' /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/amd_gpu_agent.cpp
sed -n '535,565p' /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/runtime.cpp
```

First relevant observable result:

```text
[hsa_sdma_fill] begin hsa_amd_memory_fill
GPU Blit Kernel Completed dump and reset
[hsa_sdma_fill] end hsa_amd_memory_fill: HSA_STATUS_SUCCESS
HSA_SDMA_FILL_PASSED
Exiting @ tick 14561895528 because exiting with last active thread context.
```

Queue-routing trace after the fix:

```text
AMDKFD_IOC_CREATE_QUEUE request ... queue_type 2 ...
setDeviceQueueDesc queue_id 0 ...
AMDKFD_IOC_CREATE_QUEUE assigned ... queue_type 2 backend compute ...
doorbell route ... sdma_hit 0 registered_sdma_queues 0
doorbell route queue_id 0 to HWScheduler
unsetDeviceQueueDesc queue_id 0 ...
```

Facts confirmed:

- The queue-type mapping fix worked in the full rerun.
- `queue_type 2` now routes to the compute/AQL backend.
- The AQL doorbell routes to `HWScheduler`, not `SESDMAEngine`.
- There is no `registerSDMAQueue`, no `backend sdma`, no `queue_type 1`, and
  no `SESDMAEngine` packet decode in this run.
- The trace-format fix worked: `registered_sdma_queues 0` is printed
  correctly, with no `<bad format>u`.
- The direct HSA smoke completes successfully and exits normally:
  `HSA_SDMA_FILL_PASSED`.

ROCr path facts:

- `hsa_amd_memory_fill()` calls `Runtime::FillMemory()`.
- `Runtime::FillMemory()` selects `GpuAgent::DmaFill()` for a GPU-owned
  allocation.
- `GpuAgent::DmaFill()` calls `blits_[BlitDevToDev]->SubmitLinearFillCommand`.
- ROCr initializes `BlitDevToDev` with `CreateBlitKernel(...)`
  unconditionally.
- `HSA_ENABLE_SDMA` affects the `blit_lambda` used for H2D, D2H, and XGMI
  blits, but not the `BlitDevToDev` fill path.
- The line `GPU Blit Kernel Completed dump and reset` comes from gem5's
  shader blit-kernel completion path, confirming the runtime chose a compute
  blit kernel.

Hypotheses accepted:

- Accepted: the previous `0x00011402` SDMA decode was entirely caused by
  queue-type misclassification.
- Accepted: `hsa_amd_memory_fill` on this GPU allocation is not a valid test
  of `SESDMAEngine`; ROCr 4.0.1 uses the compute blit-kernel path for this
  device-to-device fill.

Hypotheses rejected:

- Rejected: after the queue-type fix, `hsa_amd_memory_fill` should create a
  KFD SDMA queue or exercise `SESDMAEngine`.
- Rejected: the current run reveals another missing SESDMA packet opcode.
  The SDMA engine was not involved.

Files changed:

- this status document only.

Verification completed:

- Read-only log inspection of `m5out-hsa-sdma-fill/{simout.txt,simerr.txt,sdma.trace}`.
- Read-only source inspection of ROCr fill/blit selection and gem5 blit-kernel
  completion marker.

Single next diagnostic step: stop using `hsa_amd_memory_fill` as the SDMA
exercise. Add or run a direct HSA async-copy smoke that performs H2D and/or
D2H copy with `HSA_ENABLE_SDMA=1`, because ROCr's H2D/D2H blit path can
select `BlitSdma`. The expected SDMA-positive trace would contain ioctl
`queue_type 1`, `backend sdma`, `registerSDMAQueue`, and a doorbell routed
to `SESDMAEngine`.

## 2026-06-26 HSA async-copy smoke for SE SDMA MVP

Implemented a standalone HSA async-copy smoke to exercise ROCr's H2D/D2H
copy path instead of the already-rejected `hsa_amd_memory_fill` path. Codex
did not start a full gem5 simulation.

Commands run by Codex:

```bash
rg -n \
  "hsa_sdma_fill|SESDMAEngine|async-copy|SDMA MVP|hipMemset vs SDMA" \
  /home/zhangds/.codex/memories/MEMORY.md || true
sed -n '1,120p' AGENTS.md
tail -220 docs/debug/se-multigpu-status.md
git status --short
git diff -- \
  tests/test-progs/gpu/xgmi-peer-vram/Makefile \
  tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_fill.cpp \
  tests/pyunit/stdlib/test_se_viper_multigpu.py \
  docs/debug/se-multigpu-status.md
sed -n '1,240p' tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_fill.cpp
sed -n '1,120p' tests/test-progs/gpu/xgmi-peer-vram/Makefile
sed -n '780,890p' tests/pyunit/stdlib/test_se_viper_multigpu.py
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_direct_hsa_sdma_async_copy_smoke_uses_hsa_amd_memory_async_copy
make -C tests/test-progs/gpu/xgmi-peer-vram \
  hsa_sdma_async_copy \
  ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  CXX=g++
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_direct_hsa_sdma_fill_smoke_uses_hsa_amd_memory_fill \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_direct_hsa_sdma_async_copy_smoke_uses_hsa_amd_memory_async_copy \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_hsa_packet_processor_routes_sdma_doorbells \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_sdma_engine_has_phase1_packet_handlers
git diff --check -- \
  tests/test-progs/gpu/xgmi-peer-vram/Makefile \
  tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_async_copy.cpp \
  tests/pyunit/stdlib/test_se_viper_multigpu.py \
  docs/debug/se-multigpu-status.md
git diff -- \
  tests/test-progs/gpu/xgmi-peer-vram/Makefile \
  tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_async_copy.cpp \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
git status --short -- \
  tests/test-progs/gpu/xgmi-peer-vram/Makefile \
  tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_async_copy.cpp \
  tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_async_copy \
  tests/pyunit/stdlib/test_se_viper_multigpu.py \
  docs/debug/se-multigpu-status.md
```

First relevant observable result:

```text
FileNotFoundError: ... hsa_sdma_async_copy.cpp
```

This was the expected RED result before adding the new smoke source and
Makefile target.

Implemented changes:

- Added `tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_async_copy.cpp`.
- Added `HSA_SDMA_ASYNC_COPY_TARGET` and
  `HSA_SDMA_ASYNC_COPY_SRC` to the xgmi-peer-vram Makefile.
- Added `hsa_sdma_async_copy` to the Makefile `all` and `clean` targets.
- Added a pyunit source/Makefile structure test for the async-copy smoke.

Smoke behavior:

- Initializes HSA.
- Finds one CPU agent and one GPU agent.
- Finds an allocable global GPU memory pool.
- Allocates a 4096-byte GPU buffer.
- Fills a host source buffer with a deterministic byte pattern.
- Performs H2D `hsa_amd_memory_async_copy()` from host source to GPU buffer.
- Waits for the H2D completion signal with
  `hsa_signal_wait_scacquire(..., HSA_SIGNAL_CONDITION_LT, ...)`.
- Performs D2H `hsa_amd_memory_async_copy()` from GPU buffer to host
  destination.
- Waits for the D2H completion signal.
- Verifies the returned host buffer byte-for-byte.
- Prints `HSA_SDMA_ASYNC_COPY_PASSED` on success.

Facts confirmed:

- The new smoke is independent of HIP and does not call `hipMemcpy`,
  `hipMemset`, or `hsa_amd_memory_fill`.
- The test program builds successfully with ROCm 4.0.1 headers and libraries:

```text
g++ -O2 -I.../include -L.../lib -o hsa_sdma_async_copy \
  hsa_sdma_async_copy.cpp -lhsa-runtime64 -lhsakmt
```

Verification completed:

```text
Ran 4 tests in 0.001s
OK
```

The passing focused tests were:

- `test_direct_hsa_sdma_fill_smoke_uses_hsa_amd_memory_fill`
- `test_direct_hsa_sdma_async_copy_smoke_uses_hsa_amd_memory_async_copy`
- `test_hsa_packet_processor_routes_sdma_doorbells`
- `test_se_sdma_engine_has_phase1_packet_handlers`

`git diff --check` on the changed files passed with no output.

Files changed:

- `tests/test-progs/gpu/xgmi-peer-vram/Makefile`
- `tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_async_copy.cpp`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document

Generated local build artifact:

- `tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_async_copy`

Single next diagnostic step: rebuild gem5 if needed, then run the new smoke
under SE with SDMA debug flags:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hsa-sdma-async-copy \
  -r -e \
  --stdout-file=simout.txt \
  --stderr-file=simerr.txt \
  --debug-flags=GPUDriver,HSAPacketProcessor,SESDMAEngine \
  --debug-file=sdma.trace \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type atomic \
  --app tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_async_copy \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu \
  --env HSA_ENABLE_SDMA=1
```

Expected SDMA-positive trace:

```text
AMDKFD_IOC_CREATE_QUEUE ... queue_type 1 ... backend sdma
registerSDMAQueue queue_id ...
doorbell route ... sdma_hit 1 ...
doorbell route queue_id ... to SESDMAEngine
```

If the trace still contains only `queue_type 2 backend compute`, then ROCr
fell back to the compute blit path for this H2D/D2H case and the next step is
to inspect why `CreateBlitSdma()` returned null in `GpuAgent::InitDma()`.

## 2026-06-26 hsa_sdma_async_copy first SDMA-positive run

Codex inspected the user run in `m5out-hsa-sdma-async-copy`. No gem5 build or
full simulation was started by Codex.

Commands run by Codex:

```bash
sed -n '1,100p' AGENTS.md
tail -220 docs/debug/se-multigpu-status.md
git status --short
git diff -- \
  tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_async_copy.cpp \
  tests/test-progs/gpu/xgmi-peer-vram/Makefile \
  src/dev/hsa/kfd_ioctl.h \
  src/gpu-compute/gpu_compute_driver.cc \
  src/dev/hsa/hsa_packet_processor.cc \
  src/dev/hsa/se_sdma_engine.cc \
  docs/debug/se-multigpu-status.md
ls -lh m5out-hsa-sdma-async-copy
tail -180 m5out-hsa-sdma-async-copy/simout.txt
tail -240 m5out-hsa-sdma-async-copy/simerr.txt
rg -n \
  "AMDKFD_IOC_CREATE_QUEUE|queue_type|backend sdma|backend compute|setDeviceQueueDesc|registerSDMAQueue|Register SDMA queue|doorbell route|to SESDMAEngine|to HWScheduler|SDMA doorbell|SDMA queue|unsupported|HSA_SDMA_ASYNC_COPY|hsa_sdma_async_copy|hsa_amd_memory_async_copy|panic|fatal|Exiting|GPU Blit Kernel Completed|GPU Kernel Completed" \
  m5out-hsa-sdma-async-copy/sdma.trace \
  m5out-hsa-sdma-async-copy/simout.txt \
  m5out-hsa-sdma-async-copy/simerr.txt
rg -n \
  "SDMA_OP_|#define SDMA_OP|enum.*SDMA|opcode.*0x8|OP_POLL|POLL_REG|TRAP|FENCE|CONST_FILL|COPY" \
  /tmp/ROCR-Runtime-rocm-4.0.0/src/core/inc/sdma_registers.h \
  /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/amd_blit_sdma.cpp \
  src/dev/hsa/se_sdma_engine.cc \
  src/dev/hsa/se_sdma_engine.hh | head -240
sed -n '1,220p' /tmp/ROCR-Runtime-rocm-4.0.0/src/core/inc/sdma_registers.h
sed -n '130,210p' m5out-hsa-sdma-async-copy/sdma.trace
sed -n '165,260p' m5out-hsa-sdma-async-copy/sdma.trace
sed -n '335,390p' /tmp/ROCR-Runtime-rocm-4.0.0/src/core/inc/sdma_registers.h
sed -n '850,880p' /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/amd_blit_sdma.cpp
sed -n '920,945p' /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/amd_blit_sdma.cpp
sed -n '1,220p' src/dev/hsa/se_sdma_engine.hh
sed -n '220,380p' src/dev/hsa/se_sdma_engine.cc
sed -n '380,680p' src/dev/hsa/se_sdma_engine.cc
rg -n \
  "struct sdma|sdmaPoll|POLL|REGMEM|SDMA_OP_POLL|SDMA_OP_" \
  src/dev/amdgpu/sdma_packets.hh src/dev/amdgpu src/dev/hsa | head -240
sed -n '220,270p' src/dev/amdgpu/sdma_packets.hh
sed -n '950,1070p' src/dev/amdgpu/sdma_engine.cc
rg -n \
  "hdp_flush_cmd|BuildHdpFlushCommand|BuildPollCommand|SubmitCommand|poll_command_size_|flush_command_size_|Build.*Command" \
  /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/amd_blit_sdma.cpp \
  /tmp/ROCR-Runtime-rocm-4.0.0/src/core/inc/amd_blit_sdma.h | head -220
sed -n '110,170p' /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/amd_blit_sdma.cpp
sed -n '250,360p' /tmp/ROCR-Runtime-rocm-4.0.0/src/core/runtime/amd_blit_sdma.cpp
sed -n '60,112p' /tmp/ROCR-Runtime-rocm-4.0.0/src/core/util/flag.h
rg -n \
  "enable_sdma_hdp_flush" \
  /tmp/ROCR-Runtime-rocm-4.0.0/src/core \
  /tmp/ROCR-Runtime-rocm-4.0.0/src | head -80
```

First relevant observable result:

```text
[hsa_sdma_async_copy] begin H2D async copy bytes=4096
...
AMDKFD_IOC_CREATE_QUEUE request ... queue_type 1 ...
registerSDMAQueue queue_id 0 ...
AMDKFD_IOC_CREATE_QUEUE assigned ... queue_type 1 backend sdma ...
doorbell route ... sdma_hit 1 registered_sdma_queues 1
doorbell route queue_id 0 to SESDMAEngine
SDMA queue 0 header 0x8 opcode 0x8 sub_opcode 0
src/dev/hsa/se_sdma_engine.cc:355: fatal:
  SE SDMA unsupported opcode 0x8 sub-opcode 0 header 0x8
```

Facts confirmed:

- This is the first SDMA-positive SE run in this debugging sequence.
- ROCr created an actual KFD SDMA queue:
  `queue_type 1`, `backend sdma`.
- `HSAPacketProcessor` registered the queue with `SESDMAEngine`.
- The SDMA doorbell hit `sdmaQueues` and routed to `SESDMAEngine`.
- `SESDMAEngine` began decoding the SDMA ring.
- The run aborted before async-copy completion because
  `SESDMAEngine` does not implement opcode `0x8`.
- ROCr/gfx9 defines opcode `0x8` as `SDMA_OP_POLL_REGMEM`.
- The observed header is exactly `0x8`, meaning the first packet is not the
  normal memory-poll dependency form built by `BuildPollCommand()`; it is more
  consistent with the HDP flush `POLL_REGMEM` packet inserted before the copy.
- ROCr's `HSA_ENABLE_SDMA_HDP_FLUSH` default is enabled unless explicitly set
  to `0`.
- `SubmitCommand()` inserts the HDP flush packet before copying the user's
  SDMA COPY packet into the ring when `enable_sdma_hdp_flush()` is true and
  gfx9 supports HDP flush.

Hypotheses accepted:

- Accepted: `hsa_sdma_async_copy` is the correct smoke to reach the SE SDMA
  path; the queue/doorbell plumbing is working.
- Accepted: the next MVP gap is handling `SDMA_OP_POLL_REGMEM`, specifically
  the HDP-flush form emitted before H2D/D2H copy on gfx9.

Hypotheses rejected:

- Rejected: ROCr fell back to compute blit for this H2D case. It did not;
  the trace shows a real SDMA queue and SESDMAEngine doorbell.
- Rejected: the queue-type mapping is still wrong. The trace has the expected
  `queue_type 1 backend sdma`.

Files changed:

- this status document only.

Verification completed:

- Read-only inspection of
  `m5out-hsa-sdma-async-copy/{simout.txt,simerr.txt,sdma.trace}`.
- Read-only comparison against ROCr `sdma_registers.h`,
  `amd_blit_sdma.cpp`, `flag.h`, and gem5's existing
  `sdmaPollRegMem` packet definitions.

Single next diagnostic step: perform one fast isolation run with the same
command plus:

```text
--env HSA_ENABLE_SDMA_HDP_FLUSH=0
```

If disabling HDP flush exposes a subsequent `SDMA_OP_COPY` packet, then the
current `SDMA_OP_COPY` MVP can be validated before implementing HDP flush. If
the next packet still blocks, inspect that packet. For the full default ROCr
path, implement `SDMA_OP_POLL_REGMEM` in `SESDMAEngine`; for the HDP-flush
reg-poll form in SE, the MVP behavior can likely treat it as a no-op/complete
packet, while memory-poll dependency packets should use the existing
`sdmaPollRegMem` layout and comparison logic from `src/dev/amdgpu/sdma_engine.cc`.

## 2026-06-26 hsa_sdma_async_copy isolated no-HDP-flush validation

The user reran the async-copy smoke with the recommended isolation setting:

```text
--env HSA_ENABLE_SDMA=1 --env HSA_ENABLE_SDMA_HDP_FLUSH=0
```

Codex inspected the resulting logs in `m5out-hsa-sdma-async-copy`. No gem5
build or full simulation was started by Codex.

Commands run by Codex:

```bash
sed -n '1,120p' AGENTS.md
tail -260 docs/debug/se-multigpu-status.md
git status --short
git diff -- \
  docs/debug/se-multigpu-status.md \
  src/dev/hsa/se_sdma_engine.cc \
  src/dev/hsa/se_sdma_engine.hh \
  src/dev/hsa/kfd_ioctl.h \
  tests/test-progs/gpu/xgmi-peer-vram/Makefile \
  tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_async_copy.cpp
find m5out-hsa-sdma-async-copy -maxdepth 1 -type f -printf '%T@ %p %s\n'
tail -220 m5out-hsa-sdma-async-copy/simout.txt
tail -260 m5out-hsa-sdma-async-copy/simerr.txt
rg -n \
  "HSA_ENABLE_SDMA_HDP_FLUSH|AMDKFD_IOC_CREATE_QUEUE|queue_type|backend sdma|backend compute|registerSDMAQueue|setDeviceQueueDesc|doorbell route|to SESDMAEngine|to HWScheduler|SDMA doorbell|SDMA queue|header|opcode|unsupported|COPY|FENCE|TRAP|ATOMIC|POLL|HSA_SDMA_ASYNC_COPY|hsa_sdma_async_copy|hsa_amd_memory_async_copy|wait completion|mismatch|fatal|panic|Exiting|GPU Blit Kernel Completed|GPU Kernel Completed" \
  m5out-hsa-sdma-async-copy/sdma.trace \
  m5out-hsa-sdma-async-copy/simout.txt \
  m5out-hsa-sdma-async-copy/simerr.txt
sed -n '160,276p' m5out-hsa-sdma-async-copy/sdma.trace
sed -n '300,416p' m5out-hsa-sdma-async-copy/sdma.trace
rg -n \
  "SDMA copy|SDMA atomic|SDMA atomic ADD64|drained|header 0x1|header 0x5e00000a|COPY too|unsupported|fatal|panic|HSA_SDMA_ASYNC_COPY_PASSED" \
  m5out-hsa-sdma-async-copy/sdma.trace \
  m5out-hsa-sdma-async-copy/simout.txt \
  m5out-hsa-sdma-async-copy/simerr.txt
```

First relevant observable result:

```text
[hsa_sdma_async_copy] begin H2D async copy bytes=4096
[hsa_sdma_async_copy] hsa_amd_memory_async_copy: HSA_STATUS_SUCCESS
[hsa_sdma_async_copy] wait H2D completion
[hsa_sdma_async_copy] wait completion: HSA_STATUS_SUCCESS
...
[hsa_sdma_async_copy] begin D2H async copy bytes=4096
[hsa_sdma_async_copy] hsa_amd_memory_async_copy: HSA_STATUS_SUCCESS
[hsa_sdma_async_copy] wait D2H completion
[hsa_sdma_async_copy] wait completion: HSA_STATUS_SUCCESS
...
HSA_SDMA_ASYNC_COPY_PASSED
Exiting @ tick 14362648641 because exiting with last active thread context.
```

Facts confirmed:

- The isolated no-HDP-flush async-copy run completed successfully.
- There was no fatal, panic, or max-tick exit.
- ROCr created real KFD SDMA queues for both transfers:
  `queue_type 1`, `backend sdma`.
- `HSAPacketProcessor` registered both queues with `SESDMAEngine`.
- Doorbells for both queues hit the SDMA queue table and routed to
  `SESDMAEngine`, not `HWScheduler`.
- Queue 0 performed the H2D copy:
  - header `0x1`, opcode `0x1`;
  - count dword `0x00000fff`, meaning 4096 bytes;
  - source dwords identify the host source buffer;
  - destination dwords identify the GPU buffer at `0x7ffdf6a00000`.
- Queue 1 performed the D2H copy:
  - header `0x1`, opcode `0x1`;
  - count dword `0x00000fff`, meaning 4096 bytes;
  - source dwords identify the same GPU buffer at `0x7ffdf6a00000`;
  - destination dwords identify the host destination buffer.
- Both queues then executed completion packets:
  `header 0x5e00000a`, opcode `0xa`, atomic op `0x2f`.
- The implemented `SDMA_OP_ATOMIC` ADD64 path decremented the completion
  signal from `0x1` to `0`, allowing the host wait to complete.
- Both SDMA queues drained and were later unregistered.

Hypotheses accepted:

- Accepted: disabling `HSA_ENABLE_SDMA_HDP_FLUSH` isolates the current
  `SDMA_OP_COPY` plus `SDMA_OP_ATOMIC` MVP path.
- Accepted: the current SE SDMA MVP is sufficient for HSA H2D and D2H
  `hsa_amd_memory_async_copy()` when ROCr does not prepend the HDP-flush
  `POLL_REGMEM` packet.
- Accepted: queue-type mapping, SDMA queue registration, SDMA doorbell
  routing, COPY execution, and signal completion via ATOMIC are all working
  for this smoke.

Hypotheses rejected:

- Rejected: the async-copy smoke still falls back to compute blit in this
  isolated run. The trace shows `backend sdma` and SESDMAEngine execution.
- Rejected: the prior opcode `0x8` failure was required for the COPY packet
  itself. With HDP flush disabled, the first packet is the expected
  `SDMA_OP_COPY`.

Files changed:

- this status document only.

Verification completed:

- Read-only inspection of
  `m5out-hsa-sdma-async-copy/{simout.txt,simerr.txt,sdma.trace}`.
- Verified stdout success marker `HSA_SDMA_ASYNC_COPY_PASSED`.
- Verified SDMA-positive trace for both H2D and D2H transfers.

Single next diagnostic step: implement `SDMA_OP_POLL_REGMEM` handling in
`SESDMAEngine` for the default ROCr path, then rerun the same
`hsa_sdma_async_copy` smoke without `HSA_ENABLE_SDMA_HDP_FLUSH=0`. The
expected result is that the HDP-flush packet is consumed first, followed by
the same COPY and ATOMIC sequence validated here.

## 2026-06-26 SESDMAEngine POLL_REGMEM implementation

Implemented the next diagnostic step: add `SDMA_OP_POLL_REGMEM` handling to
`SESDMAEngine` so the default ROCr async-copy path can consume the HDP-flush
packet that previously aborted with opcode `0x8`.

Commands run by Codex:

```bash
find /home/zhangds/.codex \
  -path '*test-driven-development/SKILL.md' \
  -o -path '*systematic-debugging/SKILL.md'
sed -n '1,120p' AGENTS.md
tail -180 docs/debug/se-multigpu-status.md
git status --short
sed -n '1,240p' \
  /home/zhangds/.codex/plugins/cache/openai-curated/superpowers/3fdeeb49/skills/systematic-debugging/SKILL.md
sed -n '1,260p' \
  /home/zhangds/.codex/plugins/cache/openai-curated/superpowers/3fdeeb49/skills/test-driven-development/SKILL.md
sed -n '1,260p' src/dev/hsa/se_sdma_engine.hh
sed -n '1,520p' src/dev/hsa/se_sdma_engine.cc
sed -n '220,285p' src/dev/amdgpu/sdma_packets.hh
sed -n '950,1075p' src/dev/amdgpu/sdma_engine.cc
sed -n '780,1040p' tests/pyunit/stdlib/test_se_viper_multigpu.py
rg -n \
  "SDMA_OP_POLL_REGMEM|SDMA_OP_COPY|SDMA_OP_ATOMIC|SDMA_SUBOP" \
  src/dev/amdgpu/sdma_commands.hh \
  /tmp/ROCR-Runtime-rocm-4.0.0/src/core/inc/sdma_registers.h
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_sdma_engine_has_phase1_packet_handlers
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_hsa_packet_processor_routes_sdma_doorbells \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_sdma_engine_has_phase1_packet_handlers
python3 -m py_compile tests/pyunit/stdlib/test_se_viper_multigpu.py
git diff --check -- \
  src/dev/hsa/se_sdma_engine.cc \
  src/dev/hsa/se_sdma_engine.hh \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
git diff -- \
  src/dev/hsa/se_sdma_engine.cc \
  src/dev/hsa/se_sdma_engine.hh \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
```

First relevant observable result:

```text
FAIL: test_se_sdma_engine_has_phase1_packet_handlers
AssertionError: 'SDMA_OP_POLL_REGMEM' not found in ...
```

This was the expected RED result before the implementation.

Implementation:

- Added `SDMA_OP_POLL_REGMEM` dispatch in `SESDMAEngine::decodeHeader()`.
- Added `executePollRegMem()` and `executePollRegMemData()`.
- Added `pollRegMemFunc()` with comparison functions matching the existing
  gem5 full-system SDMA engine:
  - always true;
  - `<`, `<=`, `==`, `!=`, `>=`, `>`.
- For `mode == 0` register polling, `SESDMAEngine` now consumes the packet and
  continues. This is the expected MVP behavior for ROCr's gfx9 HDP-flush
  packet in SE mode, because there is no modeled SRBM/HDP register state.
- For `mode == 1` memory polling with `op == 0`, `SESDMAEngine` reads the
  target memory dword, applies the packet mask, compares against the masked
  reference value, and retries according to `retryCount`.
- Unsupported `POLL_REGMEM` operations and comparison functions remain fatal
  with explicit error messages.

Facts confirmed:

- `src/dev/amdgpu/sdma_commands.hh` and ROCr 4.0.1 both define
  `SDMA_OP_POLL_REGMEM` as opcode `8`.
- The existing gem5 `SDMAEngine::pollRegMem()` provides the reference
  comparison behavior for memory polling.
- The previous default-ROCr failure was specifically the missing opcode
  dispatch in `SESDMAEngine`, not a queue-routing problem.

Verification completed:

```text
Ran 1 test in 0.000s
OK
```

for:

- `test_se_sdma_engine_has_phase1_packet_handlers`

and:

```text
Ran 2 tests in 0.001s
OK
```

for:

- `test_hsa_packet_processor_routes_sdma_doorbells`
- `test_se_sdma_engine_has_phase1_packet_handlers`

Also completed successfully with no output:

- `python3 -m py_compile tests/pyunit/stdlib/test_se_viper_multigpu.py`
- `git diff --check -- src/dev/hsa/se_sdma_engine.cc src/dev/hsa/se_sdma_engine.hh tests/pyunit/stdlib/test_se_viper_multigpu.py`

Files changed:

- `src/dev/hsa/se_sdma_engine.hh`
- `src/dev/hsa/se_sdma_engine.cc`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document

Verification not completed:

- Codex did not start a gem5 rebuild, per repository guidance that the user
  owns gem5 compilation unless explicitly delegated.
- The default-HDP-flush `hsa_sdma_async_copy` simulation has not yet been
  rerun with the new binary.

Single next diagnostic step: rebuild gem5, then rerun
`hsa_sdma_async_copy` without `HSA_ENABLE_SDMA_HDP_FLUSH=0`. Expected trace:
first `SDMA_OP_POLL_REGMEM` is consumed by `SESDMAEngine`, followed by the
previously validated `SDMA_OP_COPY` and `SDMA_OP_ATOMIC` completion sequence,
ending with `HSA_SDMA_ASYNC_COPY_PASSED`.

## 2026-06-27 hsa_sdma_async_copy default HDP-flush validation

The user rebuilt gem5 and reran `hsa_sdma_async_copy` without disabling
ROCr's SDMA HDP flush. Codex inspected the logs in
`m5out-hsa-sdma-async-copy`. No gem5 build or full simulation was started by
Codex.

User run command recorded in `simout.txt`:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hsa-sdma-async-copy \
  -r -e \
  --stdout-file=simout.txt \
  --stderr-file=simerr.txt \
  --debug-flags=GPUDriver,HSAPacketProcessor,SESDMAEngine \
  --debug-file=sdma.trace \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type atomic \
  --app tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_async_copy \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu \
  --env HSA_ENABLE_SDMA=1
```

Commands run by Codex:

```bash
sed -n '1,220p' \
  /home/zhangds/.codex/plugins/cache/openai-curated/superpowers/3fdeeb49/skills/systematic-debugging/SKILL.md
sed -n '1,120p' AGENTS.md
tail -220 docs/debug/se-multigpu-status.md
git status --short
git diff -- \
  src/dev/hsa/se_sdma_engine.cc \
  src/dev/hsa/se_sdma_engine.hh \
  tests/pyunit/stdlib/test_se_viper_multigpu.py \
  docs/debug/se-multigpu-status.md
find m5out-hsa-sdma-async-copy -maxdepth 1 -type f -printf '%T@ %p %s\n'
tail -220 m5out-hsa-sdma-async-copy/simout.txt
tail -260 m5out-hsa-sdma-async-copy/simerr.txt
rg -n \
  "HSA_ENABLE_SDMA_HDP_FLUSH|HSA_ENABLE_SDMA=|AMDKFD_IOC_CREATE_QUEUE|queue_type|backend sdma|backend compute|registerSDMAQueue|unregisterSDMAQueue|doorbell route|to SESDMAEngine|to HWScheduler|SDMA doorbell|SDMA queue|POLL_REGMEM|header 0x8|header 0x1|header 0x5e00000a|opcode 0x8|opcode 0x1|opcode 0xa|SDMA atomic|SDMA atomic ADD64|drained|unsupported|fatal|panic|HSA_SDMA_ASYNC_COPY|hsa_amd_memory_async_copy|wait completion|Exiting|GPU Blit Kernel Completed|GPU Kernel Completed" \
  m5out-hsa-sdma-async-copy/sdma.trace \
  m5out-hsa-sdma-async-copy/simout.txt \
  m5out-hsa-sdma-async-copy/simerr.txt
sed -n '160,276p' m5out-hsa-sdma-async-copy/sdma.trace
sed -n '304,420p' m5out-hsa-sdma-async-copy/sdma.trace
rg -n \
  "fatal|panic|unsupported|HSA_SDMA_ASYNC_COPY_PASSED|POLL_REGMEM|opcode 0x8|opcode 0x1|opcode 0xa|drained|Exiting" \
  m5out-hsa-sdma-async-copy/sdma.trace \
  m5out-hsa-sdma-async-copy/simout.txt \
  m5out-hsa-sdma-async-copy/simerr.txt
```

First relevant observable result:

```text
command line: ... --env HSA_ENABLE_SDMA=1
...
[hsa_sdma_async_copy] wait H2D completion
[hsa_sdma_async_copy] wait completion: HSA_STATUS_SUCCESS
...
[hsa_sdma_async_copy] wait D2H completion
[hsa_sdma_async_copy] wait completion: HSA_STATUS_SUCCESS
...
HSA_SDMA_ASYNC_COPY_PASSED
Exiting @ tick 14361229395 because exiting with last active thread context.
```

Facts confirmed:

- The run did not pass `HSA_ENABLE_SDMA_HDP_FLUSH=0`, so this validates the
  default ROCr HDP-flush path.
- There was no fatal, panic, unsupported-opcode abort, or max-tick exit.
- ROCr created real SDMA queues for both transfers:
  `queue_type 1`, `backend sdma`.
- Doorbells for both queues routed to `SESDMAEngine`.
- Queue 0 H2D sequence:
  - `SDMA_OP_POLL_REGMEM`: `header 0x8 opcode 0x8`;
  - decoded as `mode 0 func 0 op 0 addr 0x8000000000000000`;
  - consumed as register/HDP-flush poll with no SE register model;
  - followed by `SDMA_OP_COPY`: `header 0x1 opcode 0x1`;
  - followed by completion `SDMA_OP_ATOMIC`: `header 0x5e00000a opcode 0xa`;
  - `SDMA atomic ADD64` decremented the completion signal from `0x1` to `0`;
  - queue drained at rptr `84`.
- Queue 1 D2H sequence matched the same pattern:
  `POLL_REGMEM -> COPY -> ATOMIC`, then drained at rptr `84`.
- Ring dumps show the expected packet order. The extra 24 bytes before COPY
  are the HDP-flush `POLL_REGMEM` packet, explaining why the write pointer
  advanced from 60 bytes in the no-HDP isolation run to 84 bytes here.
- Both SDMA queues were later unregistered.

Hypotheses accepted:

- Accepted: the implemented `SDMA_OP_POLL_REGMEM` handling fixes the default
  ROCr SDMA async-copy path for the observed gfx9 HDP-flush packet.
- Accepted: the current SE SDMA MVP now supports HSA H2D and D2H
  `hsa_amd_memory_async_copy()` with default HDP flush enabled.

Hypotheses rejected:

- Rejected: the HDP-flush packet still blocks progress after the
  `POLL_REGMEM` implementation.
- Rejected: the default-HDP path falls back to compute blit. The trace shows
  `backend sdma` and SESDMAEngine packet execution.

Files changed:

- this status document only.

Verification completed:

- Read-only inspection of
  `m5out-hsa-sdma-async-copy/{simout.txt,simerr.txt,sdma.trace}`.
- Verified stdout success marker `HSA_SDMA_ASYNC_COPY_PASSED`.
- Verified default-HDP trace sequence:
  `SDMA_OP_POLL_REGMEM -> SDMA_OP_COPY -> SDMA_OP_ATOMIC`.

Single next diagnostic step: broaden the SDMA MVP validation to a larger copy
and an XGMI/peer transfer smoke. The immediate low-risk step is to extend or
add an HSA async-copy smoke that copies more than one page and exercises
GPU0-to-GPU1 memory movement, then verify whether ROCr creates
`KFD_IOC_QUEUE_TYPE_SDMA_XGMI` or ordinary SDMA queues for that peer path.

## 2026-06-27 HSA SDMA peer async-copy smoke

Implemented the next diagnostic step as a standalone HSA smoke:
`hsa_sdma_peer_async_copy`. Codex did not start a full gem5 simulation.

Commands run by Codex:

```bash
rg -n \
  "hsa_sdma_async_copy|SESDMAEngine|SDMA MVP|XGMI|peer" \
  /home/zhangds/.codex/memories/MEMORY.md
sed -n '1,220p' \
  /home/zhangds/.codex/plugins/cache/openai-curated/superpowers/3fdeeb49/skills/test-driven-development/SKILL.md
sed -n '1,120p' AGENTS.md
tail -180 docs/debug/se-multigpu-status.md
git status --short
sed -n '1,260p' tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_async_copy.cpp
sed -n '260,520p' tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_async_copy.cpp
sed -n '1,140p' tests/test-progs/gpu/xgmi-peer-vram/Makefile
sed -n '780,1060p' tests/pyunit/stdlib/test_se_viper_multigpu.py
rg -n \
  "hsa_amd_agents_allow_access|hsa_amd_memory_async_copy" \
  .deps/rocm-4.0.1/root/opt/rocm-4.0.1/include/hsa/hsa_ext_amd.h \
  tests/test-progs/gpu -S
sed -n '1188,1215p' \
  .deps/rocm-4.0.1/root/opt/rocm-4.0.1/include/hsa/hsa_ext_amd.h
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_direct_hsa_sdma_peer_async_copy_smoke_uses_two_gpus
make -C tests/test-progs/gpu/xgmi-peer-vram \
  hsa_sdma_peer_async_copy \
  ROCM_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  CXX=g++
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_direct_hsa_sdma_async_copy_smoke_uses_hsa_amd_memory_async_copy \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_direct_hsa_sdma_peer_async_copy_smoke_uses_two_gpus
python3 -m py_compile tests/pyunit/stdlib/test_se_viper_multigpu.py
git diff --check -- \
  tests/test-progs/gpu/xgmi-peer-vram/Makefile \
  tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_peer_async_copy.cpp \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
```

First relevant observable result:

```text
FileNotFoundError:
  tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_peer_async_copy.cpp
```

This was the expected RED result before adding the new smoke.

Implemented changes:

- Added `tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_peer_async_copy.cpp`.
- Added `HSA_SDMA_PEER_ASYNC_COPY_TARGET` and
  `HSA_SDMA_PEER_ASYNC_COPY_SRC` to the xgmi-peer-vram Makefile.
- Added the new target to Makefile `all` and `clean`.
- Added a pyunit source/Makefile structure test for the peer async-copy
  smoke.

Smoke behavior:

- Initializes HSA.
- Finds one CPU agent and at least two GPU agents.
- Finds allocable global pools for GPU0 and GPU1.
- Allocates a 64 KiB buffer in GPU0 VRAM and a 64 KiB buffer in GPU1 VRAM.
- Calls `hsa_amd_agents_allow_access()` for CPU/GPU0/GPU1 on both VRAM
  buffers.
- Copies a deterministic host pattern to GPU0 with
  `hsa_amd_memory_async_copy()`.
- Copies GPU0 VRAM to GPU1 VRAM with `hsa_amd_memory_async_copy()`.
- Copies GPU1 VRAM back to host with `hsa_amd_memory_async_copy()`.
- Verifies the returned host buffer byte-for-byte.
- Prints `HSA_SDMA_PEER_ASYNC_COPY_PASSED` on success.

Facts confirmed:

- The new smoke is independent of HIP and does not call `hipMemcpy` or
  `hipMemset`.
- The copy size is 64 KiB, so it is larger than one 4 KiB page.
- The program builds successfully with ROCm 4.0.1 headers and libraries:

```text
g++ -O2 -I.../include -L.../lib -o hsa_sdma_peer_async_copy \
  hsa_sdma_peer_async_copy.cpp -lhsa-runtime64 -lhsakmt
```

Verification completed:

```text
Ran 1 test in 0.000s
OK
```

for:

- `test_direct_hsa_sdma_peer_async_copy_smoke_uses_two_gpus`

and:

```text
Ran 2 tests in 0.000s
OK
```

for:

- `test_direct_hsa_sdma_async_copy_smoke_uses_hsa_amd_memory_async_copy`
- `test_direct_hsa_sdma_peer_async_copy_smoke_uses_two_gpus`

Also completed successfully with no output:

- `python3 -m py_compile tests/pyunit/stdlib/test_se_viper_multigpu.py`
- `git diff --check -- tests/test-progs/gpu/xgmi-peer-vram/Makefile tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_peer_async_copy.cpp tests/pyunit/stdlib/test_se_viper_multigpu.py`

Files changed:

- `tests/test-progs/gpu/xgmi-peer-vram/Makefile`
- `tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_peer_async_copy.cpp`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document

Generated local build artifact:

- `tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_peer_async_copy`

Single next diagnostic step: run the new smoke under SE with SDMA debug flags:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hsa-sdma-peer-async-copy \
  -r -e \
  --stdout-file=simout.txt \
  --stderr-file=simerr.txt \
  --debug-flags=GPUDriver,HSAPacketProcessor,SESDMAEngine \
  --debug-file=sdma.trace \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type atomic \
  --app tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_peer_async_copy \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu \
  --env HSA_ENABLE_SDMA=1
```

Expected pass marker:

```text
HSA_SDMA_PEER_ASYNC_COPY_PASSED
```

Trace questions to answer:

- Does the GPU0-to-GPU1 copy create `KFD_IOC_QUEUE_TYPE_SDMA_XGMI`
  (`queue_type 3`) or ordinary `KFD_IOC_QUEUE_TYPE_SDMA` (`queue_type 1`)?
- Does the queue route to `SESDMAEngine` and execute
  `POLL_REGMEM -> COPY -> ATOMIC` as in the H2D/D2H default-HDP case?

## 2026-06-27 hsa_sdma_peer_async_copy first SE run

The user ran the new peer async-copy smoke under SE. Codex inspected logs in
`m5out-hsa-sdma-peer-async-copy`. No gem5 build or full simulation was
started by Codex.

User run command recorded in `simout.txt`:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hsa-sdma-peer-async-copy \
  -r -e \
  --stdout-file=simout.txt \
  --stderr-file=simerr.txt \
  --debug-flags=GPUDriver,HSAPacketProcessor,SESDMAEngine \
  --debug-file=sdma.trace \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type atomic \
  --app tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_peer_async_copy \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu \
  --env HSA_ENABLE_SDMA=1
```

Commands run by Codex:

```bash
rg -n \
  "hsa_sdma_peer_async_copy|SDMA peer|SDMA_XGMI|SESDMAEngine" \
  /home/zhangds/.codex/memories/MEMORY.md
sed -n '1,220p' \
  /home/zhangds/.codex/plugins/cache/openai-curated/superpowers/3fdeeb49/skills/systematic-debugging/SKILL.md
sed -n '1,120p' AGENTS.md
tail -220 docs/debug/se-multigpu-status.md
git status --short
git diff -- \
  docs/debug/se-multigpu-status.md \
  tests/test-progs/gpu/xgmi-peer-vram/Makefile \
  tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_peer_async_copy.cpp \
  tests/pyunit/stdlib/test_se_viper_multigpu.py \
  src/dev/hsa/se_sdma_engine.cc \
  src/dev/hsa/se_sdma_engine.hh \
  src/gpu-compute/gpu_compute_driver.cc \
  src/dev/hsa/hsa_packet_processor.cc
find m5out-hsa-sdma-peer-async-copy -maxdepth 1 -type f -printf '%T@ %p %s\n'
tail -260 m5out-hsa-sdma-peer-async-copy/simout.txt
tail -320 m5out-hsa-sdma-peer-async-copy/simerr.txt
rg -n \
  "HSA_SDMA_PEER_ASYNC_COPY|begin .*async copy|wait .*completion|hsa_amd_memory_async_copy|AMDKFD_IOC_CREATE_QUEUE|queue_type|backend sdma|backend compute|SDMA_XGMI|queue_id|registerSDMAQueue|unregisterSDMAQueue|doorbell route|to SESDMAEngine|to HWScheduler|SDMA doorbell|SDMA queue|POLL_REGMEM|opcode 0x8|opcode 0x1|opcode 0xa|SDMA atomic|ADD64|drained|unsupported|fatal|panic|mismatch|Exiting|GPU Blit Kernel Completed|GPU Kernel Completed" \
  m5out-hsa-sdma-peer-async-copy/sdma.trace \
  m5out-hsa-sdma-peer-async-copy/simout.txt \
  m5out-hsa-sdma-peer-async-copy/simerr.txt
sed -n '130,330p' m5out-hsa-sdma-peer-async-copy/sdma.trace
sed -n '240,280p' src/gpu-compute/gpu_compute_driver.cc
rg -n \
  "queue_type 3|KFD_IOC_QUEUE_TYPE_SDMA_XGMI|SE SDMA_XGMI|fatal|panic|HSA_SDMA_PEER_ASYNC_COPY_PASSED|GPU0_TO_GPU1|D2H_GPU1|Exiting|Program aborted" \
  m5out-hsa-sdma-peer-async-copy/sdma.trace \
  m5out-hsa-sdma-peer-async-copy/simout.txt \
  m5out-hsa-sdma-peer-async-copy/simerr.txt \
  src/gpu-compute/gpu_compute_driver.cc \
  src/dev/hsa/hsa_packet_processor.cc \
  src/dev/hsa/hsa_packet_processor.hh
tail -80 m5out-hsa-sdma-peer-async-copy/simerr.txt
```

First relevant observable result:

```text
[hsa_sdma_peer_async_copy] begin H2D_GPU0 async copy bytes=65536
[hsa_sdma_peer_async_copy] hsa_amd_memory_async_copy: HSA_STATUS_SUCCESS
[hsa_sdma_peer_async_copy] wait H2D_GPU0 completion
[hsa_sdma_peer_async_copy] wait completion: HSA_STATUS_SUCCESS
...
[hsa_sdma_peer_async_copy] begin GPU0_TO_GPU1 async copy bytes=65536
...
AMDKFD_IOC_CREATE_QUEUE request gpu_id 22124 queue_type 3 ...
src/gpu-compute/gpu_compute_driver.cc:263: fatal:
  SE SDMA_XGMI queue is Phase 2 and is not implemented yet
```

Facts confirmed:

- The smoke found one CPU agent and two GPU agents.
- It allocated GPU0 and GPU1 VRAM buffers and successfully enabled access for
  CPU/GPU0/GPU1 on both buffers.
- The 64 KiB H2D copy into GPU0 completed successfully through ordinary SDMA:
  `queue_type 1`, `backend sdma`, queue registered with `SESDMAEngine`,
  doorbell routed to `SESDMAEngine`, and packet sequence
  `POLL_REGMEM -> COPY -> ATOMIC`.
- For H2D, the COPY count dword was `0x0000ffff`, confirming a 64 KiB
  transfer.
- The GPU0-to-GPU1 peer copy did not fall back to compute blit or ordinary
  SDMA. ROCr requested KFD queue type `3`, i.e.
  `KFD_IOC_QUEUE_TYPE_SDMA_XGMI`.
- gem5 currently reaches the explicit Phase 2 fatal in
  `GPUComputeDriver::createQueue()` for `KFD_IOC_QUEUE_TYPE_SDMA_XGMI`.
- The smoke did not reach the D2H_GPU1 copy or the
  `HSA_SDMA_PEER_ASYNC_COPY_PASSED` marker because the simulation aborted
  while creating the XGMI SDMA queue.
- `stats.txt` is zero bytes for this run because the fatal abort occurred
  before normal stats emission.

Hypotheses accepted:

- Accepted: ROCr 4.0.1 uses `KFD_IOC_QUEUE_TYPE_SDMA_XGMI` for the
  GPU0-to-GPU1 peer async-copy path in this SE two-GPU platform.
- Accepted: the next SDMA MVP gap is XGMI SDMA queue registration/routing,
  not another missing regular SDMA packet opcode.

Hypotheses rejected:

- Rejected: GPU0-to-GPU1 peer async copy uses ordinary `queue_type 1` SDMA in
  this setup.
- Rejected: the failure is in the H2D baseline path. H2D completed normally.
- Rejected: the peer path fell back to compute blit. It requested queue type
  `3` before any compute queue path.

Files changed:

- this status document only.

Verification completed:

- Read-only inspection of
  `m5out-hsa-sdma-peer-async-copy/{simout.txt,simerr.txt,sdma.trace}`.
- Verified the H2D 64 KiB SDMA packet sequence.
- Verified the peer copy requests `queue_type 3`.

Single next diagnostic step: implement Phase 2 `KFD_IOC_QUEUE_TYPE_SDMA_XGMI`
support by initially registering XGMI queues with `SESDMAEngine` using the
same doorbell/ring mechanics as ordinary SDMA, while preserving the queue
type in debug output. Then rerun `hsa_sdma_peer_async_copy` to see whether
the XGMI queue emits the same `POLL_REGMEM -> COPY -> ATOMIC` packet stream
or exposes XGMI-specific packet semantics.

## 2026-06-27 minimal SDMA_XGMI queue routing

Codex implemented the first Phase 2 step for the direct peer async-copy path.
No gem5 build or full simulation was started.

Commands run by Codex:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kfd_queue_type_constants_match_ioctl_uapi
```

The command was first run after changing only the regression expectation. It
failed as expected because the current source still contained:

```text
SE SDMA_XGMI queue is Phase 2 and is not implemented yet
```

Codex then implemented the minimal routing change and reran:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kfd_queue_type_constants_match_ioctl_uapi
```

Result:

```text
Ran 1 test in 0.001s
OK
```

Additional lightweight verification:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kfd_queue_type_constants_match_ioctl_uapi \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_hsa_packet_processor_routes_sdma_doorbells \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_direct_hsa_sdma_peer_async_copy_smoke_uses_two_gpus
python3 -m py_compile tests/pyunit/stdlib/test_se_viper_multigpu.py
git diff --check -- \
  src/gpu-compute/gpu_compute_driver.cc \
  tests/pyunit/stdlib/test_se_viper_multigpu.py
```

Result:

```text
Ran 3 tests in 0.001s
OK
```

`py_compile` and `git diff --check` completed with no output.

First relevant observable result:

- the regression now requires `KFD_IOC_QUEUE_TYPE_SDMA_XGMI` to use
  `registerSDMAQueue`;
- the old Phase 2 fatal string is no longer allowed by the test;
- `backend sdma_xgmi` is preserved in debug output so XGMI queues remain
  distinguishable from ordinary SDMA queues.

Facts confirmed:

- `KFD_IOC_QUEUE_TYPE_SDMA_XGMI` is now accepted by
  `GPUComputeDriver::allocateQueue()`.
- The XGMI SDMA queue is registered with the same `SESDMAEngine` doorbell,
  ring, read-pointer, and write-pointer mechanics used by ordinary SDMA.
- Queue teardown already unregisters non-compute queues through
  `unregisterSDMAQueue`, so this minimal implementation does not need a new
  teardown path.

Hypotheses accepted:

- Accepted: the immediate peer-copy fatal was caused by missing queue-type
  routing, and can be removed by treating XGMI SDMA as an `SESDMAEngine`
  queue for the MVP.

Hypotheses not yet proven:

- Not yet proven: the XGMI queue uses only packet encodings already supported
  by `SESDMAEngine`.
- Not yet proven: GPU0-to-GPU1 data lands correctly in GPU1 VRAM. That
  requires rerunning the smoke.

Files changed:

- `src/gpu-compute/gpu_compute_driver.cc`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- this status document.

Verification completed:

- red/green structural regression for XGMI queue routing;
- related SDMA doorbell and peer-smoke source tests;
- Python syntax check for the modified test file;
- whitespace check on modified source/test files.

Single next diagnostic step: rebuild gem5 and rerun
`m5out-hsa-sdma-peer-async-copy` with the same command as the previous peer
run. Expected next evidence is either:

- the run reaches `HSA_SDMA_PEER_ASYNC_COPY_PASSED`; or
- the XGMI queue reaches `SESDMAEngine` and exposes a new unsupported packet
  opcode/sub-opcode in `sdma.trace`.

## 2026-06-27 hsa_sdma_peer_async_copy after SDMA_XGMI routing

The user rebuilt gem5 and reran the peer async-copy smoke. Codex inspected
logs in `m5out-hsa-sdma-peer-async-copy`. No gem5 build or full simulation
was started by Codex.

User run command recorded in `simout.txt`:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hsa-sdma-peer-async-copy \
  -r -e \
  --stdout-file=simout.txt \
  --stderr-file=simerr.txt \
  --debug-flags=GPUDriver,HSAPacketProcessor,SESDMAEngine \
  --debug-file=sdma.trace \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type atomic \
  --app tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_peer_async_copy \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu \
  --env HSA_ENABLE_SDMA=1
```

Commands run by Codex:

```bash
find m5out-hsa-sdma-peer-async-copy -maxdepth 1 -type f -printf '%T@ %p %s\n'
tail -260 m5out-hsa-sdma-peer-async-copy/simout.txt
tail -220 m5out-hsa-sdma-peer-async-copy/simerr.txt
rg -n \
  "HSA_SDMA_PEER_ASYNC_COPY|begin .*async copy|wait .*completion|AMDKFD_IOC_CREATE_QUEUE|queue_type|backend sdma|backend sdma_xgmi|registerSDMAQueue|unregisterSDMAQueue|doorbell route|to SESDMAEngine|to HWScheduler|SDMA doorbell|SDMA queue|POLL_REGMEM|opcode 0x8|opcode 0x1|opcode 0xa|SDMA atomic|ADD64|drained|unsupported|fatal|panic|mismatch|Exiting" \
  m5out-hsa-sdma-peer-async-copy/sdma.trace \
  m5out-hsa-sdma-peer-async-copy/simout.txt \
  m5out-hsa-sdma-peer-async-copy/simerr.txt
rg -n \
  "backend sdma_xgmi|queue_type 3|queue_id 1|queue 1|GPU0_TO_GPU1|unsupported|fatal|panic" \
  m5out-hsa-sdma-peer-async-copy/sdma.trace \
  m5out-hsa-sdma-peer-async-copy/simout.txt \
  m5out-hsa-sdma-peer-async-copy/simerr.txt
rg -n \
  "fatal|panic|unsupported opcode|unsupported .*sub-opcode|HSA_SDMA_PEER_ASYNC_COPY_PASSED|backend sdma_xgmi|SDMA queue 1 drained|AMDKFD_IOC_CREATE_QUEUE request gpu_id 22124 queue_type 3" \
  m5out-hsa-sdma-peer-async-copy/sdma.trace \
  m5out-hsa-sdma-peer-async-copy/simout.txt \
  m5out-hsa-sdma-peer-async-copy/simerr.txt
```

First relevant observable result:

```text
[hsa_sdma_peer_async_copy] begin H2D_GPU0 async copy bytes=65536
[hsa_sdma_peer_async_copy] wait completion: HSA_STATUS_SUCCESS
[hsa_sdma_peer_async_copy] begin GPU0_TO_GPU1 async copy bytes=65536
[hsa_sdma_peer_async_copy] wait completion: HSA_STATUS_SUCCESS
[hsa_sdma_peer_async_copy] begin D2H_GPU1 async copy bytes=65536
[hsa_sdma_peer_async_copy] wait completion: HSA_STATUS_SUCCESS
HSA_SDMA_PEER_ASYNC_COPY_PASSED
Exiting @ tick 15265791261 because exiting with last active thread context.
```

Facts confirmed:

- The full H2D_GPU0, GPU0_TO_GPU1, and D2H_GPU1 sequence completed.
- The smoke reached `HSA_SDMA_PEER_ASYNC_COPY_PASSED`.
- `stats.txt` was generated, so the run exited normally rather than aborting.
- The GPU0-to-GPU1 peer copy created `KFD_IOC_QUEUE_TYPE_SDMA_XGMI`
  (`queue_type 3`) and gem5 assigned it as `backend sdma_xgmi`.
- The XGMI queue was registered with `board.gpus0.se_sdma_engine` as queue
  id 1.
- Doorbell offset `0x8` for queue id 1 routed to `SESDMAEngine`, not
  `HWScheduler`.
- The XGMI SDMA packet stream used the same supported sequence as ordinary
  SDMA in this ROCr 4.0.1 run:
  `POLL_REGMEM -> COPY -> ATOMIC`.
- The peer COPY packet copied from `0x7ffdf6a00000` to `0x7ffdf6600000`.
  From `simerr.txt`, those are the GPU0 and GPU1 VRAM allocations,
  respectively.
- The COPY count dword was `0x0000ffff`, matching the 64 KiB copy size.
- The completion signal was decremented by `SDMA atomic ADD64` from `1` to
  `0`, and queue 1 drained at read pointer 84.
- No `fatal`, `panic`, `unsupported opcode`, or unsupported sub-opcode was
  found in the inspected trace output.

Hypotheses accepted:

- Accepted: the minimal SDMA_XGMI routing fix is sufficient for the current
  ROCr 4.0.1 peer async-copy smoke.
- Accepted: for this 64 KiB GPU0-to-GPU1 HSA async-copy path, ROCr emits an
  XGMI queue with ordinary supported SDMA packet semantics.

Hypotheses rejected:

- Rejected: XGMI peer async-copy immediately requires a new SDMA packet
  opcode/sub-opcode beyond the current `SESDMAEngine` MVP.
- Rejected: the peer path still falls back to compute or aborts during queue
  creation after the routing fix.

Files changed:

- this status document only.

Verification completed:

- Read-only inspection of
  `m5out-hsa-sdma-peer-async-copy/{simout.txt,simerr.txt,sdma.trace}`.
- Verified the pass marker and normal gem5 exit.
- Verified queue type 3 maps to `backend sdma_xgmi`.
- Verified XGMI doorbell routing to `SESDMAEngine`.
- Verified the peer packet stream and source/destination allocation addresses.

Single next diagnostic step: add a small automated trace/log regression or a
source-level structural regression for SDMA_XGMI routing if this result will
be committed soon. Then run the broader SDMA smoke set after rebuild:

```bash
hsa_sdma_async_copy
hsa_sdma_peer_async_copy
```

The broader rerun should confirm that ordinary H2D/D2H SDMA and XGMI peer
SDMA continue to work together after the queue-routing change.

## 2026-06-27 broader SDMA smoke rerun

The user explicitly delegated the broader SDMA smoke rerun. Codex ran both
full simulations with fresh output directories and did not overwrite the
previous `m5out-hsa-sdma-peer-async-copy` evidence.

Commands run by Codex:

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hsa-sdma-async-copy-broader \
  -r -e \
  --stdout-file=simout.txt \
  --stderr-file=simerr.txt \
  --debug-flags=GPUDriver,HSAPacketProcessor,SESDMAEngine \
  --debug-file=sdma.trace \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type atomic \
  --app tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_async_copy \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu \
  --env HSA_ENABLE_SDMA=1
```

```bash
build/VEGA_X86/gem5.opt \
  -d m5out-hsa-sdma-peer-async-copy-broader \
  -r -e \
  --stdout-file=simout.txt \
  --stderr-file=simerr.txt \
  --debug-flags=GPUDriver,HSAPacketProcessor,SESDMAEngine \
  --debug-file=sdma.trace \
  --listener-mode=off \
  configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
  --cpu-type atomic \
  --app tests/test-progs/gpu/xgmi-peer-vram/hsa_sdma_peer_async_copy \
  --rocm-path /home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1 \
  --env LD_LIBRARY_PATH=/home/zhangds/gem5/.deps/rocm-4.0.1/root/opt/rocm-4.0.1/lib:/usr/lib/x86_64-linux-gnu \
  --env HSA_ENABLE_SDMA=1
```

Post-run inspection commands:

```bash
tail -220 m5out-hsa-sdma-async-copy-broader/simout.txt
tail -220 m5out-hsa-sdma-peer-async-copy-broader/simout.txt
rg -n \
  "HSA_SDMA_ASYNC_COPY|HSA_SDMA_PEER_ASYNC_COPY|AMDKFD_IOC_CREATE_QUEUE|queue_type|backend sdma|backend sdma_xgmi|registerSDMAQueue|doorbell route|to SESDMAEngine|SDMA queue|POLL_REGMEM|SDMA atomic|ADD64|drained|unsupported|fatal|panic|mismatch|Exiting" \
  m5out-hsa-sdma-async-copy-broader/sdma.trace \
  m5out-hsa-sdma-async-copy-broader/simout.txt \
  m5out-hsa-sdma-async-copy-broader/simerr.txt
rg -n \
  "HSA_SDMA_ASYNC_COPY|HSA_SDMA_PEER_ASYNC_COPY|AMDKFD_IOC_CREATE_QUEUE|queue_type|backend sdma|backend sdma_xgmi|registerSDMAQueue|doorbell route|to SESDMAEngine|SDMA queue|POLL_REGMEM|SDMA atomic|ADD64|drained|unsupported|fatal|panic|mismatch|Exiting" \
  m5out-hsa-sdma-peer-async-copy-broader/sdma.trace \
  m5out-hsa-sdma-peer-async-copy-broader/simout.txt \
  m5out-hsa-sdma-peer-async-copy-broader/simerr.txt
rg -n \
  "fatal|panic|unsupported opcode|unsupported .*sub-opcode|mismatch|HSA_SDMA_ASYNC_COPY_PASSED|HSA_SDMA_PEER_ASYNC_COPY_PASSED|backend sdma_xgmi|queue_type 3" \
  m5out-hsa-sdma-async-copy-broader/simout.txt \
  m5out-hsa-sdma-async-copy-broader/simerr.txt \
  m5out-hsa-sdma-async-copy-broader/sdma.trace \
  m5out-hsa-sdma-peer-async-copy-broader/simout.txt \
  m5out-hsa-sdma-peer-async-copy-broader/simerr.txt \
  m5out-hsa-sdma-peer-async-copy-broader/sdma.trace
```

First relevant observable result:

```text
HSA_SDMA_ASYNC_COPY_PASSED
Exiting @ tick 14361229395 because exiting with last active thread context.
```

```text
HSA_SDMA_PEER_ASYNC_COPY_PASSED
Exiting @ tick 15267257793 because exiting with last active thread context.
```

Facts confirmed:

- Ordinary H2D/D2H HSA async-copy passed in
  `m5out-hsa-sdma-async-copy-broader`.
- Peer H2D_GPU0, GPU0_TO_GPU1, and D2H_GPU1 HSA async-copy passed in
  `m5out-hsa-sdma-peer-async-copy-broader`.
- The ordinary async-copy run created `queue_type 1` SDMA queues, registered
  them with `SESDMAEngine`, routed doorbells to `SESDMAEngine`, and executed
  the supported `POLL_REGMEM -> COPY -> ATOMIC` sequence.
- The peer run created the expected XGMI peer queue:
  `queue_type 3`, `backend sdma_xgmi`, queue id 1.
- The XGMI peer queue doorbell routed to `SESDMAEngine` and executed the same
  supported `POLL_REGMEM -> COPY -> ATOMIC` sequence.
- The explicit negative search found no `fatal`, `panic`, unsupported opcode,
  unsupported sub-opcode, or data `mismatch` in either broader run.

Hypotheses accepted:

- Accepted: the current `SESDMAEngine` MVP supports both ordinary H2D/D2H
  HSA async-copy and the ROCr 4.0.1 XGMI peer async-copy smoke.
- Accepted: the minimal `KFD_IOC_QUEUE_TYPE_SDMA_XGMI` routing change does
  not regress ordinary `KFD_IOC_QUEUE_TYPE_SDMA` async-copy in this smoke set.

Hypotheses rejected:

- Rejected: enabling SDMA_XGMI routing breaks ordinary H2D/D2H SDMA.
- Rejected: the tested XGMI peer path needs additional packet decoding before
  this MVP can pass.

Files changed:

- this status document only.

Generated log directories:

- `m5out-hsa-sdma-async-copy-broader`
- `m5out-hsa-sdma-peer-async-copy-broader`

Verification completed:

- Full SE atomic run for `hsa_sdma_async_copy`.
- Full SE atomic run for `hsa_sdma_peer_async_copy`.
- Trace inspection for queue type, backend routing, doorbell target, packet
  sequence, and negative error terms.

Single next diagnostic step: if preparing to commit, run the structural
pyunit checks again plus `git diff --check`, then review the changed file set
and decide whether to include the generated smoke binaries/log directories in
the commit scope or leave them untracked.
