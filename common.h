#ifndef VIEWER_COMMON_H
#define VIEWER_COMMON_H

#include <stdbool.h>

typedef enum {
    MODE_RASTER,
    MODE_RAY,
} ModeID;

typedef struct {
    ModeID mode;
    bool   renderNeedsUpdate;
    bool   shouldRun;
    bool   reload;
    bool   restart;
    bool   copySwapToHost;
} Parms;

extern Parms parms; 

#endif /* end of include guard: VIEWER_COMMON_H */

