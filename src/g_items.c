/*
** Items and the player's weapons: Quake 2's game/g_items.c and p_weapon.c,
** cut down. Items sit on the floor turning; walking over one picks it up.
** Weapons fire at Quake's rates (from its animation frames at 10 a second).
*/
#include "game.h"

g_client            client;

/* ---- the items ---- */

enum { IT_HEALTH, IT_ARMOR, IT_AMMO, IT_WEAPON, IT_POWERUP, IT_KEY };

typedef struct
{
    int             cls, model, kind;
    const char      *name;
    int             quantity, max;          /* how much it gives, up to (health, armour, ammo) */
    int             tag;                    /* ammo: AMMO_*; weapon: W_*; armour: its protection (%) */
}                   g_item;

static const g_item items[] = {
    { C_ITEM_HEALTH_SMALL, MDL_STIMPACK, IT_HEALTH, "Stimpack", 2, 0, 0 },          /* max 0: ignores it */
    { C_ITEM_HEALTH, MDL_HEALTH, IT_HEALTH, "Medium Health", 10, 100, 0 },
    { C_ITEM_HEALTH_LARGE, MDL_HEALTHL, IT_HEALTH, "Large Health", 25, 100, 0 },
    { C_ITEM_HEALTH_MEGA, MDL_MEGA, IT_HEALTH, "MegaHealth", 100, 0, 0 },
    { C_ITEM_ARMOR_SHARD, MDL_SHARD, IT_ARMOR, "Armor Shard", 2, 0, 30 },
    { C_ITEM_ARMOR_JACKET, MDL_JACKET, IT_ARMOR, "Jacket Armor", 25, 50, 30 },
    { C_ITEM_ARMOR_COMBAT, MDL_COMBAT, IT_ARMOR, "Combat Armor", 50, 100, 60 },
    { C_ITEM_ARMOR_BODY, MDL_BODY, IT_ARMOR, "Body Armor", 100, 200, 80 },
    { C_AMMO_SHELLS, MDL_SHELLS, IT_AMMO, "Shells", 10, 100, AMMO_SHELLS },
    { C_AMMO_BULLETS, MDL_BULLETS, IT_AMMO, "Bullets", 50, 200, AMMO_BULLETS },
    { C_AMMO_GRENADES, MDL_GRENADES, IT_AMMO, "Grenades", 5, 50, AMMO_GRENADES },
    { C_AMMO_ROCKETS, MDL_ROCKETS, IT_AMMO, "Rockets", 5, 50, AMMO_ROCKETS },
    { C_AMMO_CELLS, MDL_CELLS, IT_AMMO, "Cells", 50, 200, AMMO_CELLS },
    { C_AMMO_SLUGS, MDL_SLUGS, IT_AMMO, "Slugs", 10, 50, AMMO_SLUGS },
    { C_WEAPON_SHOTGUN, MDL_G_SHOTG, IT_WEAPON, "Shotgun", 10, 0, W_SHOTGUN },
    { C_WEAPON_SUPERSHOTGUN, MDL_G_SHOTG2, IT_WEAPON, "Super Shotgun", 10, 0, W_SSHOTGUN },
    { C_WEAPON_MACHINEGUN, MDL_G_MACHN, IT_WEAPON, "Machinegun", 50, 0, W_MACHINEGUN },
    { C_WEAPON_CHAINGUN, MDL_G_CHAIN, IT_WEAPON, "Chaingun", 50, 0, W_CHAINGUN },
    { C_WEAPON_GRENADELAUNCHER, MDL_G_LAUNCH, IT_WEAPON, "Grenade Launcher", 10, 0, W_GLAUNCHER },
    { C_WEAPON_ROCKETLAUNCHER, MDL_G_ROCKET, IT_WEAPON, "Rocket Launcher", 10, 0, W_RLAUNCHER },
    { C_ITEM_QUAD, MDL_QUAD, IT_POWERUP, "Quad Damage", 0, 0, 0 },
    { C_ITEM_ADRENALINE, MDL_ADRENAL, IT_POWERUP, "Adrenaline", 0, 0, 0 },
    { C_ITEM_SILENCER, MDL_SILENCER, IT_POWERUP, "Silencer", 0, 0, 0 },
    { C_ITEM_INVULNERABILITY, MDL_INVUL, IT_POWERUP, "Invulnerability", 0, 0, 0 },
    { C_KEY_BLUE_KEY, MDL_KEY, IT_KEY, "Blue Key", 0, 0, 0 },
};

/* the weapons: their ammo, how much a shot takes, and the time between shots */
const g_weapon      weapons[W_COUNT] = {
    { "Blaster", AMMO_NONE, 0, FIX(0.5) },
    { "Shotgun", AMMO_SHELLS, 1, FIX(1.1) },
    { "Super Shotgun", AMMO_SHELLS, 2, FIX(1.1) },
    { "Machinegun", AMMO_BULLETS, 1, FIX(0.1) },
    { "Chaingun", AMMO_BULLETS, 1, FIX(0.05) },
    { "Grenade Launcher", AMMO_GRENADES, 1, FIX(1.1) },
    { "Rocket Launcher", AMMO_ROCKETS, 1, FIX(0.8) },
};

static const int    ammo_max[AMMO_COUNT] = { 0, 100, 200, 50, 50, 200, 50 };

static const g_item *item_of(int cls)
{
    unsigned        i;

    for (i = 0; i < sizeof(items) / sizeof(items[0]); ++i)
        if (items[i].cls == cls)
            return &items[i];
    return NULL;
}

static char         pickup_msg[48];

static void         say_pickup(const char *name)
{
    int             n = 0;
    const char      *p = "You got the ";

    while (*p && n < 40)
        pickup_msg[n++] = *p++;
    while (*name && n < 46)
        pickup_msg[n++] = *name++;
    pickup_msg[n] = 0;
    g_centerprint(pickup_msg);
}

/* give an item: false if it's no use (and so stays put) */
static bool         pickup(const g_item *it)
{
    switch (it->kind)
    {
        case IT_HEALTH:
            if (it->max && g_player->health >= it->max)
                return false;
            g_player->health += it->quantity;
            if (it->max && g_player->health > it->max)
                g_player->health = it->max;
            break;
        case IT_ARMOR:
            if (it->cls == C_ITEM_ARMOR_SHARD)
            {
                client.armor += it->quantity;
                if (!client.armor_protect)
                    client.armor_protect = 30;
            }
            else
            {
                if (client.armor >= it->max && client.armor_protect >= it->tag)
                    return false;
                if (it->tag > client.armor_protect)
                    client.armor_protect = it->tag;
                client.armor = imin(client.armor + it->quantity, imax(it->max, client.armor));
            }
            break;
        case IT_AMMO:
            if (client.ammo[it->tag] >= ammo_max[it->tag])
                return false;
            client.ammo[it->tag] = imin(client.ammo[it->tag] + it->quantity, ammo_max[it->tag]);
            break;
        case IT_WEAPON:
        {
            const g_weapon *w = &weapons[it->tag];

            if (client.have[it->tag] && client.ammo[w->ammo] >= ammo_max[w->ammo])
                return false;
            if (!client.have[it->tag])
            {
                client.have[it->tag] = true;
                if (client.weapon == W_BLASTER)
                    client.newweapon = it->tag;     /* a real gun: use it */
            }
            client.ammo[w->ammo] = imin(client.ammo[w->ammo] + it->quantity, ammo_max[w->ammo]);
            break;
        }
        case IT_POWERUP:
            if (it->cls == C_ITEM_QUAD)
                client.quad_until = level.time + FIX(30);
            else if (it->cls == C_ITEM_INVULNERABILITY)
                client.invul_until = level.time + FIX(30);
            else if (it->cls == C_ITEM_ADRENALINE)
                g_player->health = ++g_player->max_health;
            break;
        case IT_KEY:
            client.keys |= 1;
            break;
    }
    s_play(it->kind == IT_HEALTH ? (it->cls == C_ITEM_HEALTH_SMALL ? SND_HEALTH_SMALL : it->cls == C_ITEM_HEALTH_LARGE
                                    ? SND_HEALTH_LARGE : it->cls == C_ITEM_HEALTH_MEGA ? SND_HEALTH_MEGA : SND_HEALTH)
           : it->kind == IT_ARMOR ? (it->cls == C_ITEM_ARMOR_SHARD ? SND_ARMOUR_SHARD : SND_ARMOUR)
           : it->kind == IT_AMMO ? SND_AMMO : it->kind == IT_WEAPON ? SND_WEAPON
           : it->cls == C_ITEM_QUAD ? SND_QUAD : SND_PICKUP, NULL, ATTN_NONE);
    say_pickup(it->name);
    client.pickup_flash = FIX(0.3);
    return true;
}

static void         touch_item(g_ent *self, g_ent *other)
{
    const g_item    *it = item_of(self->cls);

    if (other != g_player || g_player->dead || !it)
        return;
    if (pickup(it))
    {
        G_UseTargets(self, other);
        self->kind = EK_FREE;
    }
}

bool                g_spawn_item(g_ent *e, const q_erec *r)
{
    const g_item    *it = item_of(r->cls);
    s32             end[3];
    q_trace         t;
    int             k;

    if (!it || !models[it->model].loaded)
        return false;
    e->kind = EK_ITEM;
    e->mdl = &models[it->model];
    for (k = 0; k < 3; ++k)
    {
        e->mins[k] = -FIX(15);
        e->maxs[k] = FIX(15);
    }
    e->touch = touch_item;
    e->yaw = (int)(rng() & 0xFFFF);
    /* droptofloor */
    end[0] = e->origin[0];
    end[1] = e->origin[1];
    end[2] = e->origin[2] - FIX(128);
    t = trace_world(e->origin, e->mins, e->maxs, end, MASK_SOLID_ONLY);
    if (!t.startsolid)
        for (k = 0; k < 3; ++k)
            e->origin[k] = t.endpos[k];
    return true;
}

/* walking over items (every tick) */
void                g_touch_items(void)
{
    int             i, k;

    if (g_player->dead || pl.noclip)
        return;
    for (i = 1; i < MAX_EDICTS; ++i)
    {
        g_ent *e = &g_edicts[i];

        if (e->kind != EK_ITEM)
            continue;
        for (k = 0; k < 3; ++k)
            if (g_player->origin[k] + g_player->maxs[k] < e->origin[k] + e->mins[k]
                || g_player->origin[k] + g_player->mins[k] > e->origin[k] + e->maxs[k])
                break;
        if (k == 3)
            touch_item(e, g_player);
    }
}

/* ---- the player's weapons ---- */

void                g_client_init(void)
{
    memset(&client, 0, sizeof(client));
    client.have[W_BLASTER] = true;
    client.weapon = client.newweapon = W_BLASTER;
}

static bool         has_ammo(int w)
{
    return client.have[w] && (weapons[w].ammo == AMMO_NONE || client.ammo[weapons[w].ammo] >= weapons[w].per_shot);
}

/* the best weapon with ammo (Quake's NoAmmoWeaponChange order) */
static void         best_weapon(void)
{
    static const int order[] = { W_RLAUNCHER, W_CHAINGUN, W_MACHINEGUN, W_SSHOTGUN, W_SHOTGUN, W_BLASTER };
    unsigned        i;

    for (i = 0; i < sizeof(order) / sizeof(order[0]); ++i)
        if (has_ammo(order[i]))
        {
            client.newweapon = order[i];
            return;
        }
}

void                g_next_weapon(void)
{
    int             i, w;

    for (i = 1; i <= W_COUNT; ++i)
    {
        w = (client.weapon + i) % W_COUNT;
        if (has_ammo(w))
        {
            client.newweapon = w;
            return;
        }
    }
}

/* where shots start: in front, right and down a bit from the eye (Quake's 8, 8, -8) */
static void         shot_start(const s32 *eye, const s32 *fwd, s32 *start)
{
    s32             right[2];

    right[0] = fsin(cam.yaw);
    right[1] = -fcos(cam.yaw);
    start[0] = eye[0] + fwd[0] * 8 + right[0] * 8;
    start[1] = eye[1] + fwd[1] * 8 + right[1] * 8;
    start[2] = eye[2] + fwd[2] * 8 - FIX(8);
}

void                g_player_fire(bool held, const s32 *eye, int yaw, int pitch)
{
    s32             fwd[3], start[3];
    int             w, dmg_mul;

    if (client.newweapon != client.weapon)
    {
        client.weapon = client.newweapon;
        client.fire_time = level.time + FIX(0.3);   /* putting it up */
    }
    if (!held || g_player->dead || level.time < client.fire_time)
        return;
    w = client.weapon;
    if (!has_ammo(w))
    {
        best_weapon();
        return;
    }
    client.fire_time = level.time + weapons[w].refire;
    if (weapons[w].ammo != AMMO_NONE)
        client.ammo[weapons[w].ammo] -= weapons[w].per_shot;
    dmg_mul = level.time < client.quad_until ? 4 : 1;
    fwd[0] = fmul(fcos(yaw), fcos(pitch));
    fwd[1] = fmul(fsin(yaw), fcos(pitch));
    fwd[2] = -fsin(pitch);
    shot_start(eye, fwd, start);
    g_player_noise();
    switch (w)
    {
        case W_BLASTER:
            fx_bolt(g_player, start, fwd, 15 * dmg_mul, FIX(1000));
            g_muzzle_flash(start, 9, 7, 2);
            s_play(SND_BLASTER, NULL, ATTN_NONE);
            break;
        case W_SHOTGUN:
            g_fire_hitscan(g_player, start, fwd, 4 * dmg_mul, 500, 500, 12);
            g_muzzle_flash(start, 12, 10, 4);
            s_play(SND_SHOTGUN, NULL, ATTN_NONE);
            break;
        case W_SSHOTGUN:
        {
            /* two barrels, 5 degrees either side */
            s32 d[3];
            int s;

            for (s = -1; s <= 1; s += 2)
            {
                int a = yaw + s * 910;

                d[0] = fmul(fcos(a), fcos(pitch));
                d[1] = fmul(fsin(a), fcos(pitch));
                d[2] = fwd[2];
                g_fire_hitscan(g_player, start, d, 6 * dmg_mul, 1000, 500, 10);
            }
            g_muzzle_flash(start, 14, 11, 4);
            s_play(SND_SSHOTGUN, NULL, ATTN_NONE);
            break;
        }
        case W_MACHINEGUN:
        case W_CHAINGUN:
            g_fire_hitscan(g_player, start, fwd, (w == W_MACHINEGUN ? 8 : 6) * dmg_mul, 300, 500, 1);
            g_muzzle_flash(start, 12, 10, 4);
            s_play(w == W_MACHINEGUN ? SND_MACHINEGUN : SND_CHAINGUN, NULL, ATTN_NONE);
            break;
        case W_GLAUNCHER:
            fx_grenade(g_player, start, fwd, 120 * dmg_mul, FIX(600));
            g_muzzle_flash(start, 10, 8, 3);
            s_play(SND_GRENADE_FIRE, NULL, ATTN_NONE);
            break;
        case W_RLAUNCHER:
            fx_rocket(g_player, start, fwd, (100 + (int)(rng() % 21)) * dmg_mul, 120 * dmg_mul);
            g_muzzle_flash(start, 12, 9, 3);
            s_play(SND_ROCKET_FIRE, NULL, ATTN_NONE);
            break;
    }
}

/* damage to the player, less what the armour takes */
int                 g_armor_absorb(int damage)
{
    int             save;

    if (level.time < client.invul_until)
        return 0;
    if (!client.armor)
        return damage;
    save = (damage * client.armor_protect + 99) / 100;
    if (save > client.armor)
        save = client.armor;
    client.armor -= save;
    if (!client.armor)
        client.armor_protect = 0;
    return damage - save;
}
