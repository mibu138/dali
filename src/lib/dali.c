#include "dali.h"

void dali_EndFrame(Dali_LayerStack* layerStack, Dali_Brush* brush, Dali_UndoManager* undo)
{
    dali_LayerStackClearDirt(layerStack);
    dali_BrushClearDirt(brush);
    dali_UndoClearDirt(undo);
}
