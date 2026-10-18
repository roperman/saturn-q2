/*
** The Makron (out of Jorg on boss2's map: m_jorg.c; and standing on boss1's
** as monster_boss3_stand, gone in a flash of light when triggered): Quake 2's
** game/m_boss32.c and m_boss3.c, with the animations baked into MAKRON.MDL
** (Jorg's eight first, for the ride: m_jorg.c; then sight, run2, attack3-5,
** pain4-6, death2, stand2; its mesh collapsed to the renderer's limits,
** tools/models.txt "mesh="). 3000 health. Three attacks, each as likely: a
** BFG ball (50 of damage, 300 units about), seventeen hyperblaster bolts
** swept across (15 each), or the railgun (50, at where you stood as it
** wound up). A light hit mostly doesn't stop him. Dying he goes down over
** 95 frames and his body stays. Left out: his torso flying off (a second
** model), his walk along a path, his jump, his laugh and his roars; the
** railgun's beam is the parasite's green one.
*/
#include "game.h"

static void         makron_run(g_ent *self);
static void         makron_bfg(g_ent *self);
static void         makron_hyperblaster(g_ent *self);
static void         makron_railgun(g_ent *self);
static void         makron_saveloc(g_ent *self);
static void         makron_prerailgun(g_ent *self);
static void         makron_step_left(g_ent *self);
static void         makron_step_right(g_ent *self);
static void         makron_popup(g_ent *self);
static void         makron_taunt(g_ent *self);
static void         makron_hit(g_ent *self);
static void         makron_brainsplorch(g_ent *self);
static void         makron_dead(g_ent *self);
static void         boss3_idle(g_ent *self, s32 dist);

#define S0              { ai_stand, 0, NULL }
#define M0              { ai_move, 0, NULL }
#define MD(d, f)        { ai_move, d, f }
#define C0              { ai_charge, 0, NULL }
#define H1              { ai_move, 0, makron_hyperblaster }
static const mframe_t makron_frames_stand[] = {
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
};
static mmove_t      makron_move_stand = { "stand2", 0, 59, makron_frames_stand, NULL, 0, 0, 0, 0, 0, 0 };

static const mframe_t makron_frames_run[] = {
    { ai_run, 3, makron_step_left }, { ai_run, 12, NULL }, { ai_run, 8, NULL }, { ai_run, 8, NULL },
    { ai_run, 8, makron_step_right }, { ai_run, 6, NULL }, { ai_run, 12, NULL }, { ai_run, 9, NULL },
    { ai_run, 6, NULL }, { ai_run, 12, NULL },
};
static mmove_t      makron_move_run = { "run2", 0, 9, makron_frames_run, NULL, 0, 0, 0, 0, 0, 0 };

/* "sight" is active01-active13: up out of the wreck */
static const mframe_t makron_frames_sight[] = { M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0 };
static mmove_t      makron_move_sight = { "sight", 0, 12, makron_frames_sight, makron_run, 0, 0, 0, 0, 0, 0 };

/* "attack3" is attak301-attak308: the BFG at the 5th */
static const mframe_t makron_frames_attack3[] = { C0, C0, C0, C0, { ai_charge, 0, makron_bfg }, M0, M0, M0 };
static mmove_t      makron_move_attack3 = { "attack3", 0, 7, makron_frames_attack3, makron_run, 0, 0, 0, 0, 0, 0 };
/* "attack4" is attak401-attak426: a bolt a frame from the 5th to the 21st, swept across */
static const mframe_t makron_frames_attack4[] = {
    M0, M0, M0, M0, H1, H1, H1, H1, H1, H1, H1, H1, H1, H1, H1, H1, H1, H1, H1, H1, H1, M0, M0, M0, M0, M0,
};
static mmove_t      makron_move_attack4 = { "attack4", 0, 25, makron_frames_attack4, makron_run, 0, 0, 0, 0, 0, 0 };
/* "attack5" is attak501-attak516: the railgun wound up, your spot taken on the 7th, the shot on the 8th */
static const mframe_t makron_frames_attack5[] = {
    { ai_charge, 0, makron_prerailgun }, C0, C0, C0, C0, C0, { ai_charge, 0, makron_saveloc },
    { ai_move, 0, makron_railgun }, M0, M0, M0, M0, M0, M0, M0, M0,
};
static mmove_t      makron_move_attack5 = { "attack5", 0, 15, makron_frames_attack5, makron_run, 0, 0, 0, 0, 0, 0 };

static const mframe_t makron_frames_pain_short[] = { M0, M0, M0, M0 };
static mmove_t      makron_move_pain4 = { "pain4", 0, 3, makron_frames_pain_short, makron_run, 0, 0, 0, 0, 0, 0 };
static mmove_t      makron_move_pain5 = { "pain5", 0, 3, makron_frames_pain_short, makron_run, 0, 0, 0, 0, 0, 0 };
static const mframe_t makron_frames_pain6[] = {
    { ai_move, 0, makron_popup }, { ai_move, 0, makron_taunt }, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
    M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
};
static mmove_t      makron_move_pain6 = { "pain6", 0, 26, makron_frames_pain6, makron_run, 0, 0, 0, 0, 0, 0 };

/* "death2" is death201-death295: down, staggering, the final blow and the brains out */
static const mframe_t makron_frames_death2[] = {
    M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
    M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
    M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
    MD(-15, NULL), MD(3, NULL), MD(-12, NULL), MD(0, makron_step_left), MD(11, NULL), MD(12, NULL),
    MD(11, makron_step_right), MD(5, NULL), MD(7, NULL), MD(6, makron_step_left), MD(-1, NULL), MD(2, NULL),
    MD(-6, NULL), MD(-4, NULL), MD(-6, makron_step_right), MD(-4, NULL), MD(-4, makron_step_left), MD(-2, NULL),
    MD(-5, NULL), MD(-3, makron_step_right), MD(-8, NULL), MD(-3, makron_step_left), MD(-7, NULL), MD(-4, NULL),
    MD(-4, makron_step_right), MD(-6, NULL), MD(-7, NULL), MD(0, makron_step_left), MD(-2, NULL), MD(2, NULL),
    MD(27, makron_hit), MD(26, NULL), MD(0, makron_brainsplorch), M0,
};
static mmove_t      makron_move_death2 = { "death2", 0, 94, makron_frames_death2, makron_dead, 0, 0, 0, 0, 0, 0 };

/* (monster_boss3_stand: cycling in place, nothing more) */
static const mframe_t boss3_frames_stand[] = {
    { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL },
    { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL },
    { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL },
    { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL },
    { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL },
    { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL },
    { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL },
    { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL },
    { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL },
    { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL },
    { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL },
    { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL },
    { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL },
    { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL },
    { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL }, { boss3_idle, 0, NULL },
};
static mmove_t      boss3_move_stand = { "stand2", 0, 59, boss3_frames_stand, NULL, 0, 0, 0, 0, 0, 0 };

static mmove_t      *all_moves[] = {
    &makron_move_stand, &makron_move_run, &makron_move_sight, &makron_move_attack3, &makron_move_attack4,
    &makron_move_attack5, &makron_move_pain4, &makron_move_pain5, &makron_move_pain6, &makron_move_death2,
    &boss3_move_stand,
};

static void         project(const g_ent *self, const s32 *ofs, s32 *start)
{
    s32             fx = fcos(self->yaw), fy = fsin(self->yaw);

    start[0] = self->origin[0] + fmul(fx, ofs[0]) + fmul(fy, ofs[1]);
    start[1] = self->origin[1] + fmul(fy, ofs[0]) - fmul(fx, ofs[1]);
    start[2] = self->origin[2] + ofs[2];
}

/* the way from start to a spot, unit length */
static bool         aim_to(const s32 *start, const s32 *to, s32 *aim)
{
    s32             len;
    int             k;

    for (k = 0; k < 3; ++k)
        aim[k] = to[k] - start[k];
    len = vlen(aim);
    if (len < FIX(1))
        return false;
    for (k = 0; k < 3; ++k)
        aim[k] = fdiv(aim[k], len);
    return true;
}

static void         makron_step_left(g_ent *self)
{
    s_play(SND_MK_STEP1, self->origin, ATTN_NORM);
}

static void         makron_step_right(g_ent *self)
{
    s_play(SND_MK_STEP2, self->origin, ATTN_NORM);
}

static void         makron_popup(g_ent *self)
{
    s_play(SND_MK_POPUP, self->origin, ATTN_NONE);
}

static void         makron_hit(g_ent *self)
{
    s_play(SND_MK_HIT, self->origin, ATTN_NONE);
}

static void         makron_brainsplorch(g_ent *self)
{
    s_play(SND_MK_BRAIN, self->origin, ATTN_NORM);
}

static void         makron_prerailgun(g_ent *self)
{
    s_play(SND_MK_RAIL_UP, self->origin, ATTN_NORM);
}

/* (Quake's: heard over the whole level) */
static void         makron_taunt(g_ent *self)
{
    s32             r = frandom();

    s_play(r <= FIX(0.3) ? SND_MK_VOICE4 : r <= FIX(0.6) ? SND_MK_VOICE3 : SND_MK_VOICE, self->origin, ATTN_NONE);
}

static void         makron_stand(g_ent *self)
{
    self->move = &makron_move_stand;
}

static void         makron_run(g_ent *self)
{
    self->move = self->aiflags & AI_STAND_GROUND ? &makron_move_stand : &makron_move_run;
}

static void         makron_sight(g_ent *self)
{
    self->move = &makron_move_sight;
}

/* a BFG ball (Quake's MZ2_MAKRON_BFG): 50 of damage, 300 units about */
static void         makron_bfg(g_ent *self)
{
    static const s32 ofs[3] = { FIX(17), FIX(-19.5), FIX(62.9) };
    s32             start[3], to[3], aim[3];
    int             k;

    if (!self->enemy)
        return;
    project(self, ofs, start);
    for (k = 0; k < 3; ++k)
        to[k] = self->enemy->origin[k];
    to[2] += self->enemy->viewheight;
    if (!aim_to(start, to, aim))
        return;
    s_play(SND_MK_BFG_FIRE, self->origin, ATTN_NORM);
    fx_bfg(self, start, aim, 50, 300);
    g_muzzle_flash(start, 4, 13, 4);
}

/* your spot as the railgun winds up: the shot goes there (Quake's MakronSaveloc, pos1) */
static void         makron_saveloc(g_ent *self)
{
    int             k;

    if (!self->enemy)
        return;
    for (k = 0; k < 3; ++k)
        self->goal_pos[k] = self->enemy->origin[k];
    self->goal_pos[2] += self->enemy->viewheight;
}

static void         makron_railgun(g_ent *self)
{
    static const s32 ofs[3] = { FIX(-17.3), FIX(7.8), FIX(72.4) };
    s32             start[3], aim[3];

    project(self, ofs, start);
    if (!aim_to(start, self->goal_pos, aim))
        return;
    g_fire_rail(self, start, aim, 50);
    g_muzzle_flash(start, 6, 8, 14);
    s_play(SND_MK_RAIL, start, ATTN_NORM);
}

/* a bolt a frame (Quake's MZ2_MAKRON_BLASTER_1: the first gun's spot for all), 15 of damage, its
   yaw swept from 80 degrees one side to straight ahead, then from 70 the other to straight ahead,
   its pitch at you */
static void         makron_hyperblaster(g_ent *self)
{
    static const s32 ofs[3] = { FIX(-3.6), FIX(-24.1), FIX(59.5) };
    s32             start[3], aim[3], v[3], h[3];
    int             i = self->frame - makron_move_attack4.first, yaw, pitch = 0, cp, k;

    project(self, ofs, start);
    if (self->enemy)
    {
        for (k = 0; k < 3; ++k)
            v[k] = self->enemy->origin[k] - start[k];
        v[2] += self->enemy->viewheight;
        h[0] = v[0];
        h[1] = v[1];
        h[2] = 0;
        pitch = fatan2(v[2], vlen(h)) & 0xFFFF;
    }
    yaw = i <= 12 ? self->yaw + 1820 * (12 - i) : self->yaw - 1820 * (20 - i);    /* (10 degrees: 0x10000 / 36) */
    yaw &= 0xFFFF;
    cp = fcos(pitch);
    aim[0] = fmul(cp, fcos(yaw));
    aim[1] = fmul(cp, fsin(yaw));
    aim[2] = fsin(pitch);
    g_fire_blaster(self, start, aim, 15, FIX(1000));
    g_muzzle_flash(start, 12, 10, 3);
    s_play(SND_MK_BLASTER, start, ATTN_NORM);
}

static void         makron_attack(g_ent *self)
{
    s32             r = frandom();

    self->move = r <= FIX(0.3) ? &makron_move_attack3 : r <= FIX(0.6) ? &makron_move_attack4 : &makron_move_attack5;
}

static void         makron_pain(g_ent *self, g_ent *other, int damage)
{
    if (self->health < self->max_health / 2)
        self->skinnum = 1;
    if (level.time < self->pain_debounce)
        return;
    if (damage <= 25 && frandom() < FIX(0.2))
        return;                             /* (Quake's: a light hit seldom stops him) */
    self->pain_debounce = level.time + FIX(3);
    if (damage <= 40)
    {
        s_play(SND_MK_PAIN3, self->origin, ATTN_NONE);
        self->move = &makron_move_pain4;
    }
    else if (damage <= 110)
    {
        s_play(SND_MK_PAIN2, self->origin, ATTN_NONE);
        self->move = &makron_move_pain5;
    }
    else if (frandom() <= (damage <= 150 ? FIX(0.45) : FIX(0.35)))
    {
        s_play(SND_MK_PAIN1, self->origin, ATTN_NONE);
        self->move = &makron_move_pain6;
    }
}

static void         makron_dead(g_ent *self)
{
    self->mins[0] = self->mins[1] = -FIX(60);
    self->maxs[0] = self->maxs[1] = FIX(60);
    self->maxs[2] = FIX(72);
    self->move = NULL;                      /* (a body: it stays) */
}

static void         makron_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    self->dead = true;
    s_play(SND_MK_DEATH, self->origin, ATTN_NONE);
    self->aiflags &= ~AI_HOLD_FRAME;
    self->move = &makron_move_death2;
}

/* ---- monster_boss3_stand: the Makron standing, until he's triggered away ---- */

static void         boss3_idle(g_ent *self, s32 dist)
{
}

static void         boss3_stand(g_ent *self)
{
    self->move = &boss3_move_stand;
}

/* (Quake's TE_BOSSTPORT: a flash where he stood, and he's gone) */
static void         boss3_use(g_ent *self, g_ent *other, g_ent *activator)
{
    s32             p[3];

    p[0] = self->origin[0];
    p[1] = self->origin[1];
    p[2] = self->origin[2] + FIX(45);
    s_play(SND_MK_TELE, p, ATTN_NONE);
    fx_flash(p, FIX(400), FIX(1), 10, 12, 16);
    self->solid = false;
    self->move = NULL;
    self->kind = EK_FREE;                   /* (its render entity goes: g_render_ents) */
}

void                SP_monster_makron(g_ent *self)
{
    M_ResolveMoves(self, all_moves, sizeof(all_moves) / sizeof(all_moves[0]));
    if (self->cls == C_MONSTER_BOSS3_STAND)
    {
        self->mins[0] = self->mins[1] = -FIX(32);
        self->mins[2] = 0;
        self->maxs[0] = self->maxs[1] = FIX(32);
        self->maxs[2] = FIX(90);
        self->skinnum = 0;
        self->health = 0;                   /* (not to be shot: g_monster_spawn's takedamage goes by it) */
        self->stand = boss3_stand;
        self->run = boss3_stand;
        self->use = boss3_use;
        monster_start(self);
        self->move = &boss3_move_stand;
        return;
    }
    self->mins[0] = self->mins[1] = -FIX(30);
    self->mins[2] = 0;
    self->maxs[0] = self->maxs[1] = FIX(30);
    self->maxs[2] = FIX(90);
    self->skinnum = 0;
    self->health = 3000;
    self->gib_health = -2000;
    self->stand = makron_stand;
    self->run = makron_run;
    self->attack = makron_attack;
    self->pain = makron_pain;
    self->die = makron_die;
    self->sight = makron_sight;
    monster_start(self);
    self->move = &makron_move_sight;        /* (up out of the wreck first, as Quake's) */
}

#ifdef OVERLAY
/* (loaded per level: build.sh build_overlays) the loader finds its spawn here, first */
__attribute__((section(".ovlhead"), used)) void (*const ovl_spawn)(g_ent *self) = SP_monster_makron;
#endif
