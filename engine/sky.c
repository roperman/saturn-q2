/*
** VDP2 sky: per-line back screen gradient + NBG0 skyline (see sky.h).
**
** Bank B1 layout (bytes from VDP2 VRAM):
**   0x60000  rotation parameter tables (rotplane.c)
**   0x68000  NBG0 page 0 (2-word pattern names, 64x64)   } plane = 2x1 pages
**   0x6C000  NBG0 page 1                                 } = 1024 x 512 px
**   0x70000  NBG0 characters, 4bpp (32 bytes each)
**   0x7F000  back screen line colour table
**   0x7FFFE  back screen single colour (sky off)
** CRAM 0x100: the skyline's 16-colour palette.
*/
#include "sky.h"
#include "vdp.h"

#define PAGE0           (0x68000)
#define CHARS           (0x70000)
#define BACK_TABLE      (0x7F000)
#define PAL_CRAM        (0x100)         /* colour index */

static u16          scx_i, scx_f, scy;
static int          sky_h = 64;

void                sky_init(const u8 *pix4, int w, int h, const u16 *pal16)
{
    volatile u8     *chr = (volatile u8 *)(VDP2_VRAM + CHARS);
    volatile u32    *page = (volatile u32 *)(VDP2_VRAM + PAGE0);
    volatile u16    *cram = (volatile u16 *)0x25F00000;
    int             cells_x = w / 8, cells_y = h / 8, cx, cy, y, i;
    u32             charno0 = CHARS >> 5, blank = charno0 + (u32)(cells_x * cells_y);

    sky_h = h;
    /* image -> 8x8 4bpp cells (4 bytes per row), then one blank cell */
    for (cy = 0; cy < cells_y; ++cy)
        for (cx = 0; cx < cells_x; ++cx)
            for (y = 0; y < 8; ++y)
                for (i = 0; i < 4; ++i)
                    *chr++ = pix4[(cy * 8 + y) * (w / 2) + cx * 4 + i];
    for (i = 0; i < 32; ++i)
        *chr++ = 0;
    /* two pages side by side: page 0 = columns 0..63, page 1 = 64..127 */
    for (i = 0; i < 2; ++i)
        for (cy = 0; cy < 64; ++cy)
            for (cx = 0; cx < 64; ++cx)
                page[i * 4096 + cy * 64 + cx] = cy < cells_y ? charno0 + (u32)(cy * cells_x + i * 64 + cx) : blank;
    for (i = 0; i < 16; ++i)
        cram[PAL_CRAM + i] = pal16[i];

    REG16(VDP2_REG + 0x28) = 0x0000;        /* CHCTLA: NBG0 16 colours, 1x1 cells */
    REG16(VDP2_REG + 0x30) = 0x0000;        /* PNCN0: 2-word pattern names */
    REG16(VDP2_REG + 0x3A) = 0x0001;        /* PLSZ: NBG0 2x1 pages, RBG0 1x1 (registers are write-only) */
    REG16(VDP2_REG + 0x3C) = 0x0000;        /* MPOFN */
    REG16(VDP2_REG + 0x40) = (u16)(((PAGE0 >> 14) << 8) | (PAGE0 >> 14));   /* planes A B */
    REG16(VDP2_REG + 0x42) = (u16)(((PAGE0 >> 14) << 8) | (PAGE0 >> 14));   /* planes C D */
    REG16(VDP2_REG + 0x7C) = 1;             /* Y coordinate increment 1.0 */
    REG16(VDP2_REG + 0x7E) = 0;
    /* VRAM access: bank B1 gives NBG0 a pattern name and a character slot;
       A0/A1/B0 are rotation banks and ignore the cycle patterns */
    for (i = 0; i < 6; ++i)
        REG16(VDP2_REG + 0x10 + i * 2) = 0xFFFF;
    REG16(VDP2_REG + 0x1C) = 0x04FF;        /* CYCB1L: T0 NBG0 names, T1 NBG0 chars */
    REG16(VDP2_REG + 0x1E) = 0xFFFF;
    REG16(VDP2_REG + 0xF8) = 0x0001;        /* PRINA: NBG0 priority 1 */
    REG16(VDP2_REG + 0xE4) = PAL_CRAM >> 8; /* CRAOFA: NBG0 palette at CRAM 0x100 */
}

void                sky_set_palette(const u16 *pal16)
{
    volatile u16    *cram = (volatile u16 *)0x25F00000;
    int             i;

    for (i = 0; i < 16; ++i)
        cram[PAL_CRAM + i] = pal16[i];
}

void                sky_set_gradient(u16 top, u16 horizon, u16 below)
{
    volatile u16    *tab = (volatile u16 *)(VDP2_VRAM + BACK_TABLE);
    int             y, k;

    for (y = 0; y < 256; ++y)
    {
        if (y >= SCREEN_H / 2)
            tab[y] = below;
        else
        {
            /* linear blend per 5-bit channel, top -> horizon */
            u16 c = 0x8000;

            for (k = 0; k < 15; k += 5)
            {
                int a = (top >> k) & 31, b = (horizon >> k) & 31;

                c |= (u16)((a + (b - a) * y / (SCREEN_H / 2 - 1)) << k);
            }
            tab[y] = c;
        }
    }
}

void                sky_enable(bool on)
{
    if (on)
    {
        vdp2_bgon(0x0001, 0);               /* NBG0 on */
        REG16(VDP2_REG + 0xAC) = (u16)(0x8000 | (BACK_TABLE >> 17));    /* BKTAU: per-line table */
        REG16(VDP2_REG + 0xAE) = (u16)((BACK_TABLE >> 1) & 0xFFFF);
    }
    else
    {
        vdp2_bgon(0, 0x0001);
        REG16(VDP2_REG + 0xAC) = 0x0003;    /* single back colour at 0x7FFFE */
        REG16(VDP2_REG + 0xAE) = 0xFFFF;
    }
}

void                sky_prepare(int yaw, int horizon_line, int focal)
{
    /* at the screen centre, 1 pixel = 1/focal radian; the skyline is 1024 px
       per turn, so each screen pixel steps 1024 / (2 pi focal) skyline pixels */
    u32             zoom = (u32)(256 * 1024 * 1000 / (6283 * focal));      /* 8.8 */
    s32             centre = (s32)((-(s64)yaw * 1024 * 256) >> 16);           /* 24.8 */
    s32             x = centre - (s32)(160 * zoom);

    REG16(VDP2_REG + 0x78) = (u16)(zoom >> 8);
    REG16(VDP2_REG + 0x7A) = (u16)((zoom & 0xFF) << 8);
    scx_i = (u16)((x >> 8) & 0x7FF);
    scx_f = (u16)((x & 0xFF) << 8);
    scy = (u16)((sky_h - horizon_line + 1024) & 0x3FF);
}

void                sky_commit(void)
{
    REG16(VDP2_REG + 0x70) = scx_i;         /* SCXIN0 */
    REG16(VDP2_REG + 0x72) = scx_f;         /* SCXDN0 */
    REG16(VDP2_REG + 0x74) = scy;           /* SCYIN0 */
    REG16(VDP2_REG + 0x76) = 0;
}
