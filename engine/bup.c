/*
** Internal backup RAM (the Saturn's 32KB save memory), in the format the
** BIOS memory manager uses, so our files sit alongside other games' saves
** and show up in the BIOS "System Settings" screen.
**
** The RAM appears at 0x00180000 with one data byte at every odd address.
** It is 512 blocks of 64 bytes. Block 0 holds "BackUpRam Format" four times;
** blocks 0-1 are reserved. A file starts at a block whose first 4 bytes are
** 0x80000000 (continuation blocks start with 0x00000000); the other 60 bytes
** of each block carry one stream:
**   name[11] language comment[10] date(u32) size(u32)
**   u16 list of the file's further blocks, 0-terminated
**   the data
** Which blocks are free is found by walking every file's block list.
*/
#include "sat.h"

#define BUP_BASE        ((volatile u8 *)0x00180001)
#define BUP_BLOCKS      (512)
#define BLOCK           (64)
#define PAYLOAD         (60)

static u8           rd(u32 off) { return BUP_BASE[off * 2]; }
static void         wr(u32 off, u8 v) { BUP_BASE[off * 2] = v; }

static u32          rd32(u32 off)
{
    return ((u32)rd(off) << 24) | ((u32)rd(off + 1) << 16) | ((u32)rd(off + 2) << 8) | rd(off + 3);
}

bool                bup_formatted(void)
{
    static const char tag[] = "BackUpRam Format";
    int             i;

    for (i = 0; i < 16; ++i)
        if (rd((u32)i) != (u8)tag[i])
            return false;
    return true;
}

void                bup_format(void)
{
    static const char tag[] = "BackUpRam Format";
    u32             i;

    for (i = 0; i < BUP_BLOCKS * BLOCK; ++i)
        wr(i, 0);
    for (i = 0; i < BLOCK; ++i)
        wr(i, (u8)tag[i & 15]);
}

/* the stream of a file: byte n of its payload, following its block list */
typedef struct
{
    u16             blocks[BUP_BLOCKS];
    int             nblocks;
}                   t_chain;

static u32          stream_off(const t_chain *ch, u32 n)
{
    return (u32)ch->blocks[n / PAYLOAD] * BLOCK + 4 + n % PAYLOAD;
}

/* read the block list of the file starting at `first` */
static void         load_chain(u16 first, t_chain *ch)
{
    u32             n = 30, b;

    ch->blocks[0] = first;
    ch->nblocks = 1;
    for (;;)
    {
        /* the list itself may run into the following blocks */
        u32 o = stream_off(ch, n);

        if (n / PAYLOAD >= (u32)ch->nblocks)
            break;
        b = ((u32)rd(o) << 8);
        o = stream_off(ch, n + 1);
        if ((n + 1) / PAYLOAD >= (u32)ch->nblocks)
            break;
        b |= rd(o);
        n += 2;
        if (b == 0 || ch->nblocks >= BUP_BLOCKS)
            break;
        ch->blocks[ch->nblocks++] = (u16)b;
    }
}

static int          find_file(const char *name)
{
    int             b, i;

    for (b = 2; b < BUP_BLOCKS; ++b)
    {
        u32 o = (u32)b * BLOCK;

        if (rd32(o) != 0x80000000u)
            continue;
        for (i = 0; i < 11; ++i)
        {
            u8 c = name[i] ? (u8)name[i] : 0;

            if (rd(o + 4 + i) != c)
                break;
            if (!c)
            {
                i = 11;
                break;
            }
        }
        if (i >= 11)
            return b;
    }
    return -1;
}

static t_chain      chain;                  /* static: 1KB, too big for the stack */

int                 bup_read(const char *name, void *buf, int max)
{
    int             first = find_file(name), n, list_len;
    u32             size, i;

    if (first < 0 || !bup_formatted())
        return -1;
    load_chain((u16)first, &chain);
    size = ((u32)rd(stream_off(&chain, 26)) << 24) | ((u32)rd(stream_off(&chain, 27)) << 16) |
           ((u32)rd(stream_off(&chain, 28)) << 8) | rd(stream_off(&chain, 29));
    list_len = (chain.nblocks - 1) * 2 + 2;
    n = (int)(size < (u32)max ? size : (u32)max);
    for (i = 0; i < (u32)n; ++i)
        ((u8 *)buf)[i] = rd(stream_off(&chain, 30 + (u32)list_len + i));
    return n;
}

bool                bup_delete(const char *name)
{
    int             first = find_file(name), k;
    u32             i;

    if (first < 0)
        return false;
    load_chain((u16)first, &chain);
    for (k = 0; k < chain.nblocks; ++k)
        for (i = 0; i < BLOCK; ++i)
            wr((u32)chain.blocks[k] * BLOCK + i, 0);
    return true;
}

static u8           used[BUP_BLOCKS];

int                 bup_free_blocks(void)
{
    int             b, k, n = 0;
    static t_chain  other;

    memset(used, 0, sizeof(used));
    used[0] = used[1] = 1;
    for (b = 2; b < BUP_BLOCKS; ++b)
        if (rd32((u32)b * BLOCK) == 0x80000000u)
        {
            load_chain((u16)b, &other);
            for (k = 0; k < other.nblocks; ++k)
                used[other.blocks[k]] = 1;
        }
    for (b = 0; b < BUP_BLOCKS; ++b)
        n += !used[b];
    return n;
}

bool                bup_write(const char *name, const char *comment, const void *buf, int size)
{
    int             need, b, k;
    u32             i, pos;

    if (!bup_formatted())
        bup_format();
    bup_delete(name);
    /* blocks for: 30 header bytes + the block list + the data, 60 per block */
    for (need = 1;; ++need)
        if (30 + (need - 1) * 2 + 2 + size <= need * PAYLOAD)
            break;
    if (bup_free_blocks() < need)
        return false;
    for (b = 2, k = 0; b < BUP_BLOCKS && k < need; ++b)
        if (!used[b])
            chain.blocks[k++] = (u16)b;
    chain.nblocks = need;
    for (k = 0; k < need; ++k)
    {
        u32 o = (u32)chain.blocks[k] * BLOCK;

        for (i = 0; i < BLOCK; ++i)
            wr(o + i, 0);
        wr(o, k == 0 ? 0x80 : 0);
    }
    /* the stream */
    pos = 0;
#define PUT(v)  wr(stream_off(&chain, pos++), (u8)(v))
    for (i = 0, k = 0; i < 11; ++i)
    {
        if (name[k])
            PUT(name[k++]);
        else
            PUT(0);
    }
    PUT(1);                                 /* language: 0 Japanese, 1 English */
    for (i = 0, k = 0; i < 10; ++i)
    {
        if (comment[k])
            PUT(comment[k++]);
        else
            PUT(' ');
    }
    for (i = 0; i < 4; ++i)
        PUT(0);                             /* date: minutes since 1980 (no clock read yet) */
    PUT(size >> 24); PUT(size >> 16); PUT(size >> 8); PUT(size);
    for (k = 1; k < need; ++k)
    {
        PUT(chain.blocks[k] >> 8);
        PUT(chain.blocks[k]);
    }
    PUT(0); PUT(0);
    for (i = 0; i < (u32)size; ++i)
        PUT(((const u8 *)buf)[i]);
#undef PUT
    return true;
}
