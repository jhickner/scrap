# Codebase cleanup

## Correctness and state ownership

- [x] Make pending file-diff snapshots session-owned and test interleaved sessions.
- [x] Make session-view collapse and incremental cluster state viewport/tab-owned.
- [x] Make Telegram start/stop and partial-start failure cleanup restart-safe.
- [x] Consolidate multi-step board updates into atomic store mutations.

## Dead code and repeated functionality

- [x] Remove the unreachable session silent mode and its dead branches.
- [x] Remove the unused terminal cursor-position/CPR subsystem.
- [x] Remove unused struct fields and exported APIs found by the audit.
- [x] Share terminal/editor handoff behavior between prompt and file editing.
- [x] Share title/parent append-only key-value log mechanics.
- [x] Share board/reminder sidecar locking without creating a generic utility dump.
- [x] Reuse the existing config-directory path creation for paste storage.

## Module and header boundaries

- [x] Remove avoidable transitive includes and add direct includes at use sites.
- [x] Separate session presentation from backend/session lifecycle bookkeeping.
- [x] Split Telegram transport/queue, artifact serving, and application bridge code after state ownership is explicit.

## Build and verification

- [x] Regenerate compiled board defaults when an input is added or deleted.
- [x] Make `clean` remove artifacts left by deleted source modules.
- [x] Honor `LDFLAGS` for every tool and test link.
- [x] Deduplicate tool link recipes while retaining explicit isolated dependencies.
- [x] Make the unattended/manual harness partition explicit.
- [x] Avoid repeated JPEG-prefix shell probes.
- [x] Document build dependencies, checks, optional JPEG support, and manual harnesses.
- [x] Add CI for the app build, unattended checks, and all harness compilation.
- [x] Add focused headless coverage for corrected session and board orchestration behavior.
- [x] Run clean app build, all unattended checks, all harness builds, sanitizer checks, and standalone-header checks.

## Deliberately not doing

- Contextualizing the single process terminal/UI/status/chrome/frontend state: these model one terminal and one modal stack.
- Refactoring vendored amalgamations or compatibility fallbacks owned by external tools.
- Combining the pick and board-grid modal loops: their superficially similar loops have different state and layout semantics.
- Extracting tiny one-line helpers without a concrete dependency or consistency benefit.
