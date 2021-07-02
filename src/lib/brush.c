#include <hell/hell.h>
#include <string.h>
#include "brush.h"
#include "private.h"
#include <stdlib.h>


static void setBrushPosCmd(const Hell_Grimoire* grim, void* brushptr)
{
    Dali_Brush* brush = brushptr;
    float x = atof(hell_GetArg(grim, 1));
    float y = atof(hell_GetArg(grim, 2));
    dali_SetBrushPos(brush, x, y);
}

static void setBrushColorCmd(const Hell_Grimoire* grim, void* brushptr)
{
    Dali_Brush* brush = brushptr;
    float r = atof(hell_GetArg(grim, 1));
    float g = atof(hell_GetArg(grim, 2));
    float b = atof(hell_GetArg(grim, 3));
    dali_SetBrushColor(brush, r, g, b);
}

static void setBrushRadiusCmd(const Hell_Grimoire* grim, void* brushptr)
{
    Dali_Brush* brush = brushptr;
    float r = atof(hell_GetArg(grim, 1));
    dali_SetBrushRadius(brush, r);
}

static void setBrushActiveCmd(const Hell_Grimoire* grim, void* brushptr)
{
    Dali_Brush* brush = brushptr;
    dali_SetBrushActive(brush);
}

static void setBrushInactiveCmd(const Hell_Grimoire* grim, void* brushptr)
{
    Dali_Brush* brush = brushptr;
    dali_SetBrushInactive(brush);
}

Dali_Brush* dali_AllocBrush(void)
{
    return hell_Malloc(sizeof(Dali_Brush));
}

void dali_CreateBrush(Hell_Grimoire* grim, Dali_Brush *brush)
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

    if (grim)
    {
        hell_AddCommand(grim, "brushpos", setBrushPosCmd, brush);
        hell_AddCommand(grim, "brushcol", setBrushColorCmd, brush);
        hell_AddCommand(grim, "brushrad", setBrushRadiusCmd, brush);
        hell_AddCommand(grim, "brusha", setBrushActiveCmd, brush);
        hell_AddCommand(grim, "brushia", setBrushInactiveCmd, brush);
    }
}

void dali_SetBrushPos(Dali_Brush* brush, float x, float y)
{
    brush->x = x;
    brush->y = y;
    brush->dirt |= BRUSH_BIT;
}

void dali_SetBrushColor(Dali_Brush* brush, float r, float g, float b)
{
    brush->r = r;
    brush->g = g;
    brush->b = b;
    brush->dirt |= BRUSH_BIT;
}

void dali_SetBrushRadius(Dali_Brush* brush, float r)
{
    brush->radius = r;
    brush->dirt |= BRUSH_BIT;
}

void dali_SetBrushActive(Dali_Brush* brush)
{
    brush->active = true;
    brush->dirt |= BRUSH_BIT;
}

void dali_SetBrushInactive(Dali_Brush* brush)
{
    brush->active = false;
    brush->dirt |= BRUSH_BIT;
}
