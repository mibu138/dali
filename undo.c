#include "undo.h"
#include "painter/layer.h"
#include "layer.h"
#include "render.h"
#include <stdio.h>
#include <string.h>

#define MAX_UNDOS 8
#define MAX_STACKS 4

_Static_assert(MAX_UNDOS % 2 == 0, "MAX_UNDOS must be a multiple of 2 for bottom wrap around to work");

typedef Obdn_V_BufferRegion BufferRegion;

typedef struct UndoStack {
    uint8_t              trl; // cur cannot cross this
    uint8_t              cur;
    Obdn_V_BufferRegion bufferRegions[MAX_UNDOS];
} UndoStack;

static const PaintScene* scene;

static uint8_t maxStacks;
static uint8_t maxUndos;

static uint8_t   curStackIndex;
static uint8_t   stackNotUsedCounters[MAX_STACKS];
static L_LayerId layerCache[MAX_STACKS];
static UndoStack undoStacks[MAX_STACKS];

static void initStack(const uint8_t index, const uint32_t size)
{
    undoStacks[index].cur = 0;
    undoStacks[index].trl = 0;
    for (int i = 0; i < maxUndos; i++) 
    {
        undoStacks[index].bufferRegions[i] = obdn_v_RequestBufferRegion(size, 
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, OBDN_V_MEMORY_HOST_TRANSFER_TYPE);
    }
}

static void onLayerChange(L_LayerId newLayerId)
{
    uint8_t highestLRUCounter = 0;
    uint8_t leastRecentlyUsedStack = 0;
    bool    layerInCache = false;
    for (int i = 0; i < maxStacks; i++) 
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
    }
    stackNotUsedCounters[curStackIndex] = 0;
    assert(curStackIndex < maxStacks);
}

void u_InitUndo(const uint32_t size, const uint8_t maxStacks_, const uint8_t maxUndos_)
{
    assert(maxStacks_ > 0 && maxStacks_ <= MAX_STACKS);
    assert(maxUndos_ > 0 && maxUndos_  <= MAX_UNDOS);
    assert(maxUndos_ % 2 == 0);
    maxStacks = maxStacks_;
    maxUndos = maxUndos_;
    curStackIndex = 0;
    for (int i = 0; i < maxStacks; i++) 
    {
        layerCache[i] = 0;
        initStack(i, size);
    }

    onLayerChange(0);
}

void u_CleanUp(void)
{
    for (int i = 0; i < maxStacks; i++)
    {
        for (int j = 0; j < maxUndos; j++)
        {
            obdn_v_FreeBufferRegion(&undoStacks[i].bufferRegions[j]);
        }
    }
    curStackIndex = 0;
    memset(stackNotUsedCounters, 0, sizeof(stackNotUsedCounters));
    memset(layerCache, 0, sizeof(layerCache));
    memset(undoStacks, 0, sizeof(undoStacks));
}

Obdn_V_BufferRegion* u_GetNextBuffer(void)
{
    UndoStack* undoStack = &undoStacks[curStackIndex];
    const uint8_t stackIndex = undoStack->cur;
    undoStack->cur = (undoStack->cur + 1) % maxUndos;
    if (undoStack->cur == undoStack->trl)
    {
        undoStack->trl++;
        undoStack->trl = undoStack->trl % maxUndos;
    }
    printf("%s: cur: %d\n", __PRETTY_FUNCTION__, undoStack->cur);
    return &undoStack->bufferRegions[stackIndex];
}

Obdn_V_BufferRegion* u_GetLastBuffer(void)
{
    UndoStack* undoStack = &undoStacks[curStackIndex];
    uint8_t stackIndex = undoStack->cur - 1;
    if (stackIndex % maxUndos == undoStack->trl)
    {
        printf("Nothing to undo!\n");
        return NULL; // cannot cross trl
    }
    stackIndex--;
    stackIndex = stackIndex % maxUndos;
    printf("undoStack->cur - 1 = %d\n", undoStack->cur - 1);
    undoStack->cur--;
    undoStack->cur = undoStack->cur % maxUndos;
    printf("%s: cur: %d\n", __PRETTY_FUNCTION__, undoStack->cur);
    return &undoStack->bufferRegions[stackIndex];
}

bool u_LayerInCache(L_LayerId layer)
{
    for (int i = 0; i < maxStacks; i++)
    {
        if (layerCache[i] == layer)
            return true;
    }
    return false;
}

void u_BindScene(const PaintScene* scene_)
{
    scene = scene_;
}

void u_Update(void)
{
    assert(scene);
    if (scene->dirt & SCENE_LAYER_CHANGED_BIT)
        onLayerChange(scene->layer);
}
