/*
** Sound, SH-2 side: the driver itself runs on the 68000 (engine/m68k/).
**
** Here we only start it and talk to it. snd_init holds the 68000 in reset
** (SMPC SNDOFF), copies the driver (built into this program) to the bottom of
** sound RAM, loads the sound bank from the CD above it, releases the 68000
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

extern const u8     snd68k_bin[], snd68k_bin_end[];

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
    const u16       *drv = (const u16 *)snd68k_bin;
    int             i, n = (int)(snd68k_bin_end - snd68k_bin + 1) / 2;

    snd_stage = 1;
    smpc(0x07);                         /* SNDOFF: the 68000 held in reset */
    snd_stage = 2;
    for (i = 0; i < 0x80000 / 2; ++i)
        SND_RAM[i] = 0;
    for (i = 0; i < n; ++i)             /* the driver, at the 68000's address 0 */
        SND_RAM[i] = drv[i];
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
