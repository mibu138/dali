#include "coal/m_math.h"
#include "obsidian/r_raytrace.h"
#include "obsidian/r_render.h"
#include "obsidian/s_scene.h"
#include "obsidian/v_memory.h"
#include "painter/layer.h"
#include "render.h"
#include "common.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <obsidian/r_geo.h>
#include <obsidian/v_video.h>
#include <obsidian/u_ui.h>
#include <obsidian/r_pipeline.h>
#include <obsidian/f_file.h>
#include <obsidian/v_private.h>
#include <hell/input.h>
#include <hell/evcodes.h>
#include <hell/debug.h>
#include <pthread.h>
#include "g_api.h"
#include "dtags.h"
#include "g_houdini_api.h"

static uint16_t windowWidth;
static uint16_t windowHeight;

static bool g_Responder(const Hell_I_Event *event);

typedef struct {
    Mat4 viewInv;
    Mat4 projInv;
} Cam;

typedef enum {
    TUMBLE,
    PAN,
    ZOOM,
} DragMode ;

struct Drag {
    bool     active;
    Vec2     startPos;
    DragMode mode;
} drag;

static Vec2  mousePos;

typedef enum {
    MODE_PAINT,
    MODE_DO_NOTHING,
    MODE_VIEW,
} Mode;

static Mode  mode;

static struct Player {
    Vec3 pos;
    Vec3 target;
    Vec3 pivot;
} player;

static PaintScene*     paintScene;
static Obdn_S_Scene*   renderScene;

static Obdn_U_Widget* radiusSlider;
static Obdn_U_Widget* opacitySlider;
static Obdn_U_Widget* falloffSlider;
static Obdn_U_Widget* text;

static G_Import gi;

static void setBrushActive(bool active)
{
    paintScene->brush_active = active;
    paintScene->dirt |= SCENE_BRUSH_BIT;
}

static void incrementLayer(void)
{
    L_LayerId id;
    if (gi.incrementLayer(&id))
    {
        paintScene->layer = id;
        paintScene->dirt |= SCENE_LAYER_CHANGED_BIT;
        //if (!u_LayerInCache(id))
        //    paintScene->dirt |= SCENE_LAYER_BACKUP_BIT;
        char str[12];
        snprintf(str, 12, "Layer %d", id + 1);
        obdn_u_UpdateText(str, text);
    }
}

static void decrementLayer(void)
{
    L_LayerId id;
    if (gi.decrementLayer(&id))
    {
        paintScene->layer = id;
        paintScene->dirt |= SCENE_LAYER_CHANGED_BIT;
        //if (!u_LayerInCache(id))
        //    paintScene->dirt |= SCENE_LAYER_BACKUP_BIT;
        char str[12];
        snprintf(str, 12, "Layer %d", id + 1);
        obdn_u_UpdateText(str, text);
    }
}

static void g_SetBrushColor(const float r, const float g, const float b)
{
    paintScene->brush_r = r;
    paintScene->brush_g = g;
    paintScene->brush_b = b;
    paintScene->dirt |= SCENE_BRUSH_BIT;
}

static void g_SetBrushRadius(float r)
{
    if (r < 0.001) r = 0.001; // should not go to 0... may cause div by 0 in shader
    paintScene->brush_radius = r;
    paintScene->dirt |= SCENE_BRUSH_BIT;
}

static void g_CleanUp(void)
{
    obdn_u_DestroyWidget(text);
    hell_i_Unsubscribe(g_Responder);
    memset(&mousePos, 0, sizeof(mousePos));
}

static void g_SetBrushPos(float x, float y)
{
    paintScene->brush_x = x;
    paintScene->brush_y = y;
    paintScene->dirt |= SCENE_BRUSH_BIT;
}

static void g_SetPaintMode(PaintMode mode)
{
    paintScene->paint_mode = mode;
    paintScene->dirt |= SCENE_PAINT_MODE_BIT;
    paintScene->dirt |= SCENE_BRUSH_BIT;
}

static void g_SetBrushOpacity(float opacity)
{
    if (opacity < 0.0) opacity = 0.0;
    if (opacity > 1.0) opacity = 1.0;
    paintScene->brush_opacity = opacity;
    paintScene->dirt |= SCENE_BRUSH_BIT;
}

static void g_SetBrushFallOff(float falloff)
{
    if (falloff < 0.0) falloff = 0.0;
    if (falloff > 1.0) falloff = 1.0;
    paintScene->brush_falloff = falloff;
    paintScene->dirt |= SCENE_BRUSH_BIT;
}

static void g_Init(Obdn_S_Scene* scene_, PaintScene* paintScene_)
{
    assert(scene_);
    assert(paintScene_);
    paintScene = paintScene_;
    renderScene = scene_;

    windowWidth  = renderScene->window[0];
    windowHeight = renderScene->window[1];

    Obdn_S_PrimId primId = 0;
    if (!gi.parms->copySwapToHost)
    {
        Obdn_F_Primitive fprim;
        obdn_f_ReadPrimitive("data/pig.tnt", &fprim);
        Obdn_R_Primitive prim = obdn_f_CreateRPrimFromFPrim(&fprim);
        obdn_f_FreePrimitive(&fprim);
        primId = obdn_s_AddRPrim(renderScene, prim, NULL);
    }
    else
    {
        assert(obdn_s_PrimExists(renderScene, 0) && "we need to load a prim");
    }
    Obdn_S_TextureId texId = ++renderScene->textureCount;
    Obdn_S_MaterialId matId = obdn_s_CreateMaterial(renderScene, (Vec3){1, 1, 1}, 1.0, texId, 0, 0);
    obdn_s_BindPrimToMaterial(renderScene, primId, matId);

    //renderScene->window[0] = OBDN_WINDOW_WIDTH;
    //renderScene->window[1] = OBDN_WINDOW_HEIGHT;

    hell_i_Subscribe(g_Responder, HELL_I_MOUSE_BIT | HELL_I_KEY_BIT | HELL_I_WINDOW_BIT);

    player.pos = (Vec3){0, 0., 3};
    player.target = (Vec3){0, 0, 0};
    player.pivot = player.target;
    g_SetBrushColor(0.1, 0.95, 0.3);
    g_SetBrushRadius(0.01);
    mode = MODE_DO_NOTHING;
    setBrushActive(false);

    text = obdn_u_CreateText(10, 0, "Layer 1", NULL);
}

static bool g_Responder(const Hell_I_Event *event)
{
    switch (event->type) 
    {
        case HELL_I_KEYDOWN: switch (event->data.keyCode)
        {
            case HELL_KEY_G: g_SetPaintMode(PAINT_MODE_ERASE); break;
            case HELL_KEY_F: g_SetPaintMode(PAINT_MODE_OVER); break;
            case HELL_KEY_Z: paintScene->dirt |= SCENE_UNDO_BIT; break;
            case HELL_KEY_ESC: gi.parms->shouldRun = false; break;
            case HELL_KEY_H: decrementLayer(); break;
            case HELL_KEY_K: incrementLayer(); break;
            case HELL_KEY_L: gi.createLayer(); break;
            case HELL_KEY_SPACE: mode = MODE_VIEW; break;
            default: return true;
        } break;
        case HELL_I_KEYUP:   switch (event->data.keyCode)
        {
            case HELL_KEY_SPACE: mode = MODE_DO_NOTHING; break;
            default: return true;
        } break;
        case HELL_I_MOTION: 
        {
            mousePos.x = (float)event->data.mouseData.x / windowWidth;
            mousePos.y = 1 - (float)event->data.mouseData.y / windowHeight;
        } break;
        case HELL_I_MOUSEDOWN: switch (mode) 
        {
            case MODE_DO_NOTHING: mode = MODE_PAINT; break;
            case MODE_VIEW:
            {
                drag.active = true;
                const Vec2 p = {
                    .x = (float)event->data.mouseData.x / windowWidth,
                    .y = 1 - (float)event->data.mouseData.y / windowHeight};
                drag.startPos = p;
                if (event->data.mouseData.buttonCode == HELL_MOUSE_LEFT)
                {
                    drag.mode = TUMBLE;
                }
                if (event->data.mouseData.buttonCode == HELL_MOUSE_MID)
                {
                    drag.mode = PAN;
                }
                if (event->data.mouseData.buttonCode == HELL_MOUSE_RIGHT)
                {
                    drag.mode = ZOOM;
                }
            } break;
            default: break;
        } break;
        case HELL_I_MOUSEUP:
        {
            switch (mode) 
            {
                case MODE_PAINT: mode = MODE_DO_NOTHING; paintScene->dirt |= SCENE_LAYER_BACKUP_BIT; break;
                case MODE_VIEW:  drag.active = false; break;
                default: break;
            }
        } break;
        case HELL_I_RESIZE:
        {
            windowWidth  = event->data.resizeData.width;
            windowHeight = event->data.resizeData.height;
        } break;
        default: break;
    }
    return true;
}

static void setView(const Mat4* m)
{
    renderScene->camera.view = *m;
    renderScene->camera.xform = m_Invert4x4(m);
    renderScene->dirt |= OBDN_S_CAMERA_VIEW_BIT;
}

static void setProj(const Mat4* m)
{
    renderScene->camera.proj = *m;
    renderScene->dirt |= OBDN_S_CAMERA_PROJ_BIT;
}

static uint8_t* loadTexture(const void* data, uint32_t w, uint32_t h, VkFormat format)
{
    hell_DebugPrint(PAINT_DEBUG_TAG_GAME, "Loading texture....\n");
    assert(gi.copyTextureToLayer);
    hell_DebugPrint(PAINT_DEBUG_TAG_GAME, "Copied data to layer 1\n");
    return gi.copyTextureToLayer(1, data, w, h, format);
}

void g_Update(void)
{
    //assert(sizeof(struct Player) == sizeof(UboPlayer));
    //handleKeyMovement();
    if (radiusSlider)
        g_SetBrushRadius(radiusSlider->data.slider.sliderPos * 0.1); // TODO: find a better way
    if (opacitySlider)
        g_SetBrushOpacity(opacitySlider->data.slider.sliderPos);
    if (falloffSlider)
        g_SetBrushFallOff(falloffSlider->data.slider.sliderPos);
    //
    g_SetBrushPos(mousePos.x, mousePos.y);
    if (MODE_PAINT == mode)
        setBrushActive(true);
    else 
        setBrushActive(false);
}

G_Export handshake(G_Import gi_)
{
    gi = gi_;

    G_Export ge = {
        .init        = g_Init,
        .cleanUp     = g_CleanUp,
        .update      = g_Update
    };

    return ge;
}

G_Houdini_Export getFunctions(void)
{
    G_Houdini_Export ex = {
        .loadTexture = loadTexture,
        .setColor    = g_SetBrushColor,
        .setFallOff  = g_SetBrushFallOff,
        .setOpacity  = g_SetBrushOpacity,
        .setRadius   = g_SetBrushRadius,
        .setProj     = setProj,
        .setView     = setView
    };

    return ex;
}
