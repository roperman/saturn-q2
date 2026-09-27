/*
** VDP2 sky for outdoor scenes, costing VDP1 nothing:
**   - back screen in per-line mode: one colour per scan line (the gradient)
**   - NBG0: a 16-colour wrap-around skyline (hills, clouds) above the horizon,
**     scrolled horizontally with the camera yaw so it sits at infinity
**
** Lives in VDP2 VRAM bank B1 alongside the rotation parameter table
** (engine/rotplane.c owns A0/A1/B0). Call after rot_init().
*/
#ifndef __SKY_H__
#define __SKY_H__

#include "sat.h"

void                sky_init(const u8 *pix4, int w, int h, const u16 *pal16);  /* w = 1024, h <= 64 */
void                sky_set_palette(const u16 *pal16);                          /* skyline colours */
void                sky_set_gradient(u16 top, u16 horizon, u16 below);          /* RGB555 */
void                sky_enable(bool on);
void                sky_prepare(int yaw, int horizon_line, int focal);          /* per frame */
void                sky_commit(void);                                           /* in vblank */

#endif
