#ifndef PAINT_H
#define PAINT_H

#include <obsidian/v_def.h>

#define IMG_4K  4096
#define IMG_8K  IMG_4K * 2
#define IMG_16K IMG_8K * 2

VkSemaphore paint(VkSemaphore waitSemaphore);

#endif /* end of include guard: PAINT_H */
