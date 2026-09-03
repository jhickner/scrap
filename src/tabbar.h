#ifndef TABBAR_H
#define TABBAR_H

/* The row under the input: one entry per session this window holds, marked
   with the same glyphs tmux-agent-tabs paints on a tmux tab. */

int  tabbar_rows(int cols);

/* the bar no longer matches the sessions behind it, so its host has to repaint
   -- a background tab finishing is otherwise invisible at an idle prompt */
int  tabbar_stale(void);

void tabbar_paint(int cols);

/* the session whose entry the last paint put under a column, or -1 */
int  tabbar_hit(int col);

#endif
