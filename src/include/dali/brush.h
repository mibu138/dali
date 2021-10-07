#ifndef DALI_BRUSH_H
#define DALI_BRUSH_H

#include <coal/coal.h>

typedef struct Dali_Brush Dali_Brush;
typedef struct Obdn_Image Obdn_Image;

typedef struct Hell_Grimoire Hell_Grimoire;

typedef enum {
    DALI_PAINT_MODE_OVER,
    DALI_PAINT_MODE_ERASE
} Dali_PaintMode;

Dali_Brush* dali_AllocBrush(void);

void dali_CreateBrush(Hell_Grimoire* grim /* optional */, Dali_Brush *brush);
void dali_SetBrushActive(Dali_Brush* brush);
void dali_SetBrushInactive(Dali_Brush* brush);
void dali_SetBrushRadius(Dali_Brush* brush, float r);
void dali_SetBrushPos(Dali_Brush* brush, float x, float y);
void dali_SetBrushColor(Dali_Brush* brush, float r, float g, float b);
void dali_SetBrushOpacity(Dali_Brush* brush, float o);
void dali_SetBrushFalloff(Dali_Brush* brush, float f);
void dali_SetBrushMode(Dali_Brush* brush, Dali_PaintMode mode);
Dali_PaintMode dali_GetBrushPaintMode(const Dali_Brush* brush);
void dali_BrushClearDirt(Dali_Brush* brush);
void dali_SetBrushAlpha(Dali_Brush* brush, Obdn_Image* alpha);

void dali_SetBrushSpacing(Dali_Brush* brush, float spacing);

Coal_Vec2 dali_GetBrushPos(Dali_Brush* brush);

#endif /* end of include guard: DALI_BRUSH_H */
