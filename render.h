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
    int   hit;
} Selection;

typedef enum {
    PAINT_MODE_OVER,
    PAINT_MODE_ERASE
} PaintMode;

void  r_InitRenderer(void);
void  r_UpdateRenderCommands(const int8_t frameIndex);
int   r_GetSelectionPos(Vec3* v);
Mat4* r_GetXform(r_XformType);
Brush* r_GetBrush(void);
UboPlayer* r_GetPlayer(void);
void  r_LoadMesh(Tanto_R_Mesh mesh);
void  r_ClearMesh(void);
void  r_ClearPaintImage(void);
void  r_SetPaintMode(const PaintMode mode);
void  r_SavePaintImage(void);
void  r_CleanUp(void);
const Tanto_R_Mesh* r_GetMesh(void);

#endif /* end of include guard: R_COMMANDS_H */
