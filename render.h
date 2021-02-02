#ifndef VIEWER_R_COMMANDS_H
#define VIEWER_R_COMMANDS_H

#include <obsidian/r_geo.h>
#include "common.h"

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

typedef Obdn_Mask Scene_DirtMask;

typedef enum {
    SCENE_VIEW_BIT = 0x00000001,
    SCENE_PROJ_BIT = 0x00000002,
} Scene_DirtyBits;

typedef struct {
    Mat4           view;
    Mat4           proj;
    Scene_DirtMask dirt;
} Scene;

void         r_InitRenderer(void);
void         r_Render(void);
int          r_GetSelectionPos(Vec3* v);
Brush*       r_GetBrush(void);
UboPlayer*   r_GetPlayer(void);
VkDeviceSize r_GetTextureSize(void);
void         r_LoadPrim(Obdn_R_Primitive prim);
void         r_ClearPrim(void);
void         r_ClearPaintImage(void);
void         r_SetPaintMode(const PaintMode mode);
void         r_SavePaintImage(void);
void         r_Undo(void);
void         r_BackUpLayer(void);
void         r_CleanUp(void);
void         r_BindScene(const Scene* scene);

void* r_AcquireSwapBuffer(uint32_t* width, uint32_t* height, uint32_t* elementSize);
void  r_ReleaseSwapBuffer(void);

#endif /* end of include guard: R_COMMANDS_H */
