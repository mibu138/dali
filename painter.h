#ifndef PAINTER_H
#define PAINTER_H

#include <obsidian/f_prim.h>
#include <stdbool.h>

void painter_Init(uint32_t texSize, bool houdiniMode, const char* gModuleName);
void painter_LocalInit(uint32_t texSize);
void painter_LoadFprim(Obdn_F_Primitive* fprim); // frees fprim
void painter_ReloadPrim(Obdn_F_Primitive* fprim); // frees fprim
void painter_StartLoop(void);
void painter_StopLoop(void);
void painter_LocalCleanUp(void);
void painter_ShutDown(void);
void painter_SetColor(const float r, const float g, const float b);
void painter_SetRadius(const float r);
void painter_SetOpacity(const float o);
void painter_SetFallOff(const float f);

#endif /* end of include guard: PAINTER_H */
