/*
** The sound driver, running on the Saturn's 68000.
**
** The same sequencer as engine/snd_sh2.c (the SH-2 version, kept for
** comparison), moved onto the sound CPU: it reads the bank straight out of
** sound RAM, keeps time with the SCSP's own timer A (60 ticks a second,
** PAL or NTSC alike), and takes orders from the SH-2 through a mailbox at
** the top of sound RAM. The SH-2 never touches the SCSP any more.
**
** Music takes slots 0-15: two decks of 8, so a new song can start on one
** while the old one fades out on the other (a crossfade). Sound effects take
** 24-31 in turn. Everything can also be sent to a reverb that runs on the
** SCSP's own DSP (see dsp_init).
** Key on/off: writing KYONEX to any slot applies every slot's KYONB at once,
** so a tick does all its key-offs, then all its key-ons.
*/
#include "../snd68k.h"

typedef unsigned char   u8;
typedef signed char     s8;
typedef unsigned short  u16;
typedef unsigned long   u32;

/* the SCSP, as the 68000 sees it */
#define SCSP(r)         (*(volatile u16 *)(0x100000 + (r)))
#define SLOT(n, r)      SCSP((n) * 0x20 + (r))
#define MVOL            0x400
#define TIMA            0x418
#define SCIPD           0x420               /* interrupts pending */
#define SCIRE           0x422               /* ...and cleared by writing here */
#define TIMER_A         0x40

#define KYONEX          0x1000
#define KYONB           0x0800
#define LPCTL_LOOP      0x0020
#define PCM8B           0x0010              /* 8-bit samples (flags 4) */

#define MAX_CH          8
#ifndef SFX_FIRST
#define SFX_FIRST       16                  /* (with no songs, 16 of them; with songs, keep them clear of the decks) */
#endif
#define SFX_SLOTS       (32 - SFX_FIRST)

#define MBOX            ((volatile u16 *)SND_MBOX)
#define BANK            ((const u8 *)SND_BANK_BASE)

typedef struct
{
    u32                 sa;
    u16                 lsa, lea;
    u8                  flags, ar, d1r, dl, d2r, rr, tl, pad;
    u16                 pitch, pad2;
}                       t_inst;             /* 20 bytes, as gen_sound.py writes them */

typedef struct
{
    const u8            *pc, *loop;
    u16                 wait, gate;
    u8                  inst, vol, pan, on;
    u8                  tl;                 /* its level before any fade */
}                       t_chan;

/* a deck plays one song on 8 slots; two decks crossfade */
typedef struct
{
    t_chan              ch[MAX_CH];
    int                 song, n_ch, slot0;
    u16                 tempo_step, tempo_acc;
    int                 fade, fade_step;    /* 0 silent .. 256 full, and which way it's going */
}                       t_deck;

static const t_inst     *insts;
static const u16        *pitch_tab;
static const u8         *songs, *sfx_tab;
static u16              n_inst, n_songs, n_sfx;

static t_deck           deck[2];
static int              cur;                /* the deck with the song we're meant to be playing */
static int              fade_ticks = 30;    /* half a second */
static u16              keyoff_mask, keyon_mask;
static int              sfx_next;
static int              music_vol = 15;
static int              reverb;             /* 0-7: how much of everything goes to the reverb */
static int              dsp_on;             /* (its program running: only once some's asked for) */
static void             dsp_init(void);
static u32              ticks;

static u16              be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32              be32(const u8 *p) { return ((u32)be16(p) << 16) | be16(p + 2); }

static void             key_exec(void)
{
    SLOT(0, 0x00) = (u16)(SLOT(0, 0x00) | KYONEX);
}

static void             slot_off(int s)
{
    SLOT(s, 0x00) = (u16)(SLOT(s, 0x00) & ~(KYONB | KYONEX));
}

static void             pause(void)
{
    volatile int        d;

    for (d = 0; d < 8; ++d)             /* let the SCSP see a key-off before the key-on */
        ;
}

/* the DSP's two outputs come back through slots 0 and 1's effect send
   (EFSDL/EFPAN): one a little left, one a little right */
static u16              effect_return(int s)
{
    if (!dsp_on)
        return 0;                           /* (nothing back from it till it's running) */
    return s == 0 ? (u16)((7 << 5) | 0x18) : s == 1 ? (u16)((7 << 5) | 0x08) : 0;
}

/* set a slot up for an instrument (not keyed on yet); returns its level */
static int              slot_setup(int s, const t_inst *in, u16 pitch, int vol, int pan, int fade_att)
{
    int                 tl = in->tl + (((127 - vol) * 3) >> 2) + (15 - music_vol) * 6;
    int                 t = tl + fade_att;
    u16                 pn = (u16)(pan < 0 ? (0x10 | (-pan & 0xF)) : (pan & 0xF));

    SLOT(s, 0x00) = (u16)(((in->flags & 1) ? LPCTL_LOOP : 0) | ((in->flags & 4) ? PCM8B : 0) | ((in->sa >> 16) & 0xF));
    SLOT(s, 0x02) = (u16)in->sa;
    SLOT(s, 0x04) = in->lsa;
    SLOT(s, 0x06) = in->lea;
    SLOT(s, 0x08) = (u16)((in->d2r << 11) | (in->d1r << 6) | in->ar);
    SLOT(s, 0x0A) = (u16)((0xF << 10) | (in->dl << 5) | in->rr);     /* no key-rate scaling */
    SLOT(s, 0x0C) = (u16)(t > 255 ? 255 : t);
    SLOT(s, 0x0E) = 0;
    SLOT(s, 0x10) = pitch;
    SLOT(s, 0x12) = 0;
    SLOT(s, 0x14) = (u16)reverb;                                      /* ISEL 0: into the reverb, this much */
    SLOT(s, 0x16) = (u16)((7 << 13) | (pn << 8) | effect_return(s));  /* direct out, panned */
    return tl > 255 ? 255 : tl;
}

static int              fade_att(const t_deck *d)
{
    return (256 - d->fade) >> 1;            /* up to 128 steps of 0.375 dB */
}

static void             deck_stop(t_deck *d)
{
    int                 i;

    for (i = 0; i < MAX_CH; ++i)
        if (d->ch[i].on)
            keyoff_mask |= (u16)(1 << (d->slot0 + i));
    for (i = 0; i < MAX_CH; ++i)
        d->ch[i].on = 0;
    d->song = -1;
    d->n_ch = 0;
    d->fade_step = 0;
}

static void             deck_start(t_deck *d, int id, int fade, int fade_step)
{
    const u8            *s;
    int                 i;

    deck_stop(d);
    if (id < 0 || id >= n_songs)
        return;
    s = songs + be16(songs + id * 2);
    d->n_ch = s[2] < MAX_CH ? s[2] : MAX_CH;
    d->tempo_step = (u16)((u32)be16(s) * 48 * 256 / 3600);  /* ticks of the song per 60 Hz tick, 8.8 */
    d->tempo_acc = 0;
    for (i = 0; i < d->n_ch; ++i)
    {
        t_chan *c = &d->ch[i];

        c->pc = s + be16(s + 4 + i * 2);
        c->loop = 0;
        c->wait = 1;
        c->gate = 0;
        c->inst = 0;
        c->vol = 100;
        c->pan = 0;
        c->on = 0;
    }
    d->song = id;
    d->fade = fade;
    d->fade_step = fade_step;
}

/* a new song: crossfade to it on the other deck, or cut straight to it */
static void             play_song(int id, int cut)
{
    int                 step = 256 / (fade_ticks > 0 ? fade_ticks : 1);

    if (deck[cur].song == id && !cut)
        return;
    if (cut || deck[cur].song < 0)
    {
        deck_stop(&deck[cur ^ 1]);
        deck_start(&deck[cur], id, 256, 0);
    }
    else
    {
        deck[cur].fade_step = -step;        /* the old song fades away... */
        cur ^= 1;
        deck_start(&deck[cur], id, 0, step);    /* ...as the new one comes up */
    }
    MBOX[MB_SONG] = (u16)(id < 0 ? 0xFFFF : id);
}

static void             note_on(t_deck *d, int i, int inst, u16 pitch)
{
    t_chan              *c = &d->ch[i];
    int                 s = d->slot0 + i;

    if (inst >= n_inst)
        return;
    if (c->on)
        keyoff_mask |= (u16)(1 << s);
    c->tl = (u8)slot_setup(s, &insts[inst], pitch, c->vol, (s8)c->pan, fade_att(d));
    keyon_mask |= (u16)(1 << s);
    c->on = 1;
}

/* run one channel's commands until it has something to wait on */
static void             step_channel(t_deck *d, int i)
{
    t_chan              *c = &d->ch[i];
    int                 guard = 64;

    while (c->pc && guard-- > 0)
    {
        u8 op = *c->pc++;

        if (op < 0x80)
        {
            note_on(d, i, c->inst, pitch_tab[op]);
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
                note_on(d, i, c->pc[0], insts[c->pc[0]].pitch);
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

static void             deck_tick(t_deck *d)
{
    int                 i;

    if (d->song < 0)
        return;
    if (d->fade_step)
    {
        d->fade += d->fade_step;
        if (d->fade >= 256)
        {
            d->fade = 256;
            d->fade_step = 0;
        }
        if (d->fade <= 0)
        {
            deck_stop(d);                   /* faded right out */
            return;
        }
        for (i = 0; i < MAX_CH; ++i)        /* the fade applies to what's already sounding */
            if (d->ch[i].on)
            {
                int t = d->ch[i].tl + fade_att(d);

                SLOT(d->slot0 + i, 0x0C) = (u16)(t > 255 ? 255 : t);
            }
    }
    d->tempo_acc += d->tempo_step;
    while (d->tempo_acc >= 256)
    {
        d->tempo_acc -= 256;
        for (i = 0; i < d->n_ch; ++i)
        {
            t_chan *c = &d->ch[i];

            if (c->gate && --c->gate == 0 && c->on)
            {
                keyoff_mask |= (u16)(1 << (d->slot0 + i));
                c->on = 0;
            }
            if (c->wait && --c->wait == 0)
                step_channel(d, i);
        }
    }
}

/* the next effect's volume (0-127, times its own) and pan, if SND_CMD_SFXVP set them */
static int              vp_vol = -1, vp_pan;

static void             play_sfx(int id)
{
    const u8            *e;
    int                 s, vol, pan;

    if (id < 0 || id >= n_sfx)
        return;
    e = sfx_tab + id * 4;
    vol = e[2];
    pan = (s8)e[3];
    if (vp_vol >= 0)
    {
        vol = (vol * vp_vol) >> 7;
        pan = vp_pan;
        vp_vol = -1;
    }
    s = SFX_FIRST + sfx_next;
    sfx_next = (sfx_next + 1) % SFX_SLOTS;
    slot_off(s);
    key_exec();
    pause();
    slot_setup(s, &insts[e[0]], insts[e[0]].pitch, vol, pan, 0);
    SLOT(s, 0x00) = (u16)(SLOT(s, 0x00) | KYONB);
    key_exec();
}

/* orders from the SH-2 */
static void             take_commands(void)
{
    while (MBOX[MB_READ] != MBOX[MB_WRITE])
    {
        u16 r = MBOX[MB_READ], cmd = MBOX[MB_RING + r], arg = cmd & 0x0FFF;

        switch (cmd >> 12)
        {
            case SND_CMD_PLAY: play_song(arg, 0); break;
            case SND_CMD_CUT: play_song(arg, 1); break;
            case SND_CMD_STOP: play_song(-1, 0); break;
            case SND_CMD_REVERB: reverb = arg > 7 ? 7 : arg; if (reverb && !dsp_on) dsp_init(); break;
            case SND_CMD_SFX: play_sfx(arg); break;
            case SND_CMD_SFXVP: vp_vol = arg >> 5; vp_pan = (int)(arg & 31) - 15; break;
            case SND_CMD_VOLUME: music_vol = arg > 15 ? 15 : arg; break;
        }
        MBOX[MB_READ] = (u16)((r + 1) % SND_RING);
    }
}

/* one 60 Hz tick */
static void             tick(void)
{
    int                 i;

    take_commands();
    deck_tick(&deck[0]);
    deck_tick(&deck[1]);
    if (keyoff_mask)
    {
        for (i = 0; i < 16; ++i)
            if (keyoff_mask & (1 << i))
                SLOT(i, 0x00) = (u16)(SLOT(i, 0x00) & ~KYONB);
        key_exec();
        pause();
    }
    if (keyon_mask)
    {
        for (i = 0; i < 16; ++i)
            if (keyon_mask & (1 << i))
                SLOT(i, 0x00) = (u16)(SLOT(i, 0x00) | KYONB);
        key_exec();
    }
    keyoff_mask = keyon_mask = 0;
    ++ticks;
    MBOX[MB_TICKS_HI] = (u16)(ticks >> 16);
    MBOX[MB_TICKS_LO] = (u16)ticks;
}

/* ---------------------------------------------------------------- the reverb
** A program for the SCSP's effects DSP, which runs all 128 of its steps once
** per output sample (44.1 kHz) on its own. Two feedback delay lines of
** different lengths (32 and 43 ms) - one returned a little left, one a little
** right - make a small stereo room; how much of each voice goes in is the
** slot's IMXL (the map's 'reverb' setting). The delay lines live in a ring
** buffer in sound RAM at SND_DSP_RING.
**
** Step format (four 16-bit words, from MAME's scspdsp.cpp and the SCSP manual):
**   w0: TRA<<8 | TWT<<7 | TWA         temp RAM read / write
**   w1: XSEL<<15 | YSEL<<13 | IRA<<6 | IWT<<5 | IWA
**   w2: TABLE<<15 | MWT<<14 | MRD<<13 | EWT<<12 | EWA<<8 | ADRL<<7 | FRCL<<6 |
**       SHIFT<<4 | YRL<<3 | NEGB<<2 | ZERO<<1 | BSEL
**   w3: NOFL<<15 | COEF<<9 | MASA<<2 | ADREB<<1 | NXADR
** Each step: ACC = X * Y >> 12 + B, where X is an input (XSEL=1: IRA picks
** MEMS 0-31, MIXS 32-47), Y a coefficient (YSEL=1), B the old ACC (BSEL) or
** 0 (ZERO). Memory reads and writes (MRD, MWT) happen on odd steps; a read's
** value is picked up into MEMS[IWA] by a later IWT. EWT adds to an output.
*/
#define X_IN(ira)       ((1 << 15) | (1 << 13) | ((ira) << 6))     /* XSEL=1, YSEL=1 (coefficient) */
#define MIXS(n)         (0x20 + (n))
#define COEF(n)         ((n) << 9)
#define NOP             { 0, (1 << 13), 0, COEF(5) }                /* ACC = TEMP * 0 + TEMP: harmless */

static const u16        reverb_prog[][4] =
{
    NOP,
    { 0, (1 << 13), (1 << 13), (1 << 15) | COEF(5) | (1 << 2) },         /* 1: MRD delay A's tap (MADRS 1) */
    NOP,
    { 0, (1 << 13) | (1 << 5) | 0, 0, COEF(5) },                            /* 3: MEMS[0] = it */
    NOP,
    { 0, (1 << 13), (1 << 13), (1 << 15) | COEF(5) | (3 << 2) },         /* 5: MRD delay B's tap (MADRS 3) */
    NOP,
    { 0, (1 << 13) | (1 << 5) | 1, 0, COEF(5) },                            /* 7: MEMS[1] = it */
    { 0, X_IN(MIXS(0)), (1 << 1), COEF(0) },                                /* 8: ACC = in * gain */
    { 0, X_IN(0), 1, COEF(1) },                                             /* 9: ACC += A's tap * feedback */
    { 0, X_IN(0), 1, COEF(5) },                                             /* 10: hold ACC */
    { 0, X_IN(MIXS(0)), (1 << 14) | (1 << 1), (1 << 15) | COEF(0) | (0 << 2) },  /* 11: write A; ACC = in * gain */
    { 0, X_IN(1), 1, COEF(1) },                                             /* 12: ACC += B's tap * feedback */
    { 0, X_IN(0), (1 << 14) | (1 << 1), (1 << 15) | COEF(2) | (2 << 2) },  /* 13: write B; ACC = A's tap * wet */
    { 0, X_IN(1), (1 << 12) | (0 << 8) | (1 << 1), COEF(2) },               /* 14: out 0 += it; ACC = B's tap * wet */
    { 0, X_IN(1), (1 << 12) | (1 << 8) | (1 << 1), COEF(5) },               /* 15: out 1 += it */
};

#define DSP_COEF(n)     SCSP(0x700 + (n) * 2)
#define DSP_MADRS(n)    SCSP(0x780 + (n) * 2)
#define DSP_MPRO(n, w)  SCSP(0x800 + (n) * 8 + (w) * 2)
#define DSP_RING        0x402                   /* RBL (length) << 7 | RBP (where, in 8 KB units) */

/* stopped: its program all NOPs (it runs on its own, whatever the 68000's doing: a step at a time
   written while it runs is how a half-set-up program filled its delay lines with what was there,
   and played it back as a pop as the driver started) */
static void             dsp_stop(void)
{
    unsigned            i, w;

    for (i = 0; i < 128; ++i)
        for (w = 0; w < 4; ++w)
            DSP_MPRO(i, w) = 0;
}

/* the reverb started (the first time some's asked for): its ring placed and cleared, its
   coefficients and delays set, then its program; then slots 0 and 1 bring it back out */
static void             dsp_init(void)
{
    unsigned            i, w;
    volatile u16        *ring = (volatile u16 *)SND_DSP_RING;

    dsp_stop();
    SCSP(DSP_RING) = (u16)((0 << 7) | (SND_DSP_RING >> 13));   /* 8K words (16 KB) at SND_DSP_RING */
    for (i = 0; i < 8192; ++i)
        ring[i] = 0;
    /* coefficients are 13-bit fractions (4096 = 1.0), stored << 3 */
    DSP_COEF(0) = (u16)(2048 << 3);             /* input gain 0.5 */
    DSP_COEF(1) = (u16)(2460 << 3);             /* feedback 0.6: a longer tail */
    DSP_COEF(2) = (u16)(3280 << 3);             /* wet 0.8 */
    DSP_COEF(5) = 0;
    /* delay lines, in samples from the ring's moving write point */
    DSP_MADRS(0) = 0;                           /* A: written here... */
    DSP_MADRS(1) = 1433;                        /* ...read 32 ms later */
    DSP_MADRS(2) = 4096;                        /* B, in the other half */
    DSP_MADRS(3) = 4096 + 1901;                 /* 43 ms */
    for (i = 0; i < sizeof(reverb_prog) / sizeof(reverb_prog[0]); ++i)
        for (w = 0; w < 4; ++w)
            DSP_MPRO(i, w) = reverb_prog[i][w];
    dsp_on = 1;
    SLOT(0, 0x16) = (u16)((SLOT(0, 0x16) & ~0xFF) | effect_return(0));
    SLOT(1, 0x16) = (u16)((SLOT(1, 0x16) & ~0xFF) | effect_return(1));
}

/* timer A counts up at 44100 / 8 Hz and flags an overflow past 0xFF:
   92 counts from 164 is 59.9 Hz */
#define TIMER_RELOAD    ((3 << 8) | (256 - 92))

void                    main(void)
{
    int                 i;

    dsp_stop();                             /* (first: whatever was left running) */
    for (i = 0; i < 32; ++i)
    {
        SLOT(i, 0x00) = 0;
        SLOT(i, 0x0C) = 0xFF;
        SLOT(i, 0x16) = 0;                  /* (the reverb, if it's started, comes back through 0 and 1) */
    }
    key_exec();
    SCSP(MVOL) = 0x000F;                    /* 512 KB sound RAM, full volume */
    if (BANK[0] != 'S' || BANK[1] != 'N' || BANK[2] != 'D')
    {
        MBOX[MB_STATUS] = 1;                /* no bank: say so, and sit quietly */
        MBOX[MB_MAGIC] = SND_ALIVE;
        for (;;)
            ;
    }
    n_inst = be16(BANK + 8);
    n_songs = be16(BANK + 10);
    n_sfx = be16(BANK + 12);
    insts = (const t_inst *)(BANK + be32(BANK + 16));
    songs = BANK + be32(BANK + 20);
    sfx_tab = BANK + be32(BANK + 24);
    pitch_tab = (const u16 *)(BANK + be32(BANK + 28));
    MBOX[MB_SONG] = 0xFFFF;
    MBOX[MB_STATUS] = 0;
    deck[0].song = deck[1].song = -1;
    deck[0].slot0 = 0;
    deck[1].slot0 = 8;
    SCSP(TIMA) = TIMER_RELOAD;
    SCSP(SCIRE) = TIMER_A;
    MBOX[MB_MAGIC] = SND_ALIVE;
    for (;;)
    {
        while (!(SCSP(SCIPD) & TIMER_A))
            ;                               /* wait for the timer */
        SCSP(SCIRE) = TIMER_A;
        SCSP(TIMA) = TIMER_RELOAD;
        tick();
    }
}
