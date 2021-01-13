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

static Tanto_R_Primitive houMeshToPrim(const Painter_HouMesh pm)
{
    size_t nverts = pm.vertexCount;
    Tanto_R_Primitive prim = tanto_r_CreatePrimitive(nverts, nverts, 3);

    Tanto_R_Attribute* pos = tanto_r_GetPrimAttribute(&prim, 0);
    Tanto_R_Attribute* nor = tanto_r_GetPrimAttribute(&prim, 1);
    Tanto_R_Attribute* uvw = tanto_r_GetPrimAttribute(&prim, 2);
    Tanto_R_Index* indices = tanto_r_GetPrimIndices(&prim);

    memcpy(pos,     pm.posData,   sizeof(Tanto_R_Attribute) * nverts);
    memcpy(nor,     pm.norData,   sizeof(Tanto_R_Attribute) * nverts);
    memcpy(uvw,     pm.uvwData,   sizeof(Tanto_R_Attribute) * nverts);
    memcpy(indices, pm.indexData, sizeof(Tanto_R_Index) * nverts);

    // the new stuff

    tanto_v_TransferToDevice(&prim.vertexRegion);
    tanto_v_TransferToDevice(&prim.indexRegion);

    return prim;
}

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

void painter_LoadHouMesh(Painter_HouMesh m)
{
    Tanto_R_Primitive prim = houMeshToPrim(m);
    free(m.posData);
    free(m.norData);
    free(m.uvwData);
    free(m.indexData);

    r_LoadPrim(prim);
}

void painter_ReloadHouMesh(Painter_HouMesh pm)
{
    vkDeviceWaitIdle(device);
    r_ClearPrim();

    painter_LoadHouMesh(pm);
    painter_StartLoop();
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

        r_ClearPrim();
        Tanto_R_Primitive cube = tanto_r_CreateCubePrim(true);
        r_LoadPrim(cube);
        printf("RELOAD!\n");

        painter_StartLoop();
    }
    else if (parms.restart)
    {
        parms.restart = false;
        vkDeviceWaitIdle(device);

        painter_ShutDown();
        painter_Init();
        Tanto_R_Primitive cube = tanto_r_CreateCubePrim(true);
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
