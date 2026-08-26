# Kanban cards shrink to 15 columns

Kanban lanes can't get narrow. GRID_LANE_MIN in src/boardgrid.h is 24, so the grid refuses to draw below 24 columns and lane_w never goes under 24 (src/boardgrid.c:278-294). Lower the floor to 15 so cards shrink further and more lanes fit on a narrow terminal.

text_width() is lane_w - 3 - GRID_INDENT with a floor of 4, so a 15-wide lane leaves 10 cells of text. Check that the tile borders, the title row, the status/pin footer (foot_joined, GRID_SPIN) and the lane-name header still render without overflow at that width, and that put_fit truncation is clean.

worktree /Users/jhickner/working/mux/.claude/worktrees/whqq

## Notes

- board: started
- implement: Done and committed on `worktree-whqq` (`2b54242`).

**What changed**
- `src/boardgrid.h` — `GRID_LANE_MIN` 24 → 15. Everything downstream is derived from `lane_w`, so no other code changed.
- `tools/boardgridtest.c` — updated the lane-width expectations, moved the "too narrow to lay out" case to `GRID_LANE_MIN + 1`, and added `test_the_narrowest_lane`, which sweeps every width from one lane to four and asserts tiles stay inside the screen and no lane goes under the floor.

**Render check at 15** (done with a throwaway harness that drove `paint_under` into the ui sink at fixed widths, output measured in cells — not committed): at 32 cols the grid gives 2 lanes of 15, tile width 14, 10 cells of text. Borders, the wrapped title, the `├──┤` divider, the spinner + status row, the pins row and the log preview all come out at exactly 30 cells, and `put_fit` truncation lands as `title lon…` with the right border intact. Same clean at 47 (3 lanes). Below 32 the tty floor (`TTY_MIN_COLUMNS` 20) takes over and you get one 18-wide lane. Lane names get `lane_w - 3` = 12 cells and never run into the next lane.

**To see it**: `make check` (boardgridtest covers the geometry), then run mux, press `^b` for the board, and narrow the terminal — lanes now keep splitting down to 15 columns instead of stopping at 24.

One consequence worth naming: `GRID_LANE_MIN` is also the divisor for how many lanes to show, so on a *wide* terminal with many lanes you now get more, narrower lanes rather than fewer wide ones (200 cols, 20 lanes: 13 lanes at 15 wide, was 8 at 24). With 9 or fewer lanes it's a straight improvement — 200 cols now shows all 9 at 22 wide. If you want the narrow floor without the wide-terminal cramming, that needs a separate "preferred width" for the divisor; the card didn't ask for it, so I left it.
- board: worker rejoined the session
