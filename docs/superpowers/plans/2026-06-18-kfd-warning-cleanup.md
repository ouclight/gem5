# KFD Warning Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move normal KFD and render-driver tracing behind the `GPUDriver` debug flag while preserving actionable warnings.

**Architecture:** Add a static regression that checks logging classification by source text. Convert only normal-path `warn()` calls to `DPRINTF(GPUDriver, ...)`; leave unsupported, ignored, conflicting, and partial-cleanup warnings unchanged.

**Tech Stack:** C++, gem5 debug flags, Python `unittest`.

---

### Task 1: Add logging-classification regression

**Files:**
- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`

- [x] Add a test that rejects `warn()` for normal KFD/render trace messages,
  requires those strings in `DPRINTF(GPUDriver, ...)`, and confirms selected
  actionable warnings remain.
- [x] Run:

  ```bash
  build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib \
    -m unittest \
    test_se_viper_multigpu.SEViperMultiGPUTest.test_kfd_normal_operations_use_debug_tracing
  ```

  Expected: FAIL because normal operations still use `warn()`.

### Task 2: Reclassify normal-path diagnostics

**Files:**
- Modify: `src/gpu-compute/gpu_compute_driver.cc`
- Modify: `src/gpu-compute/gpu_render_driver.cc`

- [x] Convert normal operation messages for open, queue creation, aperture
  enumeration, event creation, VM acquisition, allocation, and peer mapping
  to `DPRINTF(GPUDriver, ...)`.
- [x] Add the `GPUDriver` debug header to the render driver and convert only
  its open message to `DPRINTF`.
- [x] Preserve warnings for unsupported ioctls, ignored policy, mmap conflicts,
  unrecognized mmap types, unsupported event types, duplicate waiters, and
  incomplete free mappings.
- [x] Rerun the focused test and expect PASS.

### Task 3: Run lightweight regression

**Files:**
- Modify: `docs/debug/se-multigpu-status.md`

- [x] Run:

  ```bash
  build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib \
    -m unittest test_se_viper_multigpu
  ```

- [x] Run:

  ```bash
  build/VEGA_X86/gem5.opt -p tests/pyunit \
    -m unittest test_gpu_viper_wb_l2_transitions
  ```

- [x] Run `git diff --check`.
- [x] Record exact commands, results, files changed, and the next review step
  in `docs/debug/se-multigpu-status.md`.
