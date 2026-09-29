/*
** SCU DSP host side: program upload and job start (see dsp.h, xform.dsp).
*/
#include "dsp.h"
#include "xform.h"
#include "xformb.h"
#include "xformp.h"
#include "xformm.h"
#ifdef DSP_LIGHT
#include "xformml.h"
#endif
#include "xformf.h"

void                dsp_init(void)
{
    int             i;

    DSP_PPAF = 0;                           /* stop */
    DSP_PPAF = 1u << 15;                    /* PC = 0 */
    for (i = 0; i < XFORM_PROG_LEN; ++i)
        DSP_PPD = xform_prog[i];
}

void                dsp_init_blocks(void)
{
    int             i;

    DSP_PPAF = 0;
    DSP_PPAF = 1u << 15;
    for (i = 0; i < XFORMB_PROG_LEN; ++i)
        DSP_PPD = xformb_prog[i];
}

/* xformp.dsp: as xformb, but the vertices are packed bytes (x y z n a word) */
void                dsp_init_packed(void)
{
    int             i;

    DSP_PPAF = 0;
    DSP_PPAF = 1u << 15;
    for (i = 0; i < XFORMP_PROG_LEN; ++i)
        DSP_PPD = xformp_prog[i];
}

/* xformm.dsp: models' vertices, both frames blended at once */
void                dsp_init_models(void)
{
    int             i;

    DSP_PPAF = 0;
    DSP_PPAF = 1u << 15;
#ifdef DSP_LIGHT
    for (i = 0; i < XFORMML_PROG_LEN; ++i)
        DSP_PPD = xformml_prog[i];          /* (the lighting after: xformml.dsp) */
#else
    for (i = 0; i < XFORMM_PROG_LEN; ++i)
        DSP_PPD = xformm_prog[i];
#endif
}

void                dsp_models(const u32 *stream, s32 *out, int models, volatile u32 *count)
{
    DSP_PDA = 32;                           /* RAM0[32..36]: headers, out, models, the count's address, 0 */
    DSP_PDD = ((u32)stream & 0x07FFFFFF) >> 2;
    DSP_PDD = ((u32)out & 0x07FFFFFF) >> 2;
    DSP_PDD = (u32)models;
    DSP_PDD = ((u32)count & 0x07FFFFFF) >> 2;
    DSP_PDD = 0;
    DSP_PPAF = (1u << 16) | (1u << 15);     /* run from PC = 0 */
}

#ifdef DSP_LIGHT
/* ...with the models' lighting (xformml.dsp): ljobs (9 words each, in the models' order,
   each header saying how many are its), the normals, a count of jobs done */
void                dsp_models_lit(const u32 *stream, s32 *out, int models, volatile u32 *count, const u32 *ljobs,
                                   int nljobs, const s32 *normals, volatile u32 *lcount)
{
    DSP_PDA = 32;                           /* RAM0[32..36]: headers, out, models, the count's address, 0 */
    DSP_PDD = ((u32)stream & 0x07FFFFFF) >> 2;
    DSP_PDD = ((u32)out & 0x07FFFFFF) >> 2;
    DSP_PDD = (u32)models;
    DSP_PDD = ((u32)count & 0x07FFFFFF) >> 2;
    DSP_PDD = 0;
    DSP_PDA = 40;                           /* RAM0[40..44]: the lighting's jobs, normals, -, count's address, 0 */
    DSP_PDD = ((u32)ljobs & 0x07FFFFFF) >> 2;
    DSP_PDD = ((u32)normals & 0x07FFFFFF) >> 2;
    DSP_PDD = (u32)nljobs;
    DSP_PDD = ((u32)lcount & 0x07FFFFFF) >> 2;
    DSP_PDD = 0;
    DSP_PPAF = (1u << 16) | (1u << 15);     /* run from PC = 0 */
}
#endif

/* xformf.dsp: faces' grids into view space (a test) */
void                dsp_init_faces(void)
{
    int             i;

    DSP_PPAF = 0;
    DSP_PPAF = 1u << 15;
    for (i = 0; i < XFORMF_PROG_LEN; ++i)
        DSP_PPD = xformf_prog[i];
}

void                dsp_faces(const u32 *jobs, int n, volatile u32 *count, const s32 *axes, const s32 *rows9,
                              const s32 *cam3)
{
    int             i;

    DSP_PDA = 0;                            /* RAM0[0..11]: the camera's rows, where it is */
    for (i = 0; i < 9; ++i)
        DSP_PDD = (u32)rows9[i];
    for (i = 0; i < 3; ++i)
        DSP_PDD = (u32)cam3[i];
    DSP_PDA = 32;                           /* RAM0[32..36]: jobs, how many, the count's address, the axes, 0 */
    DSP_PDD = ((u32)jobs & 0x07FFFFFF) >> 2;
    DSP_PDD = (u32)n;
    DSP_PDD = ((u32)count & 0x07FFFFFF) >> 2;
    DSP_PDD = ((u32)axes & 0x07FFFFFF) >> 2;
    DSP_PDD = 0;
    DSP_PDA = 44;
    DSP_PDD = 6;
    DSP_PPAF = (1u << 16) | (1u << 15);
}

void                dsp_blocks(const u32 *stream, s32 *out, int blocks)
{
    DSP_PDA = 16;                           /* RAM0[16..18]: stream, out, blocks */
    DSP_PDD = ((u32)stream & 0x07FFFFFF) >> 2;
    DSP_PDD = ((u32)out & 0x07FFFFFF) >> 2;
    DSP_PDD = (u32)blocks;
    DSP_PPAF = (1u << 16) | (1u << 15);     /* run from PC = 0 */
}

void                dsp_transform(const s32 *m, const s32 *in, s32 *out, int count)
{
    int             i;

    DSP_PDA = 0;                            /* RAM0[0..11]: matrix */
    for (i = 0; i < 12; ++i)
        DSP_PDD = (u32)m[i];
    DSP_PDA = 16;                           /* RAM0[16..18]: in, out, blocks */
    DSP_PDD = ((u32)in & 0x07FFFFFF) >> 2;
    DSP_PDD = ((u32)out & 0x07FFFFFF) >> 2;
    DSP_PDD = (u32)(count / DSP_BLOCK);
    DSP_PPAF = (1u << 16) | (1u << 15);     /* run from PC = 0 */
}
