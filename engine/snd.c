/*
** Sound, SH-2 side: the driver itself runs on the 68000 (engine/m68k/).
**
** Here we only start it and talk to it. snd_init holds the 68000 in reset
** (SMPC SNDOFF), loads the driver (SND68K.BIN, from the CD: it's only wanted
** once) to the bottom of sound RAM and the sound bank above it, releases the 68000
** (SMPC SNDON) and waits for it to say it's alive. After that, music and
** effects are requests posted into the mailbox at the top of sound RAM
** (engine/snd68k.h); the 68000 keeps its own time with the SCSP's timer.
**
** The earlier approach - the SH-2 driving the SCSP itself from its 60 Hz
** interrupt - is engine/snd_sh2.c, still buildable: SOUND=sh2 ./build.sh.
*/
#include "sat.h"
#include "snd68k.h"

#define SND_RAM         ((volatile u16 *)0x25A00000)
#define MBOX            ((volatile u16 *)(0x25A00000 + SND_MBOX))

static bool         ready;
static int          requested = -1;     /* the song we last asked for */
u32                 snd_ticks;
int                 snd_stage, snd_size;

static void         smpc(u8 cmd)
{
    while (SMPC_SF & 1)
        ;
    SMPC_SF = 1;
    SMPC_COMREG = cmd;
    while (SMPC_SF & 1)
        ;
}

static void         post(int op, int arg)
{
    u16             w;

    if (!ready)
        return;
    w = MBOX[MB_WRITE];
    if ((w + 1) % SND_RING == MBOX[MB_READ])
        return;                         /* ring full: the 68000 has stopped listening */
    MBOX[MB_RING + w] = (u16)((op << 12) | (arg & 0x0FFF));
    MBOX[MB_WRITE] = (u16)((w + 1) % SND_RING);
}

bool                snd_init(const char *file)
{
    int             i;

    snd_stage = 1;
    smpc(0x07);                         /* SNDOFF: the 68000 held in reset */
    snd_stage = 2;
    REG16(0x25B00400) = 0x020F;         /* the SCSP: MEM4MB (its RAM 4 Mbit, 512 KB: how it addresses it, for
                                           the 68000 too; Mednafen ignores it), full volume: set before
                                           anything's loaded, so the 68000's view of its RAM never changes */
    for (i = 0; i < 0x80000 / 2; ++i)
        SND_RAM[i] = 0;
    if (cd_load("SND68K.BIN", (void *)0x25A00000, SND_BANK_BASE) < 64)
        return false;                   /* the driver, at the 68000's address 0 */
    snd_size = cd_load(file, (void *)(0x25A00000 + SND_BANK_BASE), SND_DSP_RING - SND_BANK_BASE);
    snd_stage = 3;
    if (snd_size < 32)
        return false;
    smpc(0x06);                         /* SNDON: off it goes */
    for (i = 0; i < 0x200000 && MBOX[MB_MAGIC] != SND_ALIVE; ++i)
        ;
    if (MBOX[MB_MAGIC] != SND_ALIVE || MBOX[MB_STATUS] != 0)
        return false;
    ready = true;
    snd_stage = 4;
    return true;
}

/* Another bank (a new level's): the 68000 held while it's read in over the last, the
   mailbox cleared, and off again (it reads the bank's tables as it starts). The pad's asks
   of the SMPC (the vblank-out interrupt, sys.c) are held meanwhile: one landing between
   ours would be taken for it */
bool                snd_bank(const char *file)
{
    bool            pads = pad_by_vblank(false);
    int             i;

    ready = false;
    smpc(0x07);                         /* SNDOFF */
    snd_size = cd_load(file, (void *)(0x25A00000 + SND_BANK_BASE), SND_DSP_RING - SND_BANK_BASE);
    for (i = 0; i < 128; ++i)
        MBOX[i] = 0;
    if (snd_size >= 32)
    {
        smpc(0x06);                     /* SNDON */
        for (i = 0; i < 0x200000 && MBOX[MB_MAGIC] != SND_ALIVE; ++i)
            ;
        ready = MBOX[MB_MAGIC] == SND_ALIVE && MBOX[MB_STATUS] == 0;
    }
    pad_by_vblank(pads);
    return ready;
}

/* (the stats) how the start went: the stage reached, whether it's ready, the 68000's word in
   the mailbox (alive?) and its status, the bank's size in KB */
u32                 snd_state(void)
{
    return (u32)snd_stage | (u32)ready << 4 | (u32)(MBOX[MB_MAGIC] == SND_ALIVE) << 5 | (u32)(MBOX[MB_STATUS] & 15) << 8
           | (u32)(snd_size > 0 ? snd_size >> 10 : 0) << 16;
}

void                snd_music(int id)
{
    requested = id;
    post(id < 0 ? SND_CMD_STOP : SND_CMD_PLAY, id < 0 ? 0 : id);
}

/* what we asked for (the 68000 may be a tick behind) */
int                 snd_music_playing(void)
{
    return requested;
}

void                snd_sfx(int id)
{
    if (id >= 0)
        post(SND_CMD_SFX, id);
}

/* an effect at a volume (0-127, times its own) and pan (-15 left .. 15 right) */
void                snd_sfx_at(int id, int vol, int pan)
{
    if (id < 0)
        return;
    vol = vol < 0 ? 0 : vol > 127 ? 127 : vol;
    pan = pan < -15 ? -15 : pan > 15 ? 15 : pan;
    post(SND_CMD_SFXVP, vol << 5 | (pan + 15));
    post(SND_CMD_SFX, id);
}

/* ...us microseconds from now (to its fast tick: SND_FAST_HZ) */
void                snd_sfx_later(int id, int vol, int pan, int us)
{
    int             ticks = (int)((u32)(us > 0 ? us : 0) * SND_FAST_HZ / 1000000u);

    if (id < 0)
        return;
    if (ticks > 0)
        post(SND_CMD_DELAY, ticks > 0xFFF ? 0xFFF : ticks);
    snd_sfx_at(id, vol, pan);
}

/* straight to a song, no crossfade (a battle starting) */
void                snd_music_cut(int id)
{
    requested = id;
    post(id < 0 ? SND_CMD_STOP : SND_CMD_CUT, id < 0 ? 0 : id);
}

/* how much of everything goes to the reverb: 0 dry .. 7 a cathedral */
void                snd_reverb(int level)
{
    post(SND_CMD_REVERB, level < 0 ? 0 : level > 7 ? 7 : level);
}

void                snd_music_volume(int v)
{
    post(SND_CMD_VOLUME, v < 0 ? 0 : v > 15 ? 15 : v);
}

/* the 68000 keeps its own time now; this just reads its tick count for the
   debug overlay */
void                snd_tick(void)
{
    if (ready)
        snd_ticks = ((u32)MBOX[MB_TICKS_HI] << 16) | MBOX[MB_TICKS_LO];
}
