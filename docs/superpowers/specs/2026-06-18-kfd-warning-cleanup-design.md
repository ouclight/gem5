# KFD Warning Cleanup Design

## Goal

Prevent normal SE multi-GPU KFD and render-driver operations from emitting
unconditional warnings while preserving diagnostics for unsupported behavior,
invalid state, and partial cleanup.

## Design

Normal-path tracing in `GPUComputeDriver` will use the existing `GPUDriver`
debug flag through `DPRINTF`. This includes device open, queue creation,
aperture enumeration, event creation, VM acquisition, allocation, and peer
mapping details.

Warnings remain unconditional when the simulator ignores requested behavior,
encounters unsupported operations, observes conflicting mappings, or cannot
fully clean up an allocation. The render-driver open message becomes a debug
trace, while unsupported render ioctls and the existing mmap warning remain
warnings.

## Verification

A source-level regression test will distinguish normal-path trace strings from
warnings that must remain. The existing SE multi-GPU and GPU VIPER regression
suites will then be rerun. No full build or simulation is required for this
logging-only change.
