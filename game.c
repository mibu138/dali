#include "game.h"
#include "render.h"
#include "common.h"
#include "tanto/m_math.h"
#include <assert.h>
#include <tanto/t_def.h>


static bool moveForward;
static bool moveBackward;
static bool moveUp;
static bool moveDown;
//static bool moveLeft;
//static bool moveRight;
static bool turnLeft;
static bool turnRight;

static Mat4* viewMat;
static Mat4* viewInvMat;

typedef enum {
    PAINT_MODE_PAINT,
    PAINT_MODE_DO_NOTHING,
} PaintMode;

static float brushX;
static float brushY;
static Vec3  brushColor;
static float brushRadius;
static PaintMode paintMode;

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
    paintMode = 1;
    gameState.shouldRun = true;
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

void g_Responder(const Tanto_I_Event *event)
{
    switch (event->type) 
    {
        case TANTO_I_KEYDOWN: switch (event->data.keyCode)
        {
            case TANTO_KEY_W: moveForward = true; break;
            case TANTO_KEY_S: moveBackward = true; break;
            case TANTO_KEY_A: turnLeft = true; break;
            case TANTO_KEY_D: turnRight = true; break;
            case TANTO_KEY_SPACE: moveUp = true; break;
            case TANTO_KEY_CTRL: moveDown = true; break;
            case TANTO_KEY_ESC: parms.shouldRun = false; gameState.shouldRun = false; break;
            case TANTO_KEY_R:    parms.shouldRun = false; parms.reload = true; break;
            default: return;
        } break;
        case TANTO_I_KEYUP:   switch (event->data.keyCode)
        {
            case TANTO_KEY_W: moveForward = false; break;
            case TANTO_KEY_S: moveBackward = false; break;
            case TANTO_KEY_A: turnLeft = false; break;
            case TANTO_KEY_D: turnRight = false; break;
            case TANTO_KEY_SPACE: moveUp = false; break;
            case TANTO_KEY_CTRL: moveDown = false; break;
            default: return;
        } break;
        case TANTO_I_MOTION: 
        {
            brushX = (float)event->data.mouseCoords.x / TANTO_WINDOW_WIDTH;
            brushY = (float)event->data.mouseCoords.y / TANTO_WINDOW_HEIGHT;
            break;
        }
        case TANTO_I_MOUSEDOWN:
        {
            printf("foo\n");
            if (paintMode == PAINT_MODE_DO_NOTHING)
                paintMode = PAINT_MODE_PAINT;
            break;
        }
        case TANTO_I_MOUSEUP:
        {
            printf("bar\n");
            if (paintMode == PAINT_MODE_PAINT)
                paintMode = PAINT_MODE_DO_NOTHING;
            break;
        }
        default: assert(0); // error
    }
}

void g_Update(void)
{
    assert(viewMat);
    assert(brush);
    if (moveForward) 
    {
        Vec3 dir = m_Sub_Vec3(&player.target, &player.pos);
        dir = m_Normalize_Vec3(&dir);
        dir = m_Scale_Vec3(MOVE_SPEED, &dir);
        player.pos = m_Add_Vec3(&player.pos, &dir);
    }
    if (moveBackward) 
    {
        Vec3 dir = m_Sub_Vec3(&player.target, &player.pos);
        dir = m_Normalize_Vec3(&dir);
        dir = m_Scale_Vec3(-MOVE_SPEED, &dir);
        player.pos = m_Add_Vec3(&player.pos, &dir);
    }
    if( moveUp)
    {
        Vec3 up = {0.0, MOVE_SPEED, 0.0};
        player.pos = m_Add_Vec3(&player.pos, &up);
    }
    if (moveDown)
    {
        Vec3 down = {0.0, -MOVE_SPEED, 0.0};
        player.pos = m_Add_Vec3(&player.pos, &down);
    }
    if (turnLeft)
    {
        const Vec3 up = (Vec3){0, 1.0, 0.0};
        Vec3 dir = m_Sub_Vec3(&player.target, &player.pos);
        dir = m_Normalize_Vec3(&dir);
        Vec3 left = m_Cross(&up, &dir);
        left = m_Scale_Vec3(MOVE_SPEED, &left);
        player.pos = m_Add_Vec3(&player.pos, &left);
    }
    if (turnRight)
    {
        const Vec3 up = (Vec3){0, 1.0, 0.0};
        Vec3 dir = m_Sub_Vec3(&player.target, &player.pos);
        dir = m_Normalize_Vec3(&dir);
        Vec3 right = m_Cross(&dir, &up);
        right = m_Scale_Vec3(MOVE_SPEED, &right);
        player.pos = m_Add_Vec3(&player.pos, &right);
    }
    *viewMat    = generatePlayerView();
    *viewInvMat = m_Invert4x4(viewMat);
    brush->x = brushX;
    brush->y = brushY;
    brush->r = brushColor.x[0];
    brush->g = brushColor.x[1];
    brush->b = brushColor.x[2];
    brush->mode = paintMode;
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
