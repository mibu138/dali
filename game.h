#ifndef G_GAME_H
#define G_GAME_H

#include "common.h"

typedef struct {
    bool shouldRun;
} G_GameState;

struct Tanto_I_Event;

extern G_GameState gameState;

void g_Init(void);
void g_Responder(const struct Tanto_I_Event *event);
void g_Update(void);
void g_SetColor(const float r, const float g, const float b);
void g_SetRadius(const float r);

#endif /* end of include guard: G_GAME_H */
