#include "painter.h"
#include "game.h"
#include "common.h"
#include "render.h"

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

static Tanto_R_Mesh mesh;

void painter_Init(void)
{
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
    g_Init();
}

void painter_LoadMesh(Tanto_R_Mesh m)
{
    mesh = m;

    r_LoadMesh(&mesh);
    parms.mode = MODE_RAY;
    tanto_r_BuildBlas(&mesh);
    tanto_r_BuildTlas();

    r_InitRenderCommands();
    g_BindToView(r_GetXform(R_XFORM_VIEW), r_GetXform(R_XFORM_VIEW_INV));
}

void painter_LoadPreMesh(Tanto_R_PreMesh m)
{
    mesh = tanto_r_PreMeshToMesh(m);
    r_LoadMesh(&mesh);

    free(m.posData);
    free(m.colData);
    free(m.norData);
    free(m.indexData);
    //TODO should have a specialized free function. all allocation and freeing should be down in r_geo

    parms.mode = MODE_RAY;
    tanto_r_BuildBlas(&mesh);
    tanto_r_BuildTlas();

    r_InitRenderCommands();
    g_BindToView(r_GetXform(R_XFORM_VIEW), r_GetXform(R_XFORM_VIEW_INV));
}

void painter_StartLoop(void)
{
    struct timespec startTime = {0, 0};
    struct timespec endTime = {0, 0};
    struct timespec diffTime = {0, 0};
    struct timespec remTime = {0, 0}; // this is just if we get signal interupted

    uint64_t frameCount   = 0;
    uint64_t nsTotal      = 0;
    unsigned long nsDelta = 0;
    uint32_t shortestFrame = NS_PER_S;
    uint32_t longestFrame = 0;

    // initialize matrices
    Mat4* xformProj    = r_GetXform(R_XFORM_PROJ);
    Mat4* xformProjInv = r_GetXform(R_XFORM_PROJ_INV);
    *xformProj    = m_BuildPerspective(0.1, 30);
    *xformProjInv = m_Invert4x4(xformProj);

    parms.shouldRun = true;
    parms.renderNeedsUpdate = false;

    while( parms.shouldRun ) 
    {
        clock_gettime(CLOCK_MONOTONIC, &startTime);

        tanto_i_GetEvents();
        tanto_i_ProcessEvents();

        //r_WaitOnQueueSubmit(); // possibly don't need this due to render pass

        g_Update();

        if (parms.renderNeedsUpdate || frameCount == 0 ) 
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

        clock_gettime(CLOCK_MONOTONIC, &endTime);

        nsDelta  = (endTime.tv_sec * NS_PER_S + endTime.tv_nsec) - (startTime.tv_sec * NS_PER_S + startTime.tv_nsec);
        nsTotal += nsDelta;

        //printf("Delta ns: %ld\n", nsDelta);

        if (nsDelta > longestFrame) longestFrame = nsDelta;
        if (nsDelta < shortestFrame) shortestFrame = nsDelta;

        diffTime.tv_nsec = NS_TARGET > nsDelta ? NS_TARGET - nsDelta : 0;

        //assert ( NS_TARGET > nsDelta );

        nanosleep(&diffTime, &remTime);

        frameCount++;
    }
}

void painter_ReloadMesh(Tanto_R_PreMesh pm)
{
    vkDeviceWaitIdle(device);
    r_CommandCleanUp();
    tanto_r_RayTraceCleanUp();
    tanto_v_CleanUpMemory();

    tanto_v_InitMemory();
    tanto_r_InitRayTracing();
    painter_LoadPreMesh(pm);
    painter_StartLoop();
}

void painter_StopLoop(void)
{
    parms.shouldRun = false;
}
