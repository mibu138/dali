#ifndef G_API_H
#define G_API_H

#include <obsidian/s_scene.h>
#include "paint.h"
#include "common.h"

typedef struct {
    int      (*createLayer)(void);
    bool     (*incrementLayer)(uint16_t* const id);
    bool     (*decrementLayer)(uint16_t* const id);
    uint8_t* (*copyTextureToLayer)(L_LayerId id, const void* data, uint32_t w, uint32_t h, VkFormat format);
    Parms*   parms;
} G_Import;

typedef struct {
    void (*init)(Obdn_S_Scene* renderScene, PaintScene* paintScene);
    void (*update)(void);
    void (*cleanUp)(void);
} G_Export;

typedef G_Export (*G_Handshake)(G_Import import);

#endif /* end of include guard: G_API_H */
