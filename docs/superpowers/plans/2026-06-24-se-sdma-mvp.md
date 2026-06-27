# SE SDMA MVP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans for inline implementation. Do not use subagents for this repository unless the user explicitly reverses the current instruction. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a minimal SE-only SDMA engine that lets ROCm `hipMemset` and `hipMemcpy` use SDMA queues in the SE multi-GPU platform, then extend the same path to XGMI peer copies.

**Architecture:** Add `SESDMAEngine` under `src/dev/hsa/` as a `DmaVirtDevice` with one instance per `SEVegaGPU`. KFD queue creation branches SDMA queues away from `HSAPacketProcessor` compute scheduling while preserving the existing doorbell page mapping; doorbell writes are routed to either compute or SDMA backends by queue ID.

**Tech Stack:** gem5 C++ SimObjects, Python stdlib components, ROCm KFD ioctl structs, SDMA packet definitions from `src/dev/amdgpu/`, SE process page tables, Ruby DMA ports.

---

## File Structure

- `src/dev/hsa/se_sdma_engine.hh`: queue descriptors, SDMA doorbell entry point, packet decode declarations, DMA helper declarations.
- `src/dev/hsa/se_sdma_engine.cc`: SE page-table translation, ring reads, packet execution, DMA chunking, rptr/fence completion writes.
- `src/dev/hsa/HSADevice.py`: `SESDMAEngine` SimObject definition.
- `src/dev/hsa/SConscript`: source, SimObject, and `SESDMAEngine` debug flag registration.
- `src/dev/hsa/hsa_packet_processor.hh/.cc`: doorbell routing table from queue ID to `SESDMAEngine`.
- `src/gpu-compute/gpu_compute_driver.hh/.cc`: KFD queue-type dispatch and queue destroy routing.
- `src/python/gem5/components/devices/gpus/se_viper_gpu.py`: instantiate one SDMA engine per SE GPU and expose its DMA port.
- `src/python/gem5/prebuilt/viper/se_gpu_cache_hierarchy.py`: no functional change expected; tests verify it already groups the new port with the GPU cluster.
- `src/python/gem5/prebuilt/viper/se_board.py`: keep `HSA_ENABLE_SDMA=0` until Phase 1 end-to-end passes; later switch or document explicit `--env HSA_ENABLE_SDMA=1`.
- `tests/pyunit/stdlib/test_se_viper_multigpu.py`: structural tests for SDMA instantiation and queue-type handling.

## Phase 1: Single-GPU SDMA

### Task 1: Add structural tests for per-GPU SDMA instantiation

**Files:**
- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`

- [ ] **Step 1: Write the failing tests**

Add tests that construct the SE Viper board and assert:

```python
self.assertTrue(hasattr(gpu, "se_sdma_engine"))
self.assertIn(gpu.se_sdma_engine.dma, gpu.get_cpu_dma_ports())
```

Also assert `len(board.get_dma_ports_by_gpu()[gpu_index])` increases by one
relative to the current compute/HSA packet processor ports.

- [ ] **Step 2: Run the tests and confirm RED**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_viper_gpu_exposes_sdma_dma_port
```

Expected result: failure because `SEVegaGPU` has no `se_sdma_engine`.

- [ ] **Step 3: Implement SimObject skeleton**

Create `src/dev/hsa/se_sdma_engine.hh` and `src/dev/hsa/se_sdma_engine.cc`
with a minimal `SESDMAEngine : public DmaVirtDevice` that has constructor,
`name()`, queue registration stubs, and no packet execution yet.

Add to `src/dev/hsa/HSADevice.py`:

```python
class SESDMAEngine(DmaVirtDevice):
    type = "SESDMAEngine"
    cxx_header = "dev/hsa/se_sdma_engine.hh"
    cxx_class = "gem5::SESDMAEngine"
    gpuId = Param.UInt32("KFD GPU id for this SE SDMA engine")
```

Add to `src/dev/hsa/SConscript`:

```python
SimObject('HSADevice.py', sim_objects=['HSAPacketProcessor', 'SESDMAEngine'])
Source('se_sdma_engine.cc')
DebugFlag('SESDMAEngine')
```

- [ ] **Step 4: Instantiate per-GPU engine**

Modify `src/python/gem5/components/devices/gpus/se_viper_gpu.py`:

```python
from m5.objects import SESDMAEngine

self.se_sdma_engine = SESDMAEngine(gpuId=config.gpu_id)
self._cpu_dma_ports.append(self.se_sdma_engine.dma)
```

- [ ] **Step 5: Run structural tests and compile check**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_se_viper_gpu_exposes_sdma_dma_port
```

Expected result after rebuild: pass.

If Python import fails because C++ SimObject code has not been compiled, user runs:

```bash
scons build/VEGA_X86/gem5.opt -j$(nproc)
```

### Task 2: Add KFD queue backend routing tests

**Files:**
- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- Modify: `src/gpu-compute/gpu_compute_driver.hh`
- Modify: `src/gpu-compute/gpu_compute_driver.cc`

- [ ] **Step 1: Write source-level tests**

Add pyunit source checks that verify:

```python
self.assertIn("KFD_IOC_QUEUE_TYPE_SDMA", gpu_compute_driver_cc)
self.assertIn("registerSDMAQueue", gpu_compute_driver_cc)
self.assertIn("unsetSDMAQueue", gpu_compute_driver_cc)
```

and that `KFD_IOC_QUEUE_TYPE_SDMA_XGMI` is present but explicitly deferred or
handled separately.

- [ ] **Step 2: Run and confirm RED**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_kfd_sdma_queue_type_is_not_registered_as_compute
```

Expected result: failure because the driver currently sends all queues through
`hsa_pp.setDeviceQueueDesc(...)`.

- [ ] **Step 3: Add backend enum and maps**

Add a small queue-backend enum in `GPUComputeDriver`:

```cpp
enum class QueueBackend
{
    Compute,
    Sdma,
    SdmaXgmi
};
std::unordered_map<uint32_t, QueueBackend> queueIdToBackend;
```

- [ ] **Step 4: Branch create/destroy by queue type**

In `GPUComputeDriver::allocateQueue()`:

```cpp
switch (args->queue_type) {
  case KFD_IOC_QUEUE_TYPE_COMPUTE:
  case KFD_IOC_QUEUE_TYPE_COMPUTE_AQL:
      hsa_pp.setDeviceQueueDesc(...);
      queueIdToBackend[args->queue_id] = QueueBackend::Compute;
      break;
  case KFD_IOC_QUEUE_TYPE_SDMA:
      hsa_pp.registerSDMAQueue(args->queue_id, ...);
      queueIdToBackend[args->queue_id] = QueueBackend::Sdma;
      break;
  case KFD_IOC_QUEUE_TYPE_SDMA_XGMI:
      fatal("SE SDMA_XGMI queue is Phase 2 and is not implemented yet");
      break;
  default:
      fatal("Unsupported KFD queue type %u", args->queue_type);
}
```

In `AMDKFD_IOC_DESTROY_QUEUE`, dispatch to `unsetDeviceQueueDesc()` or
`unregisterSDMAQueue()` by `queueIdToBackend`.

- [ ] **Step 5: Run source-level tests**

Run the same pyunit test. Expected: pass after source changes.

### Task 3: Route doorbells to compute or SDMA backend

**Files:**
- Modify: `src/dev/hsa/hsa_packet_processor.hh`
- Modify: `src/dev/hsa/hsa_packet_processor.cc`
- Modify: `src/dev/hsa/se_sdma_engine.hh`

- [ ] **Step 1: Write source-level tests**

Add a pyunit source test checking these symbols:

```python
"registerSDMAQueue"
"unregisterSDMAQueue"
"writeDoorbell"
"sdmaQueues"
```

Expected RED: symbols do not exist.

- [ ] **Step 2: Add routing API**

In `HSAPacketProcessor`, add:

```cpp
void registerSDMAQueue(uint64_t queue_id, SESDMAEngine *engine);
void unregisterSDMAQueue(uint64_t queue_id);
```

Maintain:

```cpp
std::unordered_map<uint64_t, SESDMAEngine *> sdmaQueues;
```

- [ ] **Step 3: Route writes**

In `HSAPacketProcessor::write(Packet *pkt)`, derive the queue ID from the
doorbell offset exactly as the current compute path does. If that queue ID is
in `sdmaQueues`, call:

```cpp
engine->writeDoorbell(queue_id, doorbell_reg);
```

Otherwise keep the existing `hwSchdlr->write(...)` path unchanged.

- [ ] **Step 4: Run source-level tests**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_hsa_packet_processor_routes_sdma_doorbells
```

Expected: pass.

### Task 4: Implement SDMA NOP/FENCE/TRAP

**Files:**
- Modify: `src/dev/hsa/se_sdma_engine.hh`
- Modify: `src/dev/hsa/se_sdma_engine.cc`

- [ ] **Step 1: Add focused decode tests where possible**

If direct C++ unit scaffolding is not practical in this gem5 tree, add a
source-level pyunit test that verifies `SESDMAEngine` includes:

```python
"SDMA_OP_NOP"
"SDMA_OP_FENCE"
"SDMA_OP_TRAP"
"processQueue"
"updateReadPointer"
```

Expected RED before implementation.

- [ ] **Step 2: Implement queue descriptor**

Store:

```cpp
struct QueueDesc {
    Addr readPtrAddr;
    Addr writePtrAddr;
    Addr ringBase;
    uint64_t ringSize;
    uint32_t doorbellSize;
    uint64_t readIndex = 0;
};
```

- [ ] **Step 3: Implement ring processing loop**

On doorbell:

1. read host write pointer from the doorbell value or `writePtrAddr`;
2. while `readIndex != writeIndex`, read one packet header from the ring;
3. decode opcode;
4. advance `readIndex`;
5. write `readIndex` back to `readPtrAddr`.

- [ ] **Step 4: Implement completion-only ops**

`NOP` advances. `FENCE` writes the requested fence value to the fence address.
`TRAP` records completion and advances without interrupt modeling.

### Task 5: Implement CONST_FILL for `hipMemset`

**Files:**
- Modify: `src/dev/hsa/se_sdma_engine.cc`

- [ ] **Step 1: Add source-level test**

Verify implementation contains:

```python
"SDMA_OP_CONST_FILL"
"dmaWrite"
"fillData"
"translateRange"
```

- [ ] **Step 2: Implement virtual-to-physical chunking**

For each fill region, split at page boundaries and call the SE process page
table translation. Panic with address/size if translation fails.

- [ ] **Step 3: Implement DMA writes**

Generate a buffer containing the repeated fill pattern and issue `dmaWrite()`
chunks through the engine DMA port.

- [ ] **Step 4: User-owned end-to-end test**

After user rebuilds gem5, run:

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

Expected result:

```text
[hip_api_smoke] end hipMemset(device_data, memset_byte, bytes)
API_STAGE_PASSED stage=memset
```

### Task 6: Implement COPY_LINEAR for `hipMemcpy`

**Files:**
- Modify: `src/dev/hsa/se_sdma_engine.cc`
- Modify or create: `tests/test-progs/gpu/se-sdma-smoke/`

- [ ] **Step 1: Add source-level test**

Verify implementation contains:

```python
"SDMA_OP_COPY"
"SDMA_SUBOP_COPY_LINEAR"
"dmaRead"
"dmaWrite"
```

- [ ] **Step 2: Implement chunked copy**

Translate source and destination ranges, split chunks at both source and
destination page boundaries, DMA-read into a small temporary buffer, then
DMA-write to destination.

- [ ] **Step 3: User-owned smoke tests**

Run HIP API stages:

```bash
--opts=--stage memcpy-h2d
--opts=--stage memcpy-d2h
--opts=--stage memcpy-d2d
```

Each run uses `--env HSA_ENABLE_SDMA=1`. Expected output:

```text
API_STAGE_PASSED stage=<stage>
```

### Task 7: Phase 1 cleanup and default policy

**Files:**
- Modify: `docs/debug/se-multigpu-status.md`
- Optionally modify: `src/python/gem5/prebuilt/viper/se_board.py`

- [ ] **Step 1: Update status document**

Record commands, first relevant result, accepted/rejected hypotheses, changed
files, and next step.

- [ ] **Step 2: Decide default SDMA environment**

If Phase 1 smoke tests pass reliably, either:

1. set default `HSA_ENABLE_SDMA=1`, or
2. keep default disabled and document `--env HSA_ENABLE_SDMA=1` as the
   required SE SDMA test mode.

Prefer option 2 until both memset and memcpy smoke tests pass in KVM and
Timing CPU modes.

## Phase 2: XGMI Peer SDMA

### Task 8: Add `SDMA_XGMI` queue support

**Files:**
- Modify: `src/gpu-compute/gpu_compute_driver.cc`
- Modify: `src/dev/hsa/se_sdma_engine.hh`
- Modify: `src/dev/hsa/se_sdma_engine.cc`

- [ ] **Step 1: Add source-level test**

Assert `KFD_IOC_QUEUE_TYPE_SDMA_XGMI` is routed to an SDMA backend instead of
the compute backend.

- [ ] **Step 2: Register XGMI queue**

Use the same `SESDMAEngine` implementation initially, but tag queue descriptors
as `isXgmi=true`.

### Task 9: Add peer-copy validation

**Files:**
- Create or modify: `tests/test-progs/gpu/se-sdma-smoke/`
- Modify: `docs/debug/se-multigpu-status.md`

- [ ] **Step 1: Add smoke program mode**

Program flow:

1. `hipSetDevice(1)`;
2. allocate GPU1 VRAM;
3. `hipSetDevice(0)`;
4. enable peer access to GPU1;
5. issue `hipMemcpy` GPU0-to-GPU1 through SDMA/XGMI path;
6. verify by CPU mapped read or GPU1 validation kernel.

- [ ] **Step 2: User-owned Timing/Ruby test**

Run with Ruby debug/stat collection. Expected result:

```text
API_STAGE_PASSED stage=peer-copy
```

and stats/debug evidence that DMA traffic originated from GPU0's DMA
controller and reached GPU1 VRAM through the XGMI topology.

### Task 10: Document coherence boundary

**Files:**
- Modify: `docs/debug/se-multigpu-status.md`
- Optionally create: `docs/debug/se-sdma-mvp.md`

- [ ] **Step 1: Record exact supported behavior**

State that Phase 2 peer SDMA validates remote VRAM writes and XGMI routing,
not cache-coherent remote write visibility.

- [ ] **Step 2: Define future coherence experiment**

Future test shape:

1. GPU1 caches line;
2. GPU0 SDMA writes same line remotely;
3. GPU1 rereads without explicit invalidation;
4. expected result depends on future protocol invalidation support.

## Execution Notes

- Do not run long gem5 builds or full simulations without user delegation.
- Prefer pyunit/source-level tests and small compile checks during Codex work.
- The user owns full `scons build/VEGA_X86/gem5.opt` and full HIP smoke runs unless explicitly delegated.
- Do not commit automatically. This repository has active user-owned changes.
