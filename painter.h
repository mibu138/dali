#ifndef PAINTER_H
#define PAINTER_H

#include <tanto/r_geo.h>

void painter_Init(void);
void painter_LoadPreMesh(Tanto_R_PreMesh mesh);
void painter_LoadMesh(Tanto_R_Mesh mesh);
void painter_StartLoop(void);
void painter_ReloadMesh(Tanto_R_PreMesh mesh);
void painter_StopLoop(void);

#endif /* end of include guard: PAINTER_H */
