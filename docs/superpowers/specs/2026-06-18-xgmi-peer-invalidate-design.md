# XGMI Peer Invalidation Diagnostic Design

## Objective

Add an independent SE-mode multi-GPU diagnostic named
`xgmi-peer-invalidate`. It tests whether a GPU0 cache line sourced from GPU1
VRAM is invalidated after GPU1 writes a new value.

This is a diagnostic baseline. GPU-to-GPU cache coherence is not assumed to be
implemented, so observing stale data is a valid and useful experiment result.

## Isolation from the Existing Baseline

The established `xgmi-peer-vram` test remains unchanged. The new diagnostic
will live in:

```text
tests/test-progs/gpu/xgmi-peer-invalidate/
```

It may reuse the established HSA dispatch, HSACO build, CPU-mapped result read,
and m5 pseudo-operation patterns, but it must not add modes or branches to the passing
peer-VRAM baseline.

## Memory Placement

- The target cache line is allocated in GPU1 VRAM.
- GPU0 receives peer access to the GPU1 allocation.
- Observation and result buffers are also allocated in GPU1-visible memory and
  read directly by the CPU after the final kernel.
- The experiment uses one target word or one explicitly aligned cache line so
  the coherence event is unambiguous.

## Host-Sequenced Phases

The host serializes four HSA kernel dispatches. Each dispatch waits for its HSA
completion signal before the next phase starts:

1. GPU1 writes value A to the target.
2. GPU0 reads the target and records the observed value.
3. GPU1 writes value B to the same target.
4. GPU0 reads the target again, records the observed value, and writes a
   classification/result code.

System-scope acquire and release fences remain enabled in each AQL dispatch.
No GPU-to-GPU signaling mechanism is part of this experiment.

The SE GPU model normally performs an implicit whole-GPU cache invalidation at
every kernel launch when `impl_kern_launch_acq` is enabled. That behavior would
make the second GPU0 read inconclusive. The multi-GPU configuration therefore
provides an opt-in diagnostic flag that disables kernel-launch acquire only on
GPU0. Default platform behavior remains unchanged, and this diagnostic must be
run with that flag.

## Kernel Roles

The HSACO contains minimal kernels:

- `write_value`: write a supplied value to the target word.
- `read_value`: read the target word and store the observed value.
- `classify_value`: read the target word, store the observed value, and classify
  it as B, stale A, or an unexpected value.

The first GPU0 observation must equal A. If it does not, the setup or peer-read
path failed and the invalidation result is not meaningful.

## Result Classification

The CPU directly reads the mapped observation and classification buffers and
prints all relevant values.

Results are classified as:

- `invalidation observed`: first read is A and second read is B;
- `stale cache line observed`: first read is A and second read remains A;
- `unexpected value observed`: either observation is neither its expected value
  nor the defined stale value;
- `setup read failed`: GPU0's first read did not observe A.

The stale result is an expected diagnostic outcome, not a simulator crash.
However, the test result code must still distinguish it from coherence success.

## Exit Semantics

After printing and flushing the classification:

- coherence success calls `m5_exit(0)`;
- stale data calls `m5_fail(0, 1)`;
- setup or unexpected-data failures call `m5_fail(0, 2)`.

The first argument to `m5_exit`/`m5_fail` is the delay. Failure codes are
carried only by `m5_fail`.

The SE configuration treats both `m5_exit instruction encountered` and
`m5_fail instruction encountered` as terminal simulation causes and propagates
the event code through its existing `sys.exit(exit_event.getCode())`.

The success and diagnostic-failure paths skip HSA queues, executable objects,
HIP allocations, and other ROCm teardown. This preserves the established
workaround for unsupported SE Timing CPU teardown and avoids the later
multi-threaded `exit_group` assertion.

If an m5 pseudo instruction returns unexpectedly, the host program reports an error and
returns nonzero.

## Build and Invocation

The directory will contain its own:

- `Makefile`;
- host program;
- HIP kernel source;
- generated HSACO and binary targets.

The Makefile follows the existing ROCm 4 gfx900 and x86 `m5op` archive pattern.
The existing SE multi-GPU configuration gains:

```text
--disable-gpu0-kernel-launch-acquire
```

When selected, it sets `gpus[0].impl_kern_launch_acq = False`. This is required
for the invalidation experiment so GPU0's first cached line can survive into
the second GPU0 kernel. The option is off by default.

## Regression Coverage

Lightweight Python regression checks will verify:

- the established `xgmi-peer-vram` source is unchanged by this experiment;
- the new program dispatches the required GPU1/GPU0/GPU1/GPU0 sequence;
- the result distinguishes invalidation, stale, setup failure, and unexpected
  values;
- all terminal result paths flush output and invoke `m5_exit` or `m5_fail`
  before ROCm
  teardown;
- the Makefile links the x86 m5 pseudo-operation implementation.
- the SE configuration exposes the diagnostic flag and disables launch acquire
  only for GPU0 when requested.

## Runtime Verification

Codex may implement and run lightweight tests. The user owns:

- rebuilding the diagnostic binary with the configured ROCm 4 toolchain;
- any required gem5 rebuild;
- the full SE Timing CPU simulation.

The first runtime experiment should use bounded Ruby debug flags focused on the
target request path and write logs to files. The status document records only
the command, first relevant result, confirmed facts, accepted or rejected
hypotheses, log paths, changed files, verification, and the single next
diagnostic step.

## Success Criteria

The diagnostic is complete when it:

1. runs the four host-sequenced phases;
2. proves GPU0 initially observes A;
3. reports whether GPU0 later observes B, stale A, or another value;
4. terminates through `m5_exit` or `m5_fail` without entering ROCm teardown;
5. preserves the existing peer-VRAM baseline.
