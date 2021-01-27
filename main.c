#include "painter.h"
#include "render.h"
#include "tanto/f_file.h"
#include "tanto/r_geo.h"
#include <unistd.h>

int main(int argc, char *argv[])
{
    painter_Init();
    Tanto_F_Primitive fprim;
    tanto_f_ReadPrimitive("data/pig.tnt", &fprim);
    Tanto_R_Primitive m = tanto_f_CreateRPrimFromFPrim(&fprim);
    tanto_f_FreePrimitive(&fprim);
    r_LoadPrim(m);
    painter_StartLoop();
    painter_ShutDown();
    return 0;
}
