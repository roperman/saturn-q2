/*
** The game: Quake 2's game DLL (vendor/quake2/game), cut down and in fixed
** point. It runs at Quake's 10 frames a second (FRAMETIME); the renderer
** draws in between, blending positions, angles and animation frames.
**
** Angles are 16-bit here (0x10000 a turn) where Quake uses degrees.
*/
#ifndef GAME_H
#define GAME_H

#include "q2.h"
#include "q2classes.h"

/* an entity from the map (tools/bake_map.py entities()): UNSET where the map didn't say */
typedef struct
{
    u16             cls, model, targetname, target, killtarget, message;
    s32             origin[3];
    u16             angle, spawnflags;      /* angle 0xFFFF up, 0xFFFE down (movers) */
    s32             delay, wait, random, speed;
    s16             dmg, health, count, style;
    u16             pathtarget, team, item, pad;
}                   q_erec;
#define UNSET           ((s32)0x80000000)

#define FRAMETIME       FIX(0.1)
#define MAX_FULL        (64)                /* full entities a level, at most (g_init: the rest go) */

typedef struct g_ent_s g_ent;

/* a frame of a move: what the AI does on it, how far it steps (units), and an extra */
typedef struct
{
    void            (*ai)(g_ent *self, s32 dist);
    s8              dist;
    void            (*think)(g_ent *self);
}                   mframe_t;

/* a move: model frames first..last (resolved from the model's anims), and what happens after */
typedef struct
{
    const char      *anim;
    int             from, to;               /* frames of that anim */
    const mframe_t  *frames;
    void            (*endfunc)(g_ent *self);
    int             first, last;            /* model frames, filled in at spawn */
}                   mmove_t;

/* aiflags */
#define AI_STAND_GROUND     (0x0001)
#define AI_TEMP_STAND_GROUND (0x0002)
#define AI_SOUND_TARGET     (0x0004)
#define AI_LOST_SIGHT       (0x0008)
#define AI_PURSUIT_LAST_SEEN (0x0010)
#define AI_PURSUE_NEXT      (0x0020)
#define AI_PURSUE_TEMP      (0x0040)
#define AI_HOLD_FRAME       (0x0080)

/* attack_state */
#define AS_STRAIGHT     (1)
#define AS_SLIDING      (2)
#define AS_MELEE        (3)
#define AS_MISSILE      (4)

#define RANGE_MELEE     (0)
#define RANGE_NEAR      (1)
#define RANGE_MID       (2)
#define RANGE_FAR       (3)

/* flags */
#define FL_PARTIALGROUND (1)
#define FL_STEPPED      (2)                 /* (this tick: its own step put it on the ground) */
#define FL_IDLE_ODD     (4)                 /* (standing out of sight: this tick's the one it skips) */
#define FL_FLY          (8)                 /* flies: no steps, no falling, leans up or down towards its goal */

#define ANG(d)          ((int)((d) * 65536 / 360))     /* degrees as the game's angles (a turn is 65536) */

/* kinds */
#define EK_FREE         (0)
#define EK_PLAYER       (1)
#define EK_MONSTER      (2)
#define EK_ITEM         (3)
#define EK_TRIGGER      (4)                 /* a brush volume the player touches */
#define EK_POINT        (5)                 /* relays, timers, targets: things that get used */
#define EK_OBJECT       (6)                 /* barrels and the like: a model, solid, can be shot */

#define MASK_MONSTERSOLID (CONTENTS_SOLID | 0x20000 | CONTENTS_WINDOW | CONTENTS_MONSTER)
#define CONTENTS_DEADMONSTER (0x4000000)    /* (a body: shots and blasts find it, nothing walks into it) */
#define MASK_SHOT       (CONTENTS_SOLID | CONTENTS_MONSTER | CONTENTS_WINDOW | CONTENTS_DEADMONSTER)
#define MASK_OPAQUE     (CONTENTS_SOLID | CONTENTS_SLIME | CONTENTS_LAVA)
#define MASK_SOLID_ONLY (CONTENTS_SOLID | CONTENTS_WINDOW)

/* the player's weapons and ammo */
enum { W_BLASTER, W_SHOTGUN, W_SSHOTGUN, W_MACHINEGUN, W_CHAINGUN, W_GLAUNCHER, W_RLAUNCHER, W_COUNT };
enum { AMMO_NONE, AMMO_SHELLS, AMMO_BULLETS, AMMO_GRENADES, AMMO_ROCKETS, AMMO_CELLS, AMMO_SLUGS, AMMO_COUNT };

typedef struct
{
    const char      *name;
    int             ammo, per_shot;
    s32             refire;
}                   g_weapon;

typedef struct
{
    int             weapon, newweapon;
    bool            have[W_COUNT];
    int             ammo[AMMO_COUNT];
    int             armor, armor_protect;   /* protection: the % of damage it takes */
    int             keys;
    s32             fire_time, quad_until, invul_until, pickup_flash;
}                   g_client;

extern g_client     client;
extern const g_weapon weapons[W_COUNT];

/* An entity. The short ones (items, triggers, targets: no AI, no box, never
   hurt) are only the part before old_origin (G_SHORT bytes: g_init lays them
   out so); the full ones (the player, monsters, barrels, func_explosives) have
   it all. What's looked at every tick or frame comes first, a 16-byte line at a
   time: the thinks' (kind, nextthink, think), then g_render_ents' (and a
   monster's blend, the first of the full ones') */
struct g_ent_s
{
    u8              kind;
    u8              inactive;               /* not there yet (waits to be triggered) */
    u8              dead, solid;
    u8              takedamage, pad;
    s16             leaf;                   /* monsters, barrels: the leaf it's in (g_render_ents' slots) */
    s32             nextthink;
    void            (*think)(g_ent *self);
    s32             origin[3];
    int             yaw;
    const q_mdl     *mdl;
    void            (*touch)(g_ent *self, g_ent *other);
    void            (*use)(g_ent *self, g_ent *other, g_ent *activator);
    g_ent           *activator;
    /* the map's keys, and what the entity does */
    int             cls, spawnflags;
    u16             targetname, target, killtarget;
    const char      *message;
    s32             delay, wait, random;
    int             count, dmg, model;      /* model: its brush model, if it has one */
    int             health;
    /* the full ones' (a trigger's box is in g_trig_areas, an item's 15 units round it) */
    s32             old_origin[3];          /* at the last tick (the renderer blends) */
    int             old_yaw;
    int             frame, old_frame, skinnum;      /* model frames */
    s32             mins[3], maxs[3];
    int             max_health, gib_health;
    s32             velocity[3];
    int             ideal_yaw, yaw_speed;   /* yaw_speed: a tick */
    s32             viewheight;
    bool            on_ground;
    int             flags;
    int             waterlevel;
    /* monsterinfo */
    const mmove_t   *move;
    int             nextframe;
    int             aiflags, attack_state, lefty;
    s32             pausetime, attack_finished, search_time, idle_time, pain_debounce, show_hostile;
    s32             last_sighting[3], saved_goal[3];
    g_ent           *enemy, *goalentity;
    s32             goal_pos[3];                /* when goalentity is &goal_marker */
    void            (*stand)(g_ent *self);
    void            (*run)(g_ent *self);
    void            (*attack)(g_ent *self);
    void            (*sight)(g_ent *self);
    void            (*pain)(g_ent *self, g_ent *other, int damage);
    void            (*die)(g_ent *self, g_ent *attacker, int damage, const s32 *point);
    s32             chase_fail;             /* the tick its whole search for a way failed (M_MoveToGoal) */
};
#define G_SHORT         (__builtin_offsetof(g_ent, old_origin))
_Static_assert(G_SHORT % 16 == 0, "g_ent: the short ones a whole number of lines");

/* The monsters think in groups (edict number % MON_GROUPS), each at 10 Hz
   but a share of a tick apart, so a fight's AI is spread over the frames
   rather than all in one every 100 ms. 8 (from 4) halved the fight's worst
   frame's game time (26.6 to 15.9 ms, OVERNIGHT.md 59); 16 was no better */
#ifndef MON_GROUPS
# define MON_GROUPS     (8)
#endif

typedef struct
{
    s32             time;                   /* game seconds (16.16) */
    int             framenum;
    s32             acc;                    /* time into the current tick */
    g_ent           *sight_client;
    s32             mon_time[MON_GROUPS];   /* the monsters' ticks, a group at a time (g_frame) */
    s32             mon_acc[MON_GROUPS];
}                   g_level;

extern g_level      level;

typedef struct { u32 at, n, us; } g_trace_site;
extern g_trace_site g_trace_sites[16];
extern g_ent        *g_edicts;                  /* the full ones, g_nfull (g_init): [0] the player */
extern u8           *g_shorts;                  /* the short ones, g_nshort of G_SHORT bytes: the items first */
extern int          g_nfull, g_nshort, g_nitems, g_ntrigs;  /* (g_nitems items, then g_ntrigs triggers) */
#define G_SHORT_ENT(j)  ((g_ent *)(g_shorts + (u32)(j) * G_SHORT))
typedef struct { s16 pos[3], leaf; } g_spot;    /* an item's place (units, rounded down) and leaf (-1: gone) */
typedef struct { s32 lo[3], hi[3]; } g_area;    /* a trigger's box */
extern g_spot       *g_item_spots;              /* g_nitems: for the tests that needn't read the item */
extern g_area       *g_trig_areas;              /* g_ntrigs (g_nitems on among the short ones) */
#define ITEM_HALF       FIX(15)                 /* an item's box: this round its origin */
extern g_ent        *g_player;
extern g_ent        goal_marker;            /* a stand-in goal: a point (ai_run's tempgoal) */

/* g_main.c */
void                g_init(void);           /* after the level and models are loaded */
void                g_frame(s32 dt);        /* the game: ticks at 10 Hz as the time comes */
void                g_render_ents(void);    /* the monsters, items and barrels in view into the renderer's list, blended */
void                g_render_late(bool pvs);    /* once the camera's moved: pvs, the PVS marked is the camera's */
g_ent               *g_spawn(void);         /* a short one (G_UseTargets' stand-ins), or NULL */
g_ent               *g_ent_at(int n);       /* each entity, n 0 to g_nfull + g_nshort: the full ones, then the short */
void                g_free(g_ent *e);       /* one gone (an item: g_render_ents and the touches told) */
g_ent               *g_drawn(int s);        /* (debugging) the entity render entity s is, or NULL */
q_trace             g_trace(const s32 *start, const s32 *mins, const s32 *maxs, const s32 *end, const g_ent *pass,
                            int mask, g_ent **hit);
void                g_damage(g_ent *targ, g_ent *attacker, int damage, const s32 *point);
g_ent               *g_ent_for_model(int model);
void                g_fire_blaster(g_ent *self, const s32 *start, const s32 *dir, int damage, s32 speed);
void                g_fire_hitscan(g_ent *self, const s32 *start, const s32 *dir, int damage, int hspread, int vspread,
                                   int count);
void                g_muzzle_flash(const s32 *p, u8 r, u8 g, u8 b);
s32                 crandom(void);          /* -1..1 (16.16) */
s32                 frandom(void);          /* 0..1 */
int                 vectoyaw(const s32 *v);
s32                 vlen(const s32 *v);     /* units (16.16); to a quarter of a unit past 1024 */

/* g_ai.c: g_ai.c and m_move.c */
void                ai_move(g_ent *self, s32 dist);
void                ai_stand(g_ent *self, s32 dist);
void                ai_charge(g_ent *self, s32 dist);
void                ai_run(g_ent *self, s32 dist);
bool                ai_checkattack(g_ent *self, s32 dist);
int                 range(const g_ent *self, const g_ent *other);
bool                visible(const g_ent *self, const g_ent *other);
bool                infront(const g_ent *self, const g_ent *other);
bool                FindTarget(g_ent *self);
void                FoundTarget(g_ent *self);
void                ai_reset(void);
extern const char   *g_next_map;                /* the exit taken: "demo2$base1" (NULL: none) */

/* src/sound.c: SND_ ids (obj/gen/sound_ids.h, tools/bake_sound.py); attenuations as Quake's */
#include "sound_ids.h"
#define ATTN_NONE       (0)                     /* heard everywhere the same */
#define ATTN_NORM       (1)
#define ATTN_IDLE       (2)
#define ATTN_STATIC     (3)
void                s_init(const char *map);    /* at start-up: the driver, and the level's bank (DEMO1.MAP: DEMO1.SND) */
void                s_level(const char *map);   /* a new level: its bank */
void                s_play(int id, const s32 *origin, int atten);
void                s_play_queued(int id, const s32 *origin, int atten);    /* (the move code's: posted by the master) */
void                s_queue_flush(void);
void                s_lag(s32 ago);             /* the game's moment its sounds are of: this long ago (16.16 s; -1 now) */

/* src/menu.c */
typedef enum { MENU_NONE, MENU_MAIN, MENU_SKILL, MENU_OPTIONS, MENU_PAUSE } menu_id;
typedef enum { MA_NONE, MA_NEW_GAME, MA_RESTART, MA_RESUME, MA_TITLE } menu_action;
extern menu_id      menu_cur;
extern int          g_skill;                    /* 0-2; -1: everything spawns */
/* the records by targetname (g_init builds a chain a name, in record order; G_UseTargetName
   walks one rather than scanning every record, ~250 in low work RAM or on the cart, a use) */
#define G_TN_NONE       (0xFFFF)
extern u16          *g_tn_first;                /* a name's first record (g_ent_at's number), or G_TN_NONE */
extern u16          *g_tn_next;                 /* each record's next with its name */
extern int          g_tn_count;                 /* names with a chain: ids below it */
/* The entities with a think pending (g_tick runs them when due): a list, not a scan of every
   record (~1.2 ms a tick, the records in low work RAM or the cart). g_think_at puts one on it
   when its nextthink was 0; one on it stays till g_tick takes it off (its think ran, or it's
   no longer one g_tick thinks for). Too many for the list: g_tick scans every record again
   and rebuilds it */
#define G_THINKERS      (48)
extern g_ent        **g_thinkers;
extern int          g_nthinkers;
extern bool         g_thinkers_full;
static inline void  g_think_at(g_ent *e, s32 t)
{
    if (!e->nextthink)
    {
        if (g_nthinkers < G_THINKERS)
            g_thinkers[g_nthinkers++] = e;
        else
            g_thinkers_full = true;
    }
    e->nextthink = t;
}
extern int          opt_volume;
extern int          opt_stats;            /* (menu.c) 0 off, 1 the frame rate, 2 the debugging overlay */
extern bool         opt_crosshair;
extern bool         opt_gun_bob;
bool                menu_active(void);
bool                menu_at_title(void);
void                menu_open(menu_id m);
menu_action         menu_input(u16 pressed);
void                menu_draw(void);
int                 hud_pic(const char *name, int x, int y);   /* (hud.c) its width, 0 if there's none */
void                M_ChangeYaw(g_ent *ent);
bool                M_walkmove(g_ent *ent, int yaw, s32 dist);
void                M_MoveFrame(g_ent *self);
void                M_droptofloor(g_ent *ent);
void                monster_physics(g_ent *self);   /* falling, when not on the ground */
void                monster_start(g_ent *self);

/* g_items.c: items, and the player's weapons */
void                g_client_init(void);
bool                g_spawn_item(g_ent *e, const q_erec *r);
bool                g_item_spawns(int cls);     /* (g_init) an item this level has the model for */
void                g_touch_items(void);
void                g_next_weapon(void);
void                g_player_fire(bool held, const s32 *eye, int yaw, int pitch);
int                 g_armor_absorb(int damage);
void                g_player_noise(void);
extern bool         level_complete;
extern int          found_secrets, total_secrets, found_goals, total_goals, kills, total_monsters;

/* m_infantry.c */
void                SP_monster_infantry(g_ent *self);

/* m_gunner.c */
void                SP_monster_gunner(g_ent *self);         /* (loaded per level: g_overlays_load) */
void                SP_monster_berserk(g_ent *self);
void                SP_monster_tank(g_ent *self);
void                g_overlays_load(void);                  /* (a level's start, after the models) */

/* g_target.c: triggers, relays, timers, targets, barrels */
void                G_UseTargets(g_ent *ent, g_ent *activator);
void                G_UseTargetName(int name, g_ent *other, g_ent *activator);    /* everything called that */
bool                g_spawn_point(g_ent *e, const q_erec *r);   /* the non-monster, non-item classes */
void                g_touch_triggers(void);                     /* the player against trigger volumes */
void                g_centerprint(const char *msg);
void                g_explosion(const s32 *p, g_ent *inflictor, g_ent *attacker, int damage, s32 radius, g_ent *ignore);
void                T_RadiusDamage(const s32 *p, g_ent *inflictor, g_ent *attacker, int damage, g_ent *ignore, s32 radius);
extern const char   *center_msg;
extern s32          center_until;

/* m_soldier.c */
void                SP_monster_x_soldier(g_ent *self, int skin);

/* view.c: the gun in your hands */
void                view_level_init(void);  /* a new level (after render_init): its slots, your gun in one */
void                view_reset(void);       /* a new game, a restart: your gun (the blaster), up */
void                view_update(s32 dt);    /* once a frame */
void                view_fired(void);       /* a shot */
bool                view_ready(void);       /* the gun's up: it can fire */

#endif
