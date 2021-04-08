#include "painter.h"
#include "render.h"
#include "obsidian/f_file.h"
#include "obsidian/r_geo.h"
#include <unistd.h>
#include <string.h>
#include <hell/platform.h>

int painterMain(const char* gmod)
{
    if (gmod)
        painter_Init(IMG_4K, false, gmod);
    else
        painter_Init(IMG_4K, false, "standalone");
    painter_StartLoop();
    return 0;
}

#ifdef UNIX
int main(int argc, char *argv[])
{
    painterMain("standalone");
}
#endif

#ifdef WINDOWS
#include <hell/win_local.h>
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
    LPSTR lpCmdLine, int nCmdShow)
{
    printf("Start");
    winVars.instance = hInstance;
    return painterMain(NULL);
}
#endif