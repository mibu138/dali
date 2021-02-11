#ifndef G_GAME_H
#define G_GAME_H

#include "common.h"
#include <coal/coal.h>
#include <stdint.h>
#include "render.h"

typedef struct {
    bool shouldRun;
} G_GameState;

struct Obdn_I_Event;

extern G_GameState gameState;

void g_Init(void);
void g_CleanUp(void);
bool g_Responder(const struct Obdn_I_Event *event);
void g_Update(void);
void g_SetView(const Mat4* m);
void g_SetProj(const Mat4* m);
void g_SetWindow(uint32_t width, uint32_t height);
void g_SetBrushPos(float x, float y);
void g_SetPaintMode(PaintMode mode);
void g_SetBrushColor(const float r, const float g, const float b);
void g_SetBrushRadius(const float r);

#endif /* end of include guard: G_GAME_H */
