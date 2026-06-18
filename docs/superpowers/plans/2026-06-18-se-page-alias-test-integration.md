# SE Page Alias Test Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Register the real SE page-alias regression in gem5's formal test framework and exclude its generated artifacts.

**Architecture:** Reuse the existing test program and run configuration. A gem5 test definition uses `MakeFixture`/`TestProgram` patterns to build the binary and a regex verifier to require the success marker.

**Tech Stack:** gem5 testlib, Python, Make, static x86 C.

---

### Task 1: Add the formal test definition

**Files:**
- Create: `tests/gem5/se_mode/page_alias/test_page_alias.py`

- [ ] Inspect `MakeFixture` and `TestProgram` examples.
- [ ] Register an x86 optimized test using the existing `run.py`.
- [ ] Require `SE page alias preservation passed` with `MatchRegex`.
- [ ] Run test discovery/execution and confirm the test passes.

### Task 2: Exclude generated artifacts

**Files:**
- Create: `tests/test-progs/se/page-alias/.gitignore`

- [ ] Ignore `page_alias`, `m5op_x86.o`, and `libm5op_x86.a`.
- [ ] Verify only source, configuration, Makefile, and `.gitignore` remain as
  untracked files in this directory.

### Task 3: Verify and document

**Files:**
- Modify: `docs/debug/se-multigpu-status.md`

- [ ] Run the formal page-alias test.
- [ ] Run both lightweight Python suites.
- [ ] Run Python syntax and `git diff --check`.
- [ ] Record commands, results, facts, changed files, and next review step.
