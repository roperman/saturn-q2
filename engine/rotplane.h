/*
** VDP2 rotation background (RBG0) as a perspective floor + ceiling.
** (The taxi's fork: the floor is a streamed city map; see rotplane.c.)
**
** One RBG0 layer, two rotation parameter sets, switched per line by the
** coefficient MSB (RPMD = 2):
**   parameter A: floor, lines below the horizon
**   parameter B: ceiling, lines above it
** The coefficient table holds one scale factor per screen line (the floor or
** ceiling distance on that line); the rotation matrix walks the camera's right
** vector across each line. Lines beyond fog_z are transparent (back screen).
**
** VDP2 VRAM (4 banks, 128KB each):
**   A0 character patterns   A1 pattern name tables   B0 coefficient tables
**   B1 rotation parameter table + back screen colour (not an RBG bank)
**
** Costs VDP1 nothing: the floor and ceiling are drawn by VDP2 as it scans out.
*/
#ifndef __ROTPLANE_H__
#define __ROTPLANE_H__

#include "sat.h"

typedef struct
{
    s32             cam_x, cam_z;       /* world, 16.16, within the map (0..512 m: it repeats) */
    s32             sin_yaw, cos_yaw;   /* forward = (sin, cos) in (x, z); 16.16 */
    s32             floor_h, ceil_h;    /* eye to floor / ceiling distance, 16.16; ceil_h <= 0: no ceiling (sky) */
    s32             fog_z;              /* 16.16 world units; farther lines are transparent */
    int             focal;              /* pixels */
    int             texels_per_unit;    /* texture texels per world unit */
    int             flip;               /* the view is upside down: floor at the top, ceiling below */
}                   rot_view;

void                rot_init(void);                                     /* after vdp_init() */
void                rot_set_horizon(int line);                          /* default SCREEN_H / 2 */
void                rot_set_cells(const u8 *cells, int ncells, const u16 *pal256);  /* 8bpp 8x8 cells, up to 2048 */
volatile u16        *rot_names(void);   /* the map: 16 pages of 64x64 1-word names (cell * 2), plane n = page n */
void                rot_prepare(const rot_view *v);                     /* build tables in work RAM */
void                rot_commit(void);                                   /* copy to VRAM: call in vblank */

/* after rot_prepare(): the floor/ceiling depth on each screen line (16.16
   world units), or 0 where the plane is transparent (horizon band) */
extern s32          rot_row_z[];

#endif
