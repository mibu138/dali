#include "painter.h"

int main(int argc, char *argv[])
{
    painter_Init();
    Tanto_R_Mesh m = tanto_r_CreateCube();
    painter_LoadMesh(m);
    painter_StartLoop();
    return 0;
}
