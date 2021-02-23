#include "game.h"
#include "coal/m_math.h"
#include "obsidian/r_render.h"
#include "render.h"
#include "layer.h"
#include "undo.h"
#include "common.h"
#include "obsidian/t_utils.h"
#include <assert.h>
#include <string.h>
#include <obsidian/t_def.h>
#include <obsidian/i_input.h>
#include <obsidian/v_video.h>
#include <obsidian/u_ui.h>
#include <pthread.h>

static bool pivotChanged;

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

Parms parms;

// order matters here since we memcpy to a matching ubo
static struct Player {
    Vec3 pos;
    Vec3 target;
    Vec3 pivot;
} player;

G_GameState gameState;

static Scene          scene;
static Scene_DirtMask dirt;

bool firstFrame;

const Vec3 UP_VEC = {0, 1, 0};

static Obdn_U_Widget* slider0;
static Obdn_U_Widget* text;

static void setViewerPivotByIntersection(void)
{
    Vec3 hitPos;
    int r = r_GetSelectionPos(&hitPos);
    if (r)
    {
        player.pivot = hitPos;
    }
}

static void lerpTargetToPivot(void)
{
    const float inc = 0.001;
    static float t = 0.0;
    if (pivotChanged)
    {
        t = 0.0;
    }
    t += inc;
    if (t >= 1.0) return;
    player.target = m_Lerp_Vec3(&player.target, &player.pivot, t);
}

static Mat4 generatePlayerView(void)
{
    Mat4 m = m_LookAt(&player.pos, &player.target, &UP_VEC);
    return m_Invert4x4(&m);
    //return m;
}

static void setPaintMode(PaintMode mode)
{
}

static void setBrushActive(bool active)
{
    scene.brush_active = active;
    dirt |= SCENE_BRUSH_BIT;
}


static void handleMouseMovement(void)
{
    static struct Player cached;
    static bool   cachedDrag = false;
    if (drag.active)
    {
        if (!cachedDrag)
        {
            cached = player;
            cachedDrag = true;
        }
        switch (drag.mode) 
        {
            case TUMBLE: 
            {
                float angleY = mousePos.x - drag.startPos.x;
                float angleX = mousePos.y - drag.startPos.y;
                angleY *= -3.14;
                angleX *=  3.14;
                const Vec3 pivotToPos = m_Sub_Vec3(&cached.pos, &cached.pivot);
                const Vec3 z = m_Normalize_Vec3(&pivotToPos);
                Vec3 temp = m_Cross(&z, &UP_VEC);
                const Vec3 x = m_Normalize_Vec3(&temp);
                const Mat4 rotY = m_BuildRotate(angleY, &UP_VEC);
                const Mat4 rotX = m_BuildRotate(angleX, &x);
                const Mat4 rot = m_Mult_Mat4(&rotX, &rotY);
                Vec3 pos = m_Sub_Vec3(&cached.pos, &cached.pivot);
                pos = m_Mult_Mat4Vec3(&rot, &pos);
                pos = m_Add_Vec3(&pos, &cached.pivot);
                player.pos = pos;
                lerpTargetToPivot();
            } break;
            case PAN: 
            {
                float deltaX = mousePos.x - drag.startPos.x;
                float deltaY = mousePos.y - drag.startPos.y;
                deltaX *= 3;
                deltaY *= 3;
                Vec3 temp = m_Sub_Vec3(&cached.pos, &cached.target);
                const Vec3 z = m_Normalize_Vec3(&temp);
                temp = m_Cross(&z, &UP_VEC);
                Vec3 x = m_Normalize_Vec3(&temp);
                temp = m_Cross(&x, &z);
                Vec3 y = m_Normalize_Vec3(&temp);
                x = m_Scale_Vec3(deltaX, &x);
                y = m_Scale_Vec3(deltaY, &y);
                const Vec3 delta = m_Add_Vec3(&x, &y);
                player.pos = m_Add_Vec3(&cached.pos, &delta);
                player.target = m_Add_Vec3(&cached.target, &delta);
            } break;
            case ZOOM: 
            {
                float deltaX = mousePos.x - drag.startPos.x;
                //float deltaY = mousePos.y - drag.startPos.y;
                //float scale = -1 * (deltaX + deltaY * -1);
                float scale = -1 * deltaX;
                Vec3 temp = m_Sub_Vec3(&cached.pos, &cached.pivot);
                Vec3 z = m_Normalize_Vec3(&temp);
                z = m_Scale_Vec3(scale, &z);
                player.pos = m_Add_Vec3(&cached.pos, &z);
                lerpTargetToPivot();
            } break;
            default: break;
        }
    }
    else
    {
        cachedDrag = false;
    }
}

static void incrementLayer(void)
{
    L_LayerId id;
    if (l_IncrementLayer(&id))
    {
        scene.layer = id;
        dirt |= SCENE_LAYER_CHANGED_BIT;
        if (!u_LayerInCache(id))
            dirt |= SCENE_LAYER_BACKUP_BIT;
        char str[12];
        snprintf(str, 12, "Layer %d", id + 1);
        obdn_u_UpdateText(str, text);
    }
}

static void decrementLayer(void)
{
    L_LayerId id;
    if (l_DecrementLayer(&id))
    {
        scene.layer = id;
        dirt |= SCENE_LAYER_CHANGED_BIT;
        if (!u_LayerInCache(id))
            dirt |= SCENE_LAYER_BACKUP_BIT;
        char str[12];
        snprintf(str, 12, "Layer %d", id + 1);
        obdn_u_UpdateText(str, text);
    }
}

void g_Init(void)
{
    obdn_i_Subscribe(g_Responder);

    Mat4 initView = m_Ident_Mat4();
    Mat4 initProj = m_BuildPerspective(0.01, 50);
    g_SetView(&initView);
    g_SetProj(&initProj);

    player.pos = (Vec3){0, 0., 3};
    player.target = (Vec3){0, 0, 0};
    player.pivot = player.target;
    g_SetBrushColor(0.1, 0.95, 0.3);
    g_SetBrushRadius(0.01);
    mode = MODE_DO_NOTHING;
    setBrushActive(false);
    gameState.shouldRun = true;
    setPaintMode(PAINT_MODE_OVER);

    text = obdn_u_CreateText(10, 0, "Layer 1", NULL);
    if (!parms.copySwapToHost)
        slider0 = obdn_u_CreateSlider(0, 80, NULL);

    r_BindScene(&scene);
    u_BindScene(&scene);
}

bool g_Responder(const Obdn_I_Event *event)
{
    switch (event->type) 
    {
        case OBDN_I_KEYDOWN: switch (event->data.keyCode)
        {
            case OBDN_KEY_E: g_SetPaintMode(PAINT_MODE_ERASE); break;
            case OBDN_KEY_W: g_SetPaintMode(PAINT_MODE_OVER); break;
            case OBDN_KEY_Z: dirt |= SCENE_UNDO_BIT; break;
            case OBDN_KEY_R: g_SetBrushColor(1, 0, 0); break;
            case OBDN_KEY_G: g_SetBrushColor(0, 1, 0); break;
            case OBDN_KEY_B: g_SetBrushColor(0, 0, 1); break;
            case OBDN_KEY_P: r_SavePaintImage(); break;
            case OBDN_KEY_J: decrementLayer(); break;
            case OBDN_KEY_K: incrementLayer(); break;
            case OBDN_KEY_L: l_CreateLayer(); break;
            case OBDN_KEY_SPACE: mode = MODE_VIEW; break;
            case OBDN_KEY_ESC: parms.shouldRun = false; gameState.shouldRun = false; break;
            case OBDN_KEY_C: r_ClearPaintImage(); break;
            case OBDN_KEY_I: break;
            default: return true;
        } break;
        case OBDN_I_KEYUP:   switch (event->data.keyCode)
        {
            case OBDN_KEY_SPACE: mode = MODE_DO_NOTHING; break;
            default: return true;
        } break;
        case OBDN_I_MOTION: 
        {
            mousePos.x = (float)event->data.mouseData.x / OBDN_WINDOW_WIDTH;
            mousePos.y = (float)event->data.mouseData.y / OBDN_WINDOW_HEIGHT;
        } break;
        case OBDN_I_MOUSEDOWN: switch (mode) 
        {
            case MODE_DO_NOTHING: mode = MODE_PAINT; break;
            case MODE_VIEW:
            {
                drag.active = true;
                const Vec2 p = {
                    .x = (float)event->data.mouseData.x / OBDN_WINDOW_WIDTH,
                    .y = (float)event->data.mouseData.y / OBDN_WINDOW_HEIGHT };
                drag.startPos = p;
                if (event->data.mouseData.buttonCode == OBDN_MOUSE_LEFT)
                {
                    drag.mode = TUMBLE;
                    pivotChanged = true;
                }
                if (event->data.mouseData.buttonCode == OBDN_MOUSE_MID)
                {
                    drag.mode = PAN;
                }
                if (event->data.mouseData.buttonCode == OBDN_MOUSE_RIGHT)
                {
                    drag.mode = ZOOM;
                    pivotChanged = true;
                }
            } break;
            default: break;
        } break;
        case OBDN_I_MOUSEUP:
        {
            switch (mode) 
            {
                case MODE_PAINT: mode = MODE_DO_NOTHING; dirt |= SCENE_LAYER_BACKUP_BIT; break;
                case MODE_VIEW:  drag.active = false; break;
                default: break;
            }
        } break;
        default: break;
    }
    return true;
}

void g_Update(void)
{
    //assert(sizeof(struct Player) == sizeof(UboPlayer));
    //handleKeyMovement();
    if (slider0)
        g_SetBrushRadius(slider0->data.slider.sliderPos * 0.1); // TODO: find a better way
    //
    g_SetBrushPos(mousePos.x, mousePos.y);
    if (pivotChanged)
    {
        setViewerPivotByIntersection();
    }
    if (!parms.copySwapToHost)
    {
        handleMouseMovement();
        pivotChanged = false; //TODO must set to false after handleMouseMovement since it checks this... should find a better way
        Mat4 view = generatePlayerView();
        g_SetView(&view);
    }
    if (MODE_PAINT == mode)
        setBrushActive(true);
    else 
        setBrushActive(false);

    // the extra, local mask allows us to clear the dirt mask for the next frame while maintaining the current frame dirty bits
    scene.dirt = dirt;
    dirt = 0;

    u_Update();
}

void g_SetBrushColor(const float r, const float g, const float b)
{
    scene.brush_r = r;
    scene.brush_g = g;
    scene.brush_b = b;
    dirt |= SCENE_BRUSH_BIT;
}

void g_SetBrushRadius(const float r)
{
    scene.brush_radius = r;
    dirt |= SCENE_BRUSH_BIT;
}

void g_CleanUp(void)
{
    if (!parms.copySwapToHost)
        obdn_u_DestroyWidget(slider0);
    obdn_u_DestroyWidget(text);
    obdn_i_Unsubscribe(g_Responder);
    memset(&scene, 0, sizeof(scene));
    memset(&mousePos, 0, sizeof(mousePos));
}

void g_SetView(const Mat4* m)
{
    scene.view = *m;
    dirt |= SCENE_VIEW_BIT;
}

void g_SetProj(const Mat4* m)
{
    scene.proj = *m;
    dirt |= SCENE_PROJ_BIT;
}

void g_SetWindow(uint32_t width, uint32_t height)
{
    // TODO: make this safe some how... 
    scene.window_width  = width;
    scene.window_height = height;
    dirt |= SCENE_WINDOW_BIT;
}

void g_SetBrushPos(float x, float y)
{
    scene.brush_x = x;
    scene.brush_y = y;
    dirt |= SCENE_BRUSH_BIT;
}

void g_SetPaintMode(PaintMode mode)
{
    scene.paint_mode = mode;
    dirt |= SCENE_PAINT_MODE_BIT;
    dirt |= SCENE_BRUSH_BIT;
}
