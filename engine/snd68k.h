/*
** Shared by the SH-2 (engine/snd.c) and the 68000 sound driver (engine/m68k/):
** where things live in the 512 KB of sound RAM, and the mailbox between them.
** Addresses are the 68000's; the SH-2 sees sound RAM at 0x25A00000.
**
**   0x00000  68000 vectors, then the driver's code and data
**   0x08000  (stack top) the sound bank: tables, songs, samples (SOUND.BIN)
**   0x70000  the reverb's delay lines (the SCSP DSP's ring buffer, 16 KB)
**   0x7FF00  the mailbox page
*/
#ifndef SND68K_H
#define SND68K_H

#define SND_BANK_BASE       0x8000
#define SND_MBOX            0x7FF00
#define SND_DSP_RING        0x70000         /* the bank must end below this */

/* commands, SH-2 -> 68000: a ring of words, op << 12 | argument */
#define SND_CMD_PLAY        1       /* argument: song id */
#define SND_CMD_STOP        2
#define SND_CMD_SFX         3       /* argument: effect id */
#define SND_CMD_VOLUME      4       /* argument: 0-15 */
#define SND_CMD_REVERB      5       /* argument: 0 (dry) - 7 (cathedral) */
#define SND_CMD_CUT         6       /* argument: song id - start at once, no crossfade */
#define SND_RING            32

/* the mailbox, as 16-bit words (offsets in words from SND_MBOX) */
#define MB_MAGIC            0       /* the 68000 writes SND_ALIVE once it's running */
#define MB_SONG             1       /* the song it's playing, 0xFFFF none */
#define MB_TICKS_HI         2       /* 60 Hz ticks since it started */
#define MB_TICKS_LO         3
#define MB_STATUS           4       /* 0 fine; else what went wrong loading the bank */
#define MB_WRITE            8       /* the SH-2's next slot in the ring */
#define MB_READ             9       /* the 68000's next slot */
#define MB_RING             16      /* SND_RING words */

#define SND_ALIVE           0x6800

#endif
