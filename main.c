#include "painter.h"
#include "render.h"
#include "obsidian/f_file.h"
#include "obsidian/r_geo.h"
#include <unistd.h>
#include <string.h>

int main(int argc, char *argv[])
{
    if (argc == 2)
        painter_Init(IMG_4K, false, argv[1]);
    else
        painter_Init(IMG_4K, false, "standalone");
    painter_StartLoop();
    //painter_LocalCleanUp();
    //painter_ShutDown();
    return 0;
}
