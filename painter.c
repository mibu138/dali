#include "painter.h"
#include "game.h"
#include "common.h"
#include "render.h"
#include "tanto/r_geo.h"

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <stdbool.h>
#include <tanto/v_video.h>
#include <tanto/d_display.h>
#include <tanto/r_render.h>
#include <tanto/r_raytrace.h>
#include <tanto/m_math.h>
#include <tanto/t_utils.h>

#define NS_TARGET 16666666 // 1 / 60 seconds
//#define NS_TARGET 500000000
#define NS_PER_S  1000000000

struct Timer {
    struct timespec startTime;
    struct timespec endTime;
    clockid_t clockId;
    void (*startFn)(struct Timer*);
    void (*stopFn)(struct Timer*);
} timer;

static void timerStart(struct Timer* t)
{
    clock_gettime(t->clockId, &t->startTime);
}

static void timerStop(struct Timer* t)
{
    clock_gettime(t->clockId, &t->endTime);
}

struct Stats {
    uint64_t frameCount;
    uint64_t nsTotal;
    unsigned long nsDelta;
    uint32_t shortestFrame;
    uint32_t longestFrame;
} stats;

static void updateStats(const struct Timer* t, struct Stats* s)
{
    s->nsDelta  = (t->endTime.tv_sec * NS_PER_S + t->endTime.tv_nsec) - (t->startTime.tv_sec * NS_PER_S + t->startTime.tv_nsec);
    s->nsTotal += s->nsDelta;

    if (s->nsDelta > s->longestFrame) s->longestFrame = s->nsDelta;
    if (s->nsDelta < s->shortestFrame) s->shortestFrame = s->nsDelta;

    s->frameCount++;
}

static void sleepLoop(const struct Stats* s)
{
    struct timespec diffTime;
    diffTime.tv_nsec = NS_TARGET > s->nsDelta ? NS_TARGET - s->nsDelta : 0;
    diffTime.tv_sec  = 0;
    // we could use the second parameter to handle interrupts and signals
    nanosleep(&diffTime, NULL);
}

static void initTimer(void)
{
    memset(&timer, 0, sizeof(timer));
    timer.clockId = CLOCK_MONOTONIC;
    timer.startFn = timerStart;
    timer.stopFn  = timerStop;
}

static void initStats(void)
{
    memset(&stats, 0, sizeof(stats));
    stats.longestFrame = UINT32_MAX;
}

void painter_Init(void)
{
    tanto_v_config.rayTraceEnabled = true;
    tanto_d_Init();
    printf("Display initialized\n");
    tanto_v_Init();
    printf("Video initialized\n");
    tanto_v_InitSurfaceXcb(d_XcbWindow.connection, d_XcbWindow.window);
    tanto_v_InitSwapchain(NULL);
    printf("Swapchain initialized\n");
    tanto_r_Init();
    printf("Renderer initialized\n");
    tanto_i_Init();
    printf("Input initialized\n");
    tanto_i_Subscribe(g_Responder);
    r_InitRenderer();
    g_Init();
    g_BindToView(r_GetXform(R_XFORM_VIEW), r_GetXform(R_XFORM_VIEW_INV));
    g_BindToBrush(r_GetBrush());
}

void painter_LoadMesh(Tanto_R_Mesh m)
{
    r_LoadMesh(m);
}

void painter_LoadPreMesh(Tanto_R_PreMesh m)
{
    Tanto_R_Mesh mesh = tanto_r_PreMeshToMesh(m);
    free(m.posData);
    free(m.colData);
    free(m.norData);
    free(m.uvwData);
    free(m.indexData);

    painter_LoadMesh(mesh);
}

void painter_StartLoop(void)
{
    initTimer();
    initStats();

    // initialize matrices
    Mat4* xformProj    = r_GetXform(R_XFORM_PROJ);
    Mat4* xformProjInv = r_GetXform(R_XFORM_PROJ_INV);
    *xformProj    = m_BuildPerspective(0.1, 30);
    *xformProjInv = m_Invert4x4(xformProj);

    parms.shouldRun = true;
    parms.renderNeedsUpdate = false;

    while( parms.shouldRun ) 
    {
        timer.startFn(&timer);

        tanto_i_GetEvents();
        tanto_i_ProcessEvents();

        //r_WaitOnQueueSubmit(); // possibly don't need this due to render pass

        g_Update();

        if (parms.renderNeedsUpdate || stats.frameCount == 0 ) 
        {
            for (int i = 0; i < TANTO_FRAME_COUNT; i++) 
            {
                if (parms.renderNeedsUpdate)
                    tanto_r_WaitOnQueueSubmit();
                tanto_r_RequestFrame();
                r_UpdateRenderCommands();
                tanto_r_PresentFrame();
            }
            parms.renderNeedsUpdate = false;
        }
        else
        {
            tanto_r_RequestFrame();
            tanto_r_PresentFrame();
        }

        timer.stopFn(&timer);

        updateStats(&timer, &stats);

        printf("Delta ns: %ld\n", stats.nsDelta);

        sleepLoop(&stats);
    }
    if (parms.reload)
    {
        parms.reload = false;
        vkDeviceWaitIdle(device);

        r_ClearMesh();
        Tanto_R_Mesh cube = tanto_r_CreateCube();
        r_LoadMesh(cube);

        painter_StartLoop();

        //vkDeviceWaitIdle(device);
        //r_CleanUp();
        //tanto_r_RayTraceCleanUp();
        //tanto_v_CleanUpMemory();

        //vkDeviceWaitIdle(device);

        //tanto_v_InitMemory();
        //tanto_r_InitRayTracing();
        //Tanto_R_Mesh cube = tanto_r_CreateCube();
        //painter_LoadMesh(cube);
        //painter_StartLoop();
    }
}

void painter_ReloadMesh(Tanto_R_PreMesh pm)
{
    vkDeviceWaitIdle(device);
    r_CleanUp();
    tanto_r_RayTraceCleanUp();
    tanto_v_CleanUpMemory();

    vkDeviceWaitIdle(device);

    tanto_v_InitMemory();
    tanto_r_InitRayTracing();
    painter_LoadPreMesh(pm);
    painter_StartLoop();
}

void painter_StopLoop(void)
{
    parms.shouldRun = false;
}

void painter_ShutDown(void)
{
    vkDeviceWaitIdle(device);
    r_CleanUp();
    tanto_i_CleanUp();
    tanto_r_CleanUp();
    tanto_v_CleanUp();
    tanto_d_CleanUp();
}

bool painter_ShouldRun(void)
{
    return gameState.shouldRun;
}

void painter_SetColor(const float r, const float g, const float b)
{
    g_SetColor(r, g, b);
}

void painter_SetRadius(const float r)
{
    g_SetRadius(r);
}
