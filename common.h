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
} Parms;

extern Parms parms; 

typedef struct {
    float x;
    float y;
    float radius;
    float r;
    float g;
    float b;
    int   mode;
} Brush;

#endif /* end of include guard: VIEWER_COMMON_H */

