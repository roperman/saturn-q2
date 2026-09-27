/*
** SCU DSP host side: program upload and job start (see dsp.h, xform.dsp).
*/
#include "dsp.h"
#include "xform.h"
#include "xformb.h"
#include "xformp.h"

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
