# Remote Write After Cache Read Diagnostic Design

Date: 2026-06-27

## Goal

Create a direct SE multi-GPU diagnostic smoke that classifies GPU1's second
read of a GPU1 VRAM location after the same location is overwritten remotely
through GPU0-to-GPU1 SDMA_XGMI.

This is a diagnostic baseline for multi-GPU coherence work. It must not assume
that GPU_VIPER already implements complete multi-GPU cache coherence.

## Current Context

The SE SDMA MVP is implemented and validated for:

- H2D/D2H HSA async-copy through ordinary SDMA queues;
- GPU0-to-GPU1 peer HSA async-copy through SDMA_XGMI queues;
- the packet sequence `POLL_REGMEM -> COPY -> ATOMIC`.

The remaining research question is whether a GPU-side cached observation is
invalidated or otherwise refreshed after a remote write. The current
GPU_VIPER protocol should be treated as insufficiently specified for the
multi-GPU coherence case until direct behavior proves otherwise.

## Selected Diagnostic Shape

Use one long-running GPU1 reader kernel and one host-triggered SDMA_XGMI
overwrite:

1. GPU1 reader kernel reads a GPU1 VRAM `target` value once.
2. The reader kernel remains active and waits on a control flag.
3. The host waits until the first GPU1 read is complete.
4. The host uses HSA async-copy to copy a GPU0 VRAM source value into the
   same GPU1 VRAM `target`.
5. After SDMA completion, the host releases the GPU1 reader kernel.
6. The same GPU1 kernel reads `target` again and classifies the value.

This shape keeps both reads inside one GPU1 dispatch. It avoids a second
kernel launch becoming an implicit acquire/cache-state boundary.

## Files

Create:

- `tests/test-progs/gpu/xgmi-peer-vram/remote_cache_kernels.hip`
- `tests/test-progs/gpu/xgmi-peer-vram/hsa_remote_cache_read.cpp`

Modify:

- `tests/test-progs/gpu/xgmi-peer-vram/Makefile`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- `docs/debug/se-multigpu-status.md`

## Data Model

Use three allocations:

- GPU1 VRAM `target`: one 32-bit value, initialized to `A`.
- GPU0 VRAM `source`: one 32-bit value, initialized to `B`.
- CPU/system-visible `Control`: synchronization and result fields.

Control fields:

```cpp
struct RemoteCacheControl
{
    uint32_t first_read_done;
    uint32_t allow_second_read;
    uint32_t reader_done;
    uint32_t first_value;
    uint32_t second_value;
    uint32_t classification;
};
```

Classification values:

```text
0 = unset
1 = updated: second_value == B
2 = stale: second_value == A
3 = unexpected: second_value is neither A nor B
```

The target pointer passed to the reader kernel should not be declared
`volatile`. The control pointer should be volatile or accessed through
compiler-visible polling so the wait loop is not optimized away. The target
read is the behavior under test; adding `volatile` to the target could weaken
the cache-residency diagnostic.

## Execution Flow

### Phase 0: Setup

1. Initialize HSA.
2. Enumerate CPU, GPU0, and GPU1 agents.
3. Find allocable GPU0 and GPU1 global memory pools.
4. Allocate GPU0 `source` and GPU1 `target`.
5. Allocate or map control memory visible to host and GPU1.
6. Enable access for CPU/GPU0/GPU1 where required.
7. Initialize `target = A`, `source = B`, and all control fields to zero.

### Phase 1: First GPU1 Read

Launch the GPU1 reader kernel:

```cpp
remote_cache_reader(target, control, value_a, value_b, spin_limit)
```

The kernel:

1. reads `target[0]`;
2. stores it in `control.first_value`;
3. sets `control.first_read_done = 1`;
4. waits for `control.allow_second_read == 1`;
5. reads `target[0]` again;
6. stores `control.second_value`;
7. sets `control.classification`;
8. sets `control.reader_done = 1`.

### Phase 2: Host Waits For First Read

The host polls `control.first_read_done`. If it does not become 1 before a
bounded host poll limit, the program exits with:

```text
REMOTE_CACHE_READ_FAILED reason=first_read_timeout
```

### Phase 3: Remote Write

The host performs:

```text
GPU0 source -> GPU1 target
```

using `hsa_amd_memory_async_copy`.

Expected trace:

```text
AMDKFD_IOC_CREATE_QUEUE ... queue_type 3
backend sdma_xgmi
doorbell route ... to SESDMAEngine
POLL_REGMEM -> COPY -> ATOMIC
```

### Phase 4: Second GPU1 Read

After SDMA completion, the host writes:

```cpp
control.allow_second_read = 1;
```

The GPU1 reader kernel completes its second read and writes
`control.reader_done = 1`.

### Phase 5: Classification Output

The host prints one of:

```text
REMOTE_CACHE_READ_RESULT first=0x11111111 second=0x22222222 classification=updated
REMOTE_CACHE_READ_PASSED_UPDATED
```

```text
REMOTE_CACHE_READ_RESULT first=0x11111111 second=0x11111111 classification=stale
REMOTE_CACHE_READ_OBSERVED_STALE
```

```text
REMOTE_CACHE_READ_RESULT first=0x11111111 second=0x???????? classification=unexpected
REMOTE_CACHE_READ_FAILED_UNEXPECTED
```

The `updated` and `stale` classifications both exit with status 0 because
both are valid diagnostic outcomes. `unexpected`, setup failures, dispatch
failures, and timeouts exit non-zero.

## Acceptance Criteria

The diagnostic smoke is accepted when:

1. It builds as part of `tests/test-progs/gpu/xgmi-peer-vram`.
2. It starts one GPU1 long-running reader dispatch.
3. It performs a GPU0-to-GPU1 `hsa_amd_memory_async_copy` while the GPU1
   reader dispatch is active.
4. The trace confirms `queue_type 3`, `backend sdma_xgmi`, and doorbell
   routing to `SESDMAEngine`.
5. The program prints exactly one final classification:
   `REMOTE_CACHE_READ_PASSED_UPDATED`, `REMOTE_CACHE_READ_OBSERVED_STALE`, or
   `REMOTE_CACHE_READ_FAILED_UNEXPECTED`.
6. `updated` and `stale` are documented as observations, not as final proof of
   correctness or incorrectness of GPU_VIPER coherence.

## Non-Goals

- Do not implement coherence or invalidation in the first diagnostic smoke.
- Do not infer full multi-GPU coherence from an `updated` result.
- Do not depend on HIP `hipMemset` or `hipMemcpy`.
- Do not model complete SDMA ISA behavior beyond the existing MVP path unless
  this smoke exposes a new unsupported packet.

## Follow-Up Decisions

If the result is stale:

- inspect which GPU1 cache level retains the old line;
- identify the smallest invalidation point, likely SDMA completion, XGMI
  write arrival, or a targeted Ruby protocol event.

If the result is updated:

- verify whether the target access actually allocated in GPU1 cache;
- inspect generated memory instructions and GPU/Ruby cache stats;
- strengthen the test if needed by using repeated reads, a larger line-sized
  region, or a kernel variant that encourages cache residency.

