# SE ROCm HIP API Smoke Test Design

## Objective

Establish a reusable single-GPU ROCm HIP API compatibility baseline for the
SE-mode multi-GPU platform. The test uses public HIP APIs only and identifies
the first unsupported layer before extending coverage to multi-GPU programs.

The first milestone covers:

- `hipSetDevice`;
- `hipMalloc`;
- `hipMemset`;
- `hipMemcpy`;
- HIP kernel launch syntax;
- `hipDeviceSynchronize`;
- `hipFree`;
- normal process return.

Raw HSA queue construction, AQL packet submission, direct CPU reads of device
memory, `m5_exit`, and `m5_fail` are prohibited in this test.

## Program Structure

Add one standalone program under:

```text
tests/test-progs/gpu/hip-api-smoke/
```

The program accepts exactly one stage:

```text
--stage malloc
--stage memset
--stage memcpy
--stage launch
--stage lifecycle
```

Stages are cumulative. Each stage executes all operations from the preceding
stage before adding one new API layer.

## Common Logging Contract

Every HIP operation uses explicit markers:

```text
[hip_api_smoke] begin <exact API call>
[hip_api_smoke] end <exact API call>
```

Errors print:

- source file and line;
- exact failed API expression;
- HIP error name or string.

After target API completion and data validation, non-lifecycle stages print and
flush:

```text
API_STAGE_PASSED stage=<name>
```

The lifecycle stage prints and flushes:

```text
LIFECYCLE_PASSED
```

immediately before returning zero.

## Data Model

Use a small fixed-size buffer large enough to exercise multiple work-items,
for example 256 `uint32_t` elements.

Use distinguishable byte and word patterns:

- memset byte pattern for the memset stage;
- deterministic seed/index values for the kernel stage.

Host-side validation checks every element and reports the first mismatch plus
the total mismatch count.

## Stage Semantics

### `malloc`

Sequence:

1. `hipGetDeviceCount`;
2. require at least one device;
3. `hipSetDevice(0)`;
4. `hipMalloc`.

Acceptance:

- allocation returns success;
- pointer is non-null;
- `API_STAGE_PASSED stage=malloc` is printed.

This stage intentionally does not call `hipFree`. It isolates allocation from
the known teardown path.

### `memset`

Sequence:

1. complete `malloc`;
2. `hipMemset`;
3. `hipDeviceSynchronize`;
4. `hipMemcpy(..., hipMemcpyDeviceToHost)`;
5. validate every byte/word on the host.

Acceptance:

- memset, synchronization, and copy all return success;
- copied data matches the requested memset byte pattern;
- `API_STAGE_PASSED stage=memset` is printed.

Although this stage introduces synchronization and D2H copy as observation
mechanisms, the target under test is memset. Logs identify which API blocks if
the observation path is unsupported.

### `memcpy`

Sequence:

1. complete allocation;
2. initialize deterministic host input;
3. `hipMemcpy(..., hipMemcpyHostToDevice)`;
4. `hipMemcpy(..., hipMemcpyDeviceToHost)`;
5. validate exact round-trip equality.

Acceptance:

- both copy directions return success;
- every output element equals input;
- `API_STAGE_PASSED stage=memcpy` is printed.

### `launch`

Sequence:

1. complete allocation;
2. initialize input with `hipMemcpyHostToDevice`;
3. launch a compiled HIP kernel with normal `<<<grid, block>>>` syntax;
4. check `hipGetLastError`;
5. call `hipDeviceSynchronize`;
6. copy results with `hipMemcpyDeviceToHost`;
7. validate the kernel's deterministic transformation.

Acceptance:

- runtime kernel launch and synchronization complete;
- output data matches the expected transformation;
- `API_STAGE_PASSED stage=launch` is printed.

No raw HSA APIs or separately loaded HSACO are allowed.

### `lifecycle`

Sequence:

1. complete the full launch-stage workload and validation;
2. `hipFree`;
3. print and flush `LIFECYCLE_PASSED`;
4. return zero from `main`.

Acceptance has two layers:

1. API completion: `hipFree` returns and `LIFECYCLE_PASSED` is printed.
2. Process lifecycle: gem5 exits normally without ROCm teardown stalls,
   `TimingSimpleCPU::suspendContext()` assertions, or pseudo instructions.

## Failure Interpretation

The first unmatched `begin` marker identifies the blocked API.

Examples:

- `begin hipMemset` without `end`: memset/runtime submission blocker;
- `begin hipDeviceSynchronize` without `end`: queue completion blocker;
- `begin hipMemcpy(...DeviceToHost)` without `end`: D2H copy blocker;
- `begin hipFree` without `end`: memory teardown blocker;
- `LIFECYCLE_PASSED` followed by an assertion: generic SE process-exit blocker.

An API-stage failure must not be described as a GPU coherence failure unless
Ruby evidence independently shows that.

## Build

The program is compiled directly with ROCm 4 `hipcc` for gfx900. The host and
device code live in one `.hip` translation unit so normal HIP kernel launch
registration is exercised.

The binary must not link gem5 m5ops.

## Regression Coverage

Lightweight source regressions verify:

- all five stage names;
- cumulative ordering;
- required HIP API calls;
- normal `<<<...>>>` launch syntax;
- host data validation;
- exact pass markers;
- absence of raw HSA headers/APIs;
- absence of `m5_exit`, `m5_fail`, and direct mapped-device reads;
- lifecycle ordering: validation, `hipFree`, pass marker, `return 0`.

## Runtime Matrix

Run stages in order and stop at the first failure:

1. `malloc`;
2. `memset`;
3. `memcpy`;
4. `launch`;
5. `lifecycle`.

Each stage receives a separate output directory and log files. Broad Ruby
tracing is disabled initially. Add bounded tracing only after the first blocked
API is known.

## Ownership

Codex may:

- implement source and lightweight tests;
- inspect logs;
- make targeted fixes after identifying a root cause.

The user owns:

- building gem5;
- building the HIP test binary;
- full Timing CPU simulation runs.

## Success Criteria

The first milestone is complete when:

1. every stage provides deterministic begin/end markers;
2. every data-producing operation is validated;
3. `launch` passes using only public HIP runtime APIs;
4. `lifecycle` completes `hipFree`;
5. the process returns normally without pseudo-operation exit workarounds.
