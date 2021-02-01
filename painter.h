#ifndef PAINTER_H
#define PAINTER_H

#include <obsidian/r_geo.h>
#include <obsidian/f_file.h>

void painter_Init(void);
void painter_LoadFprim(Obdn_F_Primitive* fprim); // frees fprim
void painter_ReloadPrim(Obdn_F_Primitive* fprim); // frees fprim
void painter_StartLoop(void);
void painter_StopLoop(void);
bool painter_ShouldRun(void);
void painter_ShutDown(void);
void painter_SetColor(const float r, const float g, const float b);
void painter_SetRadius(const float r);

#endif /* end of include guard: PAINTER_H */
