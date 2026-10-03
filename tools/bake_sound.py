#!/usr/bin/env python3
"""Quake 2's sound effects into a sound bank for the SCSP (engine/m68k/driver.c),
one a level: what every level has, and the sounds of the monsters in it.

    bake_sound.py data/pak0.pak cd obj/gen/sound_ids.h      (cd/DEMO1.SND ...)

The ids are the same in every level's bank (a sound a level hasn't got is a
moment's silence there); src/sound.c loads a level's as the level loads.
The wavs are 22 kHz 16-bit mono; here they're 11 kHz 8-bit (a quarter the
size: the bank has 480 KB of sound RAM), each cut to at most its `secs`.
The bank's layout is the taxi game's (tools/gen_sound.py there):

    "SND1" u32 meta_size
    u16 n_inst  u16 n_songs  u16 n_sfx  u16 base/256
    u32 inst_off  u32 song_off  u32 sfx_off  u32 pitch_off
    pitch table:  u16[128] (unused here: no songs)
    instruments:  20 bytes each
        u32 sa (the byte address in sound RAM)  u16 lsa  u16 lea (samples)
        u8 flags (1 loop, 2 fixed pitch, 4 8-bit samples)  u8 ar d1r dl d2r rr  u8 tl  u8 pad
        u16 pitch (the OCT/FNS to play at)  u16 pad
    songs:  none (an empty table)
    sfx:    4 bytes each: u8 inst, u8 0, u8 vol, u8 pan
    samples (8-bit signed PCM)

The header of ids (SND_<NAME>) is for src/sound.c.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from bake_map import Pak                    # noqa: E402
from q2data import Bsp                      # noqa: E402

MAPS = ["demo1", "demo2", "demo3"]
# the monsters' sounds (by the name's start): only in the banks of levels with those monsters
MONSTER_SOUNDS = {
    "sol_": ("monster_soldier_light", "monster_soldier", "monster_soldier_ss"),
    "inf_": ("monster_infantry",),
    "gun_": ("monster_gunner",),
    "ber_": ("monster_berserk",),
    "tnk_": ("monster_tank",),
    "fly_": ("monster_flyer",),
    "par_": ("monster_parasite",),
}
# monsters a level has that are left out anyway (none now: the cart has room, OVERNIGHT.md 50)
LEFT_OUT = {}

RATE = 11025
BASE = 0x2000                               # engine/snd68k.h SND_BANK_BASE
TOP = 0x7A000                               # SND_DSP_RING: the reverb's delay lines

# name, the file under sound/, the most seconds kept, its volume (0-127)
SOUNDS = [
    ("blaster", "weapons/BLASTF1A", 0.6, 110),
    ("shotgun", "weapons/Shotgf1b", 0.8, 120),
    ("sshotgun", "weapons/Sshotf1b", 0.7, 127),
    ("machinegun", "weapons/Machgf1b", 0.35, 110),
    ("chaingun", "weapons/Machgf2b", 0.35, 110),
    ("grenade_fire", "weapons/GRENLF1A", 0.5, 110),
    ("rocket_fire", "weapons/ROCKLF1A", 0.6, 120),
    ("explosion", "weapons/ROCKLX1A", 0.7, 127),
    ("grenade_explode", "weapons/GRENLX1A", 0.7, 127),
    ("grenade_bounce", "weapons/HGRENB1A", 0.4, 90),
    ("blaster_hit", "weapons/lashit", 0.3, 90),
    ("noammo", "weapons/noammo", 0.2, 100),
    ("ricochet1", "world/ric1", 0.3, 70),
    ("ricochet2", "world/ric2", 0.3, 70),
    ("ricochet3", "world/ric3", 0.3, 70),
    ("gib", "misc/udeath", 0.5, 110),
    ("sol_sight", "soldier/SOLSGHT1", 0.8, 110),
    ("sol_pain1", "soldier/SOLPAIN1", 0.6, 110),
    ("sol_pain2", "soldier/SOLPAIN2", 0.6, 110),
    ("sol_death1", "soldier/SOLDETH1", 1.1, 115),
    ("sol_death2", "soldier/SOLDETH2", 1.0, 115),
    ("sol_shotgun", "soldier/Solatck1", 0.6, 110),
    ("sol_blaster", "soldier/Solatck2", 0.5, 110),
    ("sol_machinegun", "soldier/Solatck3", 0.3, 110),
    ("sol_idle", "soldier/SOLIDLE1", 0.7, 80),
    ("inf_sight", "infantry/INFSGHT1", 0.8, 110),
    ("inf_pain1", "infantry/INFPAIN1", 0.7, 110),
    ("inf_pain2", "infantry/Infpain2", 0.7, 110),
    ("inf_death1", "infantry/INFDETH1", 1.1, 115),
    ("inf_machinegun", "infantry/Infatck1", 0.3, 110),
    ("inf_melee", "infantry/melee2", 0.5, 110),
    ("gun_sight", "gunner/sight1", 0.7, 110),
    ("gun_pain1", "gunner/gunpain2", 0.5, 110),
    ("gun_pain2", "gunner/gunpain1", 0.5, 110),
    ("gun_death", "gunner/death1", 1.0, 115),
    ("gun_open", "gunner/gunatck1", 0.3, 100),
    ("gun_fire", "gunner/gunatck2", 0.1, 110),
    ("gun_grenade", "gunner/gunatck3", 0.4, 110),
    ("ber_sight", "berserk/sight", 0.8, 110),
    ("ber_pain", "berserk/berpain2", 0.6, 110),
    ("ber_death", "berserk/berdeth2", 1.0, 115),
    ("ber_punch", "berserk/attack", 0.5, 110),
    ("tnk_sight", "tank/sight1", 0.8, 110),
    ("tnk_pain", "tank/tnkpain2", 0.5, 110),
    ("tnk_death", "tank/death", 0.9, 115),
    ("tnk_step", "tank/step", 0.25, 90),
    ("tnk_blaster", "tank/tnkatck3", 0.4, 110),
    ("tnk_mg", "tank/tnkatk2a", 0.15, 110),
    ("tnk_rocket", "tank/tnkatck1", 0.5, 115),
    ("fly_sight", "flyer/Flysght1", 0.7, 110),
    ("fly_pain1", "flyer/Flypain1", 0.6, 110),
    ("fly_pain2", "flyer/Flypain2", 0.6, 110),
    ("fly_death", "flyer/Flydeth1", 1.0, 115),
    ("fly_blades", "flyer/Flyatck1", 0.5, 100),
    ("fly_slash", "flyer/Flyatck2", 0.5, 110),
    ("fly_blaster", "flyer/Flyatck3", 0.35, 100),
    ("par_sight", "parasite/Parsght1", 0.8, 110),
    ("par_pain1", "parasite/Parpain1", 0.7, 110),
    ("par_pain2", "parasite/Parpain2", 0.7, 110),
    ("par_death", "parasite/Pardeth1", 1.0, 115),
    ("par_launch", "parasite/Paratck1", 0.6, 110),
    ("par_impact", "parasite/Paratck2", 0.4, 110),
    ("par_suck", "parasite/Paratck3", 0.8, 100),
    ("par_reelin", "parasite/Paratck4", 0.5, 110),
    ("par_tap", "parasite/Paridle1", 0.25, 70),
    ("player_pain25", "player/male/pain25_1", 0.6, 120),
    ("player_pain50", "player/male/pain50_1", 0.7, 120),
    ("player_pain75", "player/male/pain75_1", 0.6, 120),
    ("player_pain100", "player/male/pain100_1", 0.6, 120),
    ("player_death", "player/male/death1", 1.0, 127),
    ("player_jump", "player/male/jump1", 0.5, 100),
    ("player_fall", "player/male/fall2", 0.5, 110),
    ("player_land", "player/land1", 0.3, 90),
    ("step1", "player/step1", 0.25, 60),
    ("step2", "player/step2", 0.25, 60),
    ("step3", "player/step3", 0.25, 60),
    ("step4", "player/step4", 0.25, 60),
    ("water_in", "player/watr_in", 0.4, 100),
    ("water_out", "player/watr_out", 0.4, 100),
    ("pickup", "items/pkup", 0.4, 110),
    ("health_small", "items/s_health", 0.6, 110),
    ("health", "items/n_health", 0.5, 110),
    ("health_large", "items/l_health", 0.5, 110),
    ("health_mega", "items/m_health", 0.5, 110),
    ("quad", "items/damage", 0.6, 120),
    ("ammo", "misc/am_pkup", 0.4, 110),
    ("armour_shard", "misc/ar2_pkup", 0.5, 110),
    ("armour", "misc/ar1_pkup", 0.5, 110),
    ("weapon", "misc/w_pkup", 0.4, 110),
    ("secret", "misc/secret", 0.6, 110),
    ("talk", "misc/talk", 0.5, 110),
    ("door_start", "doors/dr1_strt", 0.5, 100),
    ("door_end", "doors/dr1_end", 0.5, 100),
    ("plat_start", "plats/pt1_strt", 0.6, 100),
    ("plat_end", "plats/pt1_end", 0.6, 100),
    ("button", "switches/butn2", 0.4, 100),
    ("menu_move", "misc/menu1", 0.3, 100),          # (whole: cut, they ended in a click)
    ("menu_select", "misc/menu2", 0.5, 100),
    ("menu_back", "misc/menu3", 0.41, 100),
]


def wav_pcm(data):
    """the samples of a RIFF WAV as floats -1..1, and its rate"""
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    i, fmt, pcm = 12, None, None
    while i + 8 <= len(data):
        cid, n = data[i:i + 4], struct.unpack("<I", data[i + 4:i + 8])[0]
        body = data[i + 8:i + 8 + n]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            pcm = body
        i += 8 + n + (n & 1)
    _, ch, rate, _, _, bits = fmt
    if bits == 16:
        v = [s / 32768.0 for s in struct.unpack("<%dh" % (len(pcm) // 2), pcm[:len(pcm) // 2 * 2])]
    else:
        v = [(b - 128) / 128.0 for b in pcm]
    return v[::ch], rate


def to_11k8(v, rate, secs):
    """to RATE, 8-bit signed: pairs averaged (a little low-pass on the way)"""
    if rate == 2 * RATE:
        v = [(v[i] + v[i + 1]) * 0.5 for i in range(0, len(v) - 1, 2)]
    cut = len(v) > int(secs * RATE)
    v = v[:int(secs * RATE)]
    # a fade at the end: 40 ms if it was cut (a sound stopped still loud clicks), else 6
    n = min(len(v), int(0.04 * RATE) if cut else 64)
    for k in range(n):
        v[len(v) - n + k] *= (n - k) / n
    return bytes((max(-127, min(127, int(round(s * 127)))) & 0xFF) for s in v)


def pitch_reg(freq):
    """SCSP OCT/FNS to play samples at freq Hz (the SCSP's own rate is 44.1 kHz)"""
    import math
    ratio = freq / 44100.0
    octv = math.floor(math.log2(ratio))
    fns = round((ratio / 2 ** octv - 1) * 1024)
    if fns >= 1024:
        octv, fns = octv + 1, 0
    return ((octv & 0xF) << 11) | max(0, min(1023, fns))


def bank(samples, have):
    """a bank: every id, those the level hasn't got a moment's silence"""
    n = len(SOUNDS)
    hdr_len = 4 + 4 + 8 + 16
    pitch_off = hdr_len
    inst_off = pitch_off + 256
    song_off = inst_off + 20 * (n + 1)
    sfx_off = song_off + 4
    meta_size = sfx_off + 4 * n
    meta_size += -meta_size % 16
    head = struct.pack(">4sI", b"SND1", meta_size)
    head += struct.pack(">HHHH", n + 1, 0, n, BASE >> 8)
    head += struct.pack(">IIII", inst_off, song_off, sfx_off, pitch_off)
    insts = b""
    data = b""
    pitch = pitch_reg(RATE)
    for pcm in [samples[i] if have[i] else b"" for i in range(n)] + [b"\0" * 4]:
        if not pcm:
            insts += b"\0" * 20                # (not used: its sfx is the silence, the last)
            continue
        sa = BASE + meta_size + len(data)
        # one-shots at their own pitch: attack at once, no decay, a quick release
        insts += struct.pack(">IHHBBBBBBBBHH", sa, 0, len(pcm) - 1, 2 | 4, 31, 0, 0, 0, 20, 0, 0, pitch, 0)
        data += pcm + b"\0" * (-len(pcm) % 4)
    meta = head + b"\0" * 256 + insts + b"\0" * 4
    meta += b"".join(struct.pack(">BBBB", i if have[i] else n, 0, SOUNDS[i][3], 0) for i in range(n))
    meta += b"\0" * (meta_size - len(meta))
    return meta + data, len(data)


def main():
    pak = Pak(sys.argv[1])
    samples = []
    for name, path, secs, vol in SOUNDS:
        v, rate = wav_pcm(pak.read("sound/%s.wav" % path))
        samples.append(to_11k8(v, rate, secs))
    for m in MAPS:
        classes = {e.get("classname") for e in Bsp(pak.read("maps/%s.bsp" % m)).parse_entities()}
        classes -= set(LEFT_OUT.get(m, ()))
        have = []
        for name, _, _, _ in SOUNDS:
            tag = next((t for t in MONSTER_SOUNDS if name.startswith(t)), None)
            have.append(tag is None or any(c in classes for c in MONSTER_SOUNDS[tag]))
        blob, ndata = bank(samples, have)
        if BASE + len(blob) > TOP:
            sys.exit("bake_sound: %s: %d bytes, more than sound RAM's %d" % (m, len(blob), TOP - BASE))
        with open(os.path.join(sys.argv[2], m.upper() + ".SND"), "wb") as f:
            f.write(blob)
        print("%s.SND: %d effects, %d KB of samples (%d KB free)" % (m.upper(), sum(have), ndata // 1024,
                                                                    (TOP - BASE - len(blob)) // 1024))
    with open(sys.argv[3], "w") as f:
        f.write("/* generated by tools/bake_sound.py */\n")
        for i, (name, _, _, _) in enumerate(SOUNDS):
            f.write("#define SND_%s (%d)\n" % (name.upper(), i))
        f.write("#define SND_COUNT (%d)\n" % len(SOUNDS))


if __name__ == "__main__":
    main()
