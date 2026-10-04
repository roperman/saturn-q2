/*
** Sound, the first way: the SCSP driven straight from the SH-2, no 68000.
** NOT BUILT by default - the driver now runs on the 68000 (engine/snd.c and
** engine/m68k/driver.c, the same sequencer ported). Kept to compare the
** approaches; build it with SOUND=sh2 ./build.sh.
**
** The 68EC000 on the sound board is held off (SMPC SNDOFF), and the SH-2
** writes the SCSP's slot registers itself. The bank (tools/gen_sound.py) is
** loaded whole into sound RAM; its tables and songs are copied to work RAM
** for the sequencer, which runs from a 60 Hz interrupt (snd_tick, called
** from the per-field SCU timer). Music takes slots 0-15, one per channel;
** sound effects take 24-31 in turn.
**
** Key on/off: writing KYONEX to any slot applies every slot's KYONB at once,
** so one tick does all its key-offs, then all its key-ons. A slot that's
** retriggered is keyed off first, then on, so its envelope starts again.
*/
#include "sat.h"
#include "snd68k.h"                     /* the bank sits at SND_BANK_BASE either way */

#define SND_RAM         (0x25A00000)
#define SCSP_SLOT(n)    (0x25B00000 + (n) * 0x20)
#define SCSP_MVOL       REG16(0x25B00400)
#define SLOT_REG(n, r)  REG16(SCSP_SLOT(n) + (r))

#define KYONEX          (0x1000)
#define KYONB           (0x0800)
#define LPCTL_LOOP      (0x0020)

#define MAX_CH          (8)
#define SFX_FIRST       (24)
#define SFX_SLOTS       (8)
#define META_MAX        (32768)         /* tables + songs, copied to work RAM: gen_sound.py checks it */

typedef struct
{
    u32             sa;
    u16             lsa, lea;
    u8              flags, ar, d1r, dl, d2r, rr, tl, pad;
    u16             pitch, pad2;
}                   t_inst;             /* 20 bytes, as gen_sound.py writes them */

typedef struct
{
    const u8        *pc, *loop;
    u16             wait, gate;
    u8              inst, vol, pan, on;
}                   t_chan;

static u8           meta[META_MAX] __attribute__((aligned(4)));
static bool         ready;
u32                 snd_ticks;
int                 snd_stage;          /* how far snd_init got: for the debug overlay */
int                 snd_size;
static const t_inst *insts;
static const u16    *pitch_tab;
static const u8     *songs, *sfx_tab;
static int          n_inst, n_songs, n_sfx;

static volatile int song_req = -1;       /* set by the game, taken by the tick */
static volatile int sfx_req[4];
static volatile int sfx_n;
static int          song = -1, n_ch;
static t_chan       ch[MAX_CH];
static u32          tempo_step, tempo_acc;
static u16          keyoff_mask, keyon_mask;
static int          sfx_next;
static int          music_vol = 15;
static int          field_hz = 60;     /* 50 on PAL: the tempo is per field */

static int          s_min(int a, int b) { return a < b ? a : b; }

static u16          be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32          be32(const u8 *p) { return ((u32)be16(p) << 16) | be16(p + 2); }

static void         smpc(u8 cmd)
{
    while (SMPC_SF & 1)
        ;
    SMPC_SF = 1;
    SMPC_COMREG = cmd;
    while (SMPC_SF & 1)
        ;
}

static void         key_exec(void)
{
    SLOT_REG(0, 0x00) = (u16)(SLOT_REG(0, 0x00) | KYONEX);
}

static void         slot_off(int s)
{
    SLOT_REG(s, 0x00) = (u16)(SLOT_REG(s, 0x00) & ~(KYONB | KYONEX));
}

/* set a slot up for instrument i (not keyed on yet) */
static void         slot_setup(int s, const t_inst *in, u16 pitch, int vol, int pan)
{
    int             tl = in->tl + (127 - vol) * 3 / 4 + (15 - music_vol) * 6;
    u16             pn = (u16)(pan < 0 ? (0x10 | (-pan & 0xF)) : (pan & 0xF));

    SLOT_REG(s, 0x00) = (u16)(((in->flags & 1) ? LPCTL_LOOP : 0) | ((in->sa >> 16) & 0xF));
    SLOT_REG(s, 0x02) = (u16)in->sa;
    SLOT_REG(s, 0x04) = in->lsa;
    SLOT_REG(s, 0x06) = in->lea;
    SLOT_REG(s, 0x08) = (u16)((in->d2r << 11) | (in->d1r << 6) | in->ar);
    SLOT_REG(s, 0x0A) = (u16)((0xF << 10) | (in->dl << 5) | in->rr);     /* no key-rate scaling */
    SLOT_REG(s, 0x0C) = (u16)s_min(tl, 255);
    SLOT_REG(s, 0x0E) = 0;
    SLOT_REG(s, 0x10) = pitch;
    SLOT_REG(s, 0x12) = 0;
    SLOT_REG(s, 0x14) = 0;
    SLOT_REG(s, 0x16) = (u16)((7 << 13) | (pn << 8));                     /* direct out, panned */
}

bool                snd_init(const char *file)
{
    int             i, size;
    volatile u16    *ram = (volatile u16 *)SND_RAM;

    snd_stage = 1;
    smpc(0x07);                         /* SNDOFF: the 68000 stays off */
    snd_stage = 2;
    for (i = 0; i < 32; ++i)
    {
        SLOT_REG(i, 0x00) = 0;
        SLOT_REG(i, 0x0C) = 0xFF;
    }
    key_exec();
    SCSP_MVOL = 0x020F;                 /* MEM4MB (512 KB sound RAM), 16-bit DAC, full volume */
    for (i = 0; i < 0x80000 / 2; ++i)
        ram[i] = 0;
    size = cd_load(file, (void *)(SND_RAM + SND_BANK_BASE), 0x7FF00 - SND_BANK_BASE);
    snd_size = size;
    snd_stage = 3;
    if (size < 32)
        return false;
    {
        u32 msize;

        for (i = 0; i < 4; ++i)             /* sound RAM is read a word at a time */
            ((u16 *)meta)[i] = ram[SND_BANK_BASE / 2 + i];
        if (meta[0] != 'S' || meta[1] != 'N' || meta[2] != 'D')
            return false;
        msize = be32(meta + 4);
        if (msize > META_MAX)
            return false;
        for (i = 0; i < (int)msize / 2; ++i)
            ((u16 *)meta)[i] = ram[SND_BANK_BASE / 2 + i];
    }
    field_hz = (VDP2_TVSTAT & 1) ? 50 : 60;
    n_inst = be16(meta + 8);
    n_songs = be16(meta + 10);
    n_sfx = be16(meta + 12);
    insts = (const t_inst *)(meta + be32(meta + 16));
    songs = meta + be32(meta + 20);
    sfx_tab = meta + be32(meta + 24);
    pitch_tab = (const u16 *)(meta + be32(meta + 28));
    ready = true;
    snd_stage = 4;
    return true;
}

void                snd_music(int id)
{
    song_req = id < 0 ? 0xFF : id;
}

int                 snd_music_playing(void)
{
    return song;
}

void                snd_sfx(int id)
{
    if (ready && id >= 0 && id < n_sfx && sfx_n < 4)
        sfx_req[sfx_n++] = id;
}

/* the SH-2 driver has no crossfades or reverb: these are the plain versions */
void                snd_music_cut(int id)
{
    snd_music(id);
}

void                snd_reverb(int level)
{
    (void)level;
}

void                snd_music_volume(int v)
{
    music_vol = v < 0 ? 0 : v > 15 ? 15 : v;
}

static void         start_song(int id)
{
    const u8        *s;
    int             i;

    for (i = 0; i < MAX_CH; ++i)
        slot_off(i);
    keyoff_mask = keyon_mask = 0;
    key_exec();
    song = -1;
    n_ch = 0;
    if (id < 0 || id >= n_songs)
        return;
    s = songs + be16(songs + id * 2);
    n_ch = s_min(s[2], MAX_CH);
    tempo_step = (u32)be16(s) * 48 * 256 / (60 * field_hz);   /* ticks per field, 8.8 */
    tempo_acc = 0;
    for (i = 0; i < n_ch; ++i)
    {
        ch[i].pc = s + be16(s + 4 + i * 2);
        ch[i].loop = 0;
        ch[i].wait = 1;
        ch[i].gate = 0;
        ch[i].inst = 0;
        ch[i].vol = 100;
        ch[i].pan = 0;
        ch[i].on = 0;
    }
    song = id;
}

static void         note_on(int s, t_chan *c, int inst, u16 pitch)
{
    if (inst >= n_inst)
        return;
    if (c->on)
        keyoff_mask |= (u16)(1 << s);
    slot_setup(s, &insts[inst], pitch, c->vol, (s8)c->pan);
    keyon_mask |= (u16)(1 << s);
    c->on = 1;
}

/* run one channel's commands until it has something to wait on */
static void         step_channel(int s, t_chan *c)
{
    int             guard = 64;

    while (c->pc && guard-- > 0)
    {
        u8 op = *c->pc++;

        if (op < 0x80)
        {
            note_on(s, c, c->inst, pitch_tab[op]);
            c->wait = c->pc[0];
            c->gate = c->pc[1];
            c->pc += 2;
            return;
        }
        switch (op)
        {
            case 0x80: c->wait = *c->pc++; return;
            case 0x81: c->inst = *c->pc++; break;
            case 0x82: c->vol = *c->pc++; break;
            case 0x83: c->pan = *c->pc++; break;
            case 0x84: c->loop = c->pc; break;
            case 0x85:
                if (c->loop)
                    c->pc = c->loop;
                else
                {
                    c->pc = 0;
                    return;
                }
                break;
            case 0x86:
                c->wait = *c->pc++;
                if (c->on)
                    c->gate = c->wait;          /* a tie keeps it sounding */
                return;
            case 0x87:
                note_on(s, c, c->pc[0], insts[c->pc[0]].pitch);
                c->wait = c->pc[1];
                c->gate = 0;                    /* drums ring out on their own */
                c->pc += 2;
                return;
            default:
                c->pc = 0;
                return;
        }
    }
}

static void         seq_tick(void)
{
    int             i;

    for (i = 0; i < n_ch; ++i)
    {
        t_chan *c = &ch[i];

        if (c->gate && --c->gate == 0 && c->on)
        {
            keyoff_mask |= (u16)(1 << i);
            c->on = 0;
        }
        if (c->wait && --c->wait == 0)
            step_channel(i, c);
    }
}

/* 60 Hz, from the field interrupt */
void                snd_tick(void)
{
    int             i, r;
    volatile int    d;

    ++snd_ticks;
    if (!ready)
        return;
    r = song_req;
    if (r >= 0)
    {
        song_req = -1;
        start_song(r == 0xFF ? -1 : r);
    }
    if (song >= 0)
    {
        tempo_acc += tempo_step;
        while (tempo_acc >= 256)
        {
            tempo_acc -= 256;
            seq_tick();
        }
    }
    for (i = 0; i < sfx_n; ++i)
    {
        const u8 *e = sfx_tab + sfx_req[i] * 4;
        int s = SFX_FIRST + sfx_next;

        sfx_next = (sfx_next + 1) % SFX_SLOTS;
        slot_off(s);
        key_exec();
        for (d = 0; d < 40; ++d)
            ;
        slot_setup(s, &insts[e[0]], insts[e[0]].pitch, e[2], (s8)e[3]);
        SLOT_REG(s, 0x00) = (u16)(SLOT_REG(s, 0x00) | KYONB);
        key_exec();
    }
    sfx_n = 0;
    if (keyoff_mask)
    {
        /* the key-offs first (including notes about to be retriggered) */
        for (i = 0; i < 16; ++i)
            if (keyoff_mask & (1 << i))
                SLOT_REG(i, 0x00) = (u16)(SLOT_REG(i, 0x00) & ~KYONB);
        key_exec();
        for (d = 0; d < 40; ++d)            /* let the SCSP see it (a sample or so) */
            ;
    }
    if (keyon_mask)
    {
        for (i = 0; i < 16; ++i)
            if (keyon_mask & (1 << i))
                SLOT_REG(i, 0x00) = (u16)(SLOT_REG(i, 0x00) | KYONB);
        key_exec();
    }
    keyoff_mask = keyon_mask = 0;
}
