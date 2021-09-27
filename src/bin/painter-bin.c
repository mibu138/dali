#include <math.h>
#include "dali/dali.h"
#include <unistd.h>
#include <hell/hell.h>
#include <hell/platform.h>
#include <hell/len.h>
#include <hell/debug.h>
#include <obsidian/obsidian.h>
#include <obsidian/ui.h>
#include <shiv/shiv.h>
#include <string.h>
#include <stdlib.h>

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

Obdn_Geometry paintGeo;

Shiv_Renderer* renderer;

Obdn_Command renderCommand;
Obdn_Command paintCommand;

#define WWIDTH 888
#define WHEIGHT 888

static int windowWidth  = WWIDTH;
static int windowHeight = WHEIGHT;

static bool spaceDown = false;

static bool is2D;

struct SceneMemEng {
    Obdn_Scene* scene;
    Obdn_Memory* mem;
    Dali_Engine* engine;
} sceneMemEng;

static void setGeo(const Hell_Grimoire* grim, void* data)
{
    struct SceneMemEng* sm = data;
    const char* geoType = hell_GetArg(grim, 1);
    if (strcmp(geoType, "cube") == 0) 
    {
        // TODO need to free the memory thats there...
        Obdn_Geometry cube = obdn_CreateCube(sm->mem, true);
        Obdn_PrimitiveHandle ap = dali_GetActivePrim(sm->engine);
        if (ap.id == 0)
        {
            paintGeo = cube;
            Obdn_PrimitiveHandle np = obdn_SceneAddPrim(sm->scene, &paintGeo, COAL_MAT4_IDENT, dali_GetPaintMaterial(sm->engine));
            dali_SetActivePrim(sm->engine, np);
        }
        else 
        {
            Obdn_Geometry* curgeo = obdn_SceneGetPrimGeo(scene, ap, OBDN_PRIM_TOPOLOGY_CHANGED_BIT);
            obdn_FreeGeo(curgeo);
            *curgeo = cube;
        }
    }
}

static void rmGeo(const Hell_Grimoire* grim, void* data)
{
    struct SceneMemEng* sm = data;
    if (obdn_GetPrimCount(sm->scene) > 0)
    {
        Obdn_PrimitiveHandle h = dali_GetActivePrim(sm->engine);
        obdn_SceneRemovePrim(sm->scene, h);
        obdn_FreeGeo(&paintGeo);
    }
}

void 
setBrushColor(const Hell_Grimoire* grim, void* pbrush)
{
    Dali_Brush* brush = pbrush;
    float r = atof(hell_GetArg(grim, 1));
    float g = atof(hell_GetArg(grim, 2));
    float b = atof(hell_GetArg(grim, 3));
    dali_SetBrushColor(brush, r, g, b);
}

bool 
handleKeyEvent(const Hell_Event* ev, void* data)
{
    uint8_t code = hell_GetEventKeyCode(ev);
    if (code == HELL_KEY_SPACE)
    {
        spaceDown = !spaceDown;
    }
    if (code == HELL_KEY_E) //erase
    {
        if (ev->type == HELL_EVENT_TYPE_KEYDOWN)
        {
            Dali_PaintMode m = dali_GetBrushPaintMode(brush);
            if (m != DALI_PAINT_MODE_ERASE)
                dali_SetBrushMode(brush, DALI_PAINT_MODE_ERASE);
            else 
                dali_SetBrushMode(brush, DALI_PAINT_MODE_OVER);
        }
    }
    if (code == HELL_KEY_Z) //erase
    {
        if (ev->type == HELL_EVENT_TYPE_KEYDOWN)
        {
            hell_Print("Undo key hit\n");
            dali_Undo(undoManager);
        }
    }
    if (code == HELL_KEY_L) //erase
    {
        if (ev->type == HELL_EVENT_TYPE_KEYDOWN)
        {
            hell_Print("Layer create key hit\n");
            dali_CreateLayer(layerStack);
        }
    }
    if (code == HELL_KEY_D) //erase
    {
        if (ev->type == HELL_EVENT_TYPE_KEYDOWN)
        {
            hell_Print("Layer down key hit\n");
            dali_DecrementLayer(layerStack);
        }
    }
    if (code == HELL_KEY_U) //erase
    {
        if (ev->type == HELL_EVENT_TYPE_KEYDOWN)
        {
            hell_Print("Layer down key hit\n");
            dali_IncrementLayer(layerStack);
        }
    }
    return false;
}

bool 
handleWindowResizeEvent(const Hell_Event* ev, void* data)
{
    if (ev->type != HELL_EVENT_TYPE_RESIZE) return false;
    windowWidth = ev->data.winData.data.resizeData.width;
    windowHeight = ev->data.winData.data.resizeData.height;
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
    static bool spaceWasDown = false;
    if (spaceDown && !spaceWasDown)
    {
        xprev = mx;
        yprev = my;
        spaceWasDown = true;
    }
    if (!spaceDown && spaceWasDown)
    {
        spaceWasDown = false;
    }
    if (!spaceDown) return false;
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
    if (is2D)
    {
        float dx = 0, dy = 0, dz = 0;
        if (pan)
        {
            dx = (mx - xprev) * -0.001;
            dy = (my - yprev) * 0.001;
        }
        if (zoom)
            dz = (mx - xprev) * -0.001;
        obdn_SceneUpdateCamera_Pos(scene, dx, dy, dz);
    }
    else 
        obdn_UpdateCamera_ArcBall(scene, &target, windowWidth, windowHeight, 0.1,
                              xprev, mx, yprev, my, pan, tumble, zoom, false);
    xprev = mx;
    yprev = my;
    return false;
}

static bool 
handlePaintEvent(const Hell_Event* ev, void* data)
{
    float mx = (float)ev->data.winData.data.mouseData.x / windowWidth;
    float my = (float)ev->data.winData.data.mouseData.y / windowHeight;
    if (ev->type == HELL_EVENT_TYPE_MOUSEDOWN) 
    {
        dali_SetBrushActive(brush);
        dali_SetBrushPos(brush, mx, my);
    }
    if (ev->type == HELL_EVENT_TYPE_MOTION)
    {
        dali_SetBrushPos(brush, mx, my);
    }
    if (ev->type == HELL_EVENT_TYPE_MOUSEUP)
    {
        dali_SetBrushInactive(brush);
        dali_LayerBackup(layerStack);
    }
    if (ev->type == HELL_EVENT_TYPE_STYLUS)
    {
        float pressure = ev->data.winData.data.stylusData.pressure;
        pressure = pow(pressure, 3);
        dali_SetBrushOpacity(brush, pressure);
    }
    return false;
}

bool
handleMouseEvent(const Hell_Event* ev, void* data)
{
    handleViewEvent(ev, data);
    if (!spaceDown) handlePaintEvent(ev, data);
    return false;
}

#define TARGET_RENDER_INTERVAL 10000 // render every 30 ms

static VkSemaphore acquireSemaphore;

void
daliFrame(void)
{
    dali_UpdateUndo(undoManager, layerStack);

    obdn_WaitForFence(obdn_GetDevice(oInstance), &paintCommand.fence);

    VkFence                 fence = VK_NULL_HANDLE;
    const Obdn_Framebuffer* fb =
        obdn_AcquireSwapchainFramebuffer(swapchain, &fence, &acquireSemaphore);

    VkSemaphore undoWaitSemaphore = VK_NULL_HANDLE;
    obdn_ResetCommand(&paintCommand);
    obdn_BeginCommandBuffer(paintCommand.buffer);
    undoWaitSemaphore = dali_Paint(engine, scene, brush, layerStack, undoManager, paintCommand.buffer);
    obdn_EndCommandBuffer(paintCommand.buffer);

    obdn_ResetCommand(&renderCommand);
    obdn_BeginCommandBuffer(renderCommand.buffer);
    shiv_Render(renderer, scene, fb, renderCommand.buffer);
    obdn_EndCommandBuffer(renderCommand.buffer);

    obdn_SceneEndFrame(scene);
    dali_EndFrame(layerStack, brush, undoManager);

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
painterMain(const char* modelpath, bool maskMode, bool twoDMode)
{
    const Dali_Format format = maskMode ? DALI_FORMAT_R32_SFLOAT : DALI_FORMAT_R8G8B8A8_UNORM;
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
    hell_SetVar(grimoire, "maxFps", "1000", 0);

    oInstance = obdn_AllocInstance();
    oMemory   = obdn_AllocMemory();
    swapchain = obdn_AllocSwapchain();
    scene     = obdn_AllocScene();
    const char* testgeopath;
    #if UNIX
    const char* instanceExtensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_XCB_SURFACE_EXTENSION_NAME
    };
    testgeopath = modelpath;
    #elif WIN32
    const char* instanceExtensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME
    };
    obdn_SetRuntimeSpvPrefix("C:/dev/dali/build/shaders/");
    testgeopath = "C:/dev/dali/data/pig.tnt";
    #endif
    Obdn_InstanceParms ip = {
        .enableRayTracing = true,
        .enabledInstanceExentensionCount = LEN(instanceExtensions),
        .ppEnabledInstanceExtensionNames = instanceExtensions
    };
    obdn_CreateInstance(&ip, oInstance);
    obdn_CreateMemory(oInstance, 1000, 100, 1000, 2000, 0, oMemory);
    Obdn_AovInfo depthAov = {.aspectFlags = VK_IMAGE_ASPECT_DEPTH_BIT,
                             .usageFlags =
                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                             .format = VK_FORMAT_D24_UNORM_S8_UINT};
    obdn_CreateSwapchain(oInstance, oMemory, eventQueue, window,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 1, &depthAov,
                         swapchain);
    obdn_CreateScene(grimoire, oMemory, 0.01, 100, scene);

    engine      = dali_AllocEngine();
    layerStack  = dali_AllocLayerStack();
    brush       = dali_AllocBrush();
    undoManager = dali_AllocUndo();

    u64 texSize = DALI_TEXSIZE(4096, 1, 4);
    dali_CreateUndoManager(oMemory, texSize, 1, 16, undoManager);
    dali_CreateBrush(grimoire, brush);
    dali_SetBrushRadius(brush, 0.01);
    dali_CreateLayerStack(oMemory, texSize, layerStack);
    dali_CreateEngine(oInstance, oMemory, undoManager, scene,
                              brush, 4096, format, grimoire, engine);

    if (twoDMode)
    {
        paintGeo = obdn_CreateQuadNDC_2(oMemory, 0, 0, 1, 1);
        obdn_UpdateCamera_LookAt(scene, (Vec3){0, 0, 1}, (Vec3){0, 0, 0}, (Vec3){0, 1, 0});
    }
    else 
    {
        paintGeo = obdn_LoadGeo(oMemory, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, 
            testgeopath, true);
    }

    is2D = twoDMode;

    Obdn_PrimitiveHandle prim = obdn_SceneAddPrim(scene, &paintGeo, COAL_MAT4_IDENT, dali_GetPaintMaterial(engine));
    dali_SetActivePrim(engine, prim);
    dali_LayerBackup(layerStack); // initial backup

    obdn_CreateSemaphore(obdn_GetDevice(oInstance), &acquireSemaphore);
    paintCommand = obdn_CreateCommand(oInstance, OBDN_V_QUEUE_GRAPHICS_TYPE);
    renderCommand = obdn_CreateCommand(oInstance, OBDN_V_QUEUE_GRAPHICS_TYPE);
    renderer = shiv_AllocRenderer();
    Shiv_Parms sp = {
        .clearColor = (Vec4){0.1, 0.1, 0.1, 1.0},
        .grim = grimoire
    };
    shiv_CreateRenderer(oInstance, oMemory, 
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                        obdn_GetSwapchainFramebufferCount(swapchain),
                        obdn_GetSwapchainFramebuffers(swapchain), &sp, renderer);

    if (format == DALI_FORMAT_R32_SFLOAT)
        shiv_SetDrawMode(renderer, "mono");
    else 
        shiv_SetDrawMode(renderer, "uvgrid");

    sceneMemEng.scene = scene;
    sceneMemEng.mem   = oMemory;
    sceneMemEng.engine = engine;
    hell_AddCommand(grimoire, "setgeo", setGeo, &sceneMemEng);
    hell_AddCommand(grimoire, "rmgeo", rmGeo, &sceneMemEng);

    hell_Subscribe(eventQueue, HELL_EVENT_MASK_POINTER_BIT,
                   hell_GetWindowID(window), handleMouseEvent, NULL);
    hell_Subscribe(eventQueue, HELL_EVENT_MASK_KEY_BIT,
                   hell_GetWindowID(window), handleKeyEvent, &sceneMemEng);
    hell_Subscribe(eventQueue, HELL_EVENT_MASK_WINDOW_BIT,
                   hell_GetWindowID(window), handleWindowResizeEvent, NULL);
    hell_Loop(hellmouth);
    return 0;
}

#define USAGE_STR "Usage: %s [-m] [path-to-model.tnt]|[-d]\n"

#ifdef WIN32
int APIENTRY WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance,
    _In_ PSTR lpCmdLine, _In_ int nCmdShow)
{
    hell_SetHinstance(hInstance);
    painterMain(NULL);
    return 0;
}
#elif UNIX
int main(int argc, char* argv[])
{
    if (argc < 2 || argc > 3)
    {
        hell_Print(USAGE_STR, argv[0]);
        return 1;
    }
    bool maskMode = false;
    bool twoDMode = false;
    const char* filePath;
    if (argc == 3)
    {
        if (strcmp(argv[1], "-m") != 0)
        {
            hell_Print(USAGE_STR, argv[0]);
            return 1;
        }
        maskMode = true;
        filePath = argv[2];
    }
    else 
    {
        filePath = argv[1];
    }
    if (strncmp(filePath, "-d", 2) == 0)
    {
        twoDMode = true;
    }
    else
    {
        if (access(filePath, R_OK) != 0)
        {
            hell_Print("Cannot find file %s or read permissions not set\n", filePath);
            return 1;
        }
    }
    painterMain(filePath, maskMode, twoDMode);
}
#endif
