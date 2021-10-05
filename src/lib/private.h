#ifndef PRIVATE_H
#define PRIVATE_H

#include <obsidian/def.h>
#include <obsidian/video.h>
#include "obsidian/memory.h"
#include "brush.h"
#define MAX_LAYERS 64

typedef uint32_t DirtMask;

#define PAINT_MODE_OVER  DALI_PAINT_MODE_OVER
#define PAINT_MODE_ERASE DALI_PAINT_MODE_ERASE

typedef enum {
    BRUSH_GENERAL_BIT    = (DirtMask)1 << 1,
    BRUSH_PAINT_MODE_BIT = (DirtMask)1 << 2,
    BRUSH_ALPHA_BIT      = (DirtMask)1 << 3
} BrushDirtyBits;

typedef enum {
    LAYER_BACKUP_BIT  = (DirtMask)1 << 4,
    LAYER_CHANGED_BIT = (DirtMask)1 << 5,
} LayerStackDirtyBits;

typedef enum {
    UNDO_BIT          = (DirtMask)1 << 3,
} UndoDirtyBits;

typedef struct Dali_Layer {
    Obdn_BufferRegion bufferRegion;
} Dali_Layer;

typedef struct Dali_LayerStack{
    uint16_t     layerCount;
    uint16_t     activeLayer;
    VkDeviceSize layerSize;
    Dali_Layer    layers[MAX_LAYERS];
    Obdn_BufferRegion backBuffer;
    Obdn_BufferRegion frontBuffer;
    Obdn_Memory*        memory;
    DirtMask       dirt;
} Dali_LayerStack;

typedef Dali_PaintMode PaintMode;

typedef struct Dali_Brush {
    float         x;
    float         y;
    float         radius;
    float         r;
    float         g;
    float         b;
    bool          active;
    float         opacity;
    float         falloff;
    PaintMode     mode;
    Obdn_Image*   alphaImg; //non-owning
    DirtMask      dirt;
} Dali_Brush;

#define MAX_UNDOS 32
#define MAX_STACKS 4

typedef uint16_t Dali_LayerId;

typedef Obdn_BufferRegion BufferRegion;
typedef Dali_LayerId L_LayerId;

#ifndef WIN32
_Static_assert(MAX_UNDOS % 2 == 0, "MAX_UNDOS must be a multiple of 2 for bottom wrap around to work");
#endif

typedef struct UndoStack {
    uint8_t              trl; // cur cannot cross this
    uint8_t              cur;
    Obdn_BufferRegion  bufferRegions[MAX_UNDOS];
} UndoStack;

typedef struct Dali_UndoManager { 
    uint8_t   maxStacks;
    uint8_t   maxUndos;
    
    uint8_t   curStackIndex;
    uint8_t   stackNotUsedCounters[MAX_STACKS];
    L_LayerId layerCache[MAX_STACKS];
    UndoStack undoStacks[MAX_STACKS];
    DirtMask  dirt;
} Dali_UndoManager;

#endif /* end of include guard: PRIVATE_H */
