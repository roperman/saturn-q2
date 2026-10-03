/*
** SCU DSP batch transform (program: xform.dsp, assembled at build time).
**
**   out = M * v + t   for count vertices (count a multiple of 16)
**
** v: int x y z triples in high work RAM. M/t: 16.16, laid out
**   t0 m00 m01 m02   t1 m10 m11 m12   t2 m20 m21 m22
** out: per 16-vertex block, x'[16] y'[16] z'[16] (16.16). The DSP writes it by
** DMA behind the CPU cache: purge the cache (or read uncached) before use.
*/
#ifndef __DSP_H__
#define __DSP_H__

#include "sat.h"

#define DSP_BLOCK       (16)

void                dsp_init(void);                     /* loads the transform program */
void                dsp_init_blocks(void);              /* ...or the one with a matrix per block (xformb.dsp) */
void                dsp_init_packed(void);              /* ...or that with packed byte vertices (xformp.dsp) */
void                dsp_init_models(void);              /* ...or models' vertices, two frames blended (xformm.dsp) */
/* xformm: a 24-word header a model (rows t a0 a1 a2 b0 b1 b2, then the two
   frames' packed vertices >> 2 and its blocks of 16); out gets 16 x' 16 y'
   16 z' a block; *count goes up by one as each model's finished */
void                dsp_models(const u32 *stream, s32 *out, int models, volatile u32 *count);
void                dsp_models_lit(const u32 *stream, s32 *out, int models, volatile u32 *count, const u32 *ljobs,
                                   int nljobs, const s32 *normals, volatile u32 *lcount);  /* (then their lighting) */
/* the walls' dynamic lights (engine/walls0.dsp, walls1.dsp, walls2.dsp): RAM0[40..63]; addresses >> 2 */
typedef struct
{
    u32             faces;                  /* the list's entries (0: none, the models' job just ends) */
    u32             job;                    /* (walls1.dsp's: its list's next) */
    u32             blocks;                 /* (walls1.dsp's for walls2.dsp) */
    u32             out;                    /* the lit lights, one face after another */
    u32             lights, light_words, lights1;   /* walls2.dsp's, 8 words each: -x -y -z r2 -(inv << 8) r g b;
                                                       their words; how many - 1 */
    u32             axes, raw;              /* the cart's copies of the level's axes, lights */
    u32             n, rcp;                 /* N, 65536 / N */
    u32             progf, prog2;           /* walls0.dsp, walls2.dsp (256 words each) */
    u32             faces2;                 /* (walls2.dsp's count) */
    u32             planes;                 /* the cart's copy of the level's planes */
    u32             prog1, mkprog;          /* walls1.dsp; the texture maker's block (engine/make.dsp: a copy of
                                               its program, then its counts and lists; 0 none), which walls2.dsp
                                               and xformm.dsp end by loading */
    u32             prog0;                  /* the models' (xformm.dsp), back after them */
    u32             list, list_n;           /* the whole faces drawn: their indices, two to a word; how many */
    u32             faces_cart;             /* the cart's copy of the level's faces */
    u32             half;                   /* 1: the list's first's the second half of a word: (that word in
                                               walls0.dsp's RAM0[25], dsp_walls_word; the list from the next) */
    u32             acc;                    /* the faces walls0.dsp picks (after a word for how many) */
    u32             lights0;                /* walls0.dsp's lights, 10 words each, then its constants */
} dsp_walls_p;
void                dsp_walls_params(const dsp_walls_p *p);
void                dsp_walls_start(void);  /* (the walls alone: no models) */
/* the texture maker (engine/make.dsp, after the walls' or the models' job): its block (its
   program first; NULL: none, the models' job just ends): RAM0[56], dsp_walls_p's mkprog */
void                dsp_maker_params(const void *block);
void                dsp_maker_start(void);  /* (the maker alone: no models, no walls) */
void                dsp_walls_word(u32 w);  /* (dsp_walls_p's half) */
void                dsp_init_faces(void);               /* (a test) faces' grids into view space (xformf.dsp) */
void                dsp_faces(const u32 *jobs, int n, volatile u32 *count, const s32 *axes, const s32 *rows9,
                              const s32 *cam3);
/* xformb: blocks of 16 vertices, each with its own matrix; stream holds a
   13-word header per block (t0 m00 m01 m02 t1 .. m22, vertices >> 2) */
void                dsp_blocks(const u32 *stream, s32 *out, int blocks);    /* starts it */
void                dsp_transform(const s32 *mtx12, const s32 *in, s32 *out, int count);  /* starts it */
static inline bool  dsp_busy(void) { return (DSP_PPAF & (1u << 16)) != 0; }
static inline void  dsp_wait(void) { while (dsp_busy()) ; }

#endif
