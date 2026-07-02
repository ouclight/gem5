# HIP/ROCm API Compatibility Design

## Goal

Improve SE-mode multi-GPU usability for ROCm/HIP applications by making the
common HIP API path testable and incrementally compatible:

- `hipMalloc`
- `hipMemset`
- `hipMemcpy`
- `hipLaunchKernel`
- `hipFree`

The current resident-flag coherence smoke is treated as the functional
multi-GPU visibility baseline. HIP/ROCm API work should not re-prove
coherence unless a new API path creates new evidence.

## Current baseline

The following lower-level paths are already useful as known-good references:

- HSA agent enumeration works for two GPU agents.
- HSA SDMA async-copy/fill paths have been implemented and validated for the
  SE SDMA MVP.
- The resident-reader shader-store smoke passes in forward and reverse
  directions for split-line no-fence cases.
- `hip_api_smoke` already provides cumulative public HIP stages:
  `malloc`, `memset`, `memcpy`, `launch`, and `lifecycle`.
- `se_hip_compat/libse_hip_compat.so` already interposes `hipMemset` and
  dispatches a simple HSA kernel from a prebuilt HSACO.
- `hip_memset_preload_smoke` verifies the preload `hipMemset` path without
  using `hipMemcpy`.

## Problem statement

The native ROCm path is not uniformly viable under gem5 SE:

- `hipMalloc` has completed in prior KVM/SE tests.
- Native `hipMemset` can enter a long ROCr CPU/runtime path or compute blit
  path that is unsuitable for fast functional SE testing.
- Native `hipMemcpy` may use runtime paths that depend on features not fully
  modeled in SE.
- `hipLaunchKernel` is expected to work for simple kernels, but validation
  often depends on `hipMemcpy(DeviceToHost)`.
- `hipFree` and process teardown can enter unsupported ROCm cleanup paths.

The API compatibility work should isolate these cases and fix them one at a
time instead of using one cumulative test as the only signal.

## ROCm source-analysis policy

ROCm source analysis is part of the root-cause workflow for any API that hangs,
hits a gem5 panic, exceeds the tick budget, or enters an unexpected path.

The source-analysis goal is to answer four concrete questions before choosing
an implementation fix:

1. Which public HIP API entry point was called?
2. Which ROCm internal path was selected?
3. What object is the runtime waiting for: SDMA completion, HSA signal, queue
   drain, blit-kernel completion, COMGR compilation, KFD ioctl, futex, or
   process teardown?
4. Which layer should be changed: gem5 syscall/ioctl support, HSA queue/signal
   support, `SESDMAEngine`, GPU VMA/mtype handling, the test program, or the
   `se_hip_compat` preload layer?

This analysis is required before adding new preload behavior for a blocked
native API. The only exception is when an existing preload path is already the
accepted baseline, such as the current `hipMemset` compatibility shim.

ROCm source should be treated as read-only reference material. The expected
fix location is usually gem5 SE support or `se_hip_compat`, not a local fork of
ROCm.

The user approved automatically downloading ROCm source when needed. Store
source checkouts under `.deps/rocm-src-4.0.1/`, keep them read-only for
analysis, and do not commit the downloaded source tree.

## Design decision

Use a two-layer strategy.

Layer 1: Public HIP API smoke tests

- Keep `hip_api_smoke` as the public cumulative API test.
- Add small standalone smoke binaries when an API needs isolation from
  unrelated runtime behavior.
- Each smoke must print an explicit pass marker and use `m5_exit(0)` only when
  the test result is already known and ROCm teardown is not the target.

Layer 2: SE HIP compatibility preload

- Continue using `LD_PRELOAD=libse_hip_compat.so` for APIs where the native
  ROCm implementation is functionally reasonable but too slow or unsupported
  in SE.
- Keep the preload layer narrow. It should interpose only APIs needed by the
  current compatibility stage.
- For unsupported or ambiguous cases, call the real ROCm function through
  `RTLD_NEXT` rather than silently changing behavior.

## API support policy

### `hipMalloc`

Initial policy: use native ROCm.

Acceptance:

- A standalone malloc smoke completes with non-null device pointer.
- It prints `HIP_API_MALLOC_PASSED`.
- It exits through `m5_exit(0)` after result observation.

### `hipMemset`

Initial policy: use the existing `se_hip_compat` preload path.

Acceptance:

- `hip_memset_preload_smoke` sets device memory using `hipMemset`.
- The test validates contents via direct CPU read of the mapped allocation.
- It prints `HIP_MEMSET_PRELOAD_PASSED`.

### `hipMemcpy`

Initial policy: implement and validate a preload path after `hipMemset`.

The preload should support:

- `hipMemcpyHostToDevice`
- `hipMemcpyDeviceToHost`
- `hipMemcpyDeviceToDevice`

Implementation direction:

- Use HSA async copy when possible.
- Use CPU `memcpy` only for `hipMemcpyHostToHost`.
- Wait for the HSA signal before returning from synchronous `hipMemcpy`.
- Return `hipErrorInvalidValue` or `hipErrorInvalidDevicePointer` for invalid
  arguments that the compatibility layer can detect locally.
- Fall back to the native ROCm `hipMemcpy` for `hipMemcpyDefault` until pointer
  attribute support is deliberately implemented.

Acceptance:

- A preload memcpy smoke performs H2D and D2H round trip and prints
  `HIP_MEMCPY_PRELOAD_PASSED`.
- A device-to-device variant should be added after H2D/D2H passes.

### `hipLaunchKernel`

Initial policy: use native ROCm for launch.

Acceptance:

- A standalone launch smoke launches a simple kernel and verifies its result
  without requiring native `hipMemcpy(DeviceToHost)`.
- Prefer direct CPU read of the mapped device allocation for validation until
  the preload `hipMemcpy` path is proven.
- It prints `HIP_LAUNCH_PRELOAD_BASELINE_PASSED` or
  `HIP_LAUNCH_NATIVE_PASSED` depending on the validation mode.

### `hipFree`

Initial policy: isolate and measure before implementing.

Acceptance for diagnostic phase:

- A standalone free smoke calls `hipFree` after a known-good allocation and
  records whether the call returns.
- If it does not return, the result is a documented SE ROCm teardown blocker,
  not a coherence failure.

Compatibility implementation should be considered only after confirming the
exact native failure path. A fake `hipFree` interposer is allowed only for
short-lived smoke tests and must be explicitly gated by an environment
variable, because silently leaking application allocations changes semantics.

## Test and validation rules

- Do not start full gem5 simulations from Codex unless the user delegates them.
- Codex may run structural pyunit checks, string checks, and lightweight build
  checks when appropriate.
- User-owned simulation output should be placed in dedicated `m5out-*`
  directories.
- If a run blocks or fails inside native ROCm, inspect matching ROCm source
  before implementing a compatibility workaround.
- Every run should record:
  - exact command;
  - pass/fail marker;
  - first error if any;
  - whether native ROCm or `LD_PRELOAD` path was used;
  - whether `HSA_ENABLE_SDMA=1` was set.
  - ROCm source path and internal function chain if source analysis was used.

## Non-goals for this phase

- Full ROCm runtime completeness.
- Full `hipMemcpyDefault` pointer-kind inference.
- Fully correct long-lived allocation tracking for a fake `hipFree`.
- Proving arbitrary multi-GPU coherence beyond the current resident-flag
  functional baseline.

## Risks

- `LD_PRELOAD` compatibility can hide native ROCm failures. Each preload smoke
  must clearly print that the compatibility layer is active.
- HSA async-copy may not exactly match all HIP memory-copy corner cases.
- Direct CPU reads are valid for current functional smoke tests but should not
  be mistaken for a general replacement for `hipMemcpy(DeviceToHost)`.
- Skipping or faking `hipFree` is acceptable only for short-lived diagnostics
  unless allocation tracking is implemented and validated.
