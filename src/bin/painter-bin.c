#include "engine.h"
#include <hell/hell.h>
#include <hell/platform.h>
#include <hell/len.h>
#include <obsidian/obsidian.h>
#include <obsidian/ui.h>
#include <shiv/shiv.h>
#include <string.h>
#include <unistd.h>

Hell_EventQueue* eventQueue;
Hell_Grimoire*   grimoire;
Hell_Console*    console;
Hell_Window*     window;
Hell_Hellmouth*  hellmouth;

Obdn_Instance*  oInstance;
Obdn_Memory*    oMemory;
Obdn_Scene*     scene;
Obdn_Swapchain* swapchain;
Obdn_UI*        ui;

Dali_Engine*      engine;
Dali_LayerStack*  layerStack;
Dali_UndoManager* undoManager;
Dali_Brush*       brush;

Shiv_Renderer* renderer;

Obdn_Command renderCommand;
Obdn_Command paintCommand;

#define WWIDTH 666
#define WHEIGHT 666

static int windowWidth  = WWIDTH;
static int windowHeight = WHEIGHT;

static bool spaceDown = false;

bool 
handleKeyEvent(const Hell_Event* ev, void* data)
{
    uint8_t code = hell_GetEventKeyCode(ev);
    if (code == HELL_KEY_SPACE)
    {
        spaceDown = !spaceDown;
    }
    return false;
}

static bool 
handleViewEvent(const Hell_Event* ev, void* data)
{
    int         mx     = ev->data.winData.data.mouseData.x;
    int         my     = ev->data.winData.data.mouseData.y;
    static bool tumble = false;
    static bool zoom   = false;
    static bool pan    = false;
    static int  xprev  = 0;
    static int  yprev  = 0;
    if (ev->type == HELL_EVENT_TYPE_MOUSEDOWN)
    {
        switch (hell_GetEventButtonCode(ev))
        {
        case HELL_MOUSE_LEFT:
            tumble = true;
            break;
        case HELL_MOUSE_MID:
            pan = true;
            break;
        case HELL_MOUSE_RIGHT:
            zoom = true;
            break;
        }
    }
    if (ev->type == HELL_EVENT_TYPE_MOUSEUP)
    {
        switch (hell_GetEventButtonCode(ev))
        {
        case HELL_MOUSE_LEFT:
            tumble = false;
            break;
        case HELL_MOUSE_MID:
            pan = false;
            break;
        case HELL_MOUSE_RIGHT:
            zoom = false;
            break;
        }
    }
    static Vec3 target = {0, 0, 0};
    obdn_UpdateCamera_ArcBall(scene, &target, windowWidth, windowHeight, 0.1,
                              xprev, mx, yprev, my, pan, tumble, zoom, false);
    xprev = mx;
    yprev = my;
    return false;
}

static bool 
handlePaintEvent(const Hell_Event* ev, void* data)
{
    int         mx     = ev->data.winData.data.mouseData.x;
    int         my     = ev->data.winData.data.mouseData.y;
    if (ev->type == HELL_EVENT_TYPE_MOUSEDOWN) 
    {
        dali_ActivateBrush(brush);
        dali_SetBrushPos(brush, mx, my);
    }
    if (ev->type == HELL_EVENT_TYPE_MOTION)
    {
        dali_SetBrushPos(brush, mx, my);
    }
    if (ev->type == HELL_EVENT_TYPE_MOUSEUP)
    {
        dali_DeactivateBrush(brush);
    }
    return false;
}

bool
handleMouseEvent(const Hell_Event* ev, void* data)
{
    if (spaceDown) return handleViewEvent(ev, data);
    else return handlePaintEvent(ev, data);
}

#define TARGET_RENDER_INTERVAL 10000 // render every 30 ms

static VkSemaphore acquireSemaphore;

void
daliFrame(void)
{
    dali_SetBrushPos(brush, 0.5, 0.5);
    dali_ActivateBrush(brush);

    obdn_WaitForFence(obdn_GetDevice(oInstance), &paintCommand.fence);

    VkFence                 fence = VK_NULL_HANDLE;
    const Obdn_Framebuffer* fb =
        obdn_AcquireSwapchainFramebuffer(swapchain, &fence, &acquireSemaphore);

    VkSemaphore undoWaitSemaphore = VK_NULL_HANDLE;
    obdn_ResetCommand(&paintCommand);
    obdn_BeginCommandBuffer(paintCommand.buffer);
    undoWaitSemaphore = dali_Paint(engine, scene, brush, layerStack, undoManager, paintCommand.buffer);
    obdn_EndCommandBuffer(paintCommand.buffer);

    obdn_SceneDirtyTextures(scene);

    obdn_ResetCommand(&renderCommand);
    obdn_BeginCommandBuffer(renderCommand.buffer);
    shiv_Render(renderer, scene, fb, renderCommand.buffer);
    obdn_EndCommandBuffer(renderCommand.buffer);

    obdn_SceneClearDirt(scene);
    dali_LayerStackClearDirt(layerStack);

    VkPipelineStageFlags stageFlags = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkPipelineStageFlags renderStageFlags = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkSubmitInfo paintSubmit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .waitSemaphoreCount = undoWaitSemaphore == VK_NULL_HANDLE ? 0 : 1,
        .signalSemaphoreCount = 1,
        .pWaitDstStageMask = &stageFlags,
        .pSignalSemaphores = &paintCommand.semaphore,
        .pWaitSemaphores = &undoWaitSemaphore,
        .pCommandBuffers = &paintCommand.buffer,
    };
    VkSubmitInfo renderSubmit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .waitSemaphoreCount = 1,
        .signalSemaphoreCount = 1,
        .pWaitDstStageMask = &renderStageFlags,
        .pSignalSemaphores = &renderCommand.semaphore,
        .pWaitSemaphores = &paintCommand.semaphore,
        .pCommandBuffers = &renderCommand.buffer,
    };
    VkSubmitInfo submitinfos[] = {paintSubmit, renderSubmit};
    obdn_SubmitGraphicsCommands(oInstance, 0, LEN(submitinfos), submitinfos, paintCommand.fence);
    VkSemaphore waitSemas[] = {acquireSemaphore, renderCommand.semaphore};
    obdn_PresentFrame(swapchain, LEN(waitSemas), waitSemas);
}

int
painterMain(const char* gmod)
{
    eventQueue = hell_AllocEventQueue();
    grimoire   = hell_AllocGrimoire();
    console    = hell_AllocConsole();
    window     = hell_AllocWindow();
    hellmouth  = hell_AllocHellmouth();
    hell_CreateConsole(console);
    hell_CreateEventQueue(eventQueue);
    hell_CreateGrimoire(eventQueue, grimoire);
    hell_CreateWindow(eventQueue, WWIDTH, WHEIGHT, NULL, window);
    hell_CreateHellmouth(grimoire, eventQueue, console, 1, &window, daliFrame,
                         NULL, hellmouth);

    oInstance = obdn_AllocInstance();
    oMemory   = obdn_AllocMemory();
    swapchain = obdn_AllocSwapchain();
    ui        = obdn_AllocUI();
    scene     = obdn_AllocScene();
    obdn_CreateInstance(true, true, 0, NULL, oInstance);
    obdn_CreateMemory(oInstance, 1000, 100, 1000, 2000, 0, oMemory);
    Obdn_AovInfo depthAov = {.aspectFlags = VK_IMAGE_ASPECT_DEPTH_BIT,
                             .usageFlags =
                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                             .format = VK_FORMAT_D24_UNORM_S8_UINT};
    obdn_CreateSwapchain(oInstance, oMemory, eventQueue, window,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 1, &depthAov,
                         swapchain);
    obdn_CreateScene(grimoire, oMemory, 0.01, 100, scene);

    obdn_LoadPrim(scene, "../data/pig.tnt", COAL_MAT4_IDENT);

    engine      = dali_AllocEngine();
    layerStack  = dali_AllocLayerStack();
    brush       = dali_AllocBrush();
    undoManager = dali_AllocUndo();

    dali_CreateUndoManager(oMemory, 4096 * 4096 * 4, 4, 4, undoManager);
    dali_CreateBrush(brush);
    dali_CreateEngineAndStack(oInstance, oMemory, grimoire, undoManager, scene,
                              brush, 4096, engine, layerStack);

    obdn_CreateSemaphore(obdn_GetDevice(oInstance), &acquireSemaphore);
    paintCommand = obdn_CreateCommand(oInstance, OBDN_V_QUEUE_GRAPHICS_TYPE);
    renderCommand = obdn_CreateCommand(oInstance, OBDN_V_QUEUE_GRAPHICS_TYPE);
    renderer = shiv_AllocRenderer();
    shiv_CreateRenderer(oInstance, oMemory, grimoire,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                        obdn_GetSwapchainFramebufferCount(swapchain),
                        obdn_GetSwapchainFramebuffers(swapchain), renderer);
    hell_Subscribe(eventQueue, HELL_EVENT_MASK_MOUSE_BIT,
                   hell_GetWindowID(window), handleMouseEvent, NULL);
    hell_Subscribe(eventQueue, HELL_EVENT_MASK_KEY_BIT, hell_GetWindowID(window), handleKeyEvent, NULL);
    hell_Loop(hellmouth);
    return 0;
}

#ifdef UNIX
int
main(int argc, char* argv[])
{
    if (argc > 1)
        painterMain(argv[1]);
    else
        painterMain("standalone");
}
#endif

#ifdef WINDOWS
#include <hell/win_local.h>
int WINAPI
WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine,
        int nCmdShow)
{
    printf("Start");
    winVars.instance = hInstance;
    return painterMain("standalone");
}
#endif
