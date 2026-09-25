# mux

## Orientation

Run `make map` first, then read `map/MAP.md`: slash commands and other
dispatch tables, per-module dependencies, and public functions. Callers and
callees are in `map/calls.tsv`; edge kind `ref` marks a function used as a
value (table entry, callback). `cmap near FN [-d N] [--up|--down]` prints the
callers and callees within N hops of FN.

## Driving mux

`tools/rig/muxrig` runs the built `./mux` in a private tmux server, sends
keys, and snapshots the screen. Each instance keeps its config and state under
`/tmp/muxrig/<name>` via `mux --state`, so user config, live sessions, and
the user's tmux are untouched. Run `tools/rig/muxrig` for usage.

```
n=$(tools/rig/muxrig start --fake)
tools/rig/muxrig wait $n '❯'
tools/rig/muxrig type $n 'hello'
tools/rig/muxrig send $n Enter
tools/rig/muxrig wait $n 'echo: hello'
tools/rig/muxrig snap $n
tools/rig/muxrig stop $n
```

- `--fake` puts `tools/rig/fake-claude` first on PATH as `claude`: it echoes
  each prompt back. `--fixture file` replays canned stream-json turns instead,
  one turn per block, blocks separated by `---`; see `tools/rig/fixtures/`.
  A prompt `run: CMD` makes the fake run CMD in a shell and reply with its
  output, as a Bash tool call would; `mux` on its PATH is the built binary.
- `--share dir` gives instances started with the same dir one set of live
  sessions, dispatch requests, and session names, so they can reach each
  other with `mux send`.
- Without `--fake` the real CLI runs, costs tokens, and writes its transcript
  to `~/.claude`.
- `wait`, `idle`, and `expect` exit non-zero and print the screen on failure.
- Pause for `wait` or `idle` between `send` calls; keys sent back to back can
  reach mux as one escape sequence.
- `make rigtest` runs `tools/rig/tests/*.sh`. `expect name golden --update`
  (or `tests/x.sh --update`) rewrites a golden in `tools/rig/golden/`.


## Rules
- don't write comments. any you do write will be stripped during `make`
