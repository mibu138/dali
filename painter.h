#ifndef PAINTER_H
#define PAINTER_H

#include <tanto/r_geo.h>

typedef struct {
    uint32_t           vertexCount;
    Tanto_R_Attribute* posData;
    Tanto_R_Attribute* norData;
    Tanto_R_Attribute* uvwData;
    Tanto_R_Index*     indexData;
} Painter_HouMesh;

void painter_Init(void);
void painter_LoadHouMesh(Painter_HouMesh houMesh);
void painter_ReloadHouMesh(Painter_HouMesh houMesh);
void painter_LoadPreMesh(Tanto_R_PreMesh mesh);
void painter_LoadMesh(Tanto_R_Mesh mesh);
void painter_StartLoop(void);
void painter_ReloadMesh(Tanto_R_PreMesh mesh);
void painter_StopLoop(void);
bool painter_ShouldRun(void);
void painter_ShutDown(void);
void painter_SetColor(const float r, const float g, const float b);
void painter_SetRadius(const float r);

#endif /* end of include guard: PAINTER_H */
