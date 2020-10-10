#ifndef G_GAME_H
#define G_GAME_H

#include <tanto/i_input.h>
#include <tanto/m_math.h>

void g_Init(void);
void g_BindToView(Mat4* viewMat, Mat4* viewMatInv);
void g_Responder(const Tanto_I_Event *event);
void g_Update(void);

#endif /* end of include guard: G_GAME_H */
