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

            if (audit_worth_it)
                return BOARD_AUDIT;
            break;
        case BOARD_STEP_MERGE:
            return BOARD_MERGING;
        default:
            break;
        }
    }
    return BOARD_DONE;
}
