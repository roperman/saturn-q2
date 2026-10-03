/*
** The baked level (tools/bake_map.py): read off the CD onto the 4 MB RAM cart
** and used in place.
**
** The cart: its ID byte at 0x24FFFFFF reads 0x5A (1 MB) or 0x5C (4 MB). It's
** switched on and the A-bus timing set (settings.h CART_ASR0: SAROO's, which a
** Sega cart works with too); the 4 MB cart is one block at 0x02400000, read
** through the cache.
*/
#include "q2.h"

#define CART_ID         REG8(0x24FFFFFF)
#define CART_BASE       ((u8 *)0x02400000)
#define CART_SIZE       (0x400000)
#define NLUMPS          (13)

#define LWRAM_BASE      ((u8 *)0x00200000)
#ifdef SLAVE_PROF
#define LWRAM_END       ((u8 *)0x002E7000)      /* (the profiler's, above: src/main.c) */
#else
#define LWRAM_END       ((u8 *)0x00300000)
#endif
#define HWRAM_END       ((u8 *)0x060F8000)      /* the stacks are above (engine/link.ld) */

q_level             lv;
int                 cart_mb;
__attribute__((section(".lwdata"))) u32 level_try[2][5] = { { 0 } };   /* (each try at the level:
                                               cd_diag's four, the cart's first word; in low work RAM) */
__attribute__((section(".lwdata"))) int level_tries = 0;
extern u8           _bss_end[];         /* the linker's __bss_end (C names get an underscore) */
extern u8           _lwtext_end[];      /* the end of the code kept in low work RAM (engine/link.ld) */

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

#ifdef LEVEL_TEST
/* (OPT=-DLEVEL_TEST) the level file's lumps copied to work RAM whose cart copy nothing reads after
   (the DSP reads the planes', faces', lights' and axes' there): bytes */
u32                 level_dead, level_back;    /* (...and what of them went back to the cart) */

static const void   *hot_dead(const void *src, u32 bytes, bool high, u32 spare)
{
    const void      *q = hot_spare(src, bytes, high, spare);

    if (q != src)
        level_dead += bytes;
    return q;
}
#define HOT_D(s, b, h, sp)  hot_dead(s, b, h, sp)
#else
#define HOT_D(s, b, h, sp)  hot_spare(s, b, h, sp)
#endif

/* The lumps the level file has last (tools/bake_map.py TAIL, in its order): what the load copies to
   work RAM. Those copied, from the first of a run of them to the file's end, go back to the cart
   (~700 KB a level): nothing reads them there after (the DSP reads the planes', faces', lights' and
   axes', which aren't among them). A file not made that way gives nothing back */
static void         level_tail(const u32 *h, u32 size)
{
    static const u8 slot[7] = { 28, 26, 30, 10, 6, 4, 2 };  /* brushsides brushes leafbrushes cells marks leafs nodes */
    const void      *now[7];
    u32             end = size;
    int             k, i;

    now[0] = lv.brushsides; now[1] = lv.brushes; now[2] = lv.leafbrushes; now[3] = lv.cells;
    now[4] = lv.marks; now[5] = lv.leafs; now[6] = lv.nodes;
    for (k = 1; k < 7; ++k)
        if (h[slot[k]] <= h[slot[k - 1]])
            return;                         /* (not in this order) */
    for (i = 0; i < 62; i += 2)
        if (h[i] > h[slot[0]] && h[i] != h[slot[1]] && h[i] != h[slot[2]] && h[i] != h[slot[3]] && h[i] != h[slot[4]]
            && h[i] != h[slot[5]] && h[i] != h[slot[6]])
            return;                         /* (something else after them) */
    for (k = 6; k >= 0 && now[k] != CART_BASE + h[slot[k]]; --k)
        end = h[slot[k]];
#ifdef LEVEL_TEST
    level_back = size - end;
#endif
    cart_next = CART_BASE + ((end + 2047) & ~2047u);
}

/* Each brush's box, from its sides on the axes (every brush has them: qbsp adds them), in
   whole units rounded out: a trace skips a brush whose box misses its own without reading the
   brush or its sides (src/trace.c leaf_brushes). Where its box's sides are its first six (qbsp
   puts them there, -x +x -y +y -z +z) and on whole units, the box is them exactly, and the brush
   is marked BRUSH_EXACT: a trace clips those six from it (src/trace.c clip_box_brush). With the
   brushes: LWRAM if there's room with the reserve kept, else the cart */
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
        q_brush         *b = (q_brush *)&lv.brushes[i];     /* (its BRUSH_EXACT: in RAM, the copy's or the cart's) */
        s16             bb[6] = { -32767, -32767, -32767, 32767, 32767, 32767 };
        bool            exact = b->numsides >= 6;

        for (k = 0; k < 6 && exact; ++k)
        {
            const q_plane   *pl = &lv.planes[lv.brushsides[b->firstside + k].plane];

            exact = pl->type == k >> 1 && (pl->n[k >> 1] > 0) == (k & 1) && !(pl->dist & 0xFFFF)
                    && iabs(pl->dist >> 16) < 32000;
            if (exact)
                bb[(k & 1) * 3 + (k >> 1)] = (s16)(k & 1 ? pl->dist >> 16 : -(pl->dist >> 16));
        }
        b->contents = exact ? b->contents | BRUSH_EXACT : b->contents & ~BRUSH_EXACT;
        if (!exact)
            for (j = 0; j < 3; ++j)
            {
                bb[j] = -32767;
                bb[3 + j] = 32767;
            }
        for (k = 0; k < b->numsides && !exact; ++k)
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

/* (the load failed) the cart's RAM: patterns written and read back at a few places, through the
   uncached addresses; out: 0 if all good, else the address, what was written, what was read */
__attribute__((cold)) void level_ram_test(u32 *out)
{
    static const u32 at[4] = { 0, 0x100000, 0x200000, 0x3FFFF0 };
    int             i;

    out[0] = out[1] = out[2] = 0;
    for (i = 0; i < 4; ++i)
    {
        volatile u32    *p32 = (volatile u32 *)(0x22400000 + at[i]);
        volatile u16    *p16 = (volatile u16 *)(0x22400008 + at[i]);
        volatile u8     *p8 = (volatile u8 *)(0x2240000C + at[i]);
        u32             v;

        *p32 = 0x12345678 ^ at[i];
        *p16 = 0xA55A;
        *p8 = 0x3C;
        if ((v = *p32) != (0x12345678 ^ at[i]))
        {
            out[0] = 0x02400000 + at[i]; out[1] = 0x12345678 ^ at[i]; out[2] = v;
            return;
        }
        if ((v = *p16) != 0xA55A)
        {
            out[0] = 0x02400008 + at[i]; out[1] = 0xA55A; out[2] = v;
            return;
        }
        if ((v = *p8) != 0x3C)
        {
            out[0] = 0x0240000C + at[i]; out[1] = 0x3C; out[2] = v;
            return;
        }
    }
}

/* (the load failed) sector k of the file read again, alone, into low work RAM, and compared with
   what the load left on the cart: -1 the same, -2 not read, else the first byte that differs */
__attribute__((cold)) int level_sector_check(const char *name, u32 k, u32 *head)
{
    u32             lba, size, i;
    u8              *buf = (u8 *)(((u32)_lwtext_end + 15) & ~15u);     /* (nothing's there yet) */
    const volatile u8 *cart = (const volatile u8 *)(0x22400000 + k * 2048);

    if (!cd_find(name, &lba, &size) || k * 2048 >= size || !cd_read_sectors(lba + k, 1, buf))
        return -2;
    *head = (u32)buf[0] << 24 | (u32)buf[1] << 16 | (u32)buf[2] << 8 | buf[3];
    for (i = 0; i < 2048; ++i)
        if (buf[i] != cart[i])
            return (int)i;
    return -1;
}

#ifdef JUNK_RAM
/* (OPT=-DJUNK_RAM, a test) every memory the game doesn't clear itself filled with junk first, as a
   Saturn's is when it's switched on (an emulator's is all zeroes): the cart, low work RAM past the
   code there, high work RAM past .bss (not the stacks), VDP1's and VDP2's VRAM, colour RAM, sound
   RAM, the DSP's data RAM. Called first thing in main */
__attribute__((cold)) void junk_fill(void)
{
    u32             x = 0x2545F491, *p, *e;
    int             i;

    REG16(0x257EFFFE) = 1;                  /* (the cart on, as cart_init) */
    REG32(0x25FE00B0) = CART_ASR0;
    REG32(0x25FE00B8) = 0x00000013;
#define JUNK(a, b)  for (p = (u32 *)(a), e = (u32 *)(b); p < e; ++p) { x = x * 1664525u + 1013904223u; *p = x; }
    JUNK(0x22400000, 0x22800000);                                   /* the cart (uncached) */
    JUNK(((u32)_lwtext_end + 15) & ~15u, 0x00300000);               /* low work RAM */
    JUNK(((u32)_bss_end + 15) & ~15u, 0x060F8000);                  /* high work RAM, below the stacks */
    JUNK(0x25C00000, 0x25C80000);                                   /* VDP1 VRAM */
    JUNK(0x25C80000, 0x25CC0000);                                   /* VDP1's frame buffer */
    JUNK(0x25E00000, 0x25E80000);                                   /* VDP2 VRAM */
    JUNK(0x25F00000, 0x25F01000);                                   /* colour RAM */
    JUNK(0x25A00000, 0x25A80000);                                   /* sound RAM */
#undef JUNK
    DSP_PPAF = 0;                           /* (stopped) the DSP's data RAM */
    DSP_PDA = 0;
    for (i = 0; i < 256; ++i)
    {
        x = x * 1664525u + 1013904223u;
        DSP_PDD = x;
    }
    cache_purge();
}
#endif

static int          cart_init(void)
{
    u8              id = CART_ID;

    if (id != 0x5A && id != 0x5C)
        return 0;
    REG16(0x257EFFFE) = 1;                  /* switch it on */
    REG32(0x25FE00B0) = CART_ASR0;          /* SCU ASR0: A-bus CS0/CS1 timing (settings.h) */
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
    /* the level onto the cart in one read; if that fails, again in runs of 16 sectors, and every read
       after it the same (safe mode). (SAROO, an SD card drive for the Saturn, wouldn't load it with
       32 sectors a transfer: engine/cd.c's cd_max_get) */
    for (level_tries = 0; level_tries < 2; ++level_tries)
    {
        cd_diag[0] = cd_diag[1] = cd_diag[2] = cd_diag[3] = 0;
        size = cd_load(name, CART_BASE, CART_SIZE);
        memcpy(level_try[level_tries], cd_diag, 16);
#if defined(LOAD_FAIL_TEST) || defined(LOAD_SAFE_TEST)
        /* (tests: OPT=-DLOAD_SAFE_TEST spoils the first try, -DLOAD_FAIL_TEST both) */
# ifdef LOAD_SAFE_TEST
        if (level_tries == 0)
# endif
            *(volatile u8 *)b = 0;             /* (through the cache: it writes through, and a line
                                                   memcmp read stays as written) */
#endif
        level_try[level_tries][4] = *(const volatile u32 *)((u32)b | 0x20000000);
        if (size >= 128 && !memcmp(b, "Q2SL", 4))
            break;
        cd_max_play = 16;                   /* (safe mode: short plays too) */
    }
    if (level_tries == 2)
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
    /* (h[36], h[37]: the sky, which has its own file now: render_sky_init) */
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
    lv.portals = (const q_portal *)(b + h[58]);
    lv.nportals = (int)h[59];
    lv.cportals = (const u16 *)(b + h[60]);
    lv.tile_ofs = (const u32 *)(b + h[62]);
    lv.masks = (const u16 *)(b + h[64]);
    cart_next = CART_BASE + (((u32)size + 2047) & ~2047u);
    hw_next = (u8 *)(((u32)_bss_end + 15) & ~15u);
    lw_next = (u8 *)(((u32)_lwtext_end + 15) & ~15u);   /* (after the code that lives there) */
#ifdef LEVEL_TEST
    level_dead = 0;
#endif
    lv.nodes = HOT_D(lv.nodes, (u32)lv.nnodes * sizeof(q_node), true, 0);
    lv.planes_cart = lv.planes;
    lv.planes = hot(lv.planes, (u32)lv.nplanes * sizeof(q_plane), true);
    lv.leafs = HOT_D(lv.leafs, (u32)lv.nleafs * sizeof(q_leaf), true, 0);
    lv.marks = HOT_D(lv.marks, h[7] * 2, true, 0);
    lv.axes_cart = lv.axes;
    lv.axes = hot(lv.axes, h[47] * 24, true);
    lv.naxes = (int)h[47];
    lv.faces_cart = lv.faces;
    lv.faces = hot(lv.faces, (u32)lv.nfaces * sizeof(q_face), false);
    lv.cells = HOT_D(lv.cells, h[11] * sizeof(q_cell), false, 0);
    lv.lights_cart = lv.lights;
    lv.lights = hot(lv.lights, h[13] * 2, false);
    lv.brushes = HOT_D(lv.brushes, (u32)lv.nbrushes * sizeof(q_brush), false, LW_RESERVE);
    lv.brushsides = HOT_D(lv.brushsides, h[29] * sizeof(q_brushside), false, LW_RESERVE);
    lv.leafbrushes = HOT_D(lv.leafbrushes, h[31] * 2, false, LW_RESERVE);
    level_tail(h, (u32)size);
    brush_bounds();
    /* the portals (the renderer's flow reads them every frame): low work RAM if there's room */
    lv.portals = hot_spare(lv.portals, (u32)lv.nportals * sizeof(q_portal), false, LW_RESERVE);
    lv.cportals = hot_spare(lv.cportals, ((u32)lv.nclusters + 1 + 4 * (u32)lv.nportals) * 2, false, LW_RESERVE);
    {
        const s32 *s = (const s32 *)(b + h[24]);

        lv.start[0] = s[0];
        lv.start[1] = s[1];
        lv.start[2] = s[2];
        lv.start_yaw = (int)(((s64)s[3] * 0x10000 / 360) >> 16);
    }
    return true;
}

__attribute__((hot)) int level_leaf(const s32 *p)     /* (each frame, for each entity: kept in high work RAM) */
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

#ifdef NO_FACEVIS
    if (1)                                  /* (OPT=-DNO_FACEVIS: every face the PVS lets through) */
#else
    if (cluster < 0 || !lv.facevis)
#endif
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
