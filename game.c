#include "game.h"
#include "coal/m_math.h"
#include "obsidian/r_raytrace.h"
#include "obsidian/r_render.h"
#include "obsidian/s_scene.h"
#include "obsidian/v_memory.h"
#include "render.h"
#include "layer.h"
#include "undo.h"
#include "common.h"
#include "obsidian/t_utils.h"
#include <assert.h>
#include <string.h>
#include <obsidian/t_def.h>
#include <obsidian/r_geo.h>
#include <obsidian/i_input.h>
#include <obsidian/v_video.h>
#include <obsidian/u_ui.h>
#include <obsidian/r_pipeline.h>
#include <obsidian/f_file.h>
#include <obsidian/private.h>
#include <obsidian/v_private.h>
#include <pthread.h>
#include <vulkan/vulkan_core.h>

static bool pivotChanged;

#define SPVDIR "/home/michaelb/dev/painter/shaders/spv"

static Obdn_V_BufferRegion          selectionRegion;
static Obdn_V_BufferRegion          camRegion;
static Obdn_R_Description           description;
static VkDescriptorSetLayout        descriptorSetLayout;
static VkPipelineLayout             pipelineLayout;
static VkPipeline                   selectionPipeline;
static Obdn_R_ShaderBindingTable    sbt;
static Obdn_R_AccelerationStructure blas;
static Obdn_R_AccelerationStructure tlas;

typedef struct {
    float x;
    float y;
    float z;
    int   hit;
} Selection;

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

Parms parms;

static struct Player {
    Vec3 pos;
    Vec3 target;
    Vec3 pivot;
} player;

static PaintScene*     paintScene;
static Obdn_S_Scene*   renderScene;

static const Vec3 UP_VEC = {0, 1, 0};

static Obdn_U_Widget* radiusSlider;
static Obdn_U_Widget* opacitySlider;
static Obdn_U_Widget* falloffSlider;
static Obdn_U_Widget* text;

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

static Mat4 generateCameraXform(void)
{
    return m_LookAt(&player.pos, &player.target, &UP_VEC);
}

static void setBrushActive(bool active)
{
    paintScene->brush_active = active;
    paintScene->dirt |= SCENE_BRUSH_BIT;
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
                return;
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
                return;
                float deltaX = mousePos.x - drag.startPos.x;
                //float deltaY = mousePos.y - drag.startPos.y;
                //float scale = -1 * (deltaX + deltaY * -1);
                float scale = -1 * deltaX;
                Vec3 temp = m_Sub_Vec3(&cached.pos, &cached.pivot);
                Vec3 z = m_Normalize_Vec3(&temp);
                z = m_Scale_Vec3(scale, &z);
                player.pos = m_Add_Vec3(&cached.pos, &z);
                //lerpTargetToPivot();
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
        paintScene->layer = id;
        paintScene->dirt |= SCENE_LAYER_CHANGED_BIT;
        if (!u_LayerInCache(id))
            paintScene->dirt |= SCENE_LAYER_BACKUP_BIT;
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
        paintScene->layer = id;
        paintScene->dirt |= SCENE_LAYER_CHANGED_BIT;
        if (!u_LayerInCache(id))
            paintScene->dirt |= SCENE_LAYER_BACKUP_BIT;
        char str[12];
        snprintf(str, 12, "Layer %d", id + 1);
        obdn_u_UpdateText(str, text);
    }
}

static void updatePrim(void)
{
    Obdn_R_Primitive* prim = &renderScene->prims[0].rprim;
    if (blas.bufferRegion.size)
        obdn_r_DestroyAccelerationStruct(&blas);
    if (tlas.bufferRegion.size)
        obdn_r_DestroyAccelerationStruct(&tlas);
    obdn_r_BuildBlas(prim, &blas);
    Mat4 xform = m_Ident_Mat4();
    obdn_r_BuildTlasNew(1, &blas, &xform, &tlas);

    
    VkWriteDescriptorSetAccelerationStructureKHR asInfo = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures    = &tlas.handle
    };

    VkDescriptorBufferInfo storageBufInfoPos = {
        .range  = obdn_r_GetAttrRange(prim, "pos"),
        .offset = obdn_r_GetAttrOffset(prim, "pos"),
        .buffer = prim->vertexRegion.buffer,
    };

    VkDescriptorBufferInfo storageBufInfoIndices = {
        .range  = prim->indexRegion.size,
        .offset = prim->indexRegion.offset,
        .buffer = prim->indexRegion.buffer
    };

    VkWriteDescriptorSet writes[] = {{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstArrayElement = 0,
        .dstSet = description.descriptorSets[0],
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        .pNext = &asInfo
    },{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstArrayElement = 0,
        .dstSet = description.descriptorSets[0],
        .dstBinding = 3,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &storageBufInfoPos
    },{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstArrayElement = 0,
        .dstSet = description.descriptorSets[0],
        .dstBinding = 4,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &storageBufInfoIndices
    }};

    vkUpdateDescriptorSets(device, LEN(writes), writes, 0, NULL);
}

static void initGPUSelection(const Obdn_R_Primitive* prim)
{
    selectionRegion = obdn_v_RequestBufferRegion(
        sizeof(Selection), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);

    camRegion = obdn_v_RequestBufferRegion(sizeof(Cam),
                                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                           OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);

    Obdn_R_DescriptorBinding bindings[] = {{
        // as
        .descriptorCount = 1,
        .type            = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        .stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR
    },{
        // selection buffer
        .descriptorCount = 1,
        .type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
    },{
        // cam buffer
        .descriptorCount = 1,
        .type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
    },{
        // pos buffer
        .descriptorCount = 1,
        .type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .stageFlags      = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
    },{
        // index buffer
        .descriptorCount = 1,
        .type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .stageFlags      = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
    }};

    Obdn_R_DescriptorSetInfo dsInfo = {
        .bindingCount = LEN(bindings),
    };

    memcpy(dsInfo.bindings, bindings, sizeof(bindings));

    obdn_r_CreateDescriptionsAndLayouts(1, &dsInfo, &descriptorSetLayout, 1, &description);

    VkPushConstantRange pcrange = {
        .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        .offset     = 0,
        .size       = sizeof(Mat4) * 3
    };

    Obdn_R_PipelineLayoutInfo plInfo = {
        .descriptorSetCount   = 1,
        .descriptorSetLayouts = &descriptorSetLayout,
        .pushConstantCount    = 1,
        .pushConstantsRanges  = &pcrange
    };

    obdn_r_CreatePipelineLayouts(1, &plInfo, &pipelineLayout);

    Obdn_R_RayTracePipelineInfo rtPipeInfo = {
        .layout = pipelineLayout,
        .raygenCount = 1,
        .raygenShaders = (char*[]){
            SPVDIR"/select-rgen.spv",
        },
        .missCount = 1,
        .missShaders = (char*[]){
            SPVDIR"/select-rmiss.spv",
        },
        .chitCount = 1,
        .chitShaders = (char*[]){
            SPVDIR"/select-rchit.spv"
        }
    };

    obdn_r_CreateRayTracePipelines(1, &rtPipeInfo, &selectionPipeline, &sbt);

    VkDescriptorBufferInfo storageBufInfoSelection= {
        .range  = selectionRegion.size,
        .offset = selectionRegion.offset,
        .buffer = selectionRegion.buffer,
    };

    VkDescriptorBufferInfo camInfo = {
        .range  = camRegion.size,
        .offset = camRegion.offset,
        .buffer = camRegion.buffer,
    };

    VkWriteDescriptorSet writes[] = {{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstArrayElement = 0,
        .dstSet = description.descriptorSets[0],
        .dstBinding = 1,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &storageBufInfoSelection
    },{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstArrayElement = 0,
        .dstSet = description.descriptorSets[0],
        .dstBinding = 2,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo = &camInfo
    }};

    vkUpdateDescriptorSets(device, LEN(writes), writes, 0, NULL);
}

static int getSelectionPos(Vec3* v)
{
    Obdn_V_Command cmd = obdn_v_CreateCommand(OBDN_V_QUEUE_GRAPHICS_TYPE);

    obdn_v_BeginCommandBuffer(cmd.buffer);

    VkCommandBuffer cmdBuf = cmd.buffer;

    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, selectionPipeline); 

    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, 
            pipelineLayout, 0, 1, description.descriptorSets, 0, NULL);

    vkCmdPushConstants(cmdBuf, pipelineLayout, VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(float) * 2, &mousePos);

    vkCmdTraceRaysKHR(cmdBuf, 
            &sbt.raygenTable,
            &sbt.missTable,
            &sbt.hitTable,
            &sbt.callableTable,
            1, 1, 1);

    obdn_v_EndCommandBuffer(cmd.buffer);

    obdn_v_SubmitAndWait(&cmd, 0);

    obdn_v_DestroyCommand(cmd);

    Selection* sel = (Selection*)selectionRegion.hostData;
    if (sel->hit)
    {
        v->x[0] = sel->x;
        v->x[1] = sel->y;
        v->x[2] = sel->z;
        return 1;
    }
    else
        return 0;
}

static void setViewerPivotByIntersection(void)
{
    Vec3 hitPos;
    int r = getSelectionPos(&hitPos);
    if (r)
    {
        player.pivot = hitPos;
    }
}

static void swapPrims(void)
{
    static bool pig = false;
    Obdn_F_Primitive fprim;
    if (pig)
    {
        obdn_f_ReadPrimitive("data/pig.tnt", &fprim);
        pig = false;
    }
    else
    {
        obdn_f_ReadPrimitive("data/flip-uv.tnt", &fprim);
        pig = true;
    }
    Obdn_R_Primitive prim = obdn_f_CreateRPrimFromFPrim(&fprim);
    obdn_f_FreePrimitive(&fprim);
    prim = obdn_s_SwapRPrim(renderScene, &prim, 0);
    obdn_r_FreePrim(&prim);
}

void g_Init(Obdn_S_Scene* scene_, PaintScene* paintScene_)
{
    assert(scene_);
    assert(paintScene_);
    paintScene = paintScene_;
    renderScene = scene_;

    obdn_s_CreateEmptyScene(renderScene);

    Obdn_S_PrimId primId = 0;
    if (!parms.copySwapToHost)
    {
        Obdn_F_Primitive fprim;
        obdn_f_ReadPrimitive("data/grid.tnt", &fprim);
        Obdn_R_Primitive prim = obdn_f_CreateRPrimFromFPrim(&fprim);
        obdn_f_FreePrimitive(&fprim);
        primId = obdn_s_AddRPrim(renderScene, prim, NULL);
    }
    else
        assert(0 && "we need to load a prim");
    Obdn_S_TextureId texId = ++renderScene->textureCount;
    Obdn_S_MaterialId matId = obdn_s_CreateMaterial(renderScene, (Vec3){1, 1, 1}, 1.0, texId, 0, 0);
    obdn_s_BindPrimToMaterial(renderScene, primId, matId);

    renderScene->window[0] = OBDN_WINDOW_WIDTH;
    renderScene->window[1] = OBDN_WINDOW_HEIGHT;

    initGPUSelection(&renderScene->prims[primId].rprim);

    obdn_i_Subscribe(g_Responder);

    Mat4 initView = m_Ident_Mat4();
    Mat4 initProj = m_BuildPerspective(0.01, 50);
    g_SetCameraView(&initView);
    g_SetCameraProj(&initProj);

    player.pos = (Vec3){0, 0., 3};
    player.target = (Vec3){0, 0, 0};
    player.pivot = player.target;
    g_SetBrushColor(1, 1, 1);
    g_SetBrushRadius(0.01);
    g_SetBrushFallOff(0.5);
    g_SetBrushOpacity(1);
    mode = MODE_DO_NOTHING;
    setBrushActive(false);

    text = obdn_u_CreateText(10, 0, "Layer 1", NULL);
    //if (!parms.copySwapToHost)
    //{
        //radiusSlider = obdn_u_CreateSlider(40, 80, NULL);
        //obdn_u_CreateText(10, 60, "R: ", radiusSlider);
        //opacitySlider = obdn_u_CreateSlider(40, 120, NULL);
        //obdn_u_CreateText(10, 100, "O: ", opacitySlider);
        //falloffSlider = obdn_u_CreateSlider(40, 160, NULL);
        //obdn_u_CreateText(10, 140, "F: ", falloffSlider);
    //}

    u_BindScene(paintScene_);
}

bool g_Responder(const Obdn_I_Event *event)
{
    switch (event->type) 
    {
        case OBDN_I_KEYDOWN: switch (event->data.keyCode)
        {
            case OBDN_KEY_E: g_SetPaintMode(PAINT_MODE_ERASE); break;
            case OBDN_KEY_W: g_SetPaintMode(PAINT_MODE_OVER); break;
            case OBDN_KEY_Z: paintScene->dirt |= SCENE_UNDO_BIT; break;
            case OBDN_KEY_R: g_SetBrushColor(1, 0, 0); break;
            case OBDN_KEY_G: g_SetBrushColor(0, 1, 0); break;
            case OBDN_KEY_B: g_SetBrushColor(0, 0, 1); break;
            case OBDN_KEY_1: g_SetBrushRadius(0.004); break;
            case OBDN_KEY_2: g_SetBrushRadius(0.01); break;
            case OBDN_KEY_3: g_SetBrushRadius(0.1); break;
            case OBDN_KEY_Q: g_SetBrushColor(1, 1, 1); break;
            case OBDN_KEY_Y: g_SetBrushColor(1, 1, 0); break;
            case OBDN_KEY_ESC: parms.shouldRun = false; break;
            case OBDN_KEY_J: decrementLayer(); break;
            case OBDN_KEY_K: incrementLayer(); break;
            case OBDN_KEY_L: l_CreateLayer(); break;
            case OBDN_KEY_S: swapPrims(); break;
            case OBDN_KEY_SPACE: mode = MODE_VIEW; break;
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
                case MODE_PAINT: mode = MODE_DO_NOTHING; paintScene->dirt |= SCENE_LAYER_BACKUP_BIT; break;
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
    if (radiusSlider)
        g_SetBrushRadius(radiusSlider->data.slider.sliderPos * 0.1); // TODO: find a better way
    if (opacitySlider)
        g_SetBrushOpacity(opacitySlider->data.slider.sliderPos);
    if (falloffSlider)
        g_SetBrushFallOff(falloffSlider->data.slider.sliderPos);
    //
    g_SetBrushPos(mousePos.x, mousePos.y);
    if (pivotChanged)
    {
        printf("Pivot changed!\n");
        setViewerPivotByIntersection();
    }
    if (!parms.copySwapToHost)
    {
        handleMouseMovement();
        pivotChanged = false; //TODO must set to false after handleMouseMovement since it checks this... should find a better way
        Mat4 cameraXform = generateCameraXform();
        g_SetCameraXform(&cameraXform);
    }
    if (MODE_PAINT == mode)
        setBrushActive(true);
    else 
        setBrushActive(false);

    // the extra, local mask allows us to clear the paintScene->dirt mask for the next frame while maintaining the current frame dirty bits

    if (renderScene->dirt & OBDN_S_PRIMS_BIT)
    {
        updatePrim();
    }
    u_Update();
}

void g_SetBrushColor(const float r, const float g, const float b)
{
    paintScene->brush_r = r;
    paintScene->brush_g = g;
    paintScene->brush_b = b;
    paintScene->dirt |= SCENE_BRUSH_BIT;
}

void g_SetBrushRadius(float r)
{
    if (r < 0.001) r = 0.001; // should not go to 0... may cause div by 0 in shader
    paintScene->brush_radius = r;
    paintScene->dirt |= SCENE_BRUSH_BIT;
}

void g_CleanUp(void)
{
    //if (!parms.copySwapToHost)
    //{
        obdn_u_DestroyWidget(radiusSlider);
        obdn_u_DestroyWidget(opacitySlider);
        obdn_u_DestroyWidget(falloffSlider);
    //}
    obdn_u_DestroyWidget(text);
    obdn_i_Unsubscribe(g_Responder);
    memset(&mousePos, 0, sizeof(mousePos));
}

void g_SetCameraXform(const Mat4* xform)
{
    renderScene->camera.xform = *xform;
    renderScene->camera.view = m_Invert4x4(xform);
    renderScene->dirt |= OBDN_S_CAMERA_VIEW_BIT;
}

void g_SetCameraView(const Mat4* view)
{
    renderScene->camera.view = *view;
    renderScene->camera.xform = m_Invert4x4(view);
    renderScene->dirt |= OBDN_S_CAMERA_VIEW_BIT;
}

void g_SetCameraProj(const Mat4* m)
{
    renderScene->camera.proj = *m;
    renderScene->dirt |= OBDN_S_CAMERA_PROJ_BIT;
}

void g_SetWindow(uint16_t width, uint16_t height)
{
    // TODO: make this safe some how... 
    renderScene->window[0] = width;
    renderScene->window[1] = height;
    renderScene->dirt |= OBDN_S_WINDOW_BIT;
}

void g_SetBrushPos(float x, float y)
{
    paintScene->brush_x = x;
    paintScene->brush_y = y;
    paintScene->dirt |= SCENE_BRUSH_BIT;
}

void g_SetPaintMode(PaintMode mode)
{
    paintScene->paint_mode = mode;
    paintScene->dirt |= SCENE_PAINT_MODE_BIT;
    paintScene->dirt |= SCENE_BRUSH_BIT;
}

void g_SetBrushOpacity(float opacity)
{
    if (opacity < 0.0) opacity = 0.0;
    if (opacity > 1.0) opacity = 1.0;
    paintScene->brush_opacity = opacity;
    paintScene->dirt |= SCENE_BRUSH_BIT;
}

void g_SetBrushFallOff(float falloff)
{
    if (falloff < 0.0) falloff = 0.0;
    if (falloff > 1.0) falloff = 1.0;
    paintScene->brush_falloff = falloff;
    paintScene->dirt |= SCENE_BRUSH_BIT;
}
