# mux

## Orientation

Run `make map` first, then read `map/MAP.md`: slash commands and other
dispatch tables, per-module dependencies, and public functions. Callers and
callees are in `map/calls.tsv`; edge kind `ref` marks a function used as a
value (table entry, callback). `cmap near FN [-d N] [--up|--down]` prints the
callers and callees within N hops of FN.


## Rules
- don't write comments. any you do write will be stripped during `make`
