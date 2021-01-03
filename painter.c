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
#include <tanto/t_utils.h>
#include <tanto/i_input.h>
#include <tanto/u_ui.h>

#define NS_TARGET 16666666 // 1 / 60 seconds
//#define NS_TARGET 500000000
#define NS_PER_S  1000000000

void painter_Init(void)
{
    tanto_v_config.rayTraceEnabled = true;
#ifndef NDEBUG
    tanto_v_config.validationEnabled = true;
#else
    tanto_v_config.validationEnabled = false;
#endif
    tanto_d_Init(NULL);
    tanto_v_Init();
    tanto_v_InitSurfaceXcb(d_XcbWindow.connection, d_XcbWindow.window);
    tanto_r_Init();
    tanto_i_Init();
    tanto_u_Init(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    tanto_i_Subscribe(g_Responder);
    r_InitRenderer();
    g_Init();
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

    r_LoadMesh(mesh);
}

void painter_StartLoop(void)
{
    Tanto_LoopData loopData = tanto_CreateLoopData(NS_TARGET, 0, 0);

    // initialize matrices
    Mat4* xformProj    = r_GetXform(R_XFORM_PROJ);
    Mat4* xformProjInv = r_GetXform(R_XFORM_PROJ_INV);
    *xformProj    = m_BuildPerspective(0.01, 30);
    *xformProjInv = m_Invert4x4(xformProj);

    parms.shouldRun = true;

    while( parms.shouldRun ) 
    {
        tanto_FrameStart(&loopData);

        tanto_r_RequestFrame();

        tanto_i_GetEvents();
        tanto_i_ProcessEvents();

        g_Update();
        r_Render();

        tanto_u_Render();
        tanto_r_PresentFrame();

        tanto_FrameEnd(&loopData);
    }

    if (parms.reload)
    {
        parms.reload = false;
        vkDeviceWaitIdle(device);

        r_ClearMesh();
        Tanto_R_Mesh cube = tanto_r_CreateCube();
        r_LoadMesh(cube);

        painter_StartLoop();
    }
}

void painter_ReloadMesh(Tanto_R_PreMesh pm)
{
    vkDeviceWaitIdle(device);
    r_ClearMesh();

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
    tanto_d_CleanUp();
    tanto_u_CleanUp();
    tanto_v_CleanUp();
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
