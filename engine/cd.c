/*
** CD block driver: read files from the game disc. Bare metal, no Sega code.
**
** The CD block is a separate processor behind four 16-bit command registers
** (CR1-CR4), an interrupt/status register (HIRQ) and a data port (DTR). To
** run a command: clear CMOK and the flags we'll wait for in HIRQ, write
** CR1-CR4, wait for CMOK, read the status back from CR1-CR4. Reading data:
** connect the drive to buffer partition 0, "play" a range of sectors (FAD =
** LBA + 150), and as sectors arrive fetch them with Get-and-Delete Sector
** Data, then read the words from DTR once DRDY is up, and End Data Transfer.
**
** The command sequences, the timeouts and the init order follow libyaul's
** cd-block driver (MIT, (c) Israel Jacquez), which runs on real hardware;
** register behaviour cross-checked against Mednafen's cdb.cpp.
** Files: a small ISO 9660 reader, root directory only (mkisofs puts ours
** there). The BIOS has already authenticated the disc by booting it.
*/
#include "sat.h"

#define CDB             (0x25890000)
#define CD_DTR          REG16(CDB + 0x00)
#define CD_HIRQ         REG16(CDB + 0x08)
#define CD_HIRQ_MASK    REG16(CDB + 0x0C)
#define CD_CR1          REG16(CDB + 0x18)
#define CD_CR2          REG16(CDB + 0x1C)
#define CD_CR3          REG16(CDB + 0x20)
#define CD_CR4          REG16(CDB + 0x24)

#define CMOK            (0x0001)    /* ready for a command */
#define DRDY            (0x0002)    /* data transfer ready */
#define CSCT            (0x0004)    /* a sector has been read */
#define PEND            (0x0010)    /* play finished */
#define ESEL            (0x0040)    /* selector settings done */
#define EHST            (0x0080)    /* host I/O done */
#define EFLS            (0x0200)    /* file system done */

#define ST_REJECT       (0xFF)
#define ST_WAIT         (0x80)
#define TIMEOUT         (0x240000)
#define SECTOR          (2048)
#define CHUNK           (32)        /* sectors fetched per Get Data */

static bool         ready;
static u32          root_lba, root_size;
static u16          res[4];

static void         irq_off(u32 *sr)
{
    u32             v;

    __asm__ volatile ("stc sr, %0" : "=r" (v));
    *sr = v;
    v |= 0xF0;
    __asm__ volatile ("ldc %0, sr" : : "r" (v));
}

static void         irq_restore(u32 sr)
{
    __asm__ volatile ("ldc %0, sr" : : "r" (sr));
}

static bool         wait_hirq(u16 flag)
{
    u32             i;

    for (i = 0; i < TIMEOUT; ++i)
        if (CD_HIRQ & flag)
            return true;
    return false;
}

/* run one command; `wait` is extra HIRQ flags to clear first (so we can wait
   for them afterwards). Returns false if it was refused or timed out. */
static bool         command(u16 cr1, u16 cr2, u16 cr3, u16 cr4, u16 wait)
{
    u32             sr, i;
    u8              st;

    irq_off(&sr);
    if (!(CD_HIRQ & CMOK))
    {
        irq_restore(sr);
        return false;
    }
    CD_HIRQ = (u16)~(CMOK | wait);
    CD_CR1 = cr1;
    CD_CR2 = cr2;
    CD_CR3 = cr3;
    CD_CR4 = cr4;
    for (i = 0; i < TIMEOUT && !(CD_HIRQ & CMOK); ++i)
        ;
    res[0] = CD_CR1;
    res[1] = CD_CR2;
    res[2] = CD_CR3;
    res[3] = CD_CR4;
    irq_restore(sr);
    if (i >= TIMEOUT)
        return false;
    st = (u8)(res[0] >> 8);
    return st != ST_REJECT && !(st & ST_WAIT);
}

static bool         end_transfer(void)
{
    bool            ok = command(0x0600, 0, 0, 0, 0);

    CD_HIRQ = (u16)~DRDY;
    return ok;
}

bool                cd_init(void)
{
    u32             i;

    for (i = 0; i < TIMEOUT && !(CD_HIRQ & CMOK); ++i)
        ;
    command(0x7500, 0, 0, 0, EFLS);             /* abort any file operation */
    command(0x0400, 0x0002, 0, 0x0001, 0);      /* initialise, standby time */
    end_transfer();
    command(0x4800 | 0xFC, 0, 0, 0, ESEL);      /* reset every selector */
    wait_hirq(ESEL);
    command(0x6000, 0, 0, 0, ESEL);             /* sectors are 2048 bytes (mode 1) */
    ready = true;
    return true;
}

static int          sectors_waiting(void)
{
    if (!command(0x5100, 0, 0, 0, 0))           /* Get Sector Number, partition 0 */
        return 0;
    return res[3];
}

/* read `count` sectors from `lba` into dst (2-byte aligned) */
bool                cd_read_sectors(u32 lba, u32 count, void *dst)
{
    u16             *out = (u16 *)dst;
    u32             fad = lba + 150, done = 0, i, n;

    if (!ready || count == 0)
        return false;
    command(0x4800, 0, 0, 0, ESEL);             /* reset selector 0 */
    wait_hirq(ESEL);
    command(0x3000, 0, 0 << 8, 0, ESEL);        /* drive -> filter 0 -> partition 0 */
    CD_HIRQ = (u16)~(PEND | CSCT);
    if (!command((u16)(0x1080 | (fad >> 16)), (u16)fad, (u16)(0x80 | (count >> 16)), (u16)count, 0))
        return false;                           /* "play" the range, FAD addressing */
    while (done < count)
    {
        int waiting = 0;

        for (i = 0; i < TIMEOUT && (waiting = sectors_waiting()) == 0; ++i)
            ;
        if (waiting <= 0)
            return false;
        n = (u32)waiting;
        if (n > CHUNK)
            n = CHUNK;
        if (n > count - done)
            n = count - done;
        if (!command(0x6300, 0, 0 << 8, (u16)n, EHST))  /* Get then Delete Sector Data */
            return false;
        if (!wait_hirq(DRDY))
            return false;
        for (i = 0; i < n * SECTOR / 2; ++i)
            *out++ = CD_DTR;
        end_transfer();
        done += n;
    }
    return true;
}

/* ---- reading in the background: start, then collect a few sectors a frame ---- */

static struct
{
    u16             *out;
    u32             count, done;
    bool            busy;
}                   rd;

bool                cd_async_start(u32 lba, u32 count, void *dst)
{
    u32             fad = lba + 150;

    if (!ready || count == 0 || rd.busy)
        return false;
    command(0x4800, 0, 0, 0, ESEL);             /* reset selector 0 */
    wait_hirq(ESEL);
    command(0x3000, 0, 0 << 8, 0, ESEL);        /* drive -> filter 0 -> partition 0 */
    CD_HIRQ = (u16)~(PEND | CSCT);
    if (!command((u16)(0x1080 | (fad >> 16)), (u16)fad, (u16)(0x80 | (count >> 16)), (u16)count, 0))
        return false;
    rd.out = (u16 *)dst;
    rd.count = count;
    rd.done = 0;
    rd.busy = true;
    return true;
}

/* 1 finished, 0 still going, -1 failed; moves at most max_sectors */
int                 cd_async_poll(int max_sectors)
{
    int             waiting;
    u32             n, i;
    u16             *out;

    if (!rd.busy)
        return 1;
    waiting = sectors_waiting();
    if (waiting <= 0)
        return 0;                               /* the drive's still seeking or reading */
    n = (u32)waiting;
    if (n > (u32)max_sectors)
        n = (u32)max_sectors;
    if (n > rd.count - rd.done)
        n = rd.count - rd.done;
    if (!command(0x6300, 0, 0 << 8, (u16)n, EHST) || !wait_hirq(DRDY))
    {
        rd.busy = false;
        return -1;
    }
    out = rd.out;
    for (i = 0; i < n * SECTOR / 2; ++i)
        *out++ = CD_DTR;
    rd.out = out;
    end_transfer();
    rd.done += n;
    if (rd.done < rd.count)
        return 0;
    rd.busy = false;
    return 1;
}

bool                cd_async_busy(void)
{
    return rd.busy;
}

/* ---- ISO 9660, root directory ---- */

static u8           sector_buf[SECTOR] __attribute__((aligned(4)));

static u32          le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static bool         load_root(void)
{
    if (root_lba)
        return true;
    if (!cd_read_sectors(16, 1, sector_buf) || sector_buf[0] != 1 || sector_buf[1] != 'C')
        return false;                           /* the primary volume descriptor */
    root_lba = le32(sector_buf + 156 + 2);      /* its root directory record */
    root_size = le32(sector_buf + 156 + 10);
    return root_lba != 0;
}

/* case-insensitive match of "NAME.EXT" against an ISO name ("NAME.EXT;1") */
static bool         name_match(const u8 *iso, int len, const char *name)
{
    int             i;

    for (i = 0; i < len && name[i]; ++i)
    {
        char a = (char)iso[i], b = name[i];

        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b)
            return false;
    }
    return !name[i] && (i == len || iso[i] == ';');
}

bool                cd_find(const char *name, u32 *lba, u32 *size)
{
    u32             s, off;

    if (!load_root())
        return false;
    for (s = 0; s < (root_size + SECTOR - 1) / SECTOR; ++s)
    {
        if (!cd_read_sectors(root_lba + s, 1, sector_buf))
            return false;
        for (off = 0; off < SECTOR && sector_buf[off]; off += sector_buf[off])
        {
            const u8 *rec = sector_buf + off;

            if (name_match(rec + 33, rec[32], name))
            {
                *lba = le32(rec + 2);
                *size = le32(rec + 10);
                return true;
            }
        }
    }
    return false;
}

/* load a whole file; returns its size, or -1 (missing, too big, read error) */
int                 cd_load(const char *name, void *dst, u32 max)
{
    u32             lba, size;

    /* whole sectors are written, so the buffer must hold the last one in full */
    if (!cd_find(name, &lba, &size) || ((size + SECTOR - 1) & ~(u32)(SECTOR - 1)) > max)
        return -1;
    if (!cd_read_sectors(lba, (size + SECTOR - 1) / SECTOR, dst))
        return -1;
    return (int)size;
}
