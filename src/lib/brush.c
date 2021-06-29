#include <hell/hell.h>
#include <string.h>
#include "brush.h"
#include "private.h"

Dali_Brush* dali_AllocBrush(void)
{
    return hell_Malloc(sizeof(Dali_Brush));
}

void dali_CreateBrush(Dali_Brush *brush)
{
    memset(brush, 0, sizeof(Dali_Brush));
    brush->opacity = 1.0;
    brush->r = 1.0;
    brush->g = 1.0;
    brush->b = .5;
    brush->radius = 1.0;
    brush->falloff = 0.8;
    brush->mode = PAINT_MODE_OVER;
    brush->dirt |= BRUSH_BIT;
}

void dali_SetBrushPos(Dali_Brush* brush, float x, float y)
{
    brush->x = x;
    brush->y = y;
    brush->dirt |= BRUSH_BIT;
}

void dali_ActivateBrush(Dali_Brush* brush)
{
    brush->active = true;
    brush->dirt |= BRUSH_BIT;
}

void dali_DeactivateBrush(Dali_Brush* brush)
{
    brush->active = false;
    brush->dirt |= BRUSH_BIT;
}
