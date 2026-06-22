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
