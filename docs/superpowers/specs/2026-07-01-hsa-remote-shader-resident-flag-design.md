# HSA Remote Shader Resident-Flag Diagnostic Design

Date: 2026-07-01

## Goal

Create an independent SE multi-GPU functional diagnostic that tests whether a
GPU resident reader observes a remote shader-store update after another GPU
publishes a GPU-side synchronization flag.

This test is intended to strengthen the current multi-GPU coherence evidence
without adding more modes to the already broad `hsa_remote_cache_read`
diagnostic.

## Current Context

The existing remote-cache diagnostics have already covered:

- same-dispatch resident-reader with SDMA_XGMI overwrite, updated after the
  `BDW_M + delayed L3Hit` fix;
- forward and reverse SDMA multi-line and large-copy variants;
- forward and reverse shader-store variants;
- separate-dispatch shader-store reader variants with targeted launch-acquire
  suppression.

The remaining gap is a focused shader-store test where the reader remains in
the same kernel dispatch and the inter-GPU synchronization itself is performed
with shader stores, not with host-side release of a control flag or a second
reader dispatch.

## Selected Diagnostic Shape

Add a new binary:

```text
tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_shader_resident_flag
```

Add a new kernel bundle:

```text
tests/test-progs/gpu/xgmi-peer-vram/resident_flag_kernels.hip
```

Do not add another mode to `hsa_remote_cache_read`.

The default direction is:

```text
GPU0 writer -> GPU1 resident reader
```

The reverse direction is:

```text
GPU1 writer -> GPU0 resident reader
```

## Data Layout

The test uses a target allocation in the reader GPU's VRAM. It contains a
`value` location and a `flag` location.

Two layouts must be supported because they test different behavior:

```text
--layout same-line
```

Places `value` and `flag` in the same cache line. This validates the user's
base synchronization pattern directly.

```text
--layout split-line
```

Places `value` and `flag` on different cache lines. This is a stronger
cache-persistence diagnostic because polling `flag` does not directly touch the
previously-read `value` line.

Default layout:

```text
same-line
```

Concrete offsets:

```text
same-line: value_dword = 0, flag_dword = 1
split-line: value_dword = 0, flag_dword = 32
```

The split-line offset uses 128 bytes of separation, which is at least two
64-byte cache lines.

## Synchronization Semantics

The first implementation should be strict. Later runs can weaken fences to
probe ordering sensitivity.

Supported sync modes:

```text
--sync strict
--sync writer-fence-only
--sync no-fence
```

Default sync mode:

```text
strict
```

Writer behavior:

```cpp
value[value_dword] = updated;
if (sync != no_fence) {
    __threadfence_system();
}
flag[flag_dword] = 1;
if (sync == strict) {
    __threadfence_system();
}
```

Reader behavior:

```cpp
first = value[value_dword];
first_read_done = 1;
while (flag[flag_dword] == 0 && spins < spin_limit) {
    ++spins;
}
if (flag[flag_dword] == 1 && sync == strict) {
    __threadfence_system();
}
second = value[value_dword];
```

The strict mode gives the cleanest release/acquire-like interpretation. The
weaker modes are explicit diagnostic controls and should not be used as the
first-pass correctness criterion.

## Control and Result Model

Use host-visible control memory for results and for the reader's
`first_read_done` / `reader_done` status. Do not use host writes to release the
reader's second value read.

The reader is released only by observing the GPU-written target `flag`.

Control structure:

```cpp
struct ResidentFlagControl
{
    uint32_t first_read_done;
    uint32_t reader_done;
    uint32_t first_value;
    uint32_t second_value;
    uint32_t seen_flag;
    uint32_t spins;
    uint32_t classification;
};
```

Classification values:

```text
0 = unset
1 = updated: first == old, flag == 1, second == updated
2 = stale_value: first == old, flag == 1, second == old
3 = flag_timeout: reader never observed flag == 1
4 = unexpected: any malformed value combination
```

## Execution Flow

1. Initialize HSA.
2. Enumerate CPU, GPU0, and GPU1 agents.
3. Select reader and writer agents from direction.
4. Allocate target memory in reader GPU VRAM.
5. Allocate host-visible control memory.
6. Map target/control to CPU, reader GPU, and writer GPU as needed.
7. Initialize `value = old`, `flag = 0`, and zero the control structure.
8. Launch resident reader kernel on the reader GPU.
9. Host waits for `control.first_read_done == 1`.
10. Launch writer kernel on the writer GPU.
11. Writer stores updated value, applies selected fence behavior, then stores
    `flag = 1`.
12. Host waits for writer kernel completion.
13. Host waits for `control.reader_done == 1`.
14. Host waits for reader kernel completion.
15. Host prints compact markers and returns success for updated or stale
    observations, but failure for timeout or unexpected values.

## CLI

Supported arguments:

```text
--reverse
--layout same-line
--layout split-line
--sync strict
--sync writer-fence-only
--sync no-fence
--spin-limit N
```

Defaults:

```text
direction = forward
layout = same-line
sync = strict
spin-limit = existing remote-cache SpinLimit value
```

## Output Markers

Every completed run prints one result line:

```text
RESIDENT_FLAG_RESULT direction=forward layout=same-line sync=strict first=0x11112222 second=0x33334444 flag=1 spins=123 classification=1
```

Final markers:

```text
RESIDENT_FLAG_PASSED_UPDATED
RESIDENT_FLAG_OBSERVED_STALE_VALUE
RESIDENT_FLAG_FLAG_TIMEOUT
RESIDENT_FLAG_FAILED_UNEXPECTED
```

`UPDATED` and `STALE_VALUE` are both diagnostic completions. `UPDATED` is the
expected result for strict mode if the remote shader-store path provides the
desired visibility. `STALE_VALUE` is evidence that the flag became visible but
the earlier cached value did not become visible as updated under that layout
and sync mode.

## Testing Scope

Structural pyunit should prove:

- the new binary and HSACO targets are wired in the Makefile;
- the new source is independent from `hsa_remote_cache_read` modes;
- both layouts are represented by explicit offset choices;
- all three sync modes are represented;
- result markers exist;
- the writer stores value before flag.

Full gem5 runs remain user-owned unless explicitly delegated. The recommended
run order is:

1. forward, same-line, strict;
2. reverse, same-line, strict;
3. forward, split-line, strict;
4. reverse, split-line, strict;
5. writer-fence-only controls only after strict behavior is understood;
6. no-fence controls only after writer-fence-only behavior is understood.

## Non-Goals

- Do not adapt HIP API behavior in this task.
- Do not add more modes to `hsa_remote_cache_read`.
- Do not claim complete multi-GPU GPU_VIPER coherence from one passing run.
- Do not use a second reader dispatch for the main diagnostic.
- Do not use host writes to release the reader's second value read.

## Open Interpretation Rule

If `same-line` passes and `split-line` fails, the result should be documented as
layout-sensitive visibility, not as complete coherence failure. If `strict`
passes and weaker sync modes fail, the result should be documented as ordering
sensitivity, not as a failure of the strict synchronization design.
