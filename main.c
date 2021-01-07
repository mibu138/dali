#include "painter.h"
#include "render.h"
#include "tanto/f_file.h"
#include "tanto/r_geo.h"
#include <unistd.h>

void writeCube(void)
{
    Tanto_R_Primitive cube = tanto_r_CreateCubePrim(true);
    Tanto_F_Primitive fprim = tanto_f_CreateFPrimFromRPrim(&cube);
    tanto_f_WritePrimitive("data/cube-cw.tnt", &fprim);
    tanto_r_FreePrim(&cube);
    tanto_f_FreePrimitive(&fprim);
}

int main(int argc, char *argv[])
{
    painter_Init();
    Tanto_F_Primitive fprim;
    tanto_f_ReadPrimitive("data/pighead.tnt", &fprim);
    Tanto_R_Primitive m = tanto_f_CreateRPrimFromFPrim(&fprim);
    tanto_f_FreePrimitive(&fprim);
    r_LoadPrim(m);
    painter_StartLoop();
    painter_ShutDown();
    return 0;
}
