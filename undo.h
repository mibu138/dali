#ifndef UNDO_H
#define UNDO_H

#include <tanto/v_memory.h>

void u_InitUndo(const uint32_t size);

Tanto_V_BufferRegion* u_GetNextBuffer(void);
Tanto_V_BufferRegion* u_GetLastBuffer(void);

#endif /* end of include guard: UNDO_H */
