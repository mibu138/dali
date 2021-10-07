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

static void setBrushOpacityCmd(const Hell_Grimoire* grim, void* brushptr)
{
    Dali_Brush* brush = brushptr;
    float o = atof(hell_GetArg(grim, 1));
    dali_SetBrushOpacity(brush, o);
}

static void setBrushFalloffCmd(const Hell_Grimoire* grim, void* brushptr)
{
    Dali_Brush* brush = brushptr;
    float f = atof(hell_GetArg(grim, 1));
    dali_SetBrushFalloff(brush, f);
}

static void setBrushSpacingCmd(const Hell_Grimoire* grim, void* brushptr)
{
    Dali_Brush* brush = brushptr;
    float s = atof(hell_GetArg(grim, 1));
    dali_SetBrushSpacing(brush, s);
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
    brush->spacing = 0.01;
    brush->dirt = -1;

    if (grim)
    {
        hell_AddCommand(grim, "brushpos", setBrushPosCmd, brush);
        hell_AddCommand(grim, "brushcol", setBrushColorCmd, brush);
        hell_AddCommand(grim, "brushrad", setBrushRadiusCmd, brush);
        hell_AddCommand(grim, "brusha", setBrushActiveCmd, brush);
        hell_AddCommand(grim, "brushia", setBrushInactiveCmd, brush);
        hell_AddCommand(grim, "brushopac", setBrushOpacityCmd, brush);
        hell_AddCommand(grim, "brushfall", setBrushFalloffCmd, brush);
        hell_AddCommand(grim, "brushspacing", setBrushSpacingCmd, brush);
    }
}

void dali_SetBrushPos(Dali_Brush* brush, float x, float y)
{
    brush->x = x;
    brush->y = y;
    brush->dirt |= BRUSH_GENERAL_BIT;
}

void dali_SetBrushColor(Dali_Brush* brush, float r, float g, float b)
{
    brush->r = r;
    brush->g = g;
    brush->b = b;
    brush->dirt |= BRUSH_GENERAL_BIT;
}

void dali_SetBrushRadius(Dali_Brush* brush, float r)
{
    brush->radius = r;
    brush->dirt |= BRUSH_GENERAL_BIT;
}

void dali_SetBrushActive(Dali_Brush* brush)
{
    brush->active = true;
    brush->dirt |= BRUSH_GENERAL_BIT;
}

void dali_SetBrushOpacity(Dali_Brush* brush, float o)
{
    brush->opacity = o;
    brush->dirt |= BRUSH_GENERAL_BIT;
}

void dali_SetBrushFalloff(Dali_Brush* brush, float f)
{
    brush->falloff = f;
    brush->dirt |= BRUSH_GENERAL_BIT;
}

void dali_SetBrushInactive(Dali_Brush* brush)
{
    brush->active = false;
    brush->dirt |= BRUSH_GENERAL_BIT;
}

PaintMode dali_GetBrushPaintMode(const Dali_Brush* brush)
{
    return brush->mode;
}


void dali_SetBrushMode(Dali_Brush* brush, Dali_PaintMode mode)
{
    brush->mode = mode;
    brush->dirt |= BRUSH_PAINT_MODE_BIT;
}

void dali_BrushClearDirt(Dali_Brush* brush)
{
    brush->dirt = 0;
}

Vec2
dali_GetBrushPos(Dali_Brush* brush)
{
    Vec2 pos = {brush->x, brush->y};
    return pos;
}

void dali_SetBrushAlpha(Dali_Brush* brush, Obdn_Image* alpha)
{
    brush->alphaImg = alpha;
    brush->dirt |= BRUSH_ALPHA_BIT;
}

void dali_SetBrushSpacing(Dali_Brush* brush, float spacing)
{
    brush->spacing = spacing;
    brush->dirt |= BRUSH_GENERAL_BIT;
}
