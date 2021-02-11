#ifndef UNDO_H
#define UNDO_H

#include <obsidian/v_memory.h>
#include "layer.h"
#include "render.h"

void u_InitUndo(const uint32_t size);
void u_CleanUp(void);
bool u_LayerInCache(L_LayerId layer);
void u_BindScene(const Scene* scene_);
void u_Update(void);

Obdn_V_BufferRegion* u_GetNextBuffer(void);
Obdn_V_BufferRegion* u_GetLastBuffer(void);

#endif /* end of include guard: UNDO_H */
