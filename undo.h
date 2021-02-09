#ifndef UNDO_H
#define UNDO_H

#include <obsidian/v_memory.h>

void u_InitUndo(const uint32_t size);
void u_CleanUp(void);

Obdn_V_BufferRegion* u_GetNextBuffer(void);
Obdn_V_BufferRegion* u_GetLastBuffer(void);

#endif /* end of include guard: UNDO_H */
