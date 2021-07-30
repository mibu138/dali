#ifndef UNDO_H
#define UNDO_H

#include <obsidian/memory.h>
#include "layer.h"

typedef uint32_t Dali_DirtMask;
typedef struct Dali_UndoManager Dali_UndoManager;

void dali_CreateUndoManager(Obdn_Memory* memory, const uint32_t size, const uint8_t maxStacks_, const uint8_t maxUndos_, Dali_UndoManager* undo);

void dali_DestroyUndoManager(Dali_UndoManager* undo);

Obdn_V_BufferRegion* dali_GetNextUndoBuffer(Dali_UndoManager* undo);

Obdn_V_BufferRegion* dali_GetLastUndoBuffer(Dali_UndoManager* undo);

bool dali_LayerInUndoCache(Dali_UndoManager* undo, Dali_LayerId layer);

Dali_UndoManager* dali_AllocUndo(void);
void dali_UpdateUndo(Dali_UndoManager* undo, Dali_LayerStack* layerStack);

#endif /* end of include guard: UNDO_H */
