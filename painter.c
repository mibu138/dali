#include "painter.h"
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
#include <obsidian/r_render.h>
#include <obsidian/r_raytrace.h>
#include <obsidian/t_utils.h>
#include <obsidian/i_input.h>
#include <obsidian/u_ui.h>

#define NS_TARGET 16666666 // 1 / 60 seconds

#define DEF_WINDOW_WIDTH  1300
#define DEF_WINDOW_HEIGHT 1300

#define IMG_4K  4096
#define IMG_8K  IMG_4K * 2
#define IMG_16K IMG_8K * 2

#define IMG_SIZE IMG_4K

static void getMemorySizes4k(Obdn_V_MemorySizes* ms) __attribute__ ((unused));
static void getMemorySizes8k(Obdn_V_MemorySizes* ms) __attribute__ ((unused));
static void getMemorySizes16k(Obdn_V_MemorySizes* ms) __attribute__ ((unused));

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
    .hostGraphicsBufferMemorySize          = OBDN_1_GiB * 4,
    .deviceGraphicsBufferMemorySize        = OBDN_256_MiB * 2,
    .deviceGraphicsImageMemorySize         = OBDN_1_GiB * 6,
    .hostTransferBufferMemorySize          = OBDN_1_GiB * 8,
    .deviceExternalGraphicsImageMemorySize = OBDN_100_MiB };
}

void painter_Init(bool houdiniMode)
{
    obdn_v_config.rayTraceEnabled = true;
#ifndef NDEBUG
    obdn_v_config.validationEnabled = true;
#else
    obdn_v_config.validationEnabled = false;
#endif
    parms.copySwapToHost = houdiniMode;
    const VkImageLayout finalUILayout = parms.copySwapToHost ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    Obdn_D_XcbWindow xcbWindow;
    if (!parms.copySwapToHost)
        xcbWindow = obdn_d_Init(DEF_WINDOW_WIDTH, DEF_WINDOW_HEIGHT, NULL);
    else
    {
        // won't matter really. these will get set by the renderer on first update.
        OBDN_WINDOW_WIDTH = 1000;
        OBDN_WINDOW_HEIGHT = 1000;
    }
    const char* exnames[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME };
    Obdn_V_MemorySizes ms;
#if   IMG_SIZE == IMG_4K
    getMemorySizes4k(&ms);
#elif IMG_SIZE == IMG_8K
    getMemorySizes8k(&ms);
#elif IMG_SIZE == IMG_16K
    getMemorySizes16k(&ms);
#endif
    obdn_v_Init(&ms, OBDN_ARRAY_SIZE(exnames), exnames);
    if (!parms.copySwapToHost)
        obdn_v_InitSurfaceXcb(xcbWindow.connection, xcbWindow.window);
    obdn_r_Init(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, parms.copySwapToHost);
    obdn_i_Init();
    obdn_u_Init(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, finalUILayout);
    if (!parms.copySwapToHost)
    {
#if   IMG_SIZE == IMG_4K
        painter_LocalInit(IMG_4K);
#elif IMG_SIZE == IMG_8K
        painter_LocalInit(IMG_8K);
#elif IMG_SIZE == IMG_16K
        painter_LocalInit(IMG_16K);
#endif
    }
}

void painter_LocalInit(uint32_t texSize)
{
    printf(">>>> Painter local init\n");
    r_InitRenderer(texSize);
    g_Init();
}

void painter_LoadFprim(Obdn_F_Primitive* fprim)
{
    Obdn_R_Primitive prim = obdn_f_CreateRPrimFromFPrim(fprim);
    obdn_f_FreePrimitive(fprim);
    r_LoadPrim(prim);
}

void painter_ReloadPrim(Obdn_F_Primitive* fprim)
{
    vkDeviceWaitIdle(device);
    r_ClearPrim();

    painter_LoadFprim(fprim);
    //painter_StartLoop();
}

void painter_StartLoop(void)
{
    Obdn_LoopData loopData = obdn_CreateLoopData(NS_TARGET, 0, 0);

    parms.shouldRun = true;

    while( parms.shouldRun ) 
    {
        obdn_FrameStart(&loopData);

        if (!parms.copySwapToHost)
            obdn_d_DrainEventQueue();
        obdn_i_ProcessEvents();

        g_Update();
        r_Render();

        obdn_FrameEnd(&loopData);
    }

    if (parms.reload)
    {
        parms.reload = false;
        vkDeviceWaitIdle(device);

        r_ClearPrim();
        Obdn_R_Primitive cube = obdn_r_CreateCubePrim(true);
        r_LoadPrim(cube);
        printf("RELOAD!\n");

        painter_StartLoop();
    }
    else if (parms.restart)
    {
        parms.restart = false;
        vkDeviceWaitIdle(device);

        painter_ShutDown();
        painter_Init(parms.copySwapToHost);
        Obdn_R_Primitive cube = obdn_r_CreateCubePrim(true);
        r_LoadPrim(cube);
        printf("RESTART!\n");

        painter_StartLoop();
    }
}

void painter_StopLoop(void)
{
    parms.shouldRun = false;
}

void painter_ShutDown(void)
{
    printf("Painter shutdown\n");
    painter_LocalCleanUp();
    obdn_i_CleanUp();
    obdn_r_CleanUp();
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

bool painter_ShouldRun(void)
{
    return gameState.shouldRun;
}

void painter_SetColor(const float r, const float g, const float b)
{
    g_SetBrushColor(r, g, b);
}

void painter_SetRadius(const float r)
{
    g_SetBrushRadius(r);
}

