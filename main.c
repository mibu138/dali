#include "painter.h"
#include <unistd.h>

int main(int argc, char *argv[])
{
    painter_Init();
    Tanto_R_Mesh m = tanto_r_CreateCube();
    painter_LoadMesh(m);
    painter_StartLoop();
    painter_ShutDown();
    return 0;
}
