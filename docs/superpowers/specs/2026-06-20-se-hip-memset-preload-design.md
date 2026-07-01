# SE HIP Memset Preload Compatibility Design

## Goal

Allow ROCm 4.0.1 applications running on the SE multi-GPU platform to call
the public synchronous `hipMemset` API without triggering ROCclr's in-simulator
compilation of its complete internal blit program.

## Scope

This phase intercepts only:

```cpp
hipError_t hipMemset(void *dst, int value, size_t sizeBytes);
```

All allocation, device selection, module loading, kernel dispatch,
synchronization, memory copy, and free operations continue through the
unmodified ROCm runtime. `hipMemsetAsync`, two- and three-dimensional memset,
memcpy APIs, and SDMA support are outside this phase.

## Architecture

Create `tests/test-progs/gpu/hip-api-smoke/se_hip_compat/` containing:

- a gfx900 HIP kernel that writes one byte per work item;
- a build rule that produces a code-object-v2 HSACO outside simulation;
- a shared library exporting `hipMemset`;
- focused host tests for argument validation and launch parameter calculation.

The compatibility library is loaded through `LD_PRELOAD`. Its `hipMemset`
implementation loads the precompiled code-object-v2 HSACO through the HSA
runtime, resolves the kernel symbol, creates a private HSA queue, submits an
AQL dispatch packet, and waits for its completion signal before returning.

ROCm 4.0.1's `hipModuleLoad` accepted this HSACO but
`hipModuleGetFunction` could not resolve either the metadata name or its
`@kd` symbol. The HSA executable path is therefore required for
code-object-v2 compatibility and follows the repository's existing working
peer-VRAM tests.

The wrapper resolves only `hipGetDevice` with `dlsym(RTLD_NEXT, ...)` to map
the current HIP device to its corresponding HSA GPU agent. Initialization is
protected by `std::once_flag`. Initialization failure is retained and returned
on later calls rather than retried concurrently.

## Kernel Interface

The precompiled kernel has this signature:

```cpp
extern "C" __global__ void
seHipMemsetKernel(unsigned char *dst, unsigned char value, size_t size);
```

Each work item computes a linear byte index and writes only when
`index < size`. The wrapper uses 256 threads per block and computes:

```text
blocks = ceil(sizeBytes / 256)
```

A zero-byte call returns `hipSuccess` without loading the module or launching a
kernel. A null destination with nonzero size returns
`hipErrorInvalidDevicePointer`.

## Configuration

The XGMI config already supports repeatable environment overrides. A test run
passes:

```text
LD_PRELOAD=<absolute path>/libse_hip_compat.so
SE_HIP_MEMSET_HSACO=<absolute path>/se_hip_memset.hsaco
```

The library requires `SE_HIP_MEMSET_HSACO`; a missing or unreadable path
returns `hipErrorFileNotFound`.

AtomicSimpleCPU is the default diagnostic CPU for fast API and kernel-dispatch
validation. Because Ruby uses `atomic_noncaching` with an atomic CPU, an Atomic
pass is not sufficient evidence for Ruby cache/timing behavior. After the
Atomic run passes, repeat the bounded smoke stage with Timing CPU to validate
the normal Ruby request path.

## Error Handling

The wrapper reports HSA initialization, executable, queue, allocation, signal,
or dispatch failures and maps them to a HIP error. Completion is synchronous:
`hipMemset` returns only after its HSA completion signal reaches zero.

It writes one concise diagnostic line to `stderr` when module initialization
fails. It does not silently fall back to the original `hipMemset`, because
that would re-enter the known unbounded COMGR path.

## Verification

Verification has three layers:

1. Pyunit source/build contract test:
   - wrapper exports `hipMemset`;
   - HSACO build targets gfx900/code-object-v2;
   - config uses Atomic CPU and `LD_PRELOAD`.
2. Native build verification:
   - shared library and HSACO build successfully;
   - `nm -D` shows the exported `hipMemset`;
   - HSACO contains `seHipMemsetKernel`.
3. Bounded Atomic gem5 run of `hip_memset_preload_smoke`:
   - prints `HIP_MEMSET_PRELOAD_PASSED`;
   - syscall trace does not create `/tmp/comgr-*`;
   - GPU instruction and completed-workgroup counters are nonzero.
4. Bounded Timing gem5 run:
   - the same dedicated public-API diagnostic passes;
   - Ruby request/activity counters are nonzero;
   - no COMGR compilation occurs.

## Non-Goals and Follow-Up

This is an SE compatibility layer, not a replacement for SDMA. Future work may
extend the same mechanism to APIs whose ROCclr implementation depends on blit
kernels, but each API requires an independent semantic test. The preferred
long-term architecture remains SE SDMA queue and packet support.
