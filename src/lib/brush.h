#ifndef DALI_BRUSH_H
#define DALI_BRUSH_H

typedef struct Dali_Brush Dali_Brush;

typedef struct Hell_Grimoire Hell_Grimoire;

Dali_Brush* dali_AllocBrush(void);

void dali_CreateBrush(Hell_Grimoire* grim /* optional */, Dali_Brush *brush);
void dali_SetBrushActive(Dali_Brush* brush);
void dali_SetBrushInactive(Dali_Brush* brush);
void dali_SetBrushRadius(Dali_Brush* brush, float r);
void dali_SetBrushPos(Dali_Brush* brush, float x, float y);
void dali_SetBrushColor(Dali_Brush* brush, float r, float g, float b);

#endif /* end of include guard: DALI_BRUSH_H */
