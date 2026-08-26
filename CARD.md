# Kanban cards shrink to 15 columns

Kanban lanes can't get narrow. GRID_LANE_MIN in src/boardgrid.h is 24, so the grid refuses to draw below 24 columns and lane_w never goes under 24 (src/boardgrid.c:278-294). Lower the floor to 15 so cards shrink further and more lanes fit on a narrow terminal.

text_width() is lane_w - 3 - GRID_INDENT with a floor of 4, so a 15-wide lane leaves 10 cells of text. Check that the tile borders, the title row, the status/pin footer (foot_joined, GRID_SPIN) and the lane-name header still render without overflow at that width, and that put_fit truncation is clean.

worktree /Users/jhickner/working/mux/.claude/worktrees/whqq
