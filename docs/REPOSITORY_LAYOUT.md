# Repository and workspace layout

This GitHub repository contains first-party source and documentation. It is
separate from the larger local integration workspace.

- `native/`: active C++ services, contract tests, optional guest diagnostics and pinned codec.
- `tools/`: capture-file readers and optional bank-reference helpers.
- `Fable2Native/`: retired reconstruction retained as reusable source; not the active CI target.
- `docs/`: current status and historical subsystem research.
- `lua-docs/`: scripting reference documentation.
- `.github/workflows/`: native-service verification and documentation publishing.

Local checkouts named `Fable2RecompFinishable`, `rexglue-native-next`, and the
isolated TU1 project supply the playable baseline, runtime/renderer and integration
candidate respectively. Original `Fable2Recomp` data and reference code remain
local dependencies. They are not silently bundled with this source repository.

Build outputs, original XEX/banks, cooked media and research captures stay outside
source control. The September 13 workspace cleanup retired obsolete game binaries
and compiler products, compressed old logs/bitmaps after verification, and retained
useful source, data and inspection tools. It did not discard unmerged source work.

Existing unmerged branches and their worktrees retain independent history. Only
branches proven merged into the remote default branch are eligible for routine
retirement; preserve the open PR and unique commits before broader consolidation.
