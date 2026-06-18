# SE Page Alias Deallocation Design

## Goal

Add a real SE-mode regression which proves that unmapping one of two virtual
addresses backed by the same physical page does not clear or recycle the page
while the second mapping remains live.

## Test Architecture

The test program reserves two fixed anonymous VMAs without touching either
page, then executes `m5_work_begin(0, 0)`. The test configuration handles that
exit event and calls the existing `Process.map()` C++ method twice, mapping both
virtual addresses to one physical page near the end of the configured memory.

After simulation resumes, the program writes a fixed byte pattern through both
aliases, unmaps the first VMA, and verifies the pattern through the second VMA.
It prints an explicit pass marker and exits zero only when the surviving alias
retains its contents.

This avoids adding a test-only syscall, pseudo-instruction, or emulated driver.

## Production Change

`Process::deallocateMem()` must:

1. translate the virtual page to its physical page;
2. unmap the requested virtual page;
3. scan the remaining mappings for the same physical page;
4. return immediately for that page when another alias remains;
5. only for the final alias, clear the physical page and call
   `deallocPhysPage()`.

Because the unmapped virtual address can no longer be used to clear the final
page, zeroing must use a physical-memory proxy at the saved physical address.

## Verification

The RED run uses the current implementation and must report a data mismatch
after the first `munmap`.

The GREEN run must print:

```text
SE page alias preservation passed
```

and exit normally. The existing SE multi-GPU and GPU/VIPER lightweight Python
test suites must continue to pass, as must `git diff --check`.

## Scope

The test covers aliases within one SE process page table. Cross-process or GPU
page-table reference accounting remains outside this change.
