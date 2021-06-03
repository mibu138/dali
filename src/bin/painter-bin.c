#include <unistd.h>
#include <string.h>
#include <hell/platform.h>
#include <hell/common.h>
#include <obsidian/u_ui.h>
#include <obsidian/v_swapchain.h>
#include "engine.h"

Hell_EventQueue* eventQueue;
Hell_Grimoire*   grimoire;
Hell_Console*    console;
Hell_Window*     window;
Hell_Hellmouth*  hellmouth;

Obdn_Instance*   oInstance;
Obdn_Memory*     oMemory; 
Obdn_Scene*      scene;
Obdn_Swapchain*  swapchain;
Obdn_UI*         ui;

Dali_Engine*      engine;
Dali_LayerStack*  layerStack;
Dali_UndoManager* undoManager;
Dali_Brush*       brush;

void daliFrame(void)
{
}

int painterMain(const char* gmod)
{
    eventQueue = hell_AllocEventQueue();
    grimoire   = hell_AllocGrimoire();
    console    = hell_AllocConsole();
    window     = hell_AllocWindow();
    hellmouth  = hell_AllocHellmouth();
    hell_CreateConsole(console);
    hell_CreateEventQueue(eventQueue);
    hell_CreateGrimoire(eventQueue, grimoire);
    hell_CreateWindow(eventQueue, 666, 666, NULL, window);
    hell_CreateHellmouth(grimoire, eventQueue, console, 1, &window, daliFrame, NULL, hellmouth);

    oInstance = obdn_AllocInstance();
    oMemory   = obdn_AllocMemory();
    swapchain = obdn_AllocSwapchain();
    ui        = obdn_AllocUI();
    scene     = obdn_AllocScene();
    obdn_CreateInstance(true, true, 0, NULL, oInstance);
    obdn_CreateMemory(oInstance, 1000, 100, 1000, 2000, 0, oMemory);
    obdn_CreateSwapchain(oInstance, eventQueue, window, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, swapchain);
    obdn_CreateScene(hell_GetWindowWidth(window), hell_GetWindowHeight(window),
                     0.01, 100, scene);

    obdn_s_LoadPrim(scene, oMemory, "../data/pig.tnt", COAL_MAT4_IDENT);

    engine      = dali_AllocEngine();
    layerStack  = dali_AllocLayerStack();
    brush       = dali_AllocBrush();
    undoManager = dali_AllocUndo();

    dali_CreateUndoManager(oMemory, 4096 * 4096 * 4, 4, 4, undoManager);
    dali_CreateBrush(brush);
    dali_CreateEngineAndStack(oInstance, oMemory, grimoire, undoManager, scene, brush, 4096, engine, layerStack);
    hell_Loop(hellmouth);
    return 0;
}

#ifdef UNIX
int main(int argc, char *argv[])
{
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
