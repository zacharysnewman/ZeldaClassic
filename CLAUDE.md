# Claude Code Instructions — Zelda Classic

## macOS Porting

This repository is actively being ported from Windows to macOS.
Two documents track this effort:

### PORTING.md

`PORTING.md` is the **canonical porting plan**. It must be kept current.

**Update PORTING.md when:**
- A new technical challenge or risk is discovered that affects the plan.
- A phase's approach changes (e.g., a chosen library is replaced).
- An open question in the "Known Risks" table is resolved.
- New affected files are identified.
- A phase is found to be more or less complex than originally estimated.

Do not leave PORTING.md stale. If you discover something that contradicts the plan,
update the plan before or alongside the code change.

### PROGRESS.md

`PROGRESS.md` tracks **what has actually been done** during the port.

**Update PROGRESS.md when:**
- A phase or sub-task is started or completed.
- A file is modified as part of the port (log which phase it belongs to).
- A blocker is hit and work on a task is paused.
- A known risk from PORTING.md is confirmed, partially resolved, or closed.
- A decision is made that deviates from the plan (explain why).

Keep PROGRESS.md as a running log. Append new entries; do not rewrite history.
