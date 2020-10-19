#include "game.h"
#include "render.h"
#include "common.h"
#include "tanto/m_math.h"
#include <assert.h>
#include <string.h>
#include <tanto/t_def.h>


static bool zoomIn;
static bool zoomOut;
static bool tumbleUp;
static bool tumbleDown;
static bool tumbleLeft;
static bool tumbleRight;
static bool moveUp;
static bool moveDown;

struct Drag {
    bool active;
    Vec2 startPos;
} drag;

static Vec2  mousePos;

static Mat4* viewMat;
static Mat4* viewInvMat;

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

static struct Player {
    Vec3 pos;
    Vec3 target;
} player;

G_GameState gameState;

#define FORWARD {0, 0, -1};
#define MOVE_SPEED 0.04
#define TURN_SPEED 0.04

Brush* brush;
UboPlayer* uboPlayer;

const Vec3 UP_VEC = {0, 1, 0};

static Mat4 generatePlayerView(void)
{
    return m_LookAt(&player.pos, &player.target, &UP_VEC);
}

void g_Init(void)
{
    player.pos = (Vec3){0, 0., 3};
    player.target = (Vec3){0, 0, 0};
    brushX = 0;
    brushY = 0;
    brushColor = (Vec3){1.0, 0.4, 0.2};
    brushRadius = 0.01;
    mode = MODE_DO_NOTHING;
    gameState.shouldRun = true;
    viewMat = r_GetXform(R_XFORM_VIEW);
    viewInvMat = r_GetXform(R_XFORM_VIEW_INV);
    brush = r_GetBrush();
    uboPlayer = r_GetPlayer();
}

void g_BindToView(Mat4* view, Mat4* viewInv)
{
    assert(view);
    viewMat = view;
    if (viewInv)
        viewInvMat = viewInv;
}

void g_BindToBrush(Brush* br)
{
    brush = br;
}

void g_BindToPlayer(UboPlayer* ubo)
{
    uboPlayer = ubo;
}

void g_Responder(const Tanto_I_Event *event)
{
    switch (event->type) 
    {
        case TANTO_I_KEYDOWN: switch (event->data.keyCode)
        {
            case TANTO_KEY_W: zoomIn = true; break;
            case TANTO_KEY_S: zoomOut = true; break;
            case TANTO_KEY_A: tumbleLeft = true; break;
            case TANTO_KEY_D: tumbleRight = true; break;
            case TANTO_KEY_E: moveUp = true; break;
            case TANTO_KEY_Q: moveDown = true; break;
            case TANTO_KEY_P: r_SavePaintImage(); break;
            case TANTO_KEY_SPACE: mode = MODE_VIEW; break;
            case TANTO_KEY_CTRL: tumbleDown = true; break;
            case TANTO_KEY_ESC: parms.shouldRun = false; gameState.shouldRun = false; break;
            case TANTO_KEY_R:    parms.shouldRun = false; parms.reload = true; break;
            default: return;
        } break;
        case TANTO_I_KEYUP:   switch (event->data.keyCode)
        {
            case TANTO_KEY_W: zoomIn = false; break;
            case TANTO_KEY_S: zoomOut = false; break;
            case TANTO_KEY_A: tumbleLeft = false; break;
            case TANTO_KEY_D: tumbleRight = false; break;
            case TANTO_KEY_E: moveUp = false; break;
            case TANTO_KEY_Q: moveDown = false; break;
            case TANTO_KEY_SPACE: mode = MODE_DO_NOTHING; break;
            case TANTO_KEY_CTRL: tumbleDown = false; break;
            default: return;
        } break;
        case TANTO_I_MOTION: 
        {
            mousePos.x = (float)event->data.mouseCoords.x / TANTO_WINDOW_WIDTH;
            mousePos.y = (float)event->data.mouseCoords.y / TANTO_WINDOW_HEIGHT;
            break;
        }
        case TANTO_I_MOUSEDOWN:
        {
            if (mode == MODE_DO_NOTHING)
                mode = MODE_PAINT;
            if (mode == MODE_VIEW)
            {
                drag.active = true;
                const Vec2 p = {
                    .x = (float)event->data.mouseCoords.x / TANTO_WINDOW_WIDTH,
                    .y = (float)event->data.mouseCoords.y / TANTO_WINDOW_HEIGHT
                };
                drag.startPos = p;
                printf("drag active\n");
            }
            break;
        }
        case TANTO_I_MOUSEUP:
        {
            printf("bar\n");
            if (mode == MODE_PAINT)
                mode = MODE_DO_NOTHING;
            if (mode == MODE_VIEW)
                drag.active = false;
            break;
        }
        default: assert(0); // error
    }
}

static void handleKeyMovement(void)
{
    if (zoomIn) 
    {
        Vec3 dir = m_Sub_Vec3(&player.target, &player.pos);
        dir = m_Normalize_Vec3(&dir);
        dir = m_Scale_Vec3(MOVE_SPEED, &dir);
        player.pos = m_Add_Vec3(&player.pos, &dir);
    }
    if (zoomOut) 
    {
        Vec3 dir = m_Sub_Vec3(&player.target, &player.pos);
        dir = m_Normalize_Vec3(&dir);
        dir = m_Scale_Vec3(-MOVE_SPEED, &dir);
        player.pos = m_Add_Vec3(&player.pos, &dir);
    }
    if (moveUp)
    {
        Vec3 up = {0.0, MOVE_SPEED, 0.0};
        player.pos = m_Add_Vec3(&player.pos, &up);
        player.target = m_Add_Vec3(&player.target, &up);
    }
    if (moveDown)
    {
        Vec3 down = {0.0, -MOVE_SPEED, 0.0};
        player.pos = m_Add_Vec3(&player.pos, &down);
        player.target = m_Add_Vec3(&player.target, &down);
    }
    if (tumbleLeft)
    {
        const Vec3 up = (Vec3){0, 1.0, 0.0};
        Vec3 dir = m_Sub_Vec3(&player.target, &player.pos);
        dir = m_Normalize_Vec3(&dir);
        Vec3 left = m_Cross(&up, &dir);
        left = m_Scale_Vec3(MOVE_SPEED, &left);
        player.pos = m_Add_Vec3(&player.pos, &left);
    }
    if (tumbleRight)
    {
        const Vec3 up = (Vec3){0, 1.0, 0.0};
        Vec3 dir = m_Sub_Vec3(&player.target, &player.pos);
        dir = m_Normalize_Vec3(&dir);
        Vec3 right = m_Cross(&dir, &up);
        right = m_Scale_Vec3(MOVE_SPEED, &right);
        player.pos = m_Add_Vec3(&player.pos, &right);
    }
}

static void handleMouseMovement(void)
{
    static Vec3 cachedPos;
    if (drag.active)
    {
        float angleY = mousePos.x - drag.startPos.x;
        float angleX = mousePos.y - drag.startPos.y;
        angleY *= -3.14;
        angleX *=  3.14;
        Vec3 temp = m_Sub_Vec3(&cachedPos, &player.target);
        const Vec3 z = m_Normalize_Vec3(&temp);
        temp = m_Cross(&z, &UP_VEC);
        const Vec3 x = m_Normalize_Vec3(&temp);
        const Mat4 rotY = m_BuildRotate(angleY, &UP_VEC);
        const Mat4 rotX = m_BuildRotate(angleX, &x);
        const Mat4 rot = m_Mult_Mat4(&rotX, &rotY);
        player.pos = m_Mult_Mat4Vec3(&rot, &cachedPos);
    }
    else
        cachedPos = player.pos;
}

void g_Update(void)
{
    assert(viewMat);
    assert(brush);
    assert(uboPlayer);
    assert(sizeof(struct Player) == sizeof(UboPlayer));
    handleKeyMovement();
    handleMouseMovement();
    *viewMat    = generatePlayerView();
    *viewInvMat = m_Invert4x4(viewMat);
    brush->x = mousePos.x;
    brush->y = mousePos.y;
    brush->r = brushColor.x[0];
    brush->g = brushColor.x[1];
    brush->b = brushColor.x[2];
    brush->mode = mode;
    brush->radius = brushRadius;
    memcpy(uboPlayer, &player, sizeof(UboPlayer));
    printf("Brush mode %d\n", mode);
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
