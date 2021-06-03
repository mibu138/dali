#ifndef PAINTER_H
#define PAINTER_H

#include <obsidian/f_prim.h>
#include <stdbool.h>
#include <obsidian/v_vulkan.h>

struct Mat4;

void  painter_Bell(void);
void  painter_Init(uint32_t texSize, bool houdiniMode, const char* gModuleName);
void  painter_LocalInit(uint32_t texSize);
void  painter_LoadFprim(Obdn_F_Primitive* fprim); // frees fprim
void  painter_ReloadPrim(Obdn_F_Primitive* fprim); // frees fprim
void  painter_Frame(void);
void  painter_StartLoop(void);
void  painter_StopLoop(void);
void  painter_LocalCleanUp(void);
void  painter_ShutDown(void);
void* painter_GetGame(void);


#endif /* end of include guard: PAINTER_H */
