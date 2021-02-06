#include "painter.h"
#include "render.h"
#include "obsidian/f_file.h"
#include "obsidian/r_geo.h"
#include <unistd.h>

int main(int argc, char *argv[])
{
    painter_Init(true);
    Obdn_F_Primitive fprim;
    obdn_f_ReadPrimitive("data/flip-uv.tnt", &fprim);
    Obdn_R_Primitive m = obdn_f_CreateRPrimFromFPrim(&fprim);
    obdn_f_FreePrimitive(&fprim);
    r_LoadPrim(m);
    painter_StartLoop();
    painter_ShutDown();
    return 0;
}
