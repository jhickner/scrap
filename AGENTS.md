# scrap

## Orientation

Run `make map` first, then read `map/MAP.md`: slash commands and other
dispatch tables, per-module dependencies, and public functions. Callers and
callees are in `map/calls.tsv`; edge kind `ref` marks a function used as a
value (table entry, callback). `cmap near FN [-d N] [--up|--down]` prints the
callers and callees within N hops of FN.

## Driving scrap

`tools/rig/scraprig` runs the built `./scrap` in a private tmux server, sends
keys, and snapshots the screen. Each instance keeps its config and state under
`/tmp/scraprig/<name>` via `scrap --state`, so user config, live sessions, and
the user's tmux are untouched. Run `tools/rig/scraprig` for usage.

```
n=$(tools/rig/scraprig start --fake)
tools/rig/scraprig wait $n '❯'
tools/rig/scraprig type $n 'hello'
tools/rig/scraprig send $n Enter
tools/rig/scraprig wait $n 'echo: hello'
tools/rig/scraprig snap $n
tools/rig/scraprig stop $n
```

- `--fake` puts `tools/rig/fake-claude` first on PATH as `claude`: it echoes
  each prompt back. `--fixture file` replays canned stream-json turns instead,
  one turn per block, blocks separated by `---`; see `tools/rig/fixtures/`.
  A prompt `run: CMD` makes the fake run CMD in a shell and reply with its
  output, as a Bash tool call would; `scrap` on its PATH is the built binary.
- `--share dir` gives instances started with the same dir one set of live
  sessions, dispatch requests, and session names, so they can reach each
  other with `scrap send`.
- Without `--fake` the real CLI runs, costs tokens, and writes its transcript
  to `~/.claude`.
- `wait`, `idle`, and `expect` exit non-zero and print the screen on failure.
- Pause for `wait` or `idle` between `send` calls; keys sent back to back can
  reach scrap as one escape sequence.
- `make rigtest` runs `tools/rig/tests/*.sh`. `expect name golden --update`
  (or `tests/x.sh --update`) rewrites a golden in `tools/rig/golden/`.


## Rules
- don't write comments. any you do write will be stripped during `make`
