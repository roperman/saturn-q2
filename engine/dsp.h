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
/* xformb: blocks of 16 vertices, each with its own matrix; stream holds a
   13-word header per block (t0 m00 m01 m02 t1 .. m22, vertices >> 2) */
void                dsp_blocks(const u32 *stream, s32 *out, int blocks);    /* starts it */
void                dsp_transform(const s32 *mtx12, const s32 *in, s32 *out, int count);  /* starts it */
static inline bool  dsp_busy(void) { return (DSP_PPAF & (1u << 16)) != 0; }
static inline void  dsp_wait(void) { while (dsp_busy()) ; }

#endif
