#include "game.h"
#include "common.h"
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

Parms parms;

static struct Player {
    Vec3  pos;
    float angle;
} player;

#define FORWARD {0, 0, -1};
#define MOVE_SPEED 0.04
#define TURN_SPEED 0.04


static Mat4 generatePlayerView(void)
{
    Mat4 m = m_Ident_Mat4();
    m = m_RotateY_Mat4(-player.angle, &m);
    m = m_Translate_Mat4(player.pos, &m);
    return m;
}

void g_Init(void)
{
    player.pos = (Vec3){0, -2.5, -5};
}

void g_BindToView(Mat4* view, Mat4* viewInv)
{
    assert(view);
    viewMat = view;
    if (viewInv)
        viewInvMat = viewInv;
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
            case TANTO_KEY_ESC: parms.shouldRun = false; break;
            case TANTO_KEY_R:    parms.mode = (parms.mode == MODE_RAY ? MODE_RASTER : MODE_RAY); parms.renderNeedsUpdate = true; break;
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
            printf("%d %d\n", event->data.mouseCoords.x, event->data.mouseCoords.y);
            break;
        }
        case TANTO_I_MOUSEDOWN:
        {
            printf("foo\n");
            break;
        }
        case TANTO_I_MOUSEUP:
        {
            printf("bar\n");
            break;
        }
        default: assert(0); // error
    }
}

void g_Update(void)
{
    assert(viewMat);
    if (moveForward) 
    {
        Vec3 dir = FORWARD;
        dir = m_RotateY_Vec3(player.angle, &dir);
        dir = m_Scale_Vec3(-MOVE_SPEED, &dir);
        player.pos = m_Add_Vec3(&player.pos, &dir);
    }
    if (moveBackward) 
    {
        Vec3 dir = FORWARD;
        dir = m_RotateY_Vec3(player.angle, &dir);
        dir = m_Scale_Vec3(MOVE_SPEED, &dir);
        player.pos = m_Add_Vec3(&player.pos, &dir);
    }
    if( moveUp)
    {
        Vec3 up = {0.0, -MOVE_SPEED, 0.0};
        player.pos = m_Add_Vec3(&player.pos, &up);
    }
    if (moveDown)
    {
        Vec3 down = {0.0, MOVE_SPEED, 0.0};
        player.pos = m_Add_Vec3(&player.pos, &down);
    }
    if (turnLeft)
    {
        player.angle += TURN_SPEED;
    }
    if (turnRight)
    {
        player.angle -= TURN_SPEED;
    }
    *viewMat    = generatePlayerView();
    *viewInvMat = m_Invert4x4(viewMat);
}
