# Repository Agent Guidance

## SE Multi-GPU Debugging

Before working on the SE-mode multi-GPU platform, read:

- `docs/debug/se-multigpu-status.md`
- the current `git status` and relevant `git diff`

Treat the status document as the handoff record for long-running debugging.
Do not repeat experiments listed as completed or revisit rejected hypotheses
without new evidence.

After each meaningful debugging step, update the status document with:

- the exact command that was run;
- the first relevant error or observable result;
- facts confirmed by the run;
- hypotheses accepted or rejected;
- files changed;
- verification completed;
- the single next diagnostic step.

Keep large logs in files and record their paths. Do not paste complete logs
into the status document. Preserve the user's uncommitted changes and do not
commit, discard, or rewrite them unless explicitly requested.

The user owns gem5 compilation and full simulation runs unless they explicitly
delegate them. Codex may perform read-only inspection and lightweight tests,
but must not start a long gem5 build or simulation while the user is running
one.
