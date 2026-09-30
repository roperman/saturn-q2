/*
** Quake 2 on the Saturn: shared types.
**
** World: Quake units in 16.16 fixed point, Quake's axes (x forward/east,
** y left/north, z up). Angles are 16-bit (0x10000 a full turn); yaw 0 looks
** down +x, 0x4000 down +y; pitch is positive looking down, as in Quake.
*/
#ifndef Q2_H
#define Q2_H

#include "sat.h"
#include "vdp.h"

#define FIX(x)          ((s32)((x) * 65536))

static inline s32   imin(s32 a, s32 b) { return a < b ? a : b; }
static inline s32   imax(s32 a, s32 b) { return a > b ? a : b; }
static inline s32   iclamp(s32 v, s32 lo, s32 hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline s32   iabs(s32 v) { return v < 0 ? -v : v; }

/* math.c */
s32                 fsin(int a);
s32                 fcos(int a);
int                 fatan2(s32 x, s32 z);
u32                 isqrt(u32 v);
u32                 rng(void);
void                rng_seed(u32 s);
s32                 fdiv(s32 a, s32 b);                     /* 16.16 a / b */
void                cycles_measure(void);                   /* src/cycles.c: what things cost */
void                *level_alloc_low(u32 bytes);            /* low work RAM, for what isn't hot */
u32                 level_heap(void);                       /* src/level.c: the top of what the level took of work RAM (high) */
void                level_free(u32 *hw, u32 *lw, u32 *cart);    /* ...and what's left: high work RAM, low, the cart */
void                level_trace_hot(void);  /* (the last thing at a level's start) the traces' brushes into HWRAM, what fits */

/* ---- the baked level (tools/bake_map.py writes it; big-endian, so these overlay it) ---- */

typedef struct { s32 n[3]; s32 dist; u8 type, signbits; u16 pad; } q_plane;
typedef struct { u16 plane; s16 child[2]; u16 firstface, numfaces; s16 mins[3], maxs[3]; u16 pad; } q_node;
typedef struct { s16 cluster, area; s16 mins[3], maxs[3]; u16 firstmark, nummark, firstbrush, numbrushes;
                 s32 contents; } q_leaf;
/* a face: a grid of nu x nv cells from origin, du and dv apart (world, 16.16: the axes table's
   entry, du then dv; 575 pairs among 7,500 faces). Its outer lines are on the face's edges, not
   its tiles': the first column starts eu0 stored texels into its tile and the last ends eu1 into
   its own (the rows: ev0, ev1), so origin is the face's corner. 32 bytes: two cache lines. */
typedef struct { s32 origin[3]; u16 axes, plane; u8 flags, nu, nv, eu0, eu1, ev0, ev1, lodhi;
                 u32 firstcell, firstlight; } q_face;  /* (firstlight: the low 24 bits; the top 8 and lodhi,
                                                          its coarse grid's number in lv.lodfaces) */
/* a cell: its texture, the rows of it drawn (ty0, th), and the part of the
   cell they cover (u0..u1, v0..v1, in stored texels: 0..N). A whole tile
   (CELL_FULL in tex), or a crop that's exactly its grid cell (CELL_EXACT),
   is a q_cell_fast instead: the texture's colour table (TEX_TRANSPOSED in
   it for a transposed one), its first row drawn, that row's offset in 8
   bytes, its width / 8 and the rows drawn, all ready for the command
   (src/cells.s). */
typedef struct { u16 tex; u8 ty0, th, u0, u1, v0, v1; } q_cell;
/* a face's coarse grid (FF_LOD in its flags), for when it's far: 64-texel cells, the textures at half
   the resolution (the same N stored texels a cell). Its first point is offu, offv texels back from
   the face's along u and v (its texels are twice the size, so it rounds differently); its axes are
   the face's doubled. Its cells and lights are lv.lodcells', lv.lodlights'. (On the cart.) */
typedef struct { u8 nu, nv, eu0, eu1, ev0, ev1; s8 offu, offv; u32 firstcell, firstlight; } q_lodface;
typedef struct { u16 tex, lut; u8 ty0, off8, wsz, th; } q_cell_fast;
#define CELL_FULL       (0x8000)
#define CELL_EXACT      (0x4000)                /* cropped, but exactly its grid cell (so no corners to find) */
#define CELL_TEX        (0x3FFF)
typedef struct { u32 ofs; u16 lut; u8 w, h; } q_tex;
typedef struct { s32 mins[3], maxs[3], origin[3]; s32 headnode, firstface, numfaces; } q_model;
typedef struct { s32 contents; u16 firstside, numsides; } q_brush;
typedef struct { u16 plane, flags; } q_brushside;
/* a brush model's behaviour (tools/bake_map.py movers()): offset = move * frac */
typedef struct { u8 kind, flags; s16 team_next; u16 targetname, target; s32 move[3], tmin[3], tmax[3], speed, wait; } q_mover;
#define MV_NONE         (0)                 /* triggers and the like: not drawn, not solid */
#define MV_STATIC       (1)
#define MV_DOOR         (2)
#define MV_PLAT         (3)
#define MV_BUTTON       (4)
#define MF_START_OPEN   (1)                 /* rests at the far end (a plat: at the bottom) */
#define MF_PROXIMITY    (2)                 /* goes when the player's in its trigger box */
#define MF_SHOOT        (4)
/* things placed in the map (tools/bake_map.py spawns()) */
typedef struct { u16 kind, angle; u32 spawnflags; s32 origin[3]; } q_spawn;
/* the player's starts, by name (tools/bake_map.py): a level's exit names the next's */
typedef struct { char name[16]; s32 origin[3], angle; } q_start;
/* an opening between two clusters: qbsp's portals between them, a box round them (whole units,
   a unit out all round: tools/bake_map.py cluster_portals) */
typedef struct { s16 lo[3], hi[3]; } q_portal;
#define SPAWN_SOLDIER_LIGHT (1)
#define SPAWN_SOLDIER   (2)
#define SPAWN_SOLDIER_SS (3)
#define SPAWN_INFANTRY  (4)

#define FF_SKY          (1)
#define FF_WARP         (2)
#define FF_TRANS33      (4)
#define FF_TRANS66      (8)
#define FF_FLOWING      (16)
#define FF_NODRAW       (32)
#define FF_BACK         (64)                    /* on its plane's back */
#define FF_LOD          (128)                   /* it has a coarse grid (q_lodface) */
#define TEX_TRANSPOSED  (0x8000)
#define CELL_EMPTY      (0xFFFF)
#define CONTENTS_SOLID  (1)
#define CONTENTS_WINDOW (2)
#define CONTENTS_LAVA   (8)
#define CONTENTS_SLIME  (16)
#define CONTENTS_WATER  (32)
#define CONTENTS_PLAYERCLIP (0x10000)
#define CONTENTS_MONSTER (0x2000000)
#define CONTENTS_LADDER (0x20000000)
#define MASK_PLAYERSOLID (CONTENTS_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_WINDOW | CONTENTS_MONSTER)
#define MASK_WATER      (CONTENTS_WATER | CONTENTS_LAVA | CONTENTS_SLIME)
#define SURF_SLICK      (2)

typedef struct
{
    const q_plane   *planes;
    const q_node    *nodes;
    const q_leaf    *leafs;
    const u16       *marks;
    const q_face    *faces, *faces_cart;        /* (faces_cart: where they are on the cart, for the DSP) */
    const s32       *axes;                      /* du, dv: 6 words an entry */
    int             naxes;
    int             quart0;                     /* tile t quartered (8 x 32: its quarters one under another) is texture quart0 + t */
    const q_lodface *lodfaces;                  /* a face's coarse grid, and its cells and lights (on the cart) */
    const q_cell    *lodcells;
    const u16       *lodlights;
    const q_cell    *cells;
    const u16       *lights;
    const q_tex     *textures;
    const u8        *texdata;
    const u16       *luts;
    const u8        *vis;
    const q_model   *models;
    const q_brush   *brushes;
    const q_brushside *brushsides;
    const u16       *leafbrushes;
    const q_mover   *movers;
    const u8        *facevis;               /* per cluster: the faces really visible (bits, Quake's RLE) */
    const u16       *sky;                   /* w, h, palette[16], above, below, zenith, then 4bpp pixels */
    const q_spawn   *spawns;
    int             nspawns;
    const q_start   *starts;
    int             nstarts;
    const q_portal  *portals;               /* (none: nportals 0) */
    const u16       *cportals;              /* each cluster's first in the list (and one after the last), then the list:
                                               the other cluster and the portal, each */
    int             nportals;
    s16             *brushbounds;           /* each brush's box: mins[3] maxs[3], whole units (src/level.c, src/trace.c) */
    const u16       *leaflight;             /* per leaf: r g b brightness (8.8), pad */
    const void      *erecs;                 /* the map's entities (game.h q_erec) */
    int             nerecs;
    const char      *strings;
    int             nplanes, nnodes, nleafs, nfaces, ntextures, nluts, nclusters, nmodels, nbrushes;
    int             nleafbrushes;
    int             T, N, nshift;           /* cell size in texels; stored texels a side, log2 */
    s32             start[3];
    int             start_yaw;
}                   q_level;

extern q_level      lv;
extern int          cart_mb;

/* level.c */
bool                level_load(const char *name);          /* onto the RAM cart */
int                 level_leaf(const s32 *p);               /* the leaf a point is in */
const u8            *level_pvs(int cluster);                /* decompressed PVS row (static buffer) */
void                level_facevis(int cluster, u8 *bits);   /* the faces a cluster can see */
void                *level_alloc(u32 bytes);                /* high work RAM, for the level's life */
const u8            *cart_load(const char *name);           /* another file onto the cart, after the level */
u8                  *cart_alloc(u32 bytes);                 /* room on the cart, after the level (whole sectors) */
u32                 cart_free(void);
const void          *level_hot(const void *src, u32 bytes);  /* a copy in high work RAM if there's room */
const void          *level_hot_keep(const void *src, u32 bytes, u32 keep);     /* ...leaving keep bytes */

/* trace.c: Quake 2's box traces against the brushes (qcommon/cmodel.c), in 16.16 */
typedef struct
{
    bool            allsolid, startsolid;
    s32             fraction;               /* 0..1 (16.16) */
    s32             endpos[3];
    const q_plane   *plane;                 /* the plane hit, or NULL */
    int             contents, surf_flags;
    int             ent;                    /* what was hit: 0 the world, else a brush model */
}                   q_trace;

void                trace_init(void);
q_trace             trace_box(const s32 *start, const s32 *end, const s32 *mins, const s32 *maxs, int headnode,
                              int mask);
int                 point_contents(const s32 *p, int headnode);
q_trace             trace_line(const s32 *start, const s32 *end, int headnode, int mask);   /* a point: much cheaper */

/* pmove.c */
typedef struct
{
    int             yaw, pitch;             /* where the player looks */
    s32             forward, side, up;      /* units a second (16.16) */
}                   q_usercmd;

typedef struct
{
    s32             origin[3], velocity[3];
    bool            on_ground, jump_held, noclip;
    int             ground_ent;             /* what it's standing on (a brush model, or 0) */
    int             ground_flags, land_time, waterlevel, watertype;
}                   q_player;

extern q_player     pl;
void                pmove(const q_usercmd *cmd, s32 dt);    /* dt: seconds, 16.16 */
void                pmove_spawn(const s32 *origin);
q_trace             pm_trace(const s32 *start, const s32 *end);    /* the player's box: world, movers, monsters */
q_trace             trace_world(const s32 *start, const s32 *mins, const s32 *maxs, const s32 *end, int mask);  /* + movers */
void                trace_world_init(void);
extern u8           *mover_gone;
extern const s32    p_mins[3], p_maxs[3];

/* movers.c: doors, lifts, buttons */
extern s32          (*mover_ofs)[3];        /* each brush model's offset now */
void                movers_init(void);
void                movers_update(s32 dt);
void                movers_use(int name);                  /* set going everything with this targetname */
void                mover_hide(int model);                 /* gone (a func_explosive, blown up) */
bool                mover_live(int model);                 /* drawn and solid */
void                g_mover_fired(int target);             /* (the game) a button's targets */

/* the camera */
typedef struct
{
    s32             pos[3];
    int             yaw, pitch;
    s32             fwd[3], right[3], up[3];
}                   q_cam;

extern q_cam        cam;
void                cam_update(void);                       /* the axes from yaw and pitch */

/* dynamic lights and sprites: fx.c fills these each frame, render.c draws them */
typedef struct { s32 pos[3]; s32 radius; u8 r, g, b, pad; } q_dlight;  /* r g b: 5-bit units at the centre */
typedef struct { s32 pos[3]; s32 size; u16 color, halo; } q_sprite;     /* a glowing blob, size in units */
#define MAX_DLIGHTS     (8)
#define MAX_SPRITES     (64)
extern q_dlight     r_dlights[MAX_DLIGHTS];
extern int          r_ndlights;
#define LIT_POOL        (6)                     /* models lit a frame behind at once (model.c ents_light_dyn) */
extern u16          ent_lit[LIT_POOL][162];
void                lights_lag(void);           /* (main.c, as a frame starts: last frame's lights kept) */
void                ents_light_dyn(bool pvs);   /* (after ents_light_pvs: the slave's, once the master's gone on) */
void                ent_lit_forget(void);
const q_dlight      *lights_lagged(int *n);
void                r_wall_level(void);         /* (render.c: the walls' lights ahead, their buffers) */
void                r_wall_ahead(void);         /* (the slave, its drawing done) */
void                r_wall_forget(void);        /* (the master, before it draws) */
extern void         (*r_lit_wait)(void);        /* (draw_master, before its first model: the slave's done them) */
extern q_sprite     r_sprites[MAX_SPRITES];
extern int          r_nsprites;

/* fx.c: blaster bolts, flashes, sparks */
struct g_ent_s;
void                fx_fire(const s32 *eye, int yaw, int pitch);   /* the player's blaster */
void                fx_bolt(struct g_ent_s *owner, const s32 *start, const s32 *dir, int damage, s32 speed);
void                fx_flash(const s32 *p, s32 radius, s32 dur, u8 r, u8 g, u8 b);
void                fx_spark(const s32 *p);
void                fx_explosion(const s32 *p);
void                fx_grenade(struct g_ent_s *owner, const s32 *start, const s32 *dir, int damage, s32 speed);
void                fx_rocket(struct g_ent_s *owner, const s32 *start, const s32 *dir, int damage, int radius_damage);
void                fx_render(void);                       /* rockets and grenades into the renderer's entities */
void                fx_update(s32 dt);                      /* moves things, then fills the lights and sprites */
void                fx_reset(void);                         /* a new level: nothing in flight */

/* model.c: MD2 models (tools/bake_md2.py) */
typedef struct { u16 v[4], tex, flags; } q_mpoly;           /* flags 1: a triangle (v[3] == v[2]) */
typedef struct { u32 ofs; u8 w, h; u16 lut; } q_mtex;       /* lut: its colour table (of its skin's; if no lutmap) */
typedef struct { char name[12]; u16 first, count; } q_manim;
typedef struct
{
    int             nverts, npolys, nframes, nanims, nskins, ntex;
    const q_mpoly   *polys, *fpolys;        /* (fpolys: the mesh merged on a coarse grid, for far away) */
    int             nfpolys, nfverts;
    const u16       *fverts;                /* the vertices fpolys use */
    const q_mtex    *tex;
    const u8        *texdata;               /* nskins blocks of per_skin bytes */
    const u16       *luts;                  /* colour tables of 16: nluts; or nskins x nluts (no lutmap) */
    const u16       *lutmap;                /* each skin's textures (nskins x ntex): their table; NULL: skin x nluts + lut */
    const u8        *frames;                /* per frame: s32 scale[3], translate[3] (16.16), nverts x (x y z normal) */
    const q_manim   *anims;
    const u8        *shade;                 /* 16 yaw steps x 162 normals, 128 = 1.0 */
    const s16       *normals;               /* 162 x (x y z), 2.14 */
    u32             per_skin, frame_bytes;
    int             nluts;                  /* colour tables a skin: one a polygon, or one for all */
    int             tex_id0, lut0;          /* where its textures and colour tables start in the renderer's */
    bool            loaded;
}                   q_mdl;

typedef struct
{
    s32             origin[3];
    int             yaw, pitch;
    const q_mdl     *mdl;
    int             skin;
    int             frame, oldframe;        /* model frames (anims' first + n) */
    s32             lerp;                   /* 0..1 from oldframe to frame (16.16) */
    int             anim;                   /* the q_manim playing */
    s32             anim_time;
    u8              live;                   /* (bytes, not bools: they're ints here, and a q_entity stays 384) */
    u8              g_moved;                /* its origin's changed since ents_light last looked (its leaf to find) */
    /* its light by normal, as Gouraud colours: the leaf's light and Quake's
       shading, remade when it changes leaf or turns (ents_light) */
    u16             gbase[162];
    s16             g_litleaf;              /* the leaf gbase is for (with g_yaw): -1 none */
    int             g_leaf;                 /* the leaf it's in, after ents_light */
    s8              g_yaw;                  /* (0 .. 15) */
    s8              g_lit;                  /* its dynamic lights, a frame behind: ent_lit's; -1 none, -2 draw_model's */
}                   q_entity;
_Static_assert(sizeof(q_entity) == 384, "q_entity: g_moved in the padding");

#define MAX_ENTITIES    (96)                /* 0..63: the game's entities; then projectiles */
#include "q2models.h"
#define VIEW_SLOTS      (2)                     /* the gun in your hands, and the next (src/view.c) */
#define MDL_VIEW0       (MDL_COUNT)             /* ...in models[]: MDL_VIEW0 + slot */
extern q_mdl        models[MDL_COUNT + VIEW_SLOTS];
bool                model_parse(q_mdl *m, const u8 *b);     /* a baked model in memory: m its records */
extern int          nmodels_loaded;
void                models_load_all(void);                  /* tools/models.txt's this level uses, onto the cart */
void                g_models_needed(bool *need);            /* (g_items.c) which of them */
void                models_hot(void);                       /* the monsters' polygons into HWRAM (last at start-up) */
extern q_entity     ents[MAX_ENTITIES];
extern int          nents;
bool                model_load(q_mdl *m, const char *file);
int                 model_anim(const q_mdl *m, const char *name);
void                model_shade(u16 *out, const u8 *sh, const u16 *ll);    /* 162 normals' Gouraud: a light, a yaw's shading */
/* view.c: the gun in your hands */
extern bool         view_on;                /* drawn at all (not at the title, nor in the benchmark's views) */
extern s32          view_bob[3];            /* its bob: pitch (down), yaw (left), roll (right side down), radians 16.16 */
const q_mdl         *view_frame(int *f0, int *f1, s32 *lerp);   /* what to draw now, or NULL */
void                r_view_slot(int slot);  /* (render.c) a slot's new gun: its old textures forgotten */
void                r_view_luts(int slot);  /* (render.c) the gun to be drawn: its colour tables up */
void                r_view_level(void);     /* (render.c) a new level (the gun's) */
void                r_portals_level(void);  /* (render.c) ...its portals' (after the models and traces) */
extern u32          r_full[3];              /* (render.c) frames each CPU's texture cache ran out; late uploads */
void                ents_light(void);                       /* their base lighting, before drawing */
bool                ents_pvs(void);                         /* (ents_light's: the PVS marked is the camera's?) */
void                ents_light_pvs(bool pvs);               /* (ents_light, told that: main.c's slave) */

/* hud.c */
void                hud_init(void);                         /* before render_init: its pictures stay in VRAM */
void                hud_draw(void);

/* render.c */
typedef struct
{
    int             faces, cells, culled, near, uploads, nocache, dropped, leaf, cluster, nodes, proj, gverts, seen;
    int             models, mpolys, nfast, nslow, nexact, pieces, faces_out, cells_all, cells_384, cells_512, muploads, mcpu, mdsp, ns_dl, ns_crop, ns_exact, g_same, g_flat, occ_faces, occ_cells, occ_occluders, ns_small;
    u32             us_walk, t_face, t_grid, t_models, t_mlight, t_mverts, t_mpolys, t_mwait, t_masm, t_mnorm;
    u32             us_rwpre, t_dltest, t_dlsum;    /* (FIGHT_BENCH: render_world to the slave's signal; the world's dynamic lights) */
    int             n_dlfaces, n_dlpts;            /* (and dl_face's points x lights) */
#ifdef FS_STATS
    int             fs[10];                         /* (lit whole faces: points <=16 <=32 <=48 <=64 more; rows over 12, 16 points; lights 1, 2, 3+) */
#endif
    int             n_wlhit;                        /* (lit faces found lit already: r_wall_ahead) */
    int             portal_out, n_reach, n_proj, n_ptest;  /* faces not seen through the portals, clusters reached,
                                                       portals projected, looked at */
    u32             t_flow;                         /* (FIGHT_BENCH: the flow's us) */
    u32             t_view;                 /* the gun in your hands (us) */
    u32             us_pre, us_mdsp, us_tree;   /* R_PROFILE: the walk's parts (the master's, us) */
    u32             t_mfar;                     /* R_PROFILE: models beyond 400 units: their time, */
    int             mfar;                       /* and how many */
    u32             p_setup, p_grid, p_cells, p_slow, p_corners, p_xform;   /* R_PROFILE: FRT ticks */
    int             late;                       /* textures into slots two frames back (render.c tex_load) */
    u32             p_cpre;                     /* R_PROFILE: a whole face's C before its cells' assembly (FRT ticks) */
    int             n_rows, n_rfaces;           /* R_PROFILE: rows done a row at a time, and their faces */
    u32             p_casm;                     /* R_PROFILE: in cells_asm (FRT ticks), */
    int             n_casm, n_calls;            /* the commands it made, its calls */
}                   r_stats;
extern r_stats      rs;
extern int          r_debug;
extern bool         r_two_cpus;                             /* the slave draws the far half */
extern u32          r_clock;                                /* the game's time, 16.16 seconds (the water's movement) */
extern bool         r_water;                                /* water moves (the options) */
extern int          r_trans;                                /* translucent surfaces: 0 solid, 1 mesh, 2 half-transparent */
extern int          r_bright;                               /* brightness: 0 as baked, to 4 */
u16                 r_gamma(u16 c);                         /* an RGB colour at that brightness */
void                r_set_bright(int b);                    /* (every colour table again) */
void                view_relut(void);                       /* (view.c: the gun's colour tables again) */
void                hud_palette(void);                      /* (hud.c: the status bar's palette again) */
extern bool         r_use_dsp;                              /* model vertices on the SCU DSP */
void                render_init(void);                      /* after level_load: colour tables, the cache */
void                render_world(vdp_writer *w0, vdp_writer *w1);
extern void         (*r_during)(void);                      /* run on the master between the walk and its drawing */
extern void         (*r_pre_wait)(void);                    /* (render_world, before it puts the entities in their leaves) */
void                render_slave(void);                     /* the slave's part, when signalled */
void                render_sky_init(void);                  /* the skybox's horizon on a VDP2 layer */
void                render_sky(void);                       /* per frame, after cam_update() */
bool                r_leaf_in_pvs(int leaf);                /* in the camera's PVS */
bool                r_pvs_marked(int cluster);              /* the PVS marked is this cluster's (and it's one) */

#endif
