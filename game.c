#include "game.h"
#include "coal/m_math.h"
#include "obsidian/r_render.h"
#include "render.h"
#include "layer.h"
#include "common.h"
#include "obsidian/t_utils.h"
#include <assert.h>
#include <string.h>
#include <obsidian/t_def.h>
#include <obsidian/i_input.h>
#include <obsidian/v_video.h>
#include <obsidian/u_ui.h>
#include <pthread.h>


static bool zoomIn;
static bool zoomOut;
static bool tumbleUp;
static bool tumbleDown;
static bool tumbleLeft;
static bool tumbleRight;
static bool moveUp;
static bool moveDown;

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

static float brushX;
static float brushY;
static Vec3  brushColor;
static float brushRadius;
static Mode  mode;

Parms parms;

static pthread_mutex_t viewLock;
static pthread_mutex_t projLock;
static Mat4            view;
static Mat4            proj;
bool projDirty;
bool windowDirty;

// order matters here since we memcpy to a matching ubo
static struct Player {
    Vec3 pos;
    Vec3 target;
    Vec3 pivot;
} player;

G_GameState gameState;

Scene scene;

Brush* brush;

bool firstFrame;

const Vec3 UP_VEC = {0, 1, 0};

static Obdn_U_Widget* slider0;

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
                float deltaY = mousePos.y - drag.startPos.y;
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

bool g_Responder(const Obdn_I_Event *event)
{
    switch (event->type) 
    {
        case OBDN_I_KEYDOWN: switch (event->data.keyCode)
        {
            case OBDN_KEY_W: zoomIn = true; break;
            //case OBDN_KEY_S: zoomOut = true; break;
            //case OBDN_KEY_S: brushColor = (Vec3){1, 1, 1}; r_SetPaintMode(PAINT_MODE_ERASE); break;
            case OBDN_KEY_A: tumbleLeft = true; break;
            //case OBDN_KEY_D: tumbleRight = true; break;
            case OBDN_KEY_D: r_SetPaintMode(PAINT_MODE_OVER); break;
            //case OBDN_KEY_E: moveUp = true; break;
            //case OBDN_KEY_Q: moveDown = true; break;
            case OBDN_KEY_E: brushColor = (Vec3){0, 0, 1}; break;
            case OBDN_KEY_B: r_BackUpLayer(); break;
            case OBDN_KEY_Z: r_Undo(); break;
            case OBDN_KEY_Q: brushColor = (Vec3){1, 0, 0}; break;
            case OBDN_KEY_P: r_SavePaintImage(); break;
            case OBDN_KEY_J: l_SetActiveLayer(l_GetActiveLayerId() -1); break;
            case OBDN_KEY_L: l_CreateLayer(); break;
            case OBDN_KEY_SPACE: mode = MODE_VIEW; break;
            case OBDN_KEY_CTRL: tumbleDown = true; break;
            case OBDN_KEY_ESC: parms.shouldRun = false; gameState.shouldRun = false; break;
            case OBDN_KEY_R:    parms.shouldRun = false; parms.restart = true; break;
            case OBDN_KEY_K: l_SetActiveLayer(l_GetActiveLayerId() + 1);
            case OBDN_KEY_C: r_ClearPaintImage(); break;
            case OBDN_KEY_I: break;
            default: return true;
        } break;
        case OBDN_I_KEYUP:   switch (event->data.keyCode)
        {
            case OBDN_KEY_W: zoomIn = false; break;
            case OBDN_KEY_S: zoomOut = false; break;
            case OBDN_KEY_A: tumbleLeft = false; break;
            case OBDN_KEY_D: tumbleRight = false; break;
            case OBDN_KEY_E: moveUp = false; break;
            case OBDN_KEY_Q: moveDown = false; break;
            case OBDN_KEY_SPACE: mode = MODE_DO_NOTHING; break;
            case OBDN_KEY_CTRL: tumbleDown = false; break;
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
                    .y = (float)event->data.mouseData.y / OBDN_WINDOW_HEIGHT
                };
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
                case MODE_PAINT: mode = MODE_DO_NOTHING; r_BackUpLayer(); break;
                case MODE_VIEW:  drag.active = false; break;
                default: break;
            }
        } break;
        default: break;
    }
    return true;
}

void g_Init(void)
{
    view = m_Ident_Mat4();
    pthread_mutex_init(&viewLock, NULL);
    pthread_mutex_init(&projLock, NULL);

    player.pos = (Vec3){0, 0., 3};
    player.target = (Vec3){0, 0, 0};
    player.pivot = player.target;
    brushX = 0;
    brushY = 0;
    brushColor = (Vec3){0.1, 0.95, 0.3};
    brushRadius = 0.01;
    mode = MODE_DO_NOTHING;
    gameState.shouldRun = true;
    brush = r_GetBrush();
    firstFrame = true;

    slider0 = obdn_u_CreateSlider(0, 40, NULL);

    r_BindScene(&scene);
}

void g_BindToBrush(Brush* br)
{
    brush = br;
}

void g_Update(void)
{
    scene.dirt = 0;
    if (firstFrame)
    {
        scene.proj = m_BuildPerspective(0.01, 30);
        scene.dirt |= SCENE_PROJ_BIT;
        firstFrame = false;
    }
    assert(brush);
    //assert(sizeof(struct Player) == sizeof(UboPlayer));
    //handleKeyMovement();
    vkDeviceWaitIdle(device);
    brushRadius = slider0->data.slider.sliderPos * 0.1; // TODO: find a better way
    //
    brush->x = mousePos.x;
    brush->y = mousePos.y;
    if (pivotChanged)
        setViewerPivotByIntersection();
    handleMouseMovement();
    pivotChanged = false;
    if (!parms.copySwapToHost)
        scene.view = generatePlayerView();
    else
    {
        pthread_mutex_lock(&viewLock);
        scene.view = view;
        pthread_mutex_unlock(&viewLock);
    }
    if (projDirty)
    {
        pthread_mutex_lock(&projLock);
        scene.proj = proj;
        projDirty = false;
        pthread_mutex_unlock(&projLock);
        scene.dirt |= SCENE_PROJ_BIT;
    }
    if (windowDirty)
    {
        obdn_r_RecreateSwapchain();
        windowDirty = false;
    }
    scene.dirt |= SCENE_VIEW_BIT;
    brush->r = brushColor.x[0];
    brush->g = brushColor.x[1];
    brush->b = brushColor.x[2];
    brush->mode = mode;
    brush->radius = brushRadius;
}

void g_SetColor(const float r, const float g, const float b)
{
    brushColor.x[0] = r;
    brushColor.x[1] = g;
    brushColor.x[2] = b;
}

void g_SetRadius(const float r)
{
    brushRadius = r;
}

void g_CleanUp(void)
{
    brush = NULL;
    slider0 = NULL;
}

// can be called from other thread
void g_UpdateView(const Mat4* m)
{
    pthread_mutex_lock(&viewLock);
    view = *m;
    pthread_mutex_unlock(&viewLock);
}

void g_UpdateProg(const Mat4* m)
{
    pthread_mutex_lock(&projLock);
    proj = *m;
    projDirty = true;
    pthread_mutex_unlock(&projLock);
}

void g_UpdateWindow(uint32_t width, uint32_t height)
{
    // TODO: make this safe some how... 
    OBDN_WINDOW_WIDTH = width;
    OBDN_WINDOW_HEIGHT = height;
    windowDirty = true;
}
