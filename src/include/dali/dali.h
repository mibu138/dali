#ifndef DALI_H
#define DALI_H

#include "brush.h"
#include "layer.h"
#include "engine.h"
#include "undo.h"

void dali_EndFrame(Dali_LayerStack* layerStack, Dali_Brush* brush, Dali_UndoManager* undo);

#define DALI_TEXSIZE(res, bytes_per_channel, channel_count) (res * res * bytes_per_channel * channel_count)

#endif /* end of include guard: DALI_H */
