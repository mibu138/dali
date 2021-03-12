#ifndef G_GAME_H
#define G_GAME_H

#include "common.h"
#include <coal/coal.h>
#include <stdint.h>
#include "render.h"

struct Obdn_I_Event;

void g_Init(Obdn_S_Scene* scene_, PaintScene* paintScene_);
void g_CleanUp(void);
bool g_Responder(const struct Obdn_I_Event *event);
void g_Update(void);
void g_SetCameraXform(const Mat4* xform);
void g_SetCameraView(const Mat4* view);
void g_SetCameraProj(const Mat4* proj);
void g_SetWindow(uint16_t width, uint16_t height);
void g_SetBrushPos(float x, float y);
void g_SetPaintMode(PaintMode mode);
void g_SetBrushColor(const float r, const float g, const float b);
void g_SetBrushRadius(const float r);
void g_SetBrushOpacity(float opacity);
void g_SetBrushFallOff(float falloff);

#endif /* end of include guard: G_GAME_H */
