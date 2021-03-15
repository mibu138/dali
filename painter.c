#include "painter.h"
#include "paint.h"
#include "game.h"
#include "common.h"
#include "obsidian/t_def.h"
#include "render.h"
#include "obsidian/f_file.h"
#include "obsidian/r_geo.h"

#include <pthread.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <stdbool.h>
#include <obsidian/v_video.h>
#include <obsidian/d_display.h>
#include <obsidian/v_swapchain.h>
#include <obsidian/r_raytrace.h>
#include <obsidian/t_utils.h>
#include <obsidian/i_input.h>
#include <obsidian/u_ui.h>

#define NS_TARGET 16666666 // 1 / 60 seconds

#define DEF_WINDOW_WIDTH  1300
#define DEF_WINDOW_HEIGHT 1300

static void getMemorySizes4k(Obdn_V_MemorySizes* ms) __attribute__ ((unused));
static void getMemorySizes8k(Obdn_V_MemorySizes* ms) __attribute__ ((unused));
static void getMemorySizes16k(Obdn_V_MemorySizes* ms) __attribute__ ((unused));

extern Parms parms;

static void getMemorySizes4k(Obdn_V_MemorySizes* ms)
{
    *ms = (Obdn_V_MemorySizes){
    .hostGraphicsBufferMemorySize          = OBDN_1_GiB,
    .deviceGraphicsBufferMemorySize        = OBDN_256_MiB,
    .deviceGraphicsImageMemorySize         = OBDN_1_GiB,
    .hostTransferBufferMemorySize          = OBDN_1_GiB * 2,
    .deviceExternalGraphicsImageMemorySize = OBDN_100_MiB };
}

static void getMemorySizes8k(Obdn_V_MemorySizes* ms)
{
    *ms = (Obdn_V_MemorySizes){
    .hostGraphicsBufferMemorySize          = OBDN_1_GiB * 2,
    .deviceGraphicsBufferMemorySize        = OBDN_256_MiB,
    .deviceGraphicsImageMemorySize         = OBDN_1_GiB * 2,
    .hostTransferBufferMemorySize          = OBDN_1_GiB * 4,
    .deviceExternalGraphicsImageMemorySize = OBDN_100_MiB };
}

static void getMemorySizes16k(Obdn_V_MemorySizes* ms)
{
    *ms = (Obdn_V_MemorySizes){
    .hostGraphicsBufferMemorySize          = OBDN_1_GiB * 6,
    .deviceGraphicsBufferMemorySize        = OBDN_256_MiB * 2,
    .deviceGraphicsImageMemorySize         = OBDN_1_GiB * 6,
    .hostTransferBufferMemorySize          = OBDN_1_GiB * 8,
    .deviceExternalGraphicsImageMemorySize = OBDN_100_MiB };
}

static Obdn_S_Scene renderScene;
static PaintScene   paintScene;

void painter_Init(uint32_t texSize, bool houdiniMode)
{
    assert(texSize == IMG_4K || texSize == IMG_8K || texSize == IMG_16K);
    Obdn_V_Config config = {};
    config.rayTraceEnabled = true;
#ifndef NDEBUG
    config.validationEnabled = true;
#else
    config.validationEnabled = false;
#endif
    parms.copySwapToHost = houdiniMode;
    const VkImageLayout finalUILayout = parms.copySwapToHost ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    Obdn_D_XcbWindow xcbWindow;
    if (!parms.copySwapToHost)
        xcbWindow = obdn_d_Init(DEF_WINDOW_WIDTH, DEF_WINDOW_HEIGHT, NULL);
    else
    {
        // won't matter really. these will get set by the renderer on first update.
        OBDN_WINDOW_WIDTH  = 1000;
        OBDN_WINDOW_HEIGHT = 1000;
    }
    const char* exnames[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME };
    switch (texSize)
    {
        case IMG_4K:  getMemorySizes4k(&config.memorySizes);  break;
        case IMG_8K:  getMemorySizes8k(&config.memorySizes);  break;
        case IMG_16K: getMemorySizes16k(&config.memorySizes); break;
    }
    obdn_v_Init(&config, OBDN_ARRAY_SIZE(exnames), exnames);
    if (!parms.copySwapToHost)
        obdn_v_InitSurfaceXcb(xcbWindow.connection, xcbWindow.window);
    obdn_v_InitSwapchain(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, parms.copySwapToHost);
    obdn_i_Init();
    obdn_u_Init(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, finalUILayout);
    if (!parms.copySwapToHost)
        painter_LocalInit(texSize);
}

void painter_LocalInit(uint32_t texSize)
{
    printf(">>>> Painter local init\n");

    g_Init(&renderScene, &paintScene);
    p_Init(&renderScene, &paintScene, texSize);
    r_InitRenderer(&renderScene, &paintScene);
}

void painter_StartLoop(void)
{
    Obdn_LoopData loopData = obdn_CreateLoopData(NS_TARGET, 0, 0);

    parms.shouldRun = true;

    renderScene.dirt = ~0;
    paintScene.dirt  = ~0;

    while( parms.shouldRun ) 
    {
        obdn_FrameStart(&loopData);

        if (!parms.copySwapToHost)
            obdn_d_DrainEventQueue();
        obdn_i_ProcessEvents();

        g_Update();
        VkSemaphore s = VK_NULL_HANDLE;
        s = p_Paint(s);
        uint32_t i = obdn_v_RequestFrame(&renderScene.dirt, renderScene.window);
        r_Render(i, s);

        obdn_FrameEnd(&loopData);

        paintScene.dirt = 0;
        renderScene.dirt = 0;
    }
}

void painter_StopLoop(void)
{
    parms.shouldRun = false;
}

void painter_ShutDown(void)
{
    printf("Painter shutdown\n");
    obdn_i_CleanUp();
    obdn_v_CleanUpSwapchain();
    if (!parms.copySwapToHost)
        obdn_d_CleanUp();
    obdn_u_CleanUp();
    obdn_v_CleanUp();
}

void painter_LocalCleanUp(void)
{
    printf(">>>> Painter local cleanup\n");
    vkDeviceWaitIdle(device);
    g_CleanUp();
    r_CleanUp();
}

void painter_SetColor(const float r, const float g, const float b)
{
    g_SetBrushColor(r, g, b);
}

void painter_SetRadius(const float r)
{
    g_SetBrushRadius(r * 0.1);
}

void painter_SetOpacity(const float o)
{
    g_SetBrushOpacity(o);
}

void painter_SetFallOff(const float f)
{
    g_SetBrushFallOff(f);
}
