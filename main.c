#include "painter.h"
#include "render.h"
#include "obsidian/f_file.h"
#include "obsidian/r_geo.h"
#include <unistd.h>

int main(int argc, char *argv[])
{
    painter_Init(IMG_4K, false);
    painter_StartLoop();
    painter_LocalCleanUp();
    painter_ShutDown();
    return 0;
}
