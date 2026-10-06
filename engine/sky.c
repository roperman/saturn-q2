/*
** VDP2 sky: per-line back screen gradient + NBG0 skybox (see sky.h).
**
** VDP2 VRAM (bytes):
**   0x00000  (bank A0: NBG1, src/main.c's loading screens' picture)
**   0x20000  the sky's header, palettes, each cell's palette   } its file (tools/bake_sky.py), read in
**   0x22000  NBG0's cells, 4bpp (32 bytes each), a blank one   } straight from the disc (banks A1, B0)
**   0x68000  NBG0 page 0 (2-word pattern names, 64x64)   } plane = 2x1 pages
**   0x6C000  NBG0 page 1                                 } = 1024 x 512 px
**   0x7F000  back screen line colour table
**   0x7FFFE  back screen single colour (sky off)
** Colour RAM: the palettes at 0x000-0x1FF and 0x300-0x3FF (0x200: Quake's, the status bar's and
** the loading screens' picture's). NBG1's settings, the loading screens', are written as they are.
*/
#include "sky.h"
#include "vdp.h"

#define HEAD            (0x20000)
#define CELLS           (0x22000)
#define SIZE            (0x31800)       /* (the file: whole sectors) */
#define PAGE0           (0x68000)
#define BACK_TABLE      (0x7F000)

static u16          scx_i, scx_f, scy;
static int          sky_h, sky_above, sky_npal;

static u16          vram16(u32 at)
{
    return *(volatile u16 *)(VDP2_VRAM + at);
}

/* palette p's place in colour RAM (in 16s): 0-31, then 48-63 */
static u32          pal_at(int p)
{
    return (u32)(p < 32 ? p : p + 16);
}

__attribute__((cold)) bool                sky_load(const char *file)
{
    volatile u32    *page = (volatile u32 *)(VDP2_VRAM + PAGE0);
    u32             cell0 = CELLS >> 5, blank, at;
    int             cells_x, cells_y, cx, cy, i, c;

    sky_h = 0;
    if (cd_load(file, (void *)(VDP2_VRAM + HEAD), SIZE) < 0 || vram16(HEAD) != 0x5132 || vram16(HEAD + 2) != 0x534B)
        return false;                   /* ("Q2SK") */
    cells_x = vram16(HEAD + 4) / 8;
    sky_h = vram16(HEAD + 6);
    sky_above = vram16(HEAD + 8);
    sky_npal = vram16(HEAD + 10);
    cells_y = sky_h / 8;
    blank = cell0 + (u32)(cells_x * cells_y);
    at = HEAD + 20 + (u32)sky_npal * 32;    /* (each cell's palette: a byte each) */
    /* two pages side by side: page 0 = columns 0..63, page 1 = 64..127; each cell's palette in its
       pattern name's first word */
    for (i = 0; i < 2; ++i)
        for (cy = 0; cy < 64; ++cy)
            for (cx = 0; cx < 64; ++cx)
            {
                c = cy * cells_x + i * 64 + cx;
                page[i * 4096 + cy * 64 + cx] = cy < cells_y ? pal_at(vram16(at + (u32)(c & ~1)) >> (c & 1 ? 0 : 8) & 0xFF)
                                                               << 16 | (cell0 + (u32)c)
                                                             : blank;
            }

    REG16(VDP2_REG + 0x28) = 0x1200;        /* CHCTLA: NBG0 16 colours, 1x1 cells (NBG1 a 256-colour bitmap) */
    REG16(VDP2_REG + 0x30) = 0x0000;        /* PNCN0: 2-word pattern names */
    REG16(VDP2_REG + 0x3A) = 0x0001;        /* PLSZ: NBG0 2x1 pages, RBG0 1x1 (registers are write-only) */
    REG16(VDP2_REG + 0x3C) = 0x0000;        /* MPOFN */
    REG16(VDP2_REG + 0x40) = (u16)(((PAGE0 >> 14) << 8) | (PAGE0 >> 14));   /* planes A B */
    REG16(VDP2_REG + 0x42) = (u16)(((PAGE0 >> 14) << 8) | (PAGE0 >> 14));   /* planes C D */
    REG16(VDP2_REG + 0x7C) = 1;             /* Y coordinate increment 1.0 */
    REG16(VDP2_REG + 0x7E) = 0;
    /* VRAM access: NBG0's pattern names in B1, its cells in A1 and B0 (A0's NBG1's) */
    REG16(VDP2_REG + 0x14) = 0xF4FF;        /* CYCA1L: T1 NBG0 cells */
    REG16(VDP2_REG + 0x16) = 0xFFFF;
    REG16(VDP2_REG + 0x18) = 0xF4FF;        /* CYCB0L: T1 NBG0 cells */
    REG16(VDP2_REG + 0x1A) = 0xFFFF;
    REG16(VDP2_REG + 0x1C) = 0x0FFF;        /* CYCB1L: T0 NBG0 names */
    REG16(VDP2_REG + 0x1E) = 0xFFFF;
    REG16(VDP2_REG + 0xF8) = 0x0201;        /* PRINA: NBG0 priority 1 (NBG1 2) */
    REG16(VDP2_REG + 0xE4) = 0x0020;        /* CRAOFA: NBG0's palettes from colour RAM 0 (NBG1's 0x200) */
    return true;
}

int                 sky_rows(void)
{
    return sky_h;
}

int                 sky_rows_above(void)
{
    return sky_above;
}

__attribute__((cold)) void                sky_set_colours(u16 (*f)(u16), u16 *above, u16 *below, u16 *zenith)
{
    volatile u16    *cram = (volatile u16 *)0x25F00000;
    int             p, i;

    for (p = 0; p < sky_npal; ++p)
        for (i = 0; i < 16; ++i)
            cram[pal_at(p) * 16 + (u32)i] = f(vram16(HEAD + 20 + (u32)(p * 16 + i) * 2));
    *above = f(vram16(HEAD + 12));
    *below = f(vram16(HEAD + 14));
    *zenith = f(vram16(HEAD + 16));
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
