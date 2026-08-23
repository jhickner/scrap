#include "boardflow.h"

enum board_col boardflow_from(const char *kind, enum board_step from,
                              int audit_worth_it)
{
    for (int i = from; i < BOARD_STEPS; i++) {
        if (!boardcfg_kind_takes(kind, (enum board_step)i))
            continue;
        switch ((enum board_step)i) {
        case BOARD_STEP_REVIEW:
            return BOARD_REVIEW;
        case BOARD_STEP_AUDIT:
            // Configured for it, but only worth it on a change big enough to
            // read. A small one goes straight on.
            if (audit_worth_it)
                return BOARD_AUDIT;
            break;
        case BOARD_STEP_MERGE:
            return BOARD_MERGING;
        default:
            break;      /* the worktree is not a column */
        }
    }
    return BOARD_DONE;
}
