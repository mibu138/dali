#include "painter.h"
#include "obsidian/s_scene.h"
#include "paint.h"
#include "common.h"
#include "obsidian/t_def.h"
#include "render.h"
#include "obsidian/f_file.h"
#include "obsidian/r_geo.h"
#include "undo.h"

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
#include <obsidian/v_swapchain.h>
#include <obsidian/r_raytrace.h>
#include <obsidian/u_ui.h>
#include <obsidian/v_private.h>
#include <hell/input.h>
#include <hell/display.h>
#include <hell/cmd.h>
#include <dlfcn.h>
#include "g_api.h"

#define NS_TARGET 16666666 // 1 / 60 seconds

#define DEF_WINDOW_WIDTH  1500
#define DEF_WINDOW_HEIGHT 1500

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
    .hostGraphicsBufferMemorySize          = OBDN_1_GiB * 6,
    .deviceGraphicsBufferMemorySize        = OBDN_256_MiB * 2,
    .deviceGraphicsImageMemorySize         = OBDN_1_GiB * 6,
    .hostTransferBufferMemorySize          = OBDN_1_GiB * 8,
    .deviceExternalGraphicsImageMemorySize = OBDN_100_MiB };
}

static Obdn_S_Scene renderScene;
static PaintScene   paintScene;
static G_Export     ge;
static Parms        parms;

void painter_Init(uint32_t texSize, bool houdiniMode, const char* gModuleName)
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
    const Hell_Window* window = NULL;
    if (!parms.copySwapToHost)
        window = hell_d_Init(DEF_WINDOW_WIDTH, DEF_WINDOW_HEIGHT, NULL);
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
    obdn_v_InitSwapchain(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, window);
    hell_i_Init(!parms.copySwapToHost);
    hell_c_Init();
    obdn_u_Init(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, finalUILayout);
    //obdn_s_CreateEmptyScene(&renderScene);
    obdn_s_Init(&renderScene, DEF_WINDOW_WIDTH, DEF_WINDOW_HEIGHT, 0.01, 1000);

    char gmodbuf[32];
    assert(strnlen(gModuleName, 32) < 29);
    strncpy(gmodbuf, gModuleName, 28);
    strcat(gmodbuf, ".so");

    void* game = dlopen(gmodbuf, RTLD_LAZY);
    assert(game);
    printf("Game module imported successfully.\n");
    void* g_entry = dlsym(game, "handshake");
    assert(g_entry);
    printf("Game handshake function found.\n");
    G_Handshake handshake = g_entry;

    G_Import gi = {
        .createLayer    = l_CreateLayer,
        .decrementLayer = l_DecrementLayer,
        .incrementLayer = l_IncrementLayer,
        .parms          = &parms
    };
    ge = handshake(gi);

    if (!parms.copySwapToHost)
        painter_LocalInit(texSize);
}

void painter_LocalInit(uint32_t texSize)
{
    printf(">>>> Painter local init\n");

    ge.init(&renderScene, &paintScene);
    p_Init(&renderScene, &paintScene, texSize);
    r_InitRenderer(&renderScene, &paintScene, parms.copySwapToHost);
}

void painter_StartLoop(void)
{
    parms.shouldRun = true;

    renderScene.dirt = ~0;
    paintScene.dirt  = ~0;

    while( parms.shouldRun ) 
    {
        hell_i_PumpEvents();
        hell_i_DrainEvents();
        hell_c_Execute();

        ge.update();
        u_Update(&paintScene);
        VkSemaphore s = VK_NULL_HANDLE;
        s = p_Paint(s);
        uint32_t i = obdn_v_RequestFrame(&renderScene.dirt, renderScene.window);
        r_Render(i, s);

        paintScene.dirt = 0;
        renderScene.dirt = 0;
    }

    painter_LocalCleanUp();
    painter_ShutDown();
}

void painter_StopLoop(void)
{
    parms.shouldRun = false;
}

void painter_ShutDown(void)
{
    printf("Painter shutdown\n");
    p_CleanUp();
    obdn_v_CleanUpSwapchain();
    if (!parms.copySwapToHost)
        hell_d_CleanUp();
    obdn_u_CleanUp();
    obdn_v_CleanUp();
    hell_i_CleanUp();
}

void painter_LocalCleanUp(void)
{
    printf(">>>> Painter local cleanup\n");
    vkDeviceWaitIdle(device);
    ge.cleanUp();
    r_CleanUp();
}

void painter_SetColor(const float r, const float g, const float b)
{
}

void painter_SetRadius(const float r)
{
}

void painter_SetOpacity(const float o)
{
}

void painter_SetFallOff(const float f)
{
}
