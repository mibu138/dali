#ifndef VIEWER_R_COMMANDS_H
#define VIEWER_R_COMMANDS_H

#include <tanto/r_geo.h>
#include "common.h"

typedef enum {
    R_XFORM_MODEL,
    R_XFORM_VIEW,
    R_XFORM_PROJ,
    R_XFORM_VIEW_INV,
    R_XFORM_PROJ_INV
} r_XformType;

typedef struct {
    Vec4 clearColor;
    Vec3 lightDir;
    float lightIntensity;
    int   lightType;
    uint32_t colorOffset;
    uint32_t normalOffset;
} RtPushConstants;

void  r_InitRenderCommands(void);
void  r_UpdateRenderCommands(void);
Mat4* r_GetXform(r_XformType);
Brush* r_GetBrush(void);
void  r_LoadMesh(const Tanto_R_Mesh*);
void  r_CommandCleanUp(void);

#endif /* end of include guard: R_COMMANDS_H */
