/*
** Box traces against the map's brushes: Quake 2's qcommon/cmodel.c
** (CM_BoxTrace and what it calls), in 16.16 fixed point. The BSP finds the
** leaves the box's sweep passes through; each leaf lists the brushes in it;
** each brush is a convex set of planes, pushed out by the box's size.
*/
#include "q2.h"

#define DIST_EPSILON    (2048)              /* 1/32: keeps the box off the planes */
#define MAX_BOX_LEAFS   (128)
#define SHORT_MOVE      FIX(64)             /* a move no longer on any axis: by the leaves its box touches */
#ifdef TRACE_CHECK
u32                 trace_checks, trace_diffs, trace_dkind[4];     /* start, all solid, plane, plane's normal */
s32                 trace_worst;
u32                 trace_later;
#endif

static s32          t_start[3], t_end[3], t_mins[3], t_maxs[3], t_ext[3];
static s16          t_bb[6];                /* the box the whole trace sweeps, whole units (a unit more round) */
static int          t_mask;
#ifdef BOUNDS_CHECK
u32                 bounds_checks, bounds_diffs, bounds_skipped;
static bool         t_nobounds;
#endif
static bool         t_ispoint;
static q_trace      tr;
static u16          *brush_check, checkcount;
static int          box_leafs[MAX_BOX_LEAFS], nbox_leafs;
#if defined(NO_BOX_ASM) || defined(BOX_CHECK) || defined(CLIP_CHECK)
# define BOX_C                              /* (the C's leaf_brushes, clip_box_brush: OPT=-DNO_BOX_ASM, or to check) */
#endif
#ifndef NO_BOX_ASM
/* the leaves, and their brushes, in assembly (src/tbox.s): its offsets are its A_ */
typedef struct
{
    s32             tbb[6];                 /* t_bb */
    const s16       *bounds;
    u16             *check;
    u32             count;                  /* checkcount */
    s32             mask;
    q_trace         *tr;
    const q_leaf    *leafs;
    const u16       *leafbrushes;
    const q_brush   *brushes;
    const q_brushside *sides;
    const q_plane   *planes;
    const q_node    *nodes;
    s32             ispoint;
    int             *list;
    s32             nlist, test;
    s32             sl[3], sh[3], el[3], eh[3];     /* start + mins, start + maxs, end + mins, end + maxs */
    s32             start[3], end[3], mins[3], maxs[3];
    s32             c1[3], c2[3];           /* leafs_asm's box */
    s32             stack[64];
}                   box_args;
_Static_assert(__builtin_offsetof(box_args, bounds) == 24 && __builtin_offsetof(box_args, tr) == 40
               && __builtin_offsetof(box_args, planes) == 60 && __builtin_offsetof(box_args, test) == 80
               && __builtin_offsetof(box_args, sl) == 84 && __builtin_offsetof(box_args, start) == 132
               && __builtin_offsetof(box_args, mins) == 156 && __builtin_offsetof(box_args, c1) == 180
               && __builtin_offsetof(box_args, stack) == 204, "src/tbox.s: box_args");
_Static_assert(__builtin_offsetof(q_trace, startsolid) == 4 && __builtin_offsetof(q_trace, fraction) == 8
               && __builtin_offsetof(q_trace, plane) == 24 && __builtin_offsetof(q_trace, contents) == 28
               && __builtin_offsetof(q_trace, surf_flags) == 32, "src/tbox.s: q_trace");
int                 leafs_asm(box_args *a, int num);
void                brushes_asm(box_args *a);
static box_args     ba;
#endif
#ifdef FIGHT_BENCH
u32                 tr_count[8];            /* (the fight benchmark: brushes looked at, leaves, brushes clipped, sides, box traces, movers'; line traces' nodes, line traces) */
u32                 tr_ticks[5];            /* gathering the leaves, clipping, the movers, the entities; line traces */
# define TR_T0          u32 tt0 = frt_read()
# define TR_TICKS(i)    (tr_ticks[i] += (frt_read() - tt0) & 0xFFFF, tt0 = frt_read())
# define TR_COUNT(i, n) (tr_count[i] += (n))
#else
# define TR_COUNT(i, n) ((void)0)
# define TR_T0          ((void)0)
# define TR_TICKS(i)    ((void)0)
#endif

__attribute__((cold)) void trace_init(void)
{
    brush_check = level_alloc_low((u32)lv.nbrushes * 2);  /* (read only for brushes whose box the trace's meets) */
    memset(brush_check, 0, (u32)lv.nbrushes * 2);
    trace_world_init();
}

static inline s32   dot(const s32 *a, const s32 *n)
{
    return fmul(a[0], n[0]) + fmul(a[1], n[1]) + fmul(a[2], n[2]);
}

/* n / d, 16.16, kept to +-2 (the callers clamp to 0..1 anyway) */
static s32          frac_div(s32 n, s32 d)
{
    if (d < 0)
    {
        n = -n;
        d = -d;
    }
    if (d == 0 || n >= 2 * d)
        return n >= 0 ? FIX(2) : -FIX(2);
    if (n <= -2 * d)
        return -FIX(2);
    divu_start(n >> 16, (u32)n << 16, d);  /* (fdiv's, here: not a call) */
    return divu_result();
}

#ifdef BOX_C
/* clipping a brush, a side at a time: how far into the move the box enters its solid (the
   latest entering side), and leaves it (the earliest leaving one) */
typedef struct { s32 enter, leave; int side; bool getout, startout; } t_clip;

/* a side's part: d1, d2 the start's and the end's distance in front of it (pushed out for the
   box); false: the box is wholly in front of it, no hit */
static inline bool  clip_side(t_clip *c, s32 d1, s32 d2, int i)
{
    s32             f;

    if (d2 > 0)
        c->getout = true;                   /* the end isn't in the solid */
    if (d1 > 0)
        c->startout = true;
    if (d1 > 0 && d2 >= d1)
        return false;                       /* completely in front of this side: no hit */
    if (d1 <= 0 && d2 <= 0)
        return true;
    if (d1 > d2)
    {
        /* entering */
        f = frac_div(d1 - DIST_EPSILON, d1 - d2);
        if (f > c->enter)
        {
            c->enter = f;
            c->side = i;
        }
    }
    else
    {
        f = frac_div(d1 + DIST_EPSILON, d1 - d2);
        if (f < c->leave)
            c->leave = f;
    }
    return true;
}

/* bb: its box (lv.brushbounds'). A BRUSH_EXACT brush's first six sides, -x +x -y +y -z +z, are
   clipped from that (exactly their planes: the same sums), not read; the side entered is read
   only if it's the trace's */
static void         clip_box_brush(const q_brush *b, const s16 *bb)
{
    t_clip          c = { -FIX(1), FIX(1), -1, false, false };
    s32             ofs[3], dist, d1, d2;
    int             i = 0, j;

    TR_COUNT(2, 1);
    TR_COUNT(3, b->numsides);
    if (b->contents & BRUSH_EXACT)
        for (; i < 6; ++i)
        {
            int ty = i >> 1;

            if (i & 1)
            {
                dist = (s32)bb[3 + ty] * 65536 - t_mins[ty];
                d1 = t_start[ty] - dist;
                d2 = t_end[ty] - dist;
            }
            else
            {
                dist = -(s32)bb[ty] * 65536 + t_maxs[ty];
                d1 = -t_start[ty] - dist;
                d2 = -t_end[ty] - dist;
            }
            if (!clip_side(&c, d1, d2, i))
                return;
        }
    for (; i < b->numsides; ++i)
    {
        const q_plane   *pl = &lv.planes[lv.brushsides[b->firstside + i].plane];
        int             ty = pl->type;

        if (ty < 3)
        {
            /* on an axis (every brush's first six sides, its box, and most of the
               rest): the normal's +-1 along it, so no multiplies (the same sums) */
            if (pl->n[ty] > 0)
            {
                dist = pl->dist - t_mins[ty];
                d1 = t_start[ty] - dist;
                d2 = t_end[ty] - dist;
            }
            else
            {
                dist = pl->dist + t_maxs[ty];
                d1 = -t_start[ty] - dist;
                d2 = -t_end[ty] - dist;
            }
        }
        else
        {
            if (!t_ispoint)
            {
                /* push the plane out for the box */
                for (j = 0; j < 3; ++j)
                    ofs[j] = pl->n[j] < 0 ? t_maxs[j] : t_mins[j];
                dist = pl->dist - dot(ofs, pl->n);
            }
            else
                dist = pl->dist;
            d1 = dot(t_start, pl->n) - dist;
            d2 = dot(t_end, pl->n) - dist;
        }
        if (!clip_side(&c, d1, d2, i))
            return;
    }
    if (!c.startout)
    {
        /* the start was inside the brush */
        tr.startsolid = true;
        if (!c.getout)
            tr.allsolid = true;
        return;
    }
    if (c.enter < c.leave && c.enter > -FIX(1) && c.enter < tr.fraction)
    {
        const q_brushside *side = &lv.brushsides[b->firstside + c.side];

        tr.fraction = c.enter < 0 ? 0 : c.enter;
        tr.plane = &lv.planes[side->plane];
        tr.surf_flags = side->flags;
        tr.contents = b->contents & ~BRUSH_EXACT;
    }
}

#ifdef CLIP_CHECK
/* (OPT=-DCLIP_CHECK: as it was, every side read: the same?) */
u32                 clip_checks, clip_diffs, clip_near, clip_near_diffs;

static void         clip_box_brush_ref(const q_brush *b)
{
    s32             enterfrac = -FIX(1), leavefrac = FIX(1), ofs[3], dist, d1, d2, f;
    const q_plane   *clipplane = NULL;
    int             i, j, leadflags = 0;
    bool            getout = false, startout = false;


    for (i = 0; i < b->numsides; ++i)
    {
        const q_brushside   *side = &lv.brushsides[b->firstside + i];
        const q_plane       *pl = &lv.planes[side->plane];
        int                 ty = pl->type;

        if (ty < 3)
        {
            /* on an axis (every brush's first six sides, its box, and most of the
               rest): the normal's +-1 along it, so no multiplies (the same sums) */
            if (pl->n[ty] > 0)
            {
                dist = pl->dist - t_mins[ty];
                d1 = t_start[ty] - dist;
                d2 = t_end[ty] - dist;
            }
            else
            {
                dist = pl->dist + t_maxs[ty];
                d1 = -t_start[ty] - dist;
                d2 = -t_end[ty] - dist;
            }
        }
        else
        {
            if (!t_ispoint)
            {
                /* push the plane out for the box */
                for (j = 0; j < 3; ++j)
                    ofs[j] = pl->n[j] < 0 ? t_maxs[j] : t_mins[j];
                dist = pl->dist - dot(ofs, pl->n);
            }
            else
                dist = pl->dist;
            d1 = dot(t_start, pl->n) - dist;
            d2 = dot(t_end, pl->n) - dist;
        }
        if (d2 > 0)
            getout = true;                  /* the end isn't in the solid */
        if (d1 > 0)
            startout = true;
        if (d1 > 0 && d2 >= d1)
        {
            return;                         /* completely in front of this side: no hit */
        }
        if (d1 <= 0 && d2 <= 0)
            continue;
        if (d1 > d2)
        {
            /* entering */
            f = frac_div(d1 - DIST_EPSILON, d1 - d2);
            if (f > enterfrac)
            {
                enterfrac = f;
                clipplane = pl;
                leadflags = side->flags;
            }
        }
        else
        {
            f = frac_div(d1 + DIST_EPSILON, d1 - d2);
            if (f < leavefrac)
                leavefrac = f;
        }
    }
    if (!startout)
    {
        /* the start was inside the brush */
        tr.startsolid = true;
        if (!getout)
            tr.allsolid = true;
        return;
    }
    if (enterfrac < leavefrac && enterfrac > -FIX(1) && enterfrac < tr.fraction)
    {
        if (enterfrac < 0)
            enterfrac = 0;
        tr.fraction = enterfrac;
        tr.plane = clipplane;
        tr.surf_flags = leadflags;
        tr.contents = b->contents & ~BRUSH_EXACT;
    }
}

#endif

static void         test_box_brush(const q_brush *b)
{
    s32             ofs[3], dist;
    int             i, j;

    if (!b->numsides)
        return;
    for (i = 0; i < b->numsides; ++i)
    {
        const q_plane *pl = &lv.planes[lv.brushsides[b->firstside + i].plane];
        int           ty = pl->type;

        if (ty < 3)
        {
            if (pl->n[ty] > 0 ? t_start[ty] - pl->dist + t_mins[ty] > 0 : -t_start[ty] - pl->dist - t_maxs[ty] > 0)
                return;
            continue;
        }
        for (j = 0; j < 3; ++j)
            ofs[j] = pl->n[j] < 0 ? t_maxs[j] : t_mins[j];
        dist = pl->dist - dot(ofs, pl->n);
        if (dot(t_start, pl->n) - dist > 0)
            return;
    }
    tr.startsolid = tr.allsolid = true;
    tr.fraction = 0;
    tr.contents = b->contents & ~BRUSH_EXACT;
}

static void         leaf_brushes(int leafnum, bool test)
{
    const q_leaf    *leaf = &lv.leafs[leafnum];
    int             k;

    TR_COUNT(1, 1);
    if (!(leaf->contents & t_mask))
        return;
    for (k = 0; k < leaf->numbrushes; ++k)
    {
        int             bn = lv.leafbrushes[leaf->firstbrush + k];
        const q_brush   *b = &lv.brushes[bn];

        const s16       *bb = &lv.brushbounds[bn * 6];

        TR_COUNT(0, 1);                     /* (the benchmark: brushes looked at) */
        /* its box off the trace's: nothing to clip (and the brush and its sides not read); before
           the mark that it's been done (most are off, and a store's dear with the slave drawing) */
        if (bb[0] > t_bb[3] || bb[3] < t_bb[0] || bb[1] > t_bb[4] || bb[4] < t_bb[1] || bb[2] > t_bb[5]
            || bb[5] < t_bb[2])
        {
#ifdef BOUNDS_CHECK
            ++bounds_skipped;
            if (!t_nobounds)
#endif
#ifdef CLIP_CHECK
            /* (an exact box's brush the box rounded out a unit, as before, wouldn't have left: clipped
               all the same, does it change anything?) */
            if (!test && b->contents & BRUSH_EXACT && b->contents & t_mask && !(bb[0] - 1 > t_bb[3]
                || bb[3] + 1 < t_bb[0] || bb[1] - 1 > t_bb[4] || bb[4] + 1 < t_bb[1] || bb[2] - 1 > t_bb[5]
                || bb[5] + 1 < t_bb[2]))
            {
                extern u32 clip_near, clip_near_diffs;
                q_trace was = tr;

                clip_box_brush_ref(b);
                ++clip_near;
                clip_near_diffs += tr.fraction != was.fraction || tr.plane != was.plane || tr.startsolid != was.startsolid
                                   || tr.allsolid != was.allsolid;
                tr = was;
            }
#endif
            continue;
        }
        if (brush_check[bn] == checkcount)
            continue;                       /* already done from another leaf */
        brush_check[bn] = checkcount;
        if (!(b->contents & t_mask) || !b->numsides)
            continue;
        if (test)
            test_box_brush(b);
        else
        {
#ifdef CLIP_CHECK
            q_trace was = tr, ref;

            clip_box_brush_ref(b);
            ref = tr;
            tr = was;
#endif
            clip_box_brush(b, bb);
#ifdef CLIP_CHECK
            ++clip_checks;
            clip_diffs += tr.fraction != ref.fraction || tr.plane != ref.plane || tr.surf_flags != ref.surf_flags
                          || tr.contents != ref.contents || tr.startsolid != ref.startsolid || tr.allsolid != ref.allsolid;
#endif
        }
        if (!tr.fraction)
            return;
    }
}

#endif

/* a long move, down the tree along it (seldom: low work RAM) */
static __attribute__((cold)) void hull_check(int num, s32 p1f, s32 p2f, const s32 *p1, const s32 *p2)
{
    const q_node    *node;
    const q_plane   *pl;
    s32             t1, t2, offset, frac, frac2, midf, mid[3];
    int             side, i;

    if (tr.fraction <= p1f)
        return;                             /* already hit something nearer */
    if (num < 0)
    {
#ifndef NO_BOX_ASM
        int leaf = -1 - num;

        ba.list = &leaf;
        ba.nlist = 1;
        ba.test = 0;
        brushes_asm(&ba);
#else
        leaf_brushes(-1 - num, false);
#endif
        return;
    }
    node = &lv.nodes[num];
    pl = &lv.planes[node->plane];
    if (pl->type < 3)
    {
        t1 = p1[pl->type] - pl->dist;
        t2 = p2[pl->type] - pl->dist;
        offset = t_ext[pl->type];
    }
    else
    {
        t1 = dot(p1, pl->n) - pl->dist;
        t2 = dot(p2, pl->n) - pl->dist;
        offset = t_ispoint ? 0 : iabs(fmul(t_ext[0], pl->n[0])) + iabs(fmul(t_ext[1], pl->n[1]))
                               + iabs(fmul(t_ext[2], pl->n[2]));
    }
    if (t1 >= offset && t2 >= offset)
    {
        hull_check(node->child[0], p1f, p2f, p1, p2);
        return;
    }
    if (t1 < -offset && t2 < -offset)
    {
        hull_check(node->child[1], p1f, p2f, p1, p2);
        return;
    }
    /* put the crossing point DIST_EPSILON on the near side */
    if (t1 < t2)
    {
        side = 1;
        frac2 = frac_div(t1 + offset + DIST_EPSILON, t1 - t2);
        frac = frac_div(t1 - offset + DIST_EPSILON, t1 - t2);
    }
    else if (t1 > t2)
    {
        side = 0;
        frac2 = frac_div(t1 - offset - DIST_EPSILON, t1 - t2);
        frac = frac_div(t1 + offset + DIST_EPSILON, t1 - t2);
    }
    else
    {
        side = 0;
        frac = FIX(1);
        frac2 = 0;
    }
    frac = iclamp(frac, 0, FIX(1));
    midf = p1f + fmul(p2f - p1f, frac);
    for (i = 0; i < 3; ++i)
        mid[i] = p1[i] + fmul(p2[i] - p1[i], frac);
    hull_check(node->child[side], p1f, midf, p1, mid);
    frac2 = iclamp(frac2, 0, FIX(1));
    midf = p1f + fmul(p2f - p1f, frac2);
    for (i = 0; i < 3; ++i)
        mid[i] = p1[i] + fmul(p2[i] - p1[i], frac2);
    hull_check(node->child[side ^ 1], midf, p2f, mid, p2);
}

/* the leaves a box touches (leafs_asm's way: this when its stack runs out, or to check; low work RAM) */
static __attribute__((cold)) void box_leafs_r(int num, const s32 *mins, const s32 *maxs)
{
    while (num >= 0)
    {
        const q_node    *node = &lv.nodes[num];
        const q_plane   *pl = &lv.planes[node->plane];
        s32             dmin, dmax;

        if (pl->type < 3)
        {
            dmin = mins[pl->type] - pl->dist;
            dmax = maxs[pl->type] - pl->dist;
        }
        else
        {
            s32 lo[3], hi[3];
            int j;

            for (j = 0; j < 3; ++j)
            {
                lo[j] = pl->n[j] < 0 ? maxs[j] : mins[j];
                hi[j] = pl->n[j] < 0 ? mins[j] : maxs[j];
            }
            dmin = dot(lo, pl->n) - pl->dist;
            dmax = dot(hi, pl->n) - pl->dist;
        }
        if (dmin >= 0)
            num = node->child[0];
        else if (dmax < 0)
            num = node->child[1];
        else
        {
            box_leafs_r(node->child[0], mins, maxs);
            num = node->child[1];
        }
    }
    if (nbox_leafs < MAX_BOX_LEAFS)
        box_leafs[nbox_leafs++] = -1 - num;
}

/* the leaves the box c1 .. c2 touches, from node num: box_leafs, nbox_leafs (in assembly, unless
   its stack runs out) */
static void         box_leafs_get(int num, const s32 *c1, const s32 *c2)
{
#ifndef NO_BOX_ASM
    int             k;

    for (k = 0; k < 3; ++k)
    {
        ba.c1[k] = c1[k];
        ba.c2[k] = c2[k];
    }
    ba.list = box_leafs;
    if ((nbox_leafs = leafs_asm(&ba, num)) >= 0)
        return;
#endif
    nbox_leafs = 0;
    box_leafs_r(num, c1, c2);
}

/* their brushes: clipped (or, test, where the box stands), till the fraction's 0 */
static void         box_brushes(bool test)
{
#ifndef NO_BOX_ASM
    ba.list = box_leafs;
    ba.nlist = nbox_leafs;
    ba.test = test;
    brushes_asm(&ba);
#else
    int             i;

    for (i = 0; i < nbox_leafs && (test ? !tr.allsolid : tr.fraction); ++i)
        leaf_brushes(box_leafs[i], test);
#endif
}

#ifdef BOX_CHECK
/* (OPT=-DBOX_CHECK: the C's way too, after the assembly's: the same leaves, the same trace?) */
u32                 box_checks, box_ldiffs, box_diffs;

static void         box_check(int num, const s32 *c1, const s32 *c2, bool test)
{
    q_trace         was = tr;
    int             asm_leafs[MAX_BOX_LEAFS], n = nbox_leafs, i;

    memcpy(asm_leafs, box_leafs, sizeof(asm_leafs));
    memset(&tr, 0, sizeof(tr));
    tr.fraction = FIX(1);
    if (++checkcount == 0)
    {
        memset(brush_check, 0, (u32)lv.nbrushes * 2);
        checkcount = 1;
    }
    nbox_leafs = 0;
    box_leafs_r(num, c1, c2);
    ++box_checks;
    box_ldiffs += n != nbox_leafs || memcmp(asm_leafs, box_leafs, (u32)n * sizeof(int));
    if (nbox_leafs < MAX_BOX_LEAFS || test)
        for (i = 0; i < nbox_leafs && (test ? !tr.allsolid : tr.fraction); ++i)
            leaf_brushes(box_leafs[i], test);
    box_diffs += tr.fraction != was.fraction || tr.plane != was.plane || tr.surf_flags != was.surf_flags
                 || tr.contents != was.contents || tr.startsolid != was.startsolid || tr.allsolid != was.allsolid;
    tr = was;
}
#endif

static q_trace      trace_box_once(const s32 *start, const s32 *end, const s32 *mins, const s32 *maxs, int headnode,
                                   int mask);

q_trace             trace_box(const s32 *start, const s32 *end, const s32 *mins, const s32 *maxs, int headnode,
                              int mask)
{
    int             i;


    /* the box the whole trace sweeps, in whole units and a unit more all round (the brushes'
       boxes are rounded out too): a brush off it can't be met */
    for (i = 0; i < 3; ++i)
    {
        t_bb[i] = (s16)(((imin(start[i], end[i]) + mins[i]) >> 16) - 1);
        t_bb[3 + i] = (s16)(((imax(start[i], end[i]) + maxs[i]) >> 16) + 2);
    }
#ifdef BOUNDS_CHECK
    {
        /* (OPT=-DBOUNDS_CHECK: without the boxes too; the same?) */
        q_trace a, b;

        t_nobounds = true;
        b = trace_box_once(start, end, mins, maxs, headnode, mask);
        t_nobounds = false;
        a = trace_box_once(start, end, mins, maxs, headnode, mask);
        ++bounds_checks;
        if (a.fraction != b.fraction || a.plane != b.plane || a.startsolid != b.startsolid || a.allsolid != b.allsolid
            || a.contents != b.contents)
            ++bounds_diffs;
        return a;
    }
#else
    return trace_box_once(start, end, mins, maxs, headnode, mask);
#endif
}

static q_trace      trace_box_once(const s32 *start, const s32 *end, const s32 *mins, const s32 *maxs, int headnode,
                                   int mask)
{
    int             i;

    if (++checkcount == 0)
    {
        memset(brush_check, 0, (u32)lv.nbrushes * 2);
        checkcount = 1;
    }
    memset(&tr, 0, sizeof(tr));
    tr.fraction = FIX(1);
    t_mask = mask;
    for (i = 0; i < 3; ++i)
    {
        t_start[i] = start[i];
        t_end[i] = end[i];
        t_mins[i] = mins[i];
        t_maxs[i] = maxs[i];
    }
    t_ispoint = !mins[0] && !mins[1] && !mins[2] && !maxs[0] && !maxs[1] && !maxs[2];
#ifndef NO_BOX_ASM
    for (i = 0; i < 6; ++i)
        ba.tbb[i] = t_bb[i];
    ba.bounds = lv.brushbounds;
    ba.check = brush_check;
    ba.count = checkcount;
    ba.mask = mask;
    ba.tr = &tr;
    ba.leafs = lv.leafs;
    ba.leafbrushes = lv.leafbrushes;
    ba.brushes = lv.brushes;
    ba.sides = lv.brushsides;
    ba.planes = lv.planes;
    ba.nodes = lv.nodes;
    ba.ispoint = t_ispoint;
    for (i = 0; i < 3; ++i)
    {
        ba.sl[i] = start[i] + mins[i];
        ba.sh[i] = start[i] + maxs[i];
        ba.el[i] = end[i] + mins[i];
        ba.eh[i] = end[i] + maxs[i];
        ba.start[i] = start[i];
        ba.end[i] = end[i];
        ba.mins[i] = mins[i];
        ba.maxs[i] = maxs[i];
    }
#endif
    if (start[0] == end[0] && start[1] == end[1] && start[2] == end[2])
    {
        /* a position test */
        s32 c1[3], c2[3];

        for (i = 0; i < 3; ++i)
        {
            c1[i] = start[i] + mins[i] - FIX(1);
            c2[i] = start[i] + maxs[i] + FIX(1);
        }
        box_leafs_get(headnode, c1, c2);
        box_brushes(true);
#ifdef BOX_CHECK
        box_check(headnode, c1, c2, true);
#endif
        for (i = 0; i < 3; ++i)
            tr.endpos[i] = start[i];
        return tr;
    }
    TR_COUNT(4, 1);
    if (iabs(end[0] - start[0]) <= SHORT_MOVE && iabs(end[1] - start[1]) <= SHORT_MOVE
        && iabs(end[2] - start[2]) <= SHORT_MOVE)
    {
        /* A short move (a monster's step, the player's): the leaves the whole
           move's box touches, then their brushes. Walking the tree along the
           move splits it at nearly every node the box straddles (two divides
           and a recursion each); this is a plane-against-box test a node. The
           same result: every brush the move meets is clipped exactly either way */
        s32 c1[3], c2[3];
        TR_T0;

        for (i = 0; i < 3; ++i)
        {
            c1[i] = imin(start[i], end[i]) + mins[i] - FIX(1);
            c2[i] = imax(start[i], end[i]) + maxs[i] + FIX(1);
        }
        box_leafs_get(headnode, c1, c2);
        TR_TICKS(0);
        if (nbox_leafs < MAX_BOX_LEAFS)
        {
            box_brushes(false);
            TR_TICKS(1);
#ifdef BOX_CHECK
            box_check(headnode, c1, c2, false);
#endif
#ifdef TRACE_CHECK
            {
                /* (OPT=-DTRACE_CHECK: the long way too, and count where they differ) */
                q_trace short_tr = tr;

                memset(&tr, 0, sizeof(tr));
                tr.fraction = FIX(1);
                if (++checkcount == 0)
                {
                    memset(brush_check, 0, (u32)lv.nbrushes * 2);
                    checkcount = 1;
                }
                for (i = 0; i < 3; ++i)
                    t_ext[i] = imax(-mins[i], maxs[i]);
                hull_check(headnode, 0, FIX(1), start, end);
                ++trace_checks;
                if (tr.fraction != short_tr.fraction)
                {
                    ++trace_diffs;
                    if (iabs(tr.fraction - short_tr.fraction) > trace_worst)
                        trace_worst = iabs(tr.fraction - short_tr.fraction);
                    trace_later += short_tr.fraction > tr.fraction;     /* the short way stopped later */
                }
                trace_dkind[0] += tr.startsolid != short_tr.startsolid;
                trace_dkind[1] += tr.allsolid != short_tr.allsolid;
                trace_dkind[2] += tr.fraction < FIX(1) && tr.plane != short_tr.plane;
                trace_dkind[3] += tr.fraction < FIX(1) && tr.plane != short_tr.plane && tr.plane && short_tr.plane
                                  && (tr.plane->n[0] != short_tr.plane->n[0] || tr.plane->n[1] != short_tr.plane->n[1]
                                      || tr.plane->n[2] != short_tr.plane->n[2]);
                tr = short_tr;
            }
#endif
            for (i = 0; i < 3; ++i)
                tr.endpos[i] = tr.fraction == FIX(1) ? end[i] : start[i] + fmul(tr.fraction, end[i] - start[i]);
            return tr;
        }
    }
    for (i = 0; i < 3; ++i)
        t_ext[i] = imax(-mins[i], maxs[i]);
    hull_check(headnode, 0, FIX(1), start, end);
    for (i = 0; i < 3; ++i)
        tr.endpos[i] = tr.fraction == FIX(1) ? end[i] : start[i] + fmul(tr.fraction, end[i] - start[i]);
    return tr;
}

/* ---- lines: Quake 1's way (SV_RecursiveHullCheck on the point hull) ----
   Walk the BSP along the line; where it crosses from a clear leaf into one
   whose contents match the mask, that crossing is the hit: the splitting
   plane (turned towards the start) and the point on it. No brushes to clip,
   and exact for the world (leaves are all solid or all clear) - but a long
   line still walks a few hundred nodes, ~0.5 ms here. */

static int          l_mask;

/* Down the tree with the part of the line in this subtree (p1 at p1f to p2
   at p2f); entered across plane pl (NULL at the start). Reaching a leaf
   that matches the mask is the hit, at p1. false: stopped (tr filled in).
   Down a side the line's wholly on, a loop (most nodes); where a plane cuts
   it, a call for the near part, then the loop goes on with the far. (Only
   when line_asm's stack runs out: low work RAM) */
static __attribute__((cold)) bool line_check(int num, s32 p1f, s32 p2f, const s32 *p1_in, const s32 *p2, const q_plane *entered)
{
    s32             p1[3], mid[3], t1, t2, frac, midf;
    int             side, i;

    p1[0] = p1_in[0];
    p1[1] = p1_in[1];
    p1[2] = p1_in[2];
    for (;;)
    {
        const q_node    *node;
        const q_plane   *pl;

        if (num < 0)
        {
            const q_leaf *leaf = &lv.leafs[-1 - num];

            if (!(leaf->contents & l_mask))
                return true;
            tr.plane = entered;
            tr.fraction = p1f;
            tr.contents = leaf->contents;
            for (i = 0; i < 3; ++i)
                tr.endpos[i] = p1[i];
            return false;
        }
        TR_COUNT(6, 1);
        node = &lv.nodes[num];
        pl = &lv.planes[node->plane];
        if (pl->type < 3)
        {
            t1 = p1[pl->type] - pl->dist;
            t2 = p2[pl->type] - pl->dist;
        }
        else
        {
            t1 = dot(p1, pl->n) - pl->dist;
            t2 = dot(p2, pl->n) - pl->dist;
        }
        if (t1 >= 0 && t2 >= 0)
        {
            num = node->child[0];
            continue;
        }
        if (t1 < 0 && t2 < 0)
        {
            num = node->child[1];
            continue;
        }
        /* it crosses: the near side up to a hair short of the plane, then the far side from there */
        frac = iclamp(t1 < 0 ? frac_div(t1 + DIST_EPSILON, t1 - t2) : frac_div(t1 - DIST_EPSILON, t1 - t2), 0, FIX(1));
        midf = p1f + fmul(p2f - p1f, frac);
        for (i = 0; i < 3; ++i)
            mid[i] = p1[i] + fmul(p2[i] - p1[i], frac);
        side = t1 < 0;
        if (!line_check(node->child[side], p1f, midf, p1, mid, entered))
            return false;
        /* crossing to the back of a plane we're in front of: the plane faces us; the other way, its opposite */
        entered = &lv.planes[side ? node->plane ^ 1 : node->plane];
        num = node->child[side ^ 1];
        p1f = midf;
        p1[0] = mid[0];
        p1[1] = mid[1];
        p1[2] = mid[2];
    }
}

#ifdef LINE_CHECK
/* (OPT=-DLINE_CHECK: the walk as it was, a call a node, to compare) */
/* Down the tree with the part of the line in this subtree (p1 at p1f to p2
   at p2f); entered across plane pl (NULL at the start). Reaching a leaf
   that matches the mask is the hit, at p1. false: stopped (tr filled in). */
static bool         line_check_ref(int num, s32 p1f, s32 p2f, const s32 *p1, const s32 *p2, const q_plane *entered)
{
    const q_node    *node;
    const q_plane   *pl;
    s32             t1, t2, frac, midf, mid[3];
    int             side, i;

    if (num < 0)
    {
        const q_leaf *leaf = &lv.leafs[-1 - num];

        if (!(leaf->contents & l_mask))
            return true;
        tr.plane = entered;
        tr.fraction = p1f;
        tr.contents = leaf->contents;
        for (i = 0; i < 3; ++i)
            tr.endpos[i] = p1[i];
        return false;
    }
    node = &lv.nodes[num];
    pl = &lv.planes[node->plane];
    if (pl->type < 3)
    {
        t1 = p1[pl->type] - pl->dist;
        t2 = p2[pl->type] - pl->dist;
    }
    else
    {
        t1 = dot(p1, pl->n) - pl->dist;
        t2 = dot(p2, pl->n) - pl->dist;
    }
    if (t1 >= 0 && t2 >= 0)
        return line_check_ref(node->child[0], p1f, p2f, p1, p2, entered);
    if (t1 < 0 && t2 < 0)
        return line_check_ref(node->child[1], p1f, p2f, p1, p2, entered);
    /* it crosses: the near side up to a hair short of the plane, then the far side from there */
    frac = iclamp(t1 < 0 ? frac_div(t1 + DIST_EPSILON, t1 - t2) : frac_div(t1 - DIST_EPSILON, t1 - t2), 0, FIX(1));
    midf = p1f + fmul(p2f - p1f, frac);
    for (i = 0; i < 3; ++i)
        mid[i] = p1[i] + fmul(p2[i] - p1[i], frac);
    side = t1 < 0;
    if (!line_check_ref(node->child[side], p1f, midf, p1, mid, entered))
        return false;
    /* crossing to the back of a plane we're in front of: the plane faces us; the other way, its opposite */
    return line_check_ref(node->child[side ^ 1], midf, p2f, mid, p2, &lv.planes[side ? node->plane ^ 1 : node->plane]);
}

#endif

/* the walk in assembly (src/tline.s): its offsets are its L_ */
#define LINE_STACK      (64)
typedef struct
{
    const q_node    *nodes;
    const q_plane   *planes;
    const q_leaf    *leafs;
    s32             mask;
    s32             p1[3], p2[3];
    s32             p1f, p2f;
    const q_plane   *entered;
    s32             contents;
    s32             stack[LINE_STACK * 10];
}                   line_args;
_Static_assert(__builtin_offsetof(line_args, p1) == 16 && __builtin_offsetof(line_args, p1f) == 40
               && __builtin_offsetof(line_args, entered) == 48 && __builtin_offsetof(line_args, stack) == 56,
               "src/tline.s: line_args");
int                 line_asm(line_args *a, int num);

q_trace             trace_line(const s32 *start, const s32 *end, int headnode, int mask)
{
    line_args       la;
    int             i, r;
    TR_T0;

    memset(&tr, 0, sizeof(tr));
    tr.fraction = FIX(1);
    l_mask = mask;
    TR_COUNT(7, 1);
    la.nodes = lv.nodes;
    la.planes = lv.planes;
    la.leafs = lv.leafs;
    la.mask = mask;
    for (i = 0; i < 3; ++i)
    {
        la.p1[i] = start[i];
        la.p2[i] = end[i];
    }
    la.p1f = 0;
    la.p2f = FIX(1);
    la.entered = NULL;
    r = line_asm(&la, headnode);
    if (r == 2)
        r = line_check(headnode, 0, FIX(1), start, end, NULL);     /* (deeper than the asm keeps) */
    else if (r == 0)
    {
        tr.plane = la.entered;
        tr.fraction = la.p1f;
        tr.contents = la.contents;
        for (i = 0; i < 3; ++i)
            tr.endpos[i] = la.p1[i];
    }
    if (r)
        for (i = 0; i < 3; ++i)
            tr.endpos[i] = end[i];
    else if (!tr.plane)
        tr.startsolid = tr.allsolid = true;     /* solid where it started */
    TR_TICKS(4);
#ifdef LINE_CHECK
    {
        /* (OPT=-DLINE_CHECK: the old walk too: the same?) */
        extern u32 line_checks, line_diffs;
        q_trace t = tr;

        memset(&tr, 0, sizeof(tr));
        tr.fraction = FIX(1);
        if (line_check_ref(headnode, 0, FIX(1), start, end, NULL))
            for (i = 0; i < 3; ++i)
                tr.endpos[i] = end[i];
        else if (!tr.plane)
            tr.startsolid = tr.allsolid = true;
        ++line_checks;
        if (t.fraction != tr.fraction || t.plane != tr.plane || t.contents != tr.contents
            || t.endpos[0] != tr.endpos[0] || t.endpos[1] != tr.endpos[1] || t.endpos[2] != tr.endpos[2]
            || t.startsolid != tr.startsolid)
            ++line_diffs;
        tr = t;
    }
#endif
    return tr;
}
#ifdef LINE_CHECK
u32                 line_checks, line_diffs;
#endif

int                 point_contents(const s32 *p, int num)
{
    while (num >= 0)
    {
        const q_node    *node = &lv.nodes[num];
        const q_plane   *pl = &lv.planes[node->plane];
        s32             d = pl->type < 3 ? p[pl->type] - pl->dist : dot(p, pl->n) - pl->dist;

        num = node->child[d < 0];
    }
    return lv.leafs[-1 - num].contents;
}
