/*
** Box traces against the map's brushes: Quake 2's qcommon/cmodel.c
** (CM_BoxTrace and what it calls), in 16.16 fixed point. The BSP finds the
** leaves the box's sweep passes through; each leaf lists the brushes in it;
** each brush is a convex set of planes, pushed out by the box's size.
*/
#include "q2.h"

#define DIST_EPSILON    (2048)              /* 1/32: keeps the box off the planes */
#define MAX_BOX_LEAFS   (128)

static s32          t_start[3], t_end[3], t_mins[3], t_maxs[3], t_ext[3];
static int          t_mask;
static bool         t_ispoint;
static q_trace      tr;
static u16          *brush_check, checkcount;
static int          box_leafs[MAX_BOX_LEAFS], nbox_leafs;

void                trace_init(void)
{
    brush_check = level_alloc((u32)lv.nbrushes * 2);
    memset(brush_check, 0, (u32)lv.nbrushes * 2);
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
    return fdiv(n, d);
}

static void         clip_box_brush(const q_brush *b)
{
    s32             enterfrac = -FIX(1), leavefrac = FIX(1), ofs[3], dist, d1, d2, f;
    const q_plane   *clipplane = NULL;
    int             i, j, leadflags = 0;
    bool            getout = false, startout = false;

    for (i = 0; i < b->numsides; ++i)
    {
        const q_brushside   *side = &lv.brushsides[b->firstside + i];
        const q_plane       *pl = &lv.planes[side->plane];

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
        if (d2 > 0)
            getout = true;                  /* the end isn't in the solid */
        if (d1 > 0)
            startout = true;
        if (d1 > 0 && d2 >= d1)
            return;                         /* completely in front of this side: no hit */
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
        tr.contents = b->contents;
    }
}

static void         test_box_brush(const q_brush *b)
{
    s32             ofs[3], dist;
    int             i, j;

    if (!b->numsides)
        return;
    for (i = 0; i < b->numsides; ++i)
    {
        const q_plane *pl = &lv.planes[lv.brushsides[b->firstside + i].plane];

        for (j = 0; j < 3; ++j)
            ofs[j] = pl->n[j] < 0 ? t_maxs[j] : t_mins[j];
        dist = pl->dist - dot(ofs, pl->n);
        if (dot(t_start, pl->n) - dist > 0)
            return;
    }
    tr.startsolid = tr.allsolid = true;
    tr.fraction = 0;
    tr.contents = b->contents;
}

static void         leaf_brushes(int leafnum, bool test)
{
    const q_leaf    *leaf = &lv.leafs[leafnum];
    int             k;

    if (!(leaf->contents & t_mask))
        return;
    for (k = 0; k < leaf->numbrushes; ++k)
    {
        int             bn = lv.leafbrushes[leaf->firstbrush + k];
        const q_brush   *b = &lv.brushes[bn];

        if (brush_check[bn] == checkcount)
            continue;                       /* already done from another leaf */
        brush_check[bn] = checkcount;
        if (!(b->contents & t_mask) || !b->numsides)
            continue;
        if (test)
            test_box_brush(b);
        else
            clip_box_brush(b);
        if (!tr.fraction)
            return;
    }
}

static void         hull_check(int num, s32 p1f, s32 p2f, const s32 *p1, const s32 *p2)
{
    const q_node    *node;
    const q_plane   *pl;
    s32             t1, t2, offset, frac, frac2, midf, mid[3];
    int             side, i;

    if (tr.fraction <= p1f)
        return;                             /* already hit something nearer */
    if (num < 0)
    {
        leaf_brushes(-1 - num, false);
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

/* the leaves a box touches */
static void         box_leafs_r(int num, const s32 *mins, const s32 *maxs)
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

q_trace             trace_box(const s32 *start, const s32 *end, const s32 *mins, const s32 *maxs, int headnode,
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
    if (start[0] == end[0] && start[1] == end[1] && start[2] == end[2])
    {
        /* a position test */
        s32 c1[3], c2[3];

        for (i = 0; i < 3; ++i)
        {
            c1[i] = start[i] + mins[i] - FIX(1);
            c2[i] = start[i] + maxs[i] + FIX(1);
        }
        nbox_leafs = 0;
        box_leafs_r(headnode, c1, c2);
        for (i = 0; i < nbox_leafs && !tr.allsolid; ++i)
            leaf_brushes(box_leafs[i], true);
        for (i = 0; i < 3; ++i)
            tr.endpos[i] = start[i];
        return tr;
    }
    t_ispoint = !mins[0] && !mins[1] && !mins[2] && !maxs[0] && !maxs[1] && !maxs[2];
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
   that matches the mask is the hit, at p1. false: stopped (tr filled in). */
static bool         line_check(int num, s32 p1f, s32 p2f, const s32 *p1, const s32 *p2, const q_plane *entered)
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
        return line_check(node->child[0], p1f, p2f, p1, p2, entered);
    if (t1 < 0 && t2 < 0)
        return line_check(node->child[1], p1f, p2f, p1, p2, entered);
    /* it crosses: the near side up to a hair short of the plane, then the far side from there */
    frac = iclamp(t1 < 0 ? frac_div(t1 + DIST_EPSILON, t1 - t2) : frac_div(t1 - DIST_EPSILON, t1 - t2), 0, FIX(1));
    midf = p1f + fmul(p2f - p1f, frac);
    for (i = 0; i < 3; ++i)
        mid[i] = p1[i] + fmul(p2[i] - p1[i], frac);
    side = t1 < 0;
    if (!line_check(node->child[side], p1f, midf, p1, mid, entered))
        return false;
    /* crossing to the back of a plane we're in front of: the plane faces us; the other way, its opposite */
    return line_check(node->child[side ^ 1], midf, p2f, mid, p2, &lv.planes[side ? node->plane ^ 1 : node->plane]);
}

q_trace             trace_line(const s32 *start, const s32 *end, int headnode, int mask)
{
    int             i;

    memset(&tr, 0, sizeof(tr));
    tr.fraction = FIX(1);
    l_mask = mask;
    if (line_check(headnode, 0, FIX(1), start, end, NULL))
        for (i = 0; i < 3; ++i)
            tr.endpos[i] = end[i];
    else if (!tr.plane)
        tr.startsolid = tr.allsolid = true;     /* solid where it started */
    return tr;
}

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
