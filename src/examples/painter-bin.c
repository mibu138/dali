#include <unistd.h>
#include <string.h>
#include <hell/platform.h>
#include <hell/common.h>
#include <obsidian/common.h>
#include <painter/painter.h>
#include <painter/render.h>

int painterMain(const char* gmod)
{
    painter_Init(IMG_4K, false, gmod);
    hell_Loop();
    return 0;
}

#ifdef UNIX
int main(int argc, char *argv[])
{
    hell_Init(true, 0, 0);
    obdn_Init();
    if (argc > 1)
        painterMain(argv[1]);
    else
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
    return painterMain("standalone");
}
#endif
