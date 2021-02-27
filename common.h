#ifndef VIEWER_COMMON_H
#define VIEWER_COMMON_H

#include <stdbool.h>

typedef struct {
    bool   renderNeedsUpdate;
    bool   shouldRun;
    bool   reload;
    bool   restart;
    bool   copySwapToHost;
} Parms;

#endif /* end of include guard: VIEWER_COMMON_H */

