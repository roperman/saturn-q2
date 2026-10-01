/*
** VDP2 sky for outdoor scenes, costing VDP1 nothing:
**   - back screen in per-line mode: one colour per scan line (the gradient)
**   - NBG0: the skybox on a cylinder round you (tools/bake_sky.py: 1024 wide a turn, its rows the
**     screen's lines when you look level), scrolled with the camera's yaw and pitch so it sits at
**     infinity; 16-colour cells, each with its own palette
**
** The cells in VDP2 VRAM bank B0, the pages and the line colours in B1.
*/
#ifndef __SKY_H__
#define __SKY_H__

#include "sat.h"

bool                sky_load(const char *file);                                 /* from the disc; false: none */
int                 sky_rows(void);                                             /* the strip's rows, */
int                 sky_rows_above(void);                                       /* ...those above the horizon */
void                sky_set_colours(u16 (*f)(u16), u16 *above, u16 *below, u16 *zenith);  /* its palettes into colour
                                                                                   RAM through f; the gradient's */
void                sky_enable(bool on);
void                sky_prepare(int yaw, int horizon_line, int focal);          /* per frame */
void                sky_commit(void);                                           /* in vblank */

#endif
