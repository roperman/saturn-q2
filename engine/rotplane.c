/*
** VDP2 RBG0 perspective floor + ceiling - see rotplane.h.
**
** Maths: for screen line y the floor (or ceiling) is at distance
**   z = h * focal / dy,  dy = y + 0.5 - 112
** and pixel i on that line is the world point
**   cam + z * forward + z * (i - 160) / focal * right
** VDP2 computes, per pixel,  X = k * (Xsp + dX * i) + Xp  with k from the
** coefficient table. So: k = texels * z / focal (per line), Xsp = focal *
** forward - 160 * right, dX = right, Xp = texels * cam. The rotation matrix
** (A..F) supplies forward and right; Xst = -160, Zst = focal, DX = 1.
**
** The taxi's fork: the floor is the city (city.c streams it). 256-colour
** 8x8 cells in bank A0 (2048 of them), 1-word pattern names with 12-bit
** character numbers in bank A1: 16 pages of 64x64 names, one per plane, so
** the map is 4x4 planes = 256x256 cells = 2048x2048 texels that repeat. The
** world axes go straight through: map X = world x (east), map Y = world z
** (north); a heading's forward is (sin, cos) and right is (cos, -sin).
**
** Table/field formats follow Mednafen's VDP2 (FetchRotParams): 32-bit fields
** are 16.16 with the low 6 bits ignored, P/C are 16-bit integers, kx/ky and
** coefficients are 16.16 in the low 24 bits, coefficient bit 31 = transparent.
*/
#include "rotplane.h"
#include "vdp.h"

#define VRAM(off)       (VDP2_VRAM + (off))
#define CHAR_BASE(c)    ((c) ? 0x08000 : 0x00000)       /* bank A0 */
#define MAP_BASE(c)     (0x20000)                       /* bank A1: 16 pages of 8KB */
#define MAP_PAGE0       (MAP_BASE(0) >> 13)
#define COEF_BASE       (0x40000)                       /* bank B0: A at 0, B at +256 entries */
#define RPT_BASE        (0x60000)                       /* bank B1: parameter A, B at +0x80 */
#define LINES           (SCREEN_H)

static s32          coef[2][256] __attribute__((aligned(4)));
static s32          param[2][32] __attribute__((aligned(4)));
s32                 rot_row_z[LINES];
static u32          inv_d2[LINES];              /* 2^32 / |2(y - horizon) + 1|: the per-line divisor */
static int          horizon = LINES / 2;        /* the screen line the horizon sits on */

static void         make_inv_d2(void)
{
    int             i;

    for (i = 0; i < LINES; ++i)
    {
        s32 d2 = 2 * (i - horizon) + 1;         /* odd, so never 0 */

        inv_d2[i] = (u32)(0xFFFFFFFFu / (u32)(d2 < 0 ? -d2 : d2));
    }
}

void                rot_init(void)
{
    int             i;

    /* VRAM A and B each split in two banks; RBG0 roles per bank:
       A0 = character data (3), A1 = pattern names (2), B0 = coefficients (1), B1 = none */
    REG16(VDP2_REG + 0x0E) = 0x0300 | (0 << 6) | (1 << 4) | (2 << 2) | 3;
    REG16(VDP2_REG + 0x2A) = 0x1000;            /* CHCTLB: RBG0 256 colours, 1x1 cell chars */
    REG16(VDP2_REG + 0x38) = 0xC000;            /* PNCR: RBG0 1-word names, 12-bit character numbers */
    REG16(VDP2_REG + 0x3A) = 0x0000;            /* PLSZ: 1x1 page planes, repeat outside */
    REG16(VDP2_REG + 0x3E) = 0x0000;            /* MPOFR */
    for (i = 0; i < 8; ++i)
    {
        /* plane n shows page n (8KB units from VRAM 0: A1 starts at page 16) */
        REG16(VDP2_REG + 0x50 + i * 2) = (u16)(((MAP_PAGE0 + i * 2 + 1) << 8) | (MAP_PAGE0 + i * 2));
        REG16(VDP2_REG + 0x60 + i * 2) = (u16)(((MAP_PAGE0 + i * 2 + 1) << 8) | (MAP_PAGE0 + i * 2));
    }
    REG16(VDP2_REG + 0xE6) = 0x0002;            /* CRAOFB: RBG0 colours from CRAM entry 0x200 */
    REG16(VDP2_REG + 0xB0) = 2;                 /* RPMD: switch A/B by A's coefficient MSB */
    REG16(VDP2_REG + 0xB2) = 0;                 /* RPRCTL: accumulate per line */
    REG16(VDP2_REG + 0xB4) = 0x0101;            /* KTCTL: coefficients on, 2-word, scale kx+ky (A and B) */
    REG16(VDP2_REG + 0xB6) = 0x0101;            /* KTAOF: coefficient tables start at bank B0 */
    REG16(VDP2_REG + 0xBC) = (u16)((RPT_BASE >> 1) >> 16);  /* RPTA (word address) */
    REG16(VDP2_REG + 0xBE) = (u16)((RPT_BASE >> 1) & 0xFFFF);
    REG16(VDP2_REG + 0xFC) = 3;                 /* PRIR: RBG0 below the sprite layer (6) */
    vdp2_bgon(0x0010, 0);                       /* BGON: RBG0 on */
    for (i = 0; i < 256; ++i)
        coef[0][i] = coef[1][i] = (s32)0x80000000;
    make_inv_d2();
    rot_commit();
}

void                rot_set_cells(const u8 *cells, int ncells, const u16 *pal256)
{
    volatile u16    *cram = (volatile u16 *)0x25F00000;
    int             i;

    if (ncells > 2048)
        ncells = 2048;
    memcpy((void *)VRAM(CHAR_BASE(0)), cells, (u32)ncells * 64);
    for (i = 0; i < 256; ++i)
        cram[0x200 + i] = pal256[i];
}

volatile u16        *rot_names(void)
{
    return (volatile u16 *)VRAM(MAP_BASE(0));
}

void                rot_set_horizon(int line)
{
    if (line != horizon)
    {
        horizon = line;
        make_inv_d2();
    }
}

void                rot_prepare(const rot_view *v)
{
    s32             *p;
    int             i, y, d2;
    s32             s = v->texels_per_unit;

    for (i = 0; i < 2; ++i)
    {
        p = param[i];
        memset(p, 0, sizeof(param[i]));
        p[0] = -160 * 65536;                    /* Xst */
        p[1] = 0;                               /* Yst */
        p[2] = v->focal * 65536;                /* Zst */
        p[5] = 65536;                           /* DX */
        /* A B C / D E F: X = right.x * sx + forward.x * focal, Y the same
           with z; right = (cos, -sin), forward = (sin, cos) */
        p[7] = v->cos_yaw;
        p[8] = 0;
        p[9] = v->sin_yaw;
        p[10] = -v->sin_yaw;
        p[11] = 0;
        p[12] = v->cos_yaw;
        /* words 0x1A..0x21: P and C, all zero */
        p[17] = v->cam_x * s;                   /* Mx */
        p[18] = v->cam_z * s;                   /* My */
        p[19] = 65536;                          /* kx */
        p[20] = 65536;                          /* ky */
        p[21] = (i ? 256 : 0) * 65536;          /* KAst: first coefficient index */
        p[22] = 65536;                          /* DKAst: one coefficient per line */
        p[23] = 0;                              /* DKAx */
    }
    {
        /* per line: z = h * focal / dy (dy = d2 / 2). All 32-bit: h * focal * 2
           fits for heights under ~70 units. Lines past fog_z are transparent. */
        s32 fz = v->floor_h * v->focal * 2, cz = v->ceil_h * v->focal * 2;
        s32 fk = v->floor_h * s * 2, ck = v->ceil_h * s * 2;
        s32 fmin = (s32)(((s64)fz + v->fog_z - 1) / v->fog_z);    /* smallest d2 inside the fog */
        s32 cmin = v->ceil_h > 0 ? (s32)(((s64)cz + v->fog_z - 1) / v->fog_z) : 0x7FFFFFFF;

        for (y = 0; y < LINES; ++y)
        {
            d2 = 2 * (y - horizon) + 1;         /* twice the pixel-centre offset from the horizon */
            if (v->flip)
                d2 = -d2;                       /* upside down: the floor's up there now */
            coef[0][y] = coef[1][y] = (s32)0x80000000;
            rot_row_z[y] = 0;
            if (d2 >= fmin)
            {
                coef[0][y] = (s32)(((u64)(u32)fk * inv_d2[y]) >> 32) & 0x00FFFFFF;
                rot_row_z[y] = (s32)(((u64)(u32)fz * inv_d2[y]) >> 32);
            }
            else if (-d2 >= cmin)
            {
                coef[1][y] = (s32)(((u64)(u32)ck * inv_d2[y]) >> 32) & 0x00FFFFFF;
                rot_row_z[y] = (s32)(((u64)(u32)cz * inv_d2[y]) >> 32);
            }
        }
    }
}

void                rot_commit(void)
{
    memcpy((void *)VRAM(RPT_BASE), param[0], 0x80);
    memcpy((void *)VRAM(RPT_BASE + 0x80), param[1], 0x80);
    memcpy((void *)VRAM(COEF_BASE), coef[0], LINES * 4);
    memcpy((void *)VRAM(COEF_BASE + 256 * 4), coef[1], LINES * 4);
}
