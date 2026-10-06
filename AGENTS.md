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
  `pvsay` on its PATH is `tools/rig/fake-pvsay`: it appends its argv and stdin
  to `/tmp/scraprig/<name>/pvsay.log` and sleeps 30s, so `/voice on` speaks
  nothing aloud.
- `--share dir` gives instances started with the same dir one set of live
  sessions, dispatch requests, and session names, so they can reach each
  other with `scrap send`.
- `--net` gives an instance a `scrap hub` broker on the tailscale address at
  a random port (or `--port N`); instances sharing a dir share one broker, so
  `machine:@name` targets reach them, not the installed scrap. Without
  `--net` no broker runs. A broker exits once its instance or share dir is
  removed. `scraprig machine` prints the
  tailnet name, `net name` the ip and port, and `cli name ls --net` (or
  `read`, `send`, `attach`) runs the scrap CLI with an instance's config.
  `say name text [regex]` types a line, sends Enter, and waits for regex.
  `broker name` prints the broker pid. `joblog name job regex` waits for a
  scheduled job's log to match.
- `stream name id target` runs `scrap attach target` in the instance's tmux
  server; `stream-put` feeds its stdin, `stream-wait` waits on its output and
  prints it on failure, `stop` ends it. `tools/rig/netpair.bash` sets up two
  named, networked instances for the `net*` tests.
- Golden tests start scrap with `-- --name rig` so the session name, and the
  tab box sized to it, is the same on every run.
- Without `--fake` the real CLI runs, costs tokens, and writes its transcript
  to `~/.claude`.
- `wait`, `idle`, and `expect` exit non-zero and print the screen on failure.
- Pause for `wait` or `idle` between `send` calls; keys sent back to back can
  reach scrap as one escape sequence.
- `make rigtest` runs `tools/rig/tests/*.sh`. `expect name golden --update`
  (or `tests/x.sh --update`) rewrites a golden in `tools/rig/golden/`.
- When a test needs something the rig does not do (a background stream, a
  new process to observe, a new kind of wait), add it to `scraprig` as a
  subcommand that waits and prints what it saw on failure. Do not hand-roll
  it in the test with background jobs, sleeps, and grep on temp files.
- Keep each rig test to one behavior. A long script that checks several
  features in sequence stops at the first failure and hides the rest.


## Rules
- don't write comments. any you do write will be stripped during `make`
- all features should support all backends, not just claude, etc. unless that
backend is not able to support the feature
- prefer rig tests over unit tests
