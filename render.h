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
    SCENE_VIEW_BIT  = (Scene_DirtMask)1 << 0,
    SCENE_PROJ_BIT  = (Scene_DirtMask)1 << 1,
    SCENE_BRUSH_BIT = (Scene_DirtMask)1 << 2
} Scene_DirtyBits;

typedef struct {
    Mat4           view;
    Mat4           proj;
    float          brush_x;
    float          brush_y;
    float          brush_radius;
    float          brush_r;
    float          brush_g;
    float          brush_b;
    int            brush_mode;
    Scene_DirtMask dirt;
} Scene;

void         r_InitRenderer(uint32_t texSize);
void         r_Render(void);
int          r_GetSelectionPos(Vec3* v);
Brush*       r_GetBrush(void);
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

void r_GetColorDepthExternal(uint32_t* width, uint32_t* height, uint32_t* elementSize, 
        uint64_t* colorOffset, uint64_t* depthOffset);
void r_GetSwapBufferData(uint32_t* width, uint32_t* height, uint32_t* elementSize, 
        void** colorData, void** depthData);
bool r_GetExtMemoryFd(int* fd, uint64_t* size);
bool r_GetSemaphoreFds(int* obdnFrameDoneFD_0, int* obdnFrameDoneFD_1, int* extTextureReadFD);
void r_SetExtFastPath(bool isFast);

#endif /* end of include guard: R_COMMANDS_H */
