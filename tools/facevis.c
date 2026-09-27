/*
** facevis: which faces can actually be seen from each cluster.
**
** Quake's PVS says which clusters might see which (through the portals
** between leaves): conservative, and in big rooms most of what it lets
** through is hidden behind nearer walls. With painter's-order drawing
** the Saturn pays for all of it. This renders, from sample points in each
** cluster, a cube of face-ID buffers with a depth test, and keeps every face
** that shows up in any of them.
**
** Input (tools/bake_map.py writes it, little-endian):
**   int nfaces, nleafs, nnodes, nclusters, samples, res
**   faces:  int nverts, int flags (1 = occludes, 2 = drawn), float nx ny nz d, then nverts x float3
**   leafs:  int cluster, int contents, float mins[3], maxs[3], int nfaces, then nfaces x int face
**   nodes:  float nx ny nz d, int child[2]      (children < 0: -(leaf + 1))
**   pvs:    nclusters rows of (nclusters + 7) / 8 bytes
** Output: nclusters rows of (nfaces + 7) / 8 bytes.
**
**   cc -O2 -o facevis facevis.c -lm && ./facevis in.bin out.bin
*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int nverts, flags; float n[3], d; float (*v)[3]; } face_t;
typedef struct { int cluster, contents; float mins[3], maxs[3]; int nfaces; int *faces; } leaf_t;
typedef struct { float n[3], d; int child[2]; } node_t;

static face_t       *faces;
static leaf_t       *leafs;
static node_t       *nodes;
static unsigned char *pvs, *out;
static int          nfaces, nleafs, nnodes, nclusters, samples, res;
static float        *zbuf;
static int          *ibuf;
static unsigned     rng = 12345;

static float        frand(void)
{
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) / 16777216.0f;
}

static void         rd(FILE *f, void *p, size_t n)
{
    if (fread(p, 1, n, f) != n)
    {
        fprintf(stderr, "facevis: short read\n");
        exit(1);
    }
}

static int          point_leaf(const float *p)
{
    int             n = 0;

    while (n >= 0)
    {
        const node_t *nd = &nodes[n];
        float d = nd->n[0] * p[0] + nd->n[1] * p[1] + nd->n[2] * p[2] - nd->d;

        n = nd->child[d < 0];
    }
    return -(n + 1);
}

/* one view: axes (right, up, forward), 90 degrees square */
static void         render(const float *eye, const float *rt, const float *up, const float *fw,
                           const unsigned char *face_ok, unsigned char *seen)
{
    int             i, k, x, y;
    float           h = res * 0.5f;

    for (i = 0; i < res * res; ++i)
    {
        zbuf[i] = 0;                        /* 1/z: 0 = infinitely far */
        ibuf[i] = -1;
    }
    for (i = 0; i < nfaces; ++i)
    {
        face_t      *f = &faces[i];
        float       sx[64], sy[64], iz[64], cv[64][3];
        int         nv = 0;
        float       side = f->n[0] * eye[0] + f->n[1] * eye[1] + f->n[2] * eye[2] - f->d;

        if (!face_ok[i] || side <= 0.01f)
            continue;                       /* not in the PVS, or facing away */
        /* to view space, clipped to z >= 1 */
        {
            float in[64][3];
            int n = f->nverts < 60 ? f->nverts : 60;

            for (k = 0; k < n; ++k)
            {
                float d[3] = { f->v[k][0] - eye[0], f->v[k][1] - eye[1], f->v[k][2] - eye[2] };

                in[k][0] = d[0] * rt[0] + d[1] * rt[1] + d[2] * rt[2];
                in[k][1] = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
                in[k][2] = d[0] * fw[0] + d[1] * fw[1] + d[2] * fw[2];
            }
            for (k = 0; k < n; ++k)
            {
                float *p = in[k], *q = in[(k + 1) % n];
                int pin = p[2] >= 1.0f, qin = q[2] >= 1.0f;

                if (pin && nv < 63)
                {
                    cv[nv][0] = p[0]; cv[nv][1] = p[1]; cv[nv][2] = p[2];
                    ++nv;
                }
                if (pin != qin && nv < 63)
                {
                    float t = (1.0f - p[2]) / (q[2] - p[2]);

                    cv[nv][0] = p[0] + (q[0] - p[0]) * t;
                    cv[nv][1] = p[1] + (q[1] - p[1]) * t;
                    cv[nv][2] = 1.0f;
                    ++nv;
                }
            }
        }
        if (nv < 3)
            continue;
        for (k = 0; k < nv; ++k)
        {
            iz[k] = 1.0f / cv[k][2];
            sx[k] = h + cv[k][0] * iz[k] * h;
            sy[k] = h - cv[k][1] * iz[k] * h;
        }
        /* scanline fill of the convex polygon, 1/z interpolated (it's affine on screen) */
        {
            float ymin = 1e9f, ymax = -1e9f;
            float a, b, c;                  /* 1/z = a x + b y + c */
            float x1 = sx[1] - sx[0], y1 = sy[1] - sy[0], z1 = iz[1] - iz[0];
            float x2 = sx[2] - sx[0], y2 = sy[2] - sy[0], z2 = iz[2] - iz[0];
            float det = x1 * y2 - x2 * y1;
            int k2;

            /* the plane through the 3 vertices furthest apart would be better; this is fine */
            for (k2 = 3; fabsf(det) < 1e-3f && k2 < nv; ++k2)
            {
                x2 = sx[k2] - sx[0]; y2 = sy[k2] - sy[0]; z2 = iz[k2] - iz[0];
                det = x1 * y2 - x2 * y1;
            }
            if (fabsf(det) < 1e-6f)
                continue;
            a = (z1 * y2 - z2 * y1) / det;
            b = (x1 * z2 - x2 * z1) / det;
            c = iz[0] - a * sx[0] - b * sy[0];
            for (k = 0; k < nv; ++k)
            {
                if (sy[k] < ymin) ymin = sy[k];
                if (sy[k] > ymax) ymax = sy[k];
            }
            if (ymax < 0 || ymin > res)
                continue;
            for (y = (int)ceilf(ymin - 0.5f); y <= (int)floorf(ymax - 0.5f); ++y)
            {
                float yc = y + 0.5f, xl = 1e9f, xr = -1e9f;

                if (y < 0 || y >= res)
                    continue;
                for (k = 0; k < nv; ++k)
                {
                    float ya = sy[k], yb = sy[(k + 1) % nv];

                    if ((ya <= yc && yb > yc) || (yb <= yc && ya > yc))
                    {
                        float xa = sx[k] + (sx[(k + 1) % nv] - sx[k]) * (yc - ya) / (yb - ya);

                        if (xa < xl) xl = xa;
                        if (xa > xr) xr = xa;
                    }
                }
                if (xl > xr)
                    continue;
                /* a pixel's in if the polygon covers its centre; make thin ones show at least one */
                {
                    int xa = (int)ceilf(xl - 0.5f), xb = (int)floorf(xr - 0.5f);

                    if (xb < xa)
                        xb = xa = (int)floorf((xl + xr) * 0.5f);
                    if (xa < 0) xa = 0;
                    if (xb >= res) xb = res - 1;
                    for (x = xa; x <= xb; ++x)
                    {
                        float z = a * (x + 0.5f) + b * yc + c;
                        int o = y * res + x;

                        if (z > zbuf[o] * 1.0001f)
                        {
                            if (f->flags & 1)
                            {
                                zbuf[o] = z;
                                ibuf[o] = i;
                            }
                            else
                                seen[i] = 1;        /* see-through: seen, but hides nothing */
                        }
                    }
                }
            }
        }
    }
    for (i = 0; i < res * res; ++i)
        if (ibuf[i] >= 0)
            seen[ibuf[i]] = 1;
}

int                 main(int argc, char **argv)
{
    FILE            *f;
    int             i, k, c, rowc, rowf;
    unsigned char   *face_ok, *seen;
    static const float dirs[6][3][3] = {
        /* right, up, forward */
        { { 0, -1, 0 }, { 0, 0, 1 }, { 1, 0, 0 } },
        { { 0, 1, 0 }, { 0, 0, 1 }, { -1, 0, 0 } },
        { { 1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
        { { -1, 0, 0 }, { 0, 0, 1 }, { 0, -1, 0 } },
        { { 0, -1, 0 }, { -1, 0, 0 }, { 0, 0, 1 } },
        { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },
    };

    if (argc < 3 || !(f = fopen(argv[1], "rb")))
    {
        fprintf(stderr, "usage: facevis in.bin out.bin\n");
        return 1;
    }
    rd(f, &nfaces, 4); rd(f, &nleafs, 4); rd(f, &nnodes, 4); rd(f, &nclusters, 4); rd(f, &samples, 4); rd(f, &res, 4);
    faces = calloc(nfaces, sizeof(*faces));
    for (i = 0; i < nfaces; ++i)
    {
        rd(f, &faces[i].nverts, 4); rd(f, &faces[i].flags, 4); rd(f, faces[i].n, 12); rd(f, &faces[i].d, 4);
        faces[i].v = malloc(12 * (faces[i].nverts + 1));
        rd(f, faces[i].v, 12 * faces[i].nverts);
    }
    leafs = calloc(nleafs, sizeof(*leafs));
    for (i = 0; i < nleafs; ++i)
    {
        rd(f, &leafs[i].cluster, 4); rd(f, &leafs[i].contents, 4); rd(f, leafs[i].mins, 12); rd(f, leafs[i].maxs, 12);
        rd(f, &leafs[i].nfaces, 4);
        leafs[i].faces = malloc(4 * (leafs[i].nfaces + 1));
        rd(f, leafs[i].faces, 4 * leafs[i].nfaces);
    }
    nodes = calloc(nnodes, sizeof(*nodes));
    for (i = 0; i < nnodes; ++i)
    {
        rd(f, nodes[i].n, 12); rd(f, &nodes[i].d, 4); rd(f, nodes[i].child, 8);
    }
    rowc = (nclusters + 7) / 8;
    rowf = (nfaces + 7) / 8;
    pvs = malloc((size_t)rowc * nclusters);
    rd(f, pvs, (size_t)rowc * nclusters);
    fclose(f);
    out = calloc((size_t)rowf, nclusters);
    zbuf = malloc(sizeof(float) * res * res);
    ibuf = malloc(sizeof(int) * res * res);
    face_ok = malloc(nfaces);
    seen = malloc(nfaces);

    for (c = 0; c < nclusters; ++c)
    {
        int s, total = 0, nl = 0;

        /* the faces the PVS allows */
        memset(face_ok, 0, nfaces);
        for (i = 0; i < nleafs; ++i)
        {
            int lc = leafs[i].cluster;

            if (lc < 0 || !(pvs[(size_t)c * rowc + (lc >> 3)] & (1 << (lc & 7))))
                continue;
            for (k = 0; k < leafs[i].nfaces; ++k)
                face_ok[leafs[i].faces[k]] = 1;
        }
        memset(seen, 0, nfaces);
        /* sample points: in this cluster's leaves, spread by volume */
        for (i = 0; i < nleafs; ++i)
            if (leafs[i].cluster == c)
                ++nl;
        for (i = 0; i < nleafs; ++i)
        {
            leaf_t *l = &leafs[i];
            int want = samples / (nl ? nl : 1) + 2, got = 0, tries;

            if (l->cluster != c)
                continue;
            for (tries = 0; tries < want * 8 && got < want; ++tries)
            {
                float p[3];

                for (k = 0; k < 3; ++k)
                {
                    float m = 1.0f;         /* stay a touch inside */

                    p[k] = l->mins[k] + m + frand() * (l->maxs[k] - l->mins[k] - 2 * m);
                }
                if (tries == 0)
                    for (k = 0; k < 3; ++k)
                        p[k] = (l->mins[k] + l->maxs[k]) * 0.5f;
                if (point_leaf(p) != i)
                    continue;
                ++got;
                for (s = 0; s < 6; ++s)
                    render(p, dirs[s][0], dirs[s][1], dirs[s][2], face_ok, seen);
            }
        }
        for (i = 0; i < nfaces; ++i)
            if (seen[i])
            {
                out[(size_t)c * rowf + (i >> 3)] |= (unsigned char)(1 << (i & 7));
                ++total;
            }
        if (c % 100 == 0)
        {
            int allowed = 0;

            for (i = 0; i < nfaces; ++i)
                allowed += face_ok[i];
            fprintf(stderr, "  cluster %d/%d: %d faces seen of %d in the PVS\n", c, nclusters, total, allowed);
        }
    }
    f = fopen(argv[2], "wb");
    fwrite(out, (size_t)rowf, nclusters, f);
    fclose(f);
    return 0;
}
