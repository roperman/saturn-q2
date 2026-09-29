/*
** The baked level (tools/bake_map.py): read off the CD onto the 4 MB RAM cart
** and used in place.
**
** The cart: its ID byte at 0x24FFFFFF reads 0x5A (1 MB) or 0x5C (4 MB). It's
** switched on and the A-bus timing set the way libyaul does it (as the taxi
** game does); the 4 MB cart is one block at 0x02400000, read through the cache.
*/
#include "q2.h"

#define CART_ID         REG8(0x24FFFFFF)
#define CART_BASE       ((u8 *)0x02400000)
#define CART_SIZE       (0x400000)
#define NLUMPS          (13)

#define LWRAM_BASE      ((u8 *)0x00200000)
#define LWRAM_END       ((u8 *)0x00300000)
#define HWRAM_END       ((u8 *)0x060F8000)      /* the stacks are above (engine/link.ld) */

q_level             lv;
int                 cart_mb;
extern u8           _bss_end[];         /* the linker's __bss_end (C names get an underscore) */

/* The cart is on the A-bus, slow to read even through the cache. What the
   renderer reads every frame is copied into work RAM: the BSP into high
   work RAM (fastest), the faces, cells and lights into low. */
static u8           *hw_next, *lw_next, *cart_next;

static void         out_of_ram(const char *what, u32 bytes);

const u8            *cart_load(const char *name)
{
    u32             space = (u32)(CART_BASE + CART_SIZE - cart_next), lba, want;
    int             size = cd_load(name, cart_next, space);
    const u8        *p = cart_next;

    if (size < 0)
    {
        if (cd_find(name, &lba, &want) && ((want + 2047) & ~2047u) > space)
            out_of_ram("THE CART", ((want + 2047) & ~2047u) - space);    /* (not just missing) */
        return NULL;
    }
    cart_next += ((u32)size + 2047) & ~2047u;
    return p;
}

u32                 cart_free(void)
{
    return (u32)(CART_BASE + CART_SIZE - cart_next);
}

u8                  *cart_alloc(u32 bytes)
{
    u8              *p = cart_next;

    bytes = (bytes + 2047) & ~2047u;
    if (p + bytes > CART_BASE + CART_SIZE)
        out_of_ram("THE CART", (u32)(p + bytes - (CART_BASE + CART_SIZE)));
    cart_next += bytes;
    return p;
}

/* LWRAM kept free for what's allocated after the level (level_alloc_low: the
   game's entities, the BSP's parents): the brushes (only the traces read
   them) stay on the cart rather than take it */
#define LW_RESERVE      (48 * 1024)

static const void   *hot_spare(const void *src, u32 bytes, bool high, u32 spare);

static const void   *hot(const void *src, u32 bytes, bool high)
{
    return hot_spare(src, bytes, high, 0);
}

static const void   *hot_spare(const void *src, u32 bytes, bool high, u32 spare)
{
    u8              **next = high ? &hw_next : &lw_next;
    u8              *end = (high ? HWRAM_END : LWRAM_END) - spare;
    u8              *dst = *next;

    if (dst + bytes > end)
        return src;                         /* no room: read it off the cart */
    memcpy(dst, src, (bytes + 3) & ~3u);
    *next = dst + ((bytes + 15) & ~15u);
    return dst;
}

/* Each brush's box, from its sides on the axes (every brush has them: qbsp adds them), in
   whole units rounded out: a trace skips a brush whose box misses its own without reading the
   brush or its sides (src/trace.c leaf_brushes). With the brushes: LWRAM if there's room with
   the reserve kept, else the cart */
static void         brush_bounds(void)
{
    u32             bytes = (u32)lv.nbrushes * 12;
    int             i, k, j;

    if (lw_next + bytes <= LWRAM_END - LW_RESERVE)
    {
        lv.brushbounds = (s16 *)lw_next;
        lw_next += (bytes + 15) & ~15u;
    }
    else
        lv.brushbounds = (s16 *)cart_alloc(bytes);
    for (i = 0; i < lv.nbrushes; ++i)
    {
        const q_brush   *b = &lv.brushes[i];
        s16             bb[6] = { -32767, -32767, -32767, 32767, 32767, 32767 };

        for (k = 0; k < b->numsides; ++k)
        {
            const q_plane   *pl = &lv.planes[lv.brushsides[b->firstside + k].plane];
            int             ty = pl->type;

            if (ty >= 3)
                continue;
            if (pl->n[ty] > 0)
                bb[3 + ty] = (s16)imin(bb[3 + ty], ((pl->dist + 0xFFFF) >> 16) + 1);
            else
                bb[ty] = (s16)imax(bb[ty], ((-pl->dist) >> 16) - 1);
        }
        for (j = 0; j < 6; ++j)
            lv.brushbounds[i * 6 + j] = bb[j];
    }
}

/* What a trace reads for every brush it might meet, into HWRAM if there's room once
   everything else has had its share (the models' records last of all): the brushes' boxes,
   the leaves' lists of them, the brushes. Installation has the room (its boxes are on the
   cart otherwise); 4 KB kept */
void                level_trace_hot(void)
{
    lv.brushbounds = (s16 *)hot_spare(lv.brushbounds, (u32)lv.nbrushes * 12, true, 4096);
    lv.leafbrushes = hot_spare(lv.leafbrushes, (u32)lv.nleafbrushes * 2, true, 4096);
    lv.brushes = hot_spare(lv.brushes, (u32)lv.nbrushes * sizeof(q_brush), true, 4096);
}

const void          *level_hot(const void *src, u32 bytes)
{
    return hot(src, bytes, true);
}

const void          *level_hot_keep(const void *src, u32 bytes, u32 keep)
{
    return hot_spare(src, bytes, true, keep);
}

u32                 level_heap(void)
{
    return (u32)hw_next;
}

/* what's left: high work RAM, low, the cart */
void                level_free(u32 *hw, u32 *lw, u32 *cart)
{
    *hw = (u32)(HWRAM_END - hw_next);
    *lw = (u32)(LWRAM_END - lw_next);
    *cart = cart_free();
}

/* out of memory: say so and stop (going on would write over the stacks) */
static void         out_of_ram(const char *what, u32 bytes)
{
    for (;;)
    {
        vdp_begin();
        vdp_printf(16, 100, RGB(255, 80, 60), "OUT OF %s: %d BYTES MORE", what, (int)bytes);
        vdp_submit();
    }
}

/* low work RAM, for what's not read every frame (after the level's data) */
void                *level_alloc_low(u32 bytes)
{
    u8              *p = lw_next;

    bytes = (bytes + 15) & ~15u;
    if (p + bytes > LWRAM_END)
        out_of_ram("LOW WORK RAM", (u32)(p + bytes - LWRAM_END));
    lw_next += bytes;
    return p;
}

void                *level_alloc(u32 bytes)
{
    u8              *p = hw_next;

    bytes = (bytes + 15) & ~15u;
    if (p + bytes > HWRAM_END)
        out_of_ram("HIGH WORK RAM", (u32)(p + bytes - HWRAM_END));
    hw_next += bytes;
    return p;
}

static int          cart_init(void)
{
    u8              id = CART_ID;

    if (id != 0x5A && id != 0x5C)
        return 0;
    REG16(0x257EFFFE) = 1;                  /* switch it on */
    REG32(0x25FE00B0) = 0x23301FF0;         /* SCU ASR0: A-bus CS0/CS1 timing */
    REG32(0x25FE00B8) = 0x00000013;         /* SCU AREF: A-bus refresh */
    return id == 0x5C ? 4 : 1;
}

bool                level_load(const char *name)
{
    const u8        *b = CART_BASE;
    const u32       *h;
    int             size;

    cart_mb = cart_init();
    if (cart_mb < 4 || !cd_init())
        return false;
    size = cd_load(name, CART_BASE, CART_SIZE);
    if (size < 128 || memcmp(b, "Q2SL", 4))
        return false;
    h = (const u32 *)(b + 12);
    lv.T = ((const u16 *)b)[4];
    lv.N = ((const u16 *)b)[5];
    for (lv.nshift = 0; (1 << lv.nshift) < lv.N; ++lv.nshift)
        ;
    /* (offset, count) pairs, in bake_map.py's order */
    lv.planes = (const q_plane *)(b + h[0]);    lv.nplanes = (int)h[1];
    lv.nodes = (const q_node *)(b + h[2]);      lv.nnodes = (int)h[3];
    lv.leafs = (const q_leaf *)(b + h[4]);      lv.nleafs = (int)h[5];
    lv.marks = (const u16 *)(b + h[6]);
    lv.faces = (const q_face *)(b + h[8]);      lv.nfaces = (int)h[9];
    lv.cells = (const q_cell *)(b + h[10]);
    lv.lights = (const u16 *)(b + h[12]);
    lv.textures = (const q_tex *)(b + h[14]);   lv.ntextures = (int)h[15];
    lv.texdata = b + h[16];
    lv.luts = (const u16 *)(b + h[18]);         lv.nluts = (int)h[19];
    lv.vis = b + h[20];                         lv.nclusters = (int)h[21];
    lv.models = (const q_model *)(b + h[22]);   lv.nmodels = (int)h[23];
    lv.brushes = (const q_brush *)(b + h[26]);  lv.nbrushes = (int)h[27];
    lv.brushsides = (const q_brushside *)(b + h[28]);
    lv.leafbrushes = (const u16 *)(b + h[30]);   lv.nleafbrushes = (int)h[31];
    lv.movers = (const q_mover *)(b + h[32]);
    lv.facevis = h[35] ? b + h[34] : NULL;
    lv.sky = h[37] ? (const u16 *)(b + h[36]) : NULL;
    lv.spawns = (const q_spawn *)(b + h[38]);   lv.nspawns = (int)h[39];
    lv.leaflight = (const u16 *)(b + h[40]);
    lv.erecs = b + h[42];                       lv.nerecs = (int)h[43];
    lv.strings = (const char *)(b + h[44]);
    lv.axes = (const s32 *)(b + h[46]);
    lv.quart0 = (int)*(const u32 *)(b + h[48]);
    lv.lodfaces = (const q_lodface *)(b + h[50]);
    lv.lodcells = (const q_cell *)(b + h[52]);
    lv.lodlights = (const u16 *)(b + h[54]);
    lv.starts = (const q_start *)(b + h[56]);
    lv.nstarts = (int)h[57];
    cart_next = CART_BASE + (((u32)size + 2047) & ~2047u);
    hw_next = (u8 *)(((u32)_bss_end + 15) & ~15u);
    lw_next = LWRAM_BASE;
    lv.nodes = hot(lv.nodes, (u32)lv.nnodes * sizeof(q_node), true);
    lv.planes = hot(lv.planes, (u32)lv.nplanes * sizeof(q_plane), true);
    lv.leafs = hot(lv.leafs, (u32)lv.nleafs * sizeof(q_leaf), true);
    lv.marks = hot(lv.marks, h[7] * 2, true);
    lv.axes = hot(lv.axes, h[47] * 24, true);
    lv.naxes = (int)h[47];
    lv.faces_cart = lv.faces;
    lv.faces = hot(lv.faces, (u32)lv.nfaces * sizeof(q_face), false);
    lv.cells = hot(lv.cells, h[11] * sizeof(q_cell), false);
    lv.lights = hot(lv.lights, h[13] * 2, false);
    lv.brushes = hot_spare(lv.brushes, (u32)lv.nbrushes * sizeof(q_brush), false, LW_RESERVE);
    lv.brushsides = hot_spare(lv.brushsides, h[29] * sizeof(q_brushside), false, LW_RESERVE);
    lv.leafbrushes = hot_spare(lv.leafbrushes, h[31] * 2, false, LW_RESERVE);
    brush_bounds();
    {
        const s32 *s = (const s32 *)(b + h[24]);

        lv.start[0] = s[0];
        lv.start[1] = s[1];
        lv.start[2] = s[2];
        lv.start_yaw = (int)(((s64)s[3] * 0x10000 / 360) >> 16);
    }
    return true;
}

int                 level_leaf(const s32 *p)
{
    int             n = 0;

    while (n >= 0)
    {
        const q_node    *node = &lv.nodes[n];
        const q_plane   *pl = &lv.planes[node->plane];
        s32             d;

        if (pl->type < 3)
            d = p[pl->type] - pl->dist;
        else
            d = fmul(p[0], pl->n[0]) + fmul(p[1], pl->n[1]) + fmul(p[2], pl->n[2]) - pl->dist;
        n = node->child[d < 0];
    }
    return -(n + 1);
}

/* runs of zero bytes are a 0 then a count (Quake's PVS; ours for faces too) */
static void         unzero(const u8 *in, u8 *row, int bytes)
{
    int             o = 0;

    while (o < bytes)
    {
        if (*in)
        {
            row[o++] = *in++;
            continue;
        }
        {
            int c = in[1];

            in += 2;
            while (c-- && o < bytes)
                row[o++] = 0;
        }
    }
}

void                level_facevis(int cluster, u8 *bits)
{
    int             bytes = (lv.nfaces + 7) >> 3;

    if (cluster < 0 || !lv.facevis)
    {
        memset(bits, 0xFF, (u32)bytes);
        return;
    }
    unzero(lv.facevis + ((const u32 *)lv.facevis)[1 + cluster], bits, bytes);
}

const u8            *level_pvs(int cluster)
{
    static u8       row[1024];
    int             bytes = (lv.nclusters + 7) >> 3, o = 0;
    const u8        *in;

    if (cluster < 0 || !lv.nclusters || bytes > (int)sizeof(row))
    {
        memset(row, 0xFF, sizeof(row));
        return row;
    }
    in = lv.vis + ((const u32 *)lv.vis)[1 + cluster * 2];
    while (o < bytes)
    {
        if (*in)
        {
            row[o++] = *in++;
            continue;
        }
        {
            int c = in[1];

            in += 2;
            while (c-- && o < bytes)
                row[o++] = 0;
        }
    }
    return row;
}
