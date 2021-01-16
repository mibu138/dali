#include "undo.h"
#include "painter/layer.h"
#include "layer.h"
#include "render.h"
#include <stdio.h>

#define MAX_UNDOS 8
#define MAX_STACKS 4

_Static_assert(MAX_UNDOS % 2 == 0, "MAX_UNDOS must be a multiple of 2 for bottom wrap around to work");

typedef Tanto_V_BufferRegion BufferRegion;

typedef struct UndoStack {
    uint8_t              trl; // cur cannot cross this
    uint8_t              cur;
    Tanto_V_BufferRegion bufferRegions[MAX_UNDOS];
} UndoStack;

uint8_t   curStackIndex;
uint8_t   stackNotUsedCounters[MAX_STACKS];
L_LayerId layerCache[MAX_STACKS];
UndoStack undoStacks[MAX_STACKS];

static void initStack(const uint8_t index, const uint32_t size)
{
    undoStacks[index].cur = 0;
    undoStacks[index].trl = 0;
    for (int i = 0; i < MAX_UNDOS; i++) 
    {
        undoStacks[index].bufferRegions[i] = tanto_v_RequestBufferRegion(size, 
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, TANTO_V_MEMORY_HOST_TRANSFER_TYPE);
    }
}

static void onLayerChange(L_LayerId newLayerId)
{
    uint8_t highestLRUCounter = 0;
    uint8_t leastRecentlyUsedStack = 0;
    bool    layerInCache = false;
    for (int i = 0; i < MAX_STACKS; i++) 
    {
        if (layerCache[i] == newLayerId)
        {
            curStackIndex = i;
            layerInCache = true;
        }
        else
        {
            stackNotUsedCounters[i]++;
            if (stackNotUsedCounters[i] > highestLRUCounter)
            {
                highestLRUCounter = stackNotUsedCounters[i];
                leastRecentlyUsedStack = i;
            }
        }
    }
    if (!layerInCache)
    {
        curStackIndex = leastRecentlyUsedStack;
        layerCache[curStackIndex] = newLayerId; 
        undoStacks[curStackIndex].cur = undoStacks[curStackIndex].trl;
        r_BackUpLayer();
    }
    stackNotUsedCounters[curStackIndex] = 0;
    assert(curStackIndex < MAX_STACKS);
}

void u_InitUndo(const uint32_t size)
{
    curStackIndex = 0;
    for (int i = 0; i < MAX_STACKS; i++) 
    {
        layerCache[i] = 0;
        initStack(i, size);
    }

    l_RegisterLayerChangeFn(onLayerChange);
    onLayerChange(0);
}

Tanto_V_BufferRegion* u_GetNextBuffer(void)
{
    UndoStack* undoStack = &undoStacks[curStackIndex];
    const uint8_t stackIndex = undoStack->cur;
    undoStack->cur = (undoStack->cur + 1) % MAX_UNDOS;
    if (undoStack->cur == undoStack->trl)
    {
        undoStack->trl++;
        undoStack->trl = undoStack->trl % MAX_UNDOS;
    }
    printf("%s: cur: %d\n", __PRETTY_FUNCTION__, undoStack->cur);
    return &undoStack->bufferRegions[stackIndex];
}

Tanto_V_BufferRegion* u_GetLastBuffer(void)
{
    UndoStack* undoStack = &undoStacks[curStackIndex];
    uint8_t stackIndex = undoStack->cur - 1;
    if (stackIndex % MAX_UNDOS == undoStack->trl)
    {
        printf("Nothing to undo!\n");
        return NULL; // cannot cross trl
    }
    stackIndex--;
    stackIndex = stackIndex % MAX_UNDOS;
    printf("undoStack->cur - 1 = %d\n", undoStack->cur - 1);
    undoStack->cur--;
    undoStack->cur = undoStack->cur % MAX_UNDOS;
    printf("%s: cur: %d\n", __PRETTY_FUNCTION__, undoStack->cur);
    return &undoStack->bufferRegions[stackIndex];
}
