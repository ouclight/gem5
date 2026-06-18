# SE Page Alias Deallocation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Verify and fix SE page deallocation so a surviving virtual alias keeps its physical page contents.

**Architecture:** A small x86 SE program creates two VMAs and pauses at `m5_work_begin`. A dedicated Python harness maps both VMAs to one physical page using `Process.map()`, then resumes the program to exercise the real `munmap` path.

**Tech Stack:** C, gem5 m5ops, gem5 Python configuration, C++ SE process memory management.

---

### Task 1: Build the SE alias microbenchmark

**Files:**
- Create: `tests/test-progs/se/page-alias/page_alias.c`
- Create: `tests/test-progs/se/page-alias/Makefile`
- Create: `tests/test-progs/se/page-alias/run.py`

- [ ] **Step 1: Add a program that creates two untouched fixed VMAs**

Use fixed addresses `0x600000000000` and `0x600000001000`, verify both
`mmap()` calls, and invoke `m5_work_begin(0, 0)` before either mapping is
accessed.

- [ ] **Step 2: Add the alias-preservation check**

Write a nonzero pattern through the first alias, confirm it through the second,
`munmap()` the first alias, then compare every byte through the second alias.
Print `SE page alias preservation passed` and return zero on success.

- [ ] **Step 3: Add a Makefile using the existing x86 m5ops archive pattern**

Compile `util/m5/src/abi/x86/m5op.S` to an object, archive it, and link a static
x86 test binary with the gem5 public include directory.

- [ ] **Step 4: Add a lightweight SE configuration**

Create a one-core x86 AtomicSimpleCPU system with classic memory. Enable
work-item exits, map both test VMAs to one physical page in the WORKBEGIN event
handler with `process.map()`, then continue simulation.

- [ ] **Step 5: Build the test binary**

Run:

```bash
make -C tests/test-progs/se/page-alias
```

Expected: the static `page_alias` binary is built successfully.

### Task 2: Prove the current zeroing order is broken

**Files:**
- Test: `tests/test-progs/se/page-alias/page_alias`
- Test: `tests/test-progs/se/page-alias/run.py`

- [ ] **Step 1: Run the real SE regression before changing production code**

Run:

```bash
build/VEGA_X86/gem5.opt \
  -d /tmp/gem5-se-page-alias-red \
  tests/test-progs/se/page-alias/run.py
```

Expected: FAIL with a mismatch after `munmap`, proving that the surviving alias
was cleared by the current `Process::deallocateMem()` ordering.

### Task 3: Defer zeroing and deallocation until the final alias

**Files:**
- Modify: `src/sim/process.cc`

- [ ] **Step 1: Unmap before checking remaining mappings**

Keep the saved physical address, remove the virtual mapping, and scan
`pTable->getMappings()` for the same physical page.

- [ ] **Step 2: Skip all physical-page destruction when an alias remains**

When another mapping references the saved physical page, continue to the next
page without clearing or deallocating it.

- [ ] **Step 3: Clear the final page through the physical proxy**

When no alias remains and `zeroPages` is enabled, call
`system->physProxy.writeBlob(page_paddr, zero_page.data(), page_size)` before
`seWorkload->deallocPhysPage(page_paddr)`.

### Task 4: Verify the fix and regressions

**Files:**
- Modify: `docs/debug/se-multigpu-status.md`

- [ ] **Step 1: Rebuild gem5**

The user owns the C++ build. If the current binary does not include the
`process.cc` change, report the required rebuild before the GREEN runtime run.

- [ ] **Step 2: Run the SE regression**

Run:

```bash
build/VEGA_X86/gem5.opt \
  -d /tmp/gem5-se-page-alias-green \
  tests/test-progs/se/page-alias/run.py
```

Expected: `SE page alias preservation passed` and a normal zero-status exit.

- [ ] **Step 3: Run lightweight regressions**

Run:

```bash
build/VEGA_X86/gem5.opt -p tests/pyunit \
  -m unittest test_gpu_viper_wb_l2_transitions
build/VEGA_X86/gem5.opt -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
git diff --check
```

Expected: all tests and whitespace checks pass.

- [ ] **Step 4: Record exact evidence**

Update `docs/debug/se-multigpu-status.md` with the commands, RED failure, facts,
changed files, completed verification, and the single next step.
