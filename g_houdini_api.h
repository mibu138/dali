#ifndef G_HOUDINI_API_H
#define G_HOUDINI_API_H

#include <coal/coal.h>
#include <obsidian/v_vulkan.h>

typedef struct {
    void (*setColor)(float r, float g, float b);
    void (*setFallOff)(float f);
    void (*setOpacity)(float o);
    void (*setRadius)(float r);
    void (*setView)(const Mat4* m);
    void (*setProj)(const Mat4* m);
    void (*loadTexture)(const void* data, uint32_t w, uint32_t h, VkFormat format);
} G_Houdini_Export;

typedef G_Houdini_Export (*G_Houdini_FunctionLoader)(void);

#endif /* end of include guard: G_HOUDINI_API_H */
