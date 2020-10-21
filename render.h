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
    uint32_t uvwOffset;
} RtPushConstants;

typedef struct {
    Mat4 model;
    Mat4 view;
    Mat4 proj;
    Mat4 viewInv;
    Mat4 projInv;
} UboMatrices;

typedef struct {
    float posX;
    float posY;
    float posZ;
    float targetX;
    float targetY;
    float targetZ;
} UboPlayer;

typedef struct {
    float x;
    float y;
    float z;
} Selection;

void  r_InitRenderer(void);
void  r_UpdateRenderCommands(void);
Mat4* r_GetXform(r_XformType);
Brush* r_GetBrush(void);
UboPlayer* r_GetPlayer(void);
void  r_LoadMesh(Tanto_R_Mesh mesh);
void  r_ClearMesh(void);
void  r_SavePaintImage(void);
void  r_CleanUp(void);
const Tanto_R_Mesh* r_GetMesh(void);

#endif /* end of include guard: R_COMMANDS_H */
