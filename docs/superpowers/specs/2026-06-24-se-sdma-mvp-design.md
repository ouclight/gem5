# SE SDMA MVP Design

Date: 2026-06-24

## Goal

Add a minimal SE-mode SDMA path so ROCm HIP APIs that normally use SDMA
(`hipMemset`, `hipMemcpy`, and later peer copies) can run without falling back
to slow CPU-side ROCclr/COMGR blit-kernel compilation.

The research goal remains multi-GPU interconnect/coherence. This design is
therefore scoped to the smallest SDMA model needed to make API-level traffic
enter the existing SE Ruby GPU clusters.

## Selected Architecture

Add a new SE-only `SESDMAEngine` under `src/dev/hsa/`.

Do not reuse the full-system `src/dev/amdgpu/sdma_engine.*` directly. That
engine is coupled to `AMDGPUDevice`, MMIO register state, GPUVM/GART, PM4,
interrupt handling, and full-system driver behavior. Reusing its packet
definitions is useful; reusing the object as-is would pull full-system device
modeling into SE mode.

`SESDMAEngine` is an independent `DmaVirtDevice`-based SimObject with one DMA
port per SE GPU. The SE Viper board already attaches each GPU device DMA port
to that GPU's Ruby DMA controller and GPU cluster. SDMA memory traffic should
therefore become visible to Ruby and, in Phase 2, to XGMI routing.

## Phase 1: Single-GPU SDMA for HIP API Smoke Tests

### Supported KFD queue type

- `KFD_IOC_QUEUE_TYPE_SDMA`

### Supported SDMA packet operations

- `SDMA_OP_NOP`
- `SDMA_OP_CONST_FILL`
- `SDMA_OP_COPY` with `SDMA_SUBOP_COPY_LINEAR`
- `SDMA_OP_FENCE`
- `SDMA_OP_TRAP`

Unsupported operations must fail loudly with a packet dump and opcode/subop
information. They must not silently complete.

### Queue model

`GPUComputeDriver::allocateQueue()` continues to allocate queue IDs and
doorbell offsets. It must branch by queue type:

- `COMPUTE` and `COMPUTE_AQL`: existing `HSAPacketProcessor` path.
- `SDMA`: register the queue with the target GPU's `SESDMAEngine`.
- `SDMA_XGMI`: rejected in Phase 1 with a clear fatal message or deferred to
  Phase 2 once the engine supports it.

Queue destroy must unregister the same backend.

### Doorbell routing

The existing SE doorbell page should remain mapped to the HSA packet
processor PIO range. `HSAPacketProcessor::write()` should route doorbell
writes by queue ID:

- compute queues: existing hardware scheduler path;
- SDMA queues: call the registered `SESDMAEngine` with the new write pointer.

This keeps the SE mmap behavior stable and avoids adding a second PIO page
layout during MVP.

### Address translation

Phase 1 uses the SE process page table, matching the existing
`HSAPacketProcessor::translate()` behavior:

- translate ring, rptr/wptr/fence, and source/destination virtual addresses
  using the current process page table;
- break large operations into page-contained chunks;
- issue DMA reads/writes to the translated physical addresses.

### Completion model

For MVP correctness:

- update the SDMA queue read pointer after packets are consumed;
- implement fence writes;
- treat trap as a completion point but do not model real GPU interrupts unless
  a ROCm test shows they are required.

## Phase 2: XGMI Peer SDMA

### Supported KFD queue type

- `KFD_IOC_QUEUE_TYPE_SDMA_XGMI`

### Scope

Support linear peer copy between GPU VRAM apertures, initially for cold or
non-cached peer buffers. The success criterion is that the DMA request
originates from the source GPU cluster and reaches the target GPU VRAM through
the existing XGMI/Ruby topology.

### Explicit non-goal

Phase 2 does not claim GPU cache coherence. If a target GPU has cached a line
before the remote SDMA write, stale-cache behavior remains a later coherence
feature. Any peer SDMA tests must distinguish raw VRAM visibility from
cache-coherent visibility.

## Expected Files

Create:

- `src/dev/hsa/se_sdma_engine.hh`
- `src/dev/hsa/se_sdma_engine.cc`

Modify:

- `src/dev/hsa/HSADevice.py`
- `src/dev/hsa/SConscript`
- `src/dev/hsa/hsa_packet_processor.hh`
- `src/dev/hsa/hsa_packet_processor.cc`
- `src/gpu-compute/gpu_compute_driver.hh`
- `src/gpu-compute/gpu_compute_driver.cc`
- `src/python/gem5/components/devices/gpus/se_viper_gpu.py`
- `src/python/gem5/prebuilt/viper/se_board.py` only after Phase 1 passes, to
  consider changing the default `HSA_ENABLE_SDMA`
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`

Optional focused program:

- `tests/test-progs/gpu/se-sdma-smoke/`

## Acceptance Criteria

Phase 1 is accepted when:

1. pyunit tests verify SE GPUs instantiate an SDMA DMA port per GPU and keep
   the port grouped with the correct GPU cluster.
2. KFD SDMA queues are not registered as compute queues.
3. `hip_api_smoke --stage memset` reaches `end hipMemset` with
   `HSA_ENABLE_SDMA=1`.
4. Host-to-device, device-to-host, and same-GPU device-to-device copies pass
   data validation.
5. The run does not enter the previous long COMGR blit-kernel path.

Phase 2 is accepted when:

1. `SDMA_XGMI` queue creation succeeds.
2. GPU0-originated peer copy to GPU1 VRAM completes and validates.
3. Ruby stats/debug output show DMA traffic attached to the source GPU cluster
   and remote VRAM activity through the XGMI topology.
4. Documentation states that cache-coherent remote write visibility is still
   future work.
