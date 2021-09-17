#include "undo.h"
#include "layer.h"
#include "engine.h"
#include "private.h"
#include "dtags.h"
#include <hell/debug.h>
#include <hell/common.h>
#include <string.h>



typedef Dali_UndoManager UndoManager;

static void createStack(Obdn_Memory* memory, UndoManager* undo, const uint8_t index, const uint32_t size)
{
    undo->undoStacks[index].cur = 0;
    undo->undoStacks[index].trl = 0;
    for (int i = 0; i < undo->maxUndos; i++) 
    {
        undo->undoStacks[index].bufferRegions[i] = obdn_RequestBufferRegion(memory, size, 
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, OBDN_MEMORY_HOST_TRANSFER_TYPE);
    }
}

static void onLayerChange(UndoManager* undo, L_LayerId newLayerId)
{
    uint8_t highestLRUCounter = 0;
    uint8_t leastRecentlyUsedStack = 0;
    bool    layerInCache = false;
    for (int i = 0; i < undo->maxStacks; i++) 
    {
        if (undo->layerCache[i] == newLayerId)
        {
            undo->curStackIndex = i;
            layerInCache = true;
        }
        else
        {
            undo->stackNotUsedCounters[i]++;
            if (undo->stackNotUsedCounters[i] > highestLRUCounter)
            {
                highestLRUCounter = undo->stackNotUsedCounters[i];
                leastRecentlyUsedStack = i;
            }
        }
    }
    if (!layerInCache)
    {
        undo->curStackIndex = leastRecentlyUsedStack;
        undo->layerCache[undo->curStackIndex] = newLayerId; 
        undo->undoStacks[undo->curStackIndex].cur = undo->undoStacks[undo->curStackIndex].trl;
    }
    undo->stackNotUsedCounters[undo->curStackIndex] = 0;
    assert(undo->curStackIndex < undo->maxStacks);
}

void dali_CreateUndoManager(Obdn_Memory* memory, const uint32_t size, const uint8_t maxStacks_, const uint8_t maxUndos_, UndoManager* undo)
{
    assert(memory);
    assert(undo);
    assert(maxStacks_ > 0 && maxStacks_ <= MAX_STACKS);
    assert(maxUndos_ > 0 && maxUndos_  <= MAX_UNDOS);
    assert(maxUndos_ % 2 == 0);
    memset(undo, 0, sizeof(UndoManager));
    undo->maxStacks = maxStacks_;
    undo->maxUndos = maxUndos_;
    undo->curStackIndex = 0;
    for (int i = 0; i < undo->maxStacks; i++) 
    {
        undo->layerCache[i] = 0;
        createStack(memory, undo, i, size);
    }

    onLayerChange(undo, 0);
}

void dali_DestroyUndoManager(UndoManager* undo)
{
    for (int i = 0; i < undo->maxStacks; i++)
    {
        for (int j = 0; j < undo->maxUndos; j++)
        {
            obdn_FreeBufferRegion(&undo->undoStacks[i].bufferRegions[j]);
        }
    }
    memset(undo, 0, sizeof(UndoManager));
}

Obdn_BufferRegion* dali_GetNextUndoBuffer(UndoManager* undo)
{
    UndoStack* undoStack = &undo->undoStacks[undo->curStackIndex];
    const uint8_t stackIndex = undoStack->cur;
    undoStack->cur = (undoStack->cur + 1) % undo->maxUndos;
    if (undoStack->cur == undoStack->trl)
    {
        undoStack->trl++;
        undoStack->trl = undoStack->trl % undo->maxUndos;
    }
    hell_DebugPrint(PAINT_DEBUG_TAG_UNDO, "cur: %d\n", undoStack->cur);
    return &undoStack->bufferRegions[stackIndex];
}

Obdn_BufferRegion* dali_GetLastUndoBuffer(UndoManager* undo)
{
    UndoStack* undoStack = &undo->undoStacks[undo->curStackIndex];
    uint8_t stackIndex = undoStack->cur - 1;
    if (stackIndex % undo->maxUndos == undoStack->trl)
    {
        hell_Print("Nothing to undo!\n");
        return NULL; // cannot cross trl
    }
    stackIndex--;
    stackIndex = stackIndex % undo->maxUndos;
    hell_DebugPrint(PAINT_DEBUG_TAG_UNDO, "undoStack->cur - 1 = %d\n", undoStack->cur - 1);
    undoStack->cur--;
    undoStack->cur = undoStack->cur % undo->maxUndos;
    hell_DebugPrint(PAINT_DEBUG_TAG_UNDO, "cur: %d\n", undoStack->cur);
    return &undoStack->bufferRegions[stackIndex];
}

bool dali_LayerInUndoCache(UndoManager* undo, L_LayerId layer)
{
    for (int i = 0; i < undo->maxStacks; i++)
    {
        if (undo->layerCache[i] == layer)
            return true;
    }
    return false;
}

void dali_UpdateUndo(UndoManager* undo, Dali_LayerStack* layerStack)
{
    if (layerStack->dirt & LAYER_CHANGED_BIT)
    {
        if (!dali_LayerInUndoCache(undo, layerStack->activeLayer))
            layerStack->dirt |= LAYER_BACKUP_BIT;
        onLayerChange(undo, layerStack->activeLayer);
    }
}

Dali_UndoManager* dali_AllocUndo(void)
{
    return hell_Malloc(sizeof(Dali_UndoManager));
}

void dali_Undo(UndoManager* undo)
{
    undo->dirt |= UNDO_BIT;
}

void dali_UndoClearDirt(UndoManager* undo)
{
    undo->dirt = 0;
}
