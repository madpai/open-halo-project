/* Android platform layer — the only native file that touches Android APIs.
 *
 * Responsibilities: NativeActivity lifecycle, locating the user's own Halo
 * Trial data, translating touch/gamepad into engine input, driving the
 * renderer. All game logic lives in src/engine and src/asset.
 *
 * DATA: nothing proprietary ships in this APK. SetupActivity (Java) lets the
 * user pick their own legally obtained Trial map via the system document
 * picker and copies it into the app-private external files directory:
 *   /sdcard/Android/data/net.hta.halotrial/files/
 * which needs no runtime permission and no root. Native still also searches
 * Download/ etc. if All-files access happens to be granted.
 */
#include "platform.h"
#include "../engine/engine.h"
#include "../engine/camera.h"
#include "../engine/player.h"
#include "../engine/gun.h"
#include "../engine/projectile.h"
#include "../engine/particle.h"
#include "../engine/vitals.h"
#include "../asset/dialogue.h"
#include "../engine/actor.h"
#include "../engine/pickup.h"
#include "../engine/bot.h"
#include "../engine/vehicle.h"
#include "../engine/contrail.h"
#include "../engine/shake.h"
#include <stdatomic.h>
#include "../engine/ammo.h"
#include "../engine/hud.h"
#include "../engine/viewmodel.h"
#include "../asset/cache.h"
#include "../asset/bsp.h"
#include "../asset/bitmap.h"
#include "../asset/biped.h"
#include "../asset/weapon.h"
#include "../asset/sound.h"
#include "../asset/effect.h"
#include "../asset/model.h"
#include "../gfx/gfx.h"
#include "../engine/scene_light.h"
#include "audio_android.h"
#include "../net/session.h"
#include "../net/replication.h"
#include "../game/game.h"
#include "../game/view.h"
#include "../game/nav.h"
#include "../game/menu.h"
#include "../asset/items.h"
#include "../asset/strings.h"

#include <android/log.h>
#include <android/input.h>
#include <android/asset_manager.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

#define TAG "halo-trial-android"

void hta_log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, TAG, fmt, ap);
    va_end(ap);
}

double hta_time_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

bool hta_probe_fixed_map(uint64_t addr, size_t len)
{
    void *want = (void *)(uintptr_t)addr;
    void *got = mmap(want, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (got == MAP_FAILED) {
        hta_log("[probe] mmap at 0x%llx FAILED", (unsigned long long)addr);
        return false;
    }
    bool exact = (got == want);
    if (exact) ((volatile unsigned char *)got)[0] = 0xAB;
    hta_log("[probe] mmap at 0x%llx -> %p (%s)", (unsigned long long)addr, got,
            exact ? "EXACT" : "MOVED");
    munmap(got, len);
    return exact;
}

/* ------------------------------------------------------------------ */

/* Tags, not clips -- see HTA_AUDIO_MAX_CLIPS. Weapon fire, dry-fire, zoom
 * in and out, one impact per material the map contains, the projectile's
 * detonation, the grenade's, and a footstep per material: Blood Gulch asks
 * for around thirty of these, and 24 was not enough to hold them. */
#define HTA_SND_MAX_BANK  192u
#define HTA_SND_MAX_PERMS   8u

typedef struct {
    struct android_app *app;

    /* asset state */
    char      map_path[512];
    char      bitmaps_path[512];
    char      sounds_path[512];
    uint8_t  *map_data;
    size_t    map_size;
    uint8_t  *bitmaps_data;
    size_t    bitmaps_size;
    uint8_t  *sounds_data;
    size_t    sounds_size;
    bool      map_loaded;
    char      status[256];
    /* What was mapped, for unmapping: an APK asset maps from a page
     * boundary before its data, so the pointer handed out is not the base. */
    void     *mapped_base[8];
    size_t    mapped_len[8];
    uint32_t  mapped_count;

    hta_cache     cache;
    hta_bsp_mesh  mesh;
    hta_bsp_mesh  sky;
    hta_bsp_mesh  coll_mesh;
    hta_viewmodel vm;
    hta_collision col;
    hta_vehicles vehicles;
    /* One static mesh per vehicle type, drawn as rigid parts. */
    hta_gfx_mesh *gpu_vtypes[HTA_VEHICLE_TYPES];
    int           vehicle_roster;     /* HTA_VROSTER_*, from the match setup */
    int32_t       my_car, my_seat;    /* where this player sits, -1 on foot */
    float         seat_look[2];       /* a Warthog driver's look, relative to the hull */
    bool          hud_alt;            /* the vehicle's second trigger, held */
    bool          veh_fire;           /* the vehicle gun's trigger, for the host */
    bool          show_self;          /* our own body is on screen: third person */
    uint16_t      net_action_count;
    uint16_t      peer_action_seen[8];
    uint32_t      vehicles_applied_tick;
    uint32_t      game_applied_tick;
    double        vsnap_time;
    hta_net_vehicle vfrom[HTA_VEHICLE_MAX], vto[HTA_VEHICLE_MAX];
    bool          vhave[HTA_VEHICLE_MAX];
    uint32_t      veh_in_snd, veh_out_snd;
    uint32_t      veh_engine[HTA_VEHICLE_TYPES];   /* each type's engine `snd!` */
    float         veh_engine_gain[HTA_VEHICLE_TYPES];
    bool          veh_engine_on[HTA_VEHICLE_MAX];
    uint32_t      vfire_recipe[HTA_GAME_MAX_WEAPONS];
    /* Tracers and trails, from each round's own contrail tag. */
    hta_contrails trails;
    hta_gfx_mesh *gpu_trails;
    uint32_t      wtrail[HTA_GAME_MAX_WEAPONS], ptrail[HTA_GAME_MAX_POOLS];
    /* Rounds each shooter has fired since its last tracer (the trigger's
     * `projectiles between contrails`); the last slot is ours on foot. */
    uint8_t       since_tracer[HTA_GAME_MAX_UNITS + 1];
    /* The view thrown about by blasts and by our own gun's kick. */
    hta_shake     shake;
    uint32_t      local_trail;     /* the held weapon's flying round */

    /* Sound. One bank entry per snd! tag actually asked for, decoded once and
     * kept; Halo tags carry several permutations of the same sound and pick
     * between them, so each entry holds every permutation as its own clip. */
    hta_resource_map sounds_rm;
    hta_resource_map bitmaps_rm;
    hta_audio        audio;
    bool             audio_ok;
    struct {
        uint32_t tag_id;
        uint32_t count;
        uint32_t clip[HTA_SND_MAX_PERMS];
        int16_t *pcm[HTA_SND_MAX_PERMS];
    } bank[HTA_SND_MAX_BANK];
    uint32_t bank_count;
    uint32_t fire_snd;       /* snd! id of the weapon's gunshot, 0 if none */
    /* A weapon whose firing effect has no sound at all roars continuously
     * instead: the flamethrower. Held for as long as the trigger is. */
    uint32_t fire_loop_snd;
    float    fire_loop_gain;
    bool     fire_loop_on;
    /* And the same weapon sprays rather than shoots: the flamethrower's jet
     * is a `pctl` particle system attached to its `spawn fire` marker,
     * emitted for as long as the trigger is held rather than burst. */
    uint32_t jet_recipe;
    uint32_t empty_snd;      /* the click when the magazine is out */
    uint32_t foot_snd[33];   /* per MaterialType, resolved on first use */
    uint8_t  foot_known[33];
    uint32_t impact_snd[33];
    uint8_t  impact_known[33];

    /* Every weapon in the cache a player could hold, and which one is up. */
    /* Every weapon the cache has, kept only so a missing loadout has
     * something to fall back on. What you are actually CARRYING is `held`. */
    uint32_t weapons[24];
    uint32_t weapon_count;
    /* Two, because the scenario says two: its player starting profile has a
     * primary weapon and a secondary and nowhere to put a third, and the
     * map's starting equipment hands out exactly two collections. `held_slot`
     * is the one in your hands. */
    uint32_t held[HTA_CARRY_MAX];
    /* What is in the gun NOT in hand. The one in hand is `ammo`; switching
     * used to re-init it from the tag, which refilled it for free. */
    hta_ammo held_ammo[HTA_CARRY_MAX];
    bool     held_ammo_set[HTA_CARRY_MAX];
    uint32_t held_count;
    uint32_t held_slot;
    uint32_t start_weapon[HTA_CARRY_MAX];
    uint32_t start_count;
    bool     hud_swap;
    bool     hud_zoom;
    int      zoom_level;     /* 0 = not zoomed */
    float    base_fov;
    bool     bitmaps_ok;
    uint32_t rng;

    /* Rounds you can watch fly: the rocket and the needle. */
    hta_projectiles proj;
    /* And the grenades, which come from the globals table rather than
     * from anything you are holding. */
    hta_projectiles nades;
    hta_gfx_mesh   *gpu_nades;
    uint32_t        nade_recipe;
    uint32_t        nade_snd;
    int             nade_count;
    int             nade_max;
    /* And the smoke and fire their detonation throws out, plus the dust a
     * bullet kicks up. Blood Gulch is made of four materials, so four
     * impact effects cover every surface a round can land on. */
    hta_particles   parts;
    uint32_t        det_recipe;
    uint32_t        casing_recipe;
    uint32_t        impact_recipe[33];
    uint8_t         map_material[33];      /* the ones this map contains */
    uint32_t        map_material_count;

    /* The game: Slayer against bots, everyone's damage attributed. The
     * local player is unit `me`, mirrored in every frame; the bots live
     * entirely inside it and are drawn from `gview`. */
    /* The main menu: ui.map's ring and sky, the HALO logo and the shell's
     * words, before any level is loaded. */
    bool          menu_mode;
    hta_menu      menu;
    hta_cache     ui_cache;
    uint8_t      *ui_data;
    size_t        ui_size;
    hta_gfx_mesh *gpu_menu_scene, *gpu_menu_sky, *gpu_menu_ui;
    int           menu_pressed;
    bool          menu_go;           /* MULTIPLAYER was chosen: load the level */
    uint32_t      menu_clip[3];      /* cursor, forward, back */
    uint32_t      music_in, music_loop;
    float         music_left;        /* seconds of the intro still playing */
    bool          music_looping;
    int16_t      *menu_pcm[8];
    uint32_t      menu_pcm_count;

    hta_game      game;
    hta_game_view gview;
    hta_nav       nav;
    bool          game_on;
    int32_t       me;
    int           bot_count, bot_skill;
    /* The match the menu set up: kills to win (0 leaves the game's
     * default), minutes of play (0 none), seconds to come back. */
    int           score_limit, time_limit_min;
    float         respawn_delay;
    int           game_mode;       /* hta_game_mode the menu chose */
    int8_t        carried_flag;    /* the flag this player held last frame, or -1 */
    uint32_t      flag_take_snd;
    hta_gfx_mesh *gpu_units[HTA_GAME_MAX_UNITS];
    hta_gfx_mesh *gpu_held[HTA_GAME_MAX_WEAPONS];
    hta_gfx_mesh *gpu_pools[HTA_GAME_MAX_POOLS];
    uint32_t      pool_recipe[HTA_GAME_MAX_POOLS];
    /* A vehicle going up, and sparks off a hull that is nearly gone. */
    uint32_t      wreck_recipe, wreck_snd, spark_recipe;
    float         spark_clock;
    uint32_t      unit_fire_snd[HTA_GAME_MAX_WEAPONS];
    uint8_t       unit_fire_known[HTA_GAME_MAX_WEAPONS];
    uint32_t      line_snd[HTA_LINE_COUNT];
    char          feed[4][96];
    float         feed_age[4];
    char          banner[96];
    float         banner_age;
    char          item_msg[96];      /* "Picked up a rocket launcher" */
    float         item_msg_age;
    float         over_timer;

    /* Health and shield, and what takes them away. `vit` points at these
     * until the game starts, then at the game's own copy for this player,
     * so a bot's round and the local HUD read the same numbers. */
    hta_vitals vitals;
    hta_vitals *vit;

    /* Dying. Halo's multiplayer respawn is five seconds; that number is the
     * gametype's, and no gametype ships in the map, so it is ours. The rest
     * is the tag's: the Chief has a quiet death and a violent one, and
     * which you get depends on what killed you. */
    bool     dead;
    float    damage_flash_left;
    float    dead_timer;        /* seconds until the respawn */
    float    death_pos[3];      /* where it happened, so we come back elsewhere */
    uint32_t death_quiet_snd, death_violent_snd, death_falling_snd;
    /* The shield's own voice, from the unit HUD tag. */
    uint32_t shield_charge_snd, shield_hit_snd, shield_low_snd;
    uint32_t shield_empty_snd, health_low_snd;
    bool     shield_charge_on, shield_low_on, health_low_on;
    uint32_t spawn_rng;
    hta_spawn_point spawn[64];
    uint32_t spawn_count;
    /* Your own body, for looking at from outside. The only third-person
     * thing in the game so far. */
    hta_actor     corpse;
    hta_gfx_mesh *gpu_corpse;
    bool          corpse_up;

    /* What the map leaves on the ground. */
    hta_pickups   items;
    hta_gfx_mesh *gpu_items;
    /* Frames left to keep uploading the item geometry. The dynamic path
     * writes one vertex slot per in-flight frame, so a single change has to
     * reach every one of them before it can stop -- and how many there are
     * is the swapchain's image count, which is not ours to know. Eight
     * covers any of them; the cost of a few extra copies after something is
     * picked up is nothing beside doing it every frame forever. */
    int           items_upload;
#define HTA_ITEMS_UPLOAD_FRAMES 8
    uint32_t      pickup_snd_health, pickup_snd_shield, pickup_snd_camo;
    uint32_t      pickup_snd_ammo;
    float         powerup_timer;
    hta_item_kind powerup;
    bool          throwing;      /* the arm is mid-throw-grenade */
    /* The DBG pad, as (action + 1) so zero means nothing pending. Blood
     * Gulch places neither the needler nor the plasma pistol, so playing
     * the map honestly puts them out of reach; this is the way back to
     * them, and the place to hang the next debug thing off. */
    int           hud_debug;
    uint32_t      debug_weapon;  /* where the roster walk has got to */

    /* Somebody to shoot at. One for now, and the reason melee, grenades and
     * every weapon can be said to work at all. */
    hta_bot       bot;
    hta_gfx_mesh *gpu_bot;
    /* First LAN slice: one other live Spartan, using the same world actor
     * path as the corpse and target bot. Multiple remote meshes come later. */
    hta_actor     remote[2]; /* map starting AR and pistol slots */
    hta_gfx_mesh *gpu_remote[2];
    hta_net_client net;
    hta_net_server host_server;
    bool          net_hosting;
    bool          net_enabled, remote_visible;
    uint8_t       remote_id;
    hta_net_player remote_from, remote_to;
    double        remote_snapshot_time, net_last_send;
    uint64_t      net_last_snapshots;
    uint32_t      net_event_id;
    int8_t        peer_unit[HTA_NET_MAX_PLAYERS];
    uint16_t      peer_melee_seen[HTA_NET_MAX_PLAYERS];
    uint16_t      peer_grenade_seen[HTA_NET_MAX_PLAYERS];
    uint16_t      peer_reload_seen[HTA_NET_MAX_PLAYERS];
    uint16_t      peer_pickup_seen[HTA_NET_MAX_PLAYERS];
    uint16_t      net_melee_count, net_grenade_count, net_reload_count, net_pickup_count;
    hta_net_control net_control;
    uint16_t      world_round;
    uint32_t      world_applied_tick;
    uint32_t      projectile_applied_tick;
    bool          world_local_bound;
    hta_net_stats net_stats_prev;
    double        remote_action_until;
    bool          net_spawned;
    uint32_t      impact_jpt;    /* the held weapon's own damage tag */
    float         melee_damage;  /* the cyborg's, 1000 -- see the handoff */

    hta_ammo ammo;
    float    dry_cooldown;   /* stops an empty trigger clicking every frame */
    bool     hud_reload;
    bool     hud_melee;
    bool     hud_grenade;
    bool          have_mesh;
    bool          have_sky;
    bool          have_coll;
    bool          have_fp;
    hta_weapon_def weap;

    /* runtime */
    hta_gfx      *gfx;
    hta_gfx_mesh *gpu_mesh;
    hta_gfx_mesh *gpu_sky;
    hta_gfx_mesh *gpu_fx;
    hta_gfx_mesh *gpu_proj;
    hta_gfx_mesh *gpu_parts;
    hta_gfx_mesh *gpu_fp;
    hta_hud       hud;
    hta_gfx_mesh *gpu_hud;
    hta_camera    cam;
    hta_player    player;
    hta_gun       gun;
    hta_scene     scene;
    bool          has_window;
    int32_t       win_w, win_h;  /* window size the current swapchain was built for */
    double        last_time;
    uint64_t      frames;
    double        fps_accum;
    uint32_t      fps_frames;
    bool          probe_done;

    /* input */
    int32_t move_pointer, look_pointer;
    float   move_origin[2], move_cur[2];
    float   look_last[2];
    float   pad_move[2], pad_look[2];
    bool    jump_held;
    bool    fire_held;
    bool    pad_fire;
    float   pending_yaw, pending_pitch;

    /* Java HUD (GameActivity). When ready, touch move/jump/fire/look
     * come from JNI instead of hot-corners. */
    bool    hud_ready;
    float   hud_move[2];
    bool    hud_jump, hud_fire, hud_crouch;
} hta_android;

static hta_android *g_android;
/* 0 walk, 1 near a free seat, 2 driving, 3 gunning, 4 riding armed,
 * 5 riding; +16 when the seat has a second trigger. */
static _Atomic int g_vehicle_mode;
static char g_vehicle_text[96];
/* 0..255 edge flash, read by the Java overlay on its own thread. */
static _Atomic int g_damage_flash;
#define HTA_DAMAGE_FLASH_TIME 0.55f
/* 0 solo, 1 joining, 2 in match, 3 hosting alone, 4 hosting with peer,
 * 5 unavailable, 6 connected without match state, 7 map mismatch, 8 full. */
static _Atomic int g_net_status;
/* The pause screen is up: the world holds still and the HUD shows it. */
static _Atomic int g_paused;
/* Which submenu the Java overlay has up: 0 none (the main menu), 1 solo,
 * 2 multiplayer. It decides the camera shot and what BACK does. */
static _Atomic int g_shell_screen;
static hta_shell g_shell;
static _Atomic int g_shell_ready;
static int g_shell_shown = -1;
static void call_activity(hta_android *s, const char *method);
static bool         g_hud_wanted;

/* ---------------------------- asset loading ---------------------------- */

static bool file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* Looks for the user's map.
 *
 * Android 11+ hides <externalDataPath> (/sdcard/Android/data/<pkg>/files) from
 * file managers, so we cannot rely on the user putting files there without adb.
 * We therefore also search ordinary, reachable locations like Download/.
 * Reading those needs "All files access", granted once in
 *   Settings -> Apps -> Halo Trial PoC -> Permissions -> Files and media.
 *
 * A hta_data.txt file in externalDataPath can override the directory entirely. */
#define HTA_MAX_SEARCH_DIRS 12
/* Built-in Trial data. A personal build (publish_apk.sh --with-assets)
 * carries the owner's own maps uncompressed under assets/maps/, and they
 * are mapped straight out of the APK -- no copy, no picker. A path of the
 * form "apk:maps/<name>" names one of those. */
#define HTA_APK_PREFIX "apk:"

static bool apk_has(hta_android *s, const char *name)
{
    AAssetManager *am = s->app->activity->assetManager;
    if (!am) return false;
    AAsset *a = AAssetManager_open(am, name, AASSET_MODE_UNKNOWN);
    if (!a) return false;
    off64_t start = 0, len = 0;
    int fd = AAsset_openFileDescriptor64(a, &start, &len);
    AAsset_close(a);
    if (fd < 0) {
        hta_log("[assets] %s is in the APK but compressed; it cannot be mapped", name);
        return false;
    }
    close(fd);
    return len > 0;
}

/* Maps a data file read-only, from disk or from the APK. */
static bool map_data_file(hta_android *s, const char *path, uint8_t **out, size_t *out_size)
{
    int fd = -1;
    off64_t start = 0, len = 0;
    if (!strncmp(path, HTA_APK_PREFIX, strlen(HTA_APK_PREFIX))) {
        AAssetManager *am = s->app->activity->assetManager;
        AAsset *a = am ? AAssetManager_open(am, path + strlen(HTA_APK_PREFIX),
                                            AASSET_MODE_UNKNOWN) : NULL;
        if (!a) return false;
        fd = AAsset_openFileDescriptor64(a, &start, &len);
        AAsset_close(a);
    } else {
        fd = open(path, O_RDONLY);
        struct stat st;
        if (fd >= 0 && fstat(fd, &st) == 0) len = st.st_size;
    }
    if (fd < 0) return false;
    if (len <= 0 || s->mapped_count >= 8) { close(fd); return false; }
    long page = sysconf(_SC_PAGESIZE);
    off64_t aligned = start - (start % page);
    size_t maplen = (size_t)(len + (start - aligned));
    void *p = mmap(NULL, maplen, PROT_READ, MAP_PRIVATE, fd, aligned);
    close(fd);
    if (p == MAP_FAILED) return false;
    s->mapped_base[s->mapped_count] = p;
    s->mapped_len[s->mapped_count] = maplen;
    s->mapped_count++;
    *out = (uint8_t *)p + (start - aligned);
    *out_size = (size_t)len;
    return true;
}

static bool find_map(hta_android *s)
{
    if (apk_has(s, "maps/bloodgulch.map")) {
        snprintf(s->map_path, sizeof(s->map_path), HTA_APK_PREFIX "maps/bloodgulch.map");
        hta_log("[assets] using the Trial data built into this APK");
        return true;
    }
    const char *ext = s->app->activity->externalDataPath;
    const char *intn = s->app->activity->internalDataPath;
    char dirs[HTA_MAX_SEARCH_DIRS][400];
    int ndirs = 0;

    /* optional override file: one line containing a directory path */
    if (ext) {
        char cfg[512];
        snprintf(cfg, sizeof(cfg), "%s/hta_data.txt", ext);
        FILE *f = fopen(cfg, "r");
        if (f) {
            char line[400];
            if (fgets(line, sizeof(line), f)) {
                size_t n = strlen(line);
                while (n && (line[n-1] == '\n' || line[n-1] == '\r' || line[n-1] == ' ')) line[--n] = 0;
                if (n) { snprintf(dirs[ndirs++], 400, "%s", line);
                         hta_log("[assets] using override directory from hta_data.txt: %s", line); }
            }
            fclose(f);
        }
    }
    /* app-private (works without any permission, but hard to write to) */
    if (ext)  { snprintf(dirs[ndirs++], 400, "%s/maps", ext); snprintf(dirs[ndirs++], 400, "%s", ext); }
    if (intn) snprintf(dirs[ndirs++], 400, "%s", intn);

    /* ordinary user-reachable locations (need All-files access) */
    static const char *public_dirs[] = {
        "/sdcard/halo-trial/maps",
        "/sdcard/halo-trial",
        "/sdcard/Download/halo-trial",
        "/sdcard/Download",
        "/sdcard/Documents",
        "/storage/emulated/0/Download",
        "/sdcard",
    };
    for (unsigned i = 0; i < sizeof(public_dirs)/sizeof(public_dirs[0]) &&
                         ndirs < HTA_MAX_SEARCH_DIRS; i++)
        snprintf(dirs[ndirs++], 400, "%s", public_dirs[i]);

    for (int i = 0; i < ndirs; i++) {
        char cand[520];
        snprintf(cand, sizeof(cand), "%s/bloodgulch.map", dirs[i]);
        if (file_exists(cand)) { snprintf(s->map_path, sizeof(s->map_path), "%s", cand); return true; }
    }
    for (int i = 0; i < ndirs; i++) {
        DIR *d = opendir(dirs[i]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            size_t n = strlen(e->d_name);
            if (n > 4 && strcasecmp(e->d_name + n - 4, ".map") == 0) {
                if (!strcasecmp(e->d_name, "bitmaps.map") ||
                    !strcasecmp(e->d_name, "sounds.map") ||
                    !strcasecmp(e->d_name, "ui.map")) continue;
                char cand[520];
                snprintf(cand, sizeof(cand), "%s/%s", dirs[i], e->d_name);
                if (file_exists(cand)) {
                    snprintf(s->map_path, sizeof(s->map_path), "%s", cand);
                    closedir(d);
                    return true;
                }
            }
        }
        closedir(d);
    }

    snprintf(s->status, sizeof(s->status), "no .map found in any search path");
    hta_log("[assets] ============================================================");
    hta_log("[assets] NO MAP FOUND. Put your own bloodgulch.map in ONE of these:");
    for (int i = 0; i < ndirs; i++) {
        DIR *probe = opendir(dirs[i]);
        hta_log("[assets]   %s  (%s)", dirs[i], probe ? "readable" : "not readable");
        if (probe) closedir(probe);
    }
    hta_log("[assets] Easiest: /sdcard/Download/bloodgulch.map, then grant");
    hta_log("[assets]   Settings > Apps > Halo Trial PoC > Permissions >");
    hta_log("[assets]   Files and media > Allow management of all files");
    hta_log("[assets] ============================================================");
    return false;
}

/* Decode every permutation of a snd! tag once, and remember it. Returns the
 * bank index, or -1. Called from the game thread only. */
static int bank_get(hta_android *s, uint32_t tag_id)
{
    if (!tag_id || tag_id == 0xFFFFFFFFu || !s->audio_ok) return -1;
    for (uint32_t i = 0; i < s->bank_count; i++)
        if (s->bank[i].tag_id == tag_id) return (int)i;
    if (s->bank_count >= HTA_SND_MAX_BANK) {
        /* Never let this starve quietly again: it cost three weapons their
         * sound and looked like a decoder bug. */
        hta_log("[audio] sound bank FULL at %u tags; 0x%08X will be silent",
                s->bank_count, tag_id);
        return -1;
    }

    char err[HTA_ERRLEN] = {0};
    hta_sound_info info;
    if (!hta_sound_info_load(&s->cache, tag_id, &info, err, sizeof(err))) {
        hta_log("[audio] snd! 0x%08X: %s", tag_id, err);
        return -1;
    }
    uint32_t idx = s->bank_count;
    s->bank[idx].tag_id = tag_id;
    s->bank[idx].count = 0;
    uint32_t want = info.permutations;
    if (want > HTA_SND_MAX_PERMS) want = HTA_SND_MAX_PERMS;
    for (uint32_t p = 0; p < want; p++) {
        hta_pcm pcm;
        if (!hta_sound_decode(&s->cache, &s->sounds_rm, tag_id, p, &pcm,
                              err, sizeof(err))) {
            /* Ogg permutations land here; the rest of the tag still plays. */
            hta_log("[audio] snd! 0x%08X perm %u: %s", tag_id, p, err);
            continue;
        }
        uint32_t clip = hta_audio_add_clip(&s->audio, pcm.samples, pcm.frame_count,
                                           pcm.sample_rate, pcm.channels);
        if (clip == HTA_AUDIO_NO_CLIP) {
            hta_log("[audio] clip table FULL; 0x%08X keeps %u of %u "
                    "permutation(s)", tag_id, s->bank[idx].count, want);
            hta_pcm_free(&pcm);
            break;
        }
        uint32_t k = s->bank[idx].count++;
        s->bank[idx].clip[k] = clip;
        s->bank[idx].pcm[k] = pcm.samples;   /* the mixer holds this pointer */
    }
    if (!s->bank[idx].count) return -1;
    s->bank_count++;
    hta_log("[audio] snd! 0x%08X ready: %u permutation(s)", tag_id, s->bank[idx].count);
    return (int)idx;
}

/* Halo picks between a sound's permutations rather than repeating one. */
static void play_tag(hta_android *s, uint32_t tag_id, float gain)
{
    int b = bank_get(s, tag_id);
    if (b < 0) return;
    uint32_t n = s->bank[b].count;
    s->rng = s->rng * 1664525u + 1013904223u;
    uint32_t pick = n > 1 ? (s->rng >> 16) % n : 0u;
    hta_audio_play(&s->audio, s->bank[b].clip[pick], gain);
}

/* How hard a grenade is thrown.
 *
 * INVENTED -- the fourth number in this project that is. The frag
 * grenade's own projectile tag says an initial velocity of 0.00, because
 * in Halo the throw comes from the player rather than the tag, and the
 * player's throw strength is an engine constant that is not in the data.
 * Nine world units a second puts one about twenty-five out on a flat
 * throw, which is roughly the distance it goes in the real game. */
#define HTA_GRENADE_THROW 9.0f

/* How far a world sound carries.
 *
 * INVENTED, and the third number in this project that is. Halo keeps a
 * minimum and maximum distance on every `snd!` (at +8 and +12) -- and in
 * the Trial every single one of them reads 0.0 .. 0.0, because the real
 * values live in per-CLASS defaults inside the engine rather than in the
 * data. The tag's `sound class` field is set (weapon fire 4, projectile
 * impact 0, object impacts 13) but the ranges those map to are not
 * shippable data we have.
 *
 * So: full volume within three world units, inverse falloff after that,
 * silent at sixty -- which is about the length of Blood Gulch. If the
 * class ranges ever turn up, these two lines are what to replace. */
#define HTA_SOUND_NEAR  3.0f
#define HTA_SOUND_FAR  60.0f

/* A sound that happens somewhere in the world rather than in your hands:
 * quieter with distance, and placed left or right of where you are
 * looking. */
static void play_tag_at(hta_android *s, uint32_t tag_id, const float at[3],
                        float gain)
{
    if (!tag_id || !at) return;
    float d[3] = { at[0] - s->cam.pos[0],
                   at[1] - s->cam.pos[1],
                   at[2] - s->cam.pos[2] };
    float dist = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (dist >= HTA_SOUND_FAR) return;               /* too far to hear */

    float g = gain;
    if (dist > HTA_SOUND_NEAR) {
        g *= (HTA_SOUND_NEAR / dist);                /* inverse falloff */
        g *= (1.0f - dist / HTA_SOUND_FAR);          /* and reach zero cleanly */
    }
    if (g <= 0.001f) return;

    float pan = 0.0f;
    if (dist > 0.01f) {
        float right[3];
        hta_camera_right(&s->cam, right);
        pan = (d[0]*right[0] + d[1]*right[1] + d[2]*right[2]) / dist;
    }

    int b = bank_get(s, tag_id);
    if (b < 0) return;
    uint32_t n = s->bank[b].count;
    s->rng = s->rng * 1664525u + 1013904223u;
    uint32_t pick = n > 1 ? (s->rng >> 16) % n : 0u;
    hta_audio_play_pan(&s->audio, s->bank[b].clip[pick], g, pan);
}

/* The one continuous voice we keep; any non-zero id would do. */
#define HTA_LOOP_FIRE 1u

/* The unit HUD's own sounds. Halo hangs these off the `unhi`, latched to a
 * condition each: the shield charging back up, the hit that broke it, the
 * warning tones. Loop ids of their own so they can sound together -- the
 * heartbeat under the recharge hum is exactly right. */
#define HTA_LOOP_SHIELD_CHARGE  2u
#define HTA_LOOP_SHIELD_LOW     3u
#define HTA_LOOP_HEALTH_LOW     4u

/* When "low" starts. Halo has no threshold for this in any tag -- the HUD
 * flashes and the heartbeat starts on a hardcoded fraction -- so this one
 * is ours. A quarter left is about where the real game begins to nag. */
#define HTA_VITALS_LOW  0.25f

/* Holds one of those loops while its condition lasts. Safe to call every
 * frame: the mixer leaves a running loop alone rather than restarting it. */
static void hud_loop(hta_android *s, uint32_t loop_id, uint32_t snd, bool on,
                     bool *state)
{
    if (!snd) return;
    if (on) {
        int b = bank_get(s, snd);
        if (b < 0) return;
        hta_audio_loop(&s->audio, loop_id, s->bank[b].clip[0], 1.0f);
        *state = true;
    } else if (*state) {
        hta_audio_loop_stop(&s->audio, loop_id);
        *state = false;
    }
}

/* Starts or stops the weapon's continuous firing sound. Calling this every
 * frame while the trigger is held is the intended use -- the mixer leaves a
 * running loop alone rather than restarting it. */
static void fire_loop(hta_android *s, bool on)
{
    if (!s->fire_loop_snd) return;
    if (on) {
        int b = bank_get(s, s->fire_loop_snd);
        if (b < 0) return;
        hta_audio_loop(&s->audio, HTA_LOOP_FIRE, s->bank[b].clip[0],
                       s->fire_loop_gain);
        s->fire_loop_on = true;
    } else if (s->fire_loop_on) {
        hta_audio_loop_stop(&s->audio, HTA_LOOP_FIRE);
        s->fire_loop_on = false;
    }
}

/* Magnification at a zoom level. Halo spreads the tag's first and last
 * magnification evenly across however many levels the weapon has, so the
 * sniper's two become 2x and 8x and the pistol's single one is just 2x. */
static float zoom_magnification(const hta_weapon_def *w, int level)
{
    if (!w || level <= 0 || level > w->zoom_levels) return 1.0f;
    if (w->zoom_levels == 1) return w->zoom_mag[0] > 1.0f ? w->zoom_mag[0] : 1.0f;
    float t = (float)(level - 1) / (float)(w->zoom_levels - 1);
    float m = w->zoom_mag[0] + (w->zoom_mag[1] - w->zoom_mag[0]) * t;
    return m > 1.0f ? m : 1.0f;
}

/* The player writes the camera's field of view every update, so the zoom has
 * to live there rather than being poked into the camera -- doing that gave
 * exactly one zoomed frame before the next update put it back. */
static void apply_zoom(hta_android *s)
{
    hta_player_set_zoom(&s->player, zoom_magnification(&s->weap, s->zoom_level));
    /* And the scope furniture: the sniper's brackets and reticle ticks are
     * per zoom level in its HUD tag. */
    hta_hud_set_zoom(&s->hud, s->zoom_level);
}

/* Step to the next zoom level, wrapping back to none. Weapons the tag gives
 * no zoom simply have nothing to step through. */
static void cycle_zoom(hta_android *s)
{
    if (s->weap.zoom_levels <= 0) return;
    int was = s->zoom_level;
    s->zoom_level = (s->zoom_level + 1) % (s->weap.zoom_levels + 1);
    apply_zoom(s);
    uint32_t snd = s->zoom_level ? s->weap.zoom_in_snd_id : s->weap.zoom_out_snd_id;
    (void)was;
    play_tag(s, snd, 1.0f);
    hta_log("[weapon] zoom %dx", (int)zoom_magnification(&s->weap, s->zoom_level));
}

/* Put a weapon in the player's hands.
 *
 * Everything the game shows and hears about a weapon comes from its own
 * tags, so swapping means rebuilding all of it: the first-person model and
 * its animation graph, the muzzle flash and the on-gun counter that hang
 * off that model, the magazine, the rate of fire and the error cone, the
 * firing and dry-fire sounds, and the HUD's crosshair and ammo block.
 *
 * The GPU meshes are freed and re-uploaded, so this must not run while a
 * frame is in flight -- it is called from the game thread between frames.
 */
/* Overshield stacks on top of your own, and drains back down over the time
 * the equipment tag says -- 60 s for the overshield, 45 for camouflage. The
 * MULTIPLIER is ours: Halo's overshield is famously "three bars", and the
 * tag says how long it lasts but not how much it gives. */
#define HTA_OVERSHIELD_MULT 3.0f

/* How much of a blast reaches a point: full inside the core, tapering to
 * nothing at the edge. The player already had this inline twice; a rocket
 * and a grenade now have to ask it about a body as well. */
static float blast_falloff(const float centre[3], const float at[3],
                           float core, float radius)
{
    float dx = at[0] - centre[0];
    float dy = at[1] - centre[1];
    float dz = at[2] - centre[2];
    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
    if (dist >= radius) return 0.0f;
    if (dist <= core || radius <= core) return 1.0f;
    return 1.0f - (dist - core) / (radius - core);
}

/* How far a swing reaches, in world units -- about a metre and a half.
 * Ours: no tag carries a melee range, only what the blow does. */
#define HTA_MELEE_REACH 0.5f

/* Halo's multiplayer respawn. The five seconds are the GAMETYPE's, not any
 * tag's -- no gametype ships inside a map -- so this one number is ours. The
 * fade is shaped around it: black by the time the body has settled, black
 * while you wait, and open again as you come back. */
#define HTA_RESPAWN_DELAY   5.0f
#define HTA_RESPAWN_MIN     1.5f   /* "INSTANT": the fall, then the fade. Ours */
/* The black comes at the END, not the start. Fading out as you die would
 * mean the body you have just been shown is on screen for half a second
 * before the screen swallows it; Halo lets you watch the whole time and
 * only closes the shot to cover the respawn. */
#define HTA_DEATH_FADE_OUT  0.8f   /* seconds of black BEFORE coming back */
#define HTA_DEATH_FADE_IN   0.6f   /* and back, once you are standing */
/* How far the camera sinks as the body goes down, in world units. The
 * Trial's cyborg stands with its eye 0.62 above its feet. */
#define HTA_DEATH_EYE_DROP  0.45f
/* Where the camera goes to watch. World units: 1 wu is 3.05 m, so this
 * settles about 6 m behind the body and 2.5 m above it, aimed at its chest.
 * Halo's own death camera is closer than that, but Halo's is looking at a
 * ragdoll that is still moving -- ours holds the last frame of a kill
 * animation, and a little distance is kinder to it. */
#define HTA_DEATH_CAM_BACK  2.0f
#define HTA_DEATH_CAM_UP    0.8f
#define HTA_DEATH_LOOK_AT   0.35f   /* up the body from its feet, to the chest */
#define HTA_DEATH_PULLBACK  1.2f    /* seconds for the camera to get there */

static void equip_weapon(hta_android *s, uint32_t weap_tag_id);
static int32_t held_roster(hta_android *s);

static void respawn(hta_android *s)
{
    if (!s->spawn_count) return;
    uint32_t i = hta_scenario_spawn_pick(s->spawn, s->spawn_count,
                                         s->death_pos, &s->spawn_rng);
    hta_spawn_point chosen = s->spawn[i];
    /* In a game, away from the people trying to kill you. */
    if (s->game_on)
        hta_game_pick_spawn(&s->game, s->me, chosen.position, &chosen.facing);
    hta_player_spawn(&s->player, &chosen);
    float gz;
    if (s->col.built &&
        hta_collision_ground(&s->col, s->player.pos[0], s->player.pos[1],
                             s->player.pos[2] + 8.0f, &gz)) {
        s->player.pos[2] = gz;
        s->player.on_ground = true;
    }
    s->cam.yaw = chosen.facing;
    s->cam.pitch = 0.0f;

    hta_vitals_reset(s->vit);
    /* You come back with what the map arms you with, not with whatever you
     * had scavenged. */
    /* Every slot, not just the first: a weapon picked up (or handed over by
     * the debug pad) into the second hand used to survive a death. */
    bool rearm = s->start_count && s->held_count != s->start_count;
    for (uint32_t k = 0; s->start_count && !rearm && k < s->start_count; k++)
        if (s->held[k] != s->start_weapon[k]) rearm = true;
    if (s->start_count && s->held_slot != 0) rearm = true;
    if (rearm) {
        s->held_count = s->start_count;
        for (uint32_t i = 0; i < s->start_count; i++) s->held[i] = s->start_weapon[i];
        s->held_slot = 0;
        equip_weapon(s, s->held[0]);
    }
    /* A fresh magazine and a full reserve, and the weapon comes up unzoomed
     * with its idle pose rather than mid-reload. */
    hta_ammo_init(&s->ammo, &s->weap);
    for (uint32_t k = 0; k < HTA_CARRY_MAX; k++) s->held_ammo_set[k] = false;
    s->zoom_level = 0;
    apply_zoom(s);
    fire_loop(s, false);
    if (s->vm.loaded) hta_viewmodel_play(&s->vm, HTA_VM_IDLE);
    s->nade_count = s->nade_max;

    s->corpse_up = false;
    if (s->game_on) {
        hta_game_revive(&s->game, s->me);
        hta_game_sync_local(&s->game, &s->player, &s->cam, held_roster(s));
    }
    hta_log("[player] respawned at spawn %u (%.2f %.2f %.2f)",
            i, s->player.pos[0], s->player.pos[1], s->player.pos[2]);
}

static void equip_weapon(hta_android *s, uint32_t weap_tag_id)
{
    if (!weap_tag_id) return;
    char err[HTA_ERRLEN] = {0};
    hta_resource_map *bm = s->bitmaps_ok ? &s->bitmaps_rm : NULL;

    hta_weapon_def def;
    if (!hta_weapon_load_id(&s->cache, bm, weap_tag_id, &def, NULL, err, sizeof(err))) {
        hta_log("[weapon] cannot equip 0x%08X: %s", weap_tag_id, err);
        return;
    }
    s->weap = def;

    s->gun.fire_interval = s->weap.cooldown;
    hta_gun_set_error(&s->gun, s->weap.error_angle,
                      s->weap.error_accel, s->weap.error_decel);

    /* The gunshot is not on the weapon: it hangs off the trigger's firing
     * effect, among that effect's parts. */
    s->fire_snd = hta_effect_first_sound(&s->cache, s->weap.firing_fx_id);
    if (s->fire_snd) bank_get(s, s->fire_snd);

    /* The flamethrower's firing effect has no sound in it at all: its roar
     * is a looping sound attached to the weapon OBJECT's `primary trigger`.
     * Only reach for that when the effect gives us nothing, because the
     * plasma pistol hangs its overcharge whine on the same marker. */
    fire_loop(s, false);
    s->fire_loop_snd = 0;
    s->fire_loop_gain = 1.0f;
    if (!s->fire_snd) {
        hta_loop_sound ls;
        if (hta_object_loop_sound(&s->cache, weap_tag_id, "primary trigger", &ls)) {
            s->fire_loop_snd = ls.loop ? ls.loop : ls.start;
            s->fire_loop_gain = ls.gain;
            if (s->fire_loop_snd) {
                bank_get(s, s->fire_loop_snd);
                hta_log("[weapon] continuous firing sound 0x%08X", s->fire_loop_snd);
            }
        }
    }
    s->empty_snd = hta_effect_first_sound(&s->cache, s->weap.empty_fx_id);
    if (s->empty_snd) bank_get(s, s->empty_snd);
    /* Decoding a sound takes long enough to be a visible hitch, so the zoom
     * sounds are decoded on equip rather than on the first press. */
    if (s->weap.zoom_in_snd_id)  bank_get(s, s->weap.zoom_in_snd_id);
    if (s->weap.zoom_out_snd_id) bank_get(s, s->weap.zoom_out_snd_id);
    /* Impacts are this projectile's, so forget the last weapon's. */
    memset(s->impact_known, 0, sizeof(s->impact_known));
    /* And what a round of it does to a man. */
    s->impact_jpt = hta_projectile_impact_damage(&s->cache, s->weap.projectile_id);

    /* And the art its marks are drawn with. A rocket chars; a rifle leaves
     * a hole. Halo hangs the decal off the impact effect, so take the first
     * material response that names one -- the marks were a flat 1x1 square
     * until now. */
    {
        uint32_t decal = s->proj.decal_id;
        if (!decal) {
            for (uint8_t m = 0; m < 33u && !decal; m++) {
                uint32_t fx = hta_projectile_response_effect(&s->cache,
                                                             s->weap.projectile_id, m);
                if (fx) hta_effect_detonation(&s->cache, fx, NULL, NULL, &decal);
            }
        }
        uint32_t bm = hta_decal_bitmap(&s->cache, decal);
        hta_bitmap img;
        memset(&img, 0, sizeof(img));
        char derr[HTA_ERRLEN];
        if (bm && hta_bitmap_decode(&s->cache, s->bitmaps_ok ? &s->bitmaps_rm : NULL,
                                    bm, 0, &img, derr, sizeof(derr))) {
            hta_gun_set_decal(&s->gun, img.rgba, img.width, img.height);
            hta_log("[weapon] impact decal %ux%u", img.width, img.height);
            hta_bitmap_free(&img);
        } else {
            hta_gun_set_decal(&s->gun, NULL, 0, 0);
        }
    }

    /* The weapon's own round, if it is an object rather than a particle.
     * Most of the roster has nothing to draw, which is not a failure. */
    if (s->gpu_nades) { hta_gfx_mesh_free(s->gfx, s->gpu_nades); s->gpu_nades = NULL; }
    if (s->gpu_parts) { hta_gfx_mesh_free(s->gfx, s->gpu_parts); s->gpu_parts = NULL; }
    if (s->gpu_proj) { hta_gfx_mesh_free(s->gfx, s->gpu_proj); s->gpu_proj = NULL; }
    {
        char perr[HTA_ERRLEN];
        if (hta_projectiles_equip(&s->proj, &s->cache,
                                  s->bitmaps_ok ? &s->bitmaps_rm : NULL,
                                  &s->weap, perr, sizeof(perr))) {
            hta_log("[weapon] projectile: %u verts, %.1f wu/s, range %.0f, "
                    "blast %.2f", s->proj.verts_each, s->proj.speed_initial,
                    s->proj.range, s->proj.blast_radius);
            if (s->proj.detonation_snd) bank_get(s, s->proj.detonation_snd);
            if (s->gfx)
                s->gpu_proj = hta_gfx_mesh_upload_dynamic_world(s->gfx, &s->proj.mesh,
                                                          perr, sizeof(perr));
        }
        /* Everything this weapon will ever throw: its detonation, and one
         * impact effect per material the MAP actually contains. Built once
         * here, because interning a texture mid-game would move the mesh
         * under the buffer the GPU is reading. */
        if (s->gpu_parts) {
            hta_gfx_mesh_free(s->gfx, s->gpu_parts);
            s->gpu_parts = NULL;
        }
        hta_particles_free(&s->parts);
        hta_particles_init(&s->parts);
        s->det_recipe = HTA_PART_NO_RECIPE;
        s->casing_recipe = HTA_PART_NO_RECIPE;
        for (uint32_t m = 0; m < 33u; m++) s->impact_recipe[m] = HTA_PART_NO_RECIPE;

        const hta_resource_map *pbm = s->bitmaps_ok ? &s->bitmaps_rm : NULL;
        if (s->proj.det_effect)
            s->det_recipe = hta_particles_add(&s->parts, &s->cache, pbm,
                                              s->proj.det_effect);
        for (uint32_t k = 0; k < s->map_material_count; k++) {
            uint8_t m = s->map_material[k];
            uint32_t fx = hta_projectile_response_effect(&s->cache,
                                                         s->weap.projectile_id, m);
            if (fx) s->impact_recipe[m] = hta_particles_add(&s->parts, &s->cache,
                                                            pbm, fx);
        }
        /* The brass. A weapon's firing effect carries its muzzle flashes
         * AND its ejected casing; the flash is already drawn by the
         * viewmodel, so only the particles on `primary ejection` are
         * taken. The covenant weapons have none, which is correct. */
        if (s->weap.firing_fx_id)
            s->casing_recipe = hta_particles_add_marker(&s->parts, &s->cache, pbm,
                                                        s->weap.firing_fx_id,
                                                        "primary ejection");
        /* A continuous weapon sprays a particle SYSTEM rather than firing
         * a burst: the flamethrower's jet is a `pctl` on its `spawn fire`
         * marker, at the speed of the flame projectile it also launches. */
        s->jet_recipe = HTA_PART_NO_RECIPE;
        {
            uint32_t pctl = hta_object_attachment(&s->cache, weap_tag_id,
                                                  "spawn fire",
                                                  HTA_FOURCC('p','c','t','l'));
            if (pctl) {
                float jet = s->proj.speed_initial > 0.0f ? s->proj.speed_initial
                                                         : 3.0f;
                s->jet_recipe = hta_particles_add_system(&s->parts, &s->cache,
                                                         pbm, pctl, jet);
                if (s->jet_recipe != HTA_PART_NO_RECIPE)
                    hta_log("[weapon] continuous jet 0x%08X at %.1f wu/s",
                            pctl, (double)jet);
            }
        }

        /* The grenade blast, which is the same whatever you are holding. */
        s->nade_recipe = HTA_PART_NO_RECIPE;
        if (s->nades.det_effect)
            s->nade_recipe = hta_particles_add(&s->parts, &s->cache, pbm,
                                               s->nades.det_effect);
        if (s->nade_snd) bank_get(s, s->nade_snd);
        /* The vehicle guns' own muzzle flashes and brass. */
        for (uint32_t w = 0; w < HTA_GAME_MAX_WEAPONS; w++) {
            s->vfire_recipe[w] = HTA_PART_NO_RECIPE;
            if (s->game_on && w < s->game.weapon_count && s->game.weapons[w].vehicle &&
                s->game.weapons[w].def.firing_fx_id)
                s->vfire_recipe[w] = hta_particles_add(&s->parts, &s->cache, pbm,
                                                       s->game.weapons[w].def.firing_fx_id);
        }
        /* A wreck: the tank shell's explosion, thrown twice over. And the
         * sparks a hull on its last legs gives off: the chaingun's own
         * impact on thick metal. */
        s->wreck_recipe = s->spark_recipe = HTA_PART_NO_RECIPE;
        s->wreck_snd = 0;
        if (s->game_on && s->game.wreck_effect) {
            s->wreck_recipe = hta_particles_add(&s->parts, &s->cache, pbm, s->game.wreck_effect);
            float r;
            uint32_t deca;
            hta_effect_detonation(&s->cache, s->game.wreck_effect, &s->wreck_snd, &r, &deca);
            if (s->wreck_snd) bank_get(s, s->wreck_snd);
            uint32_t sparks = 0;
            for (uint32_t w = 0; w < s->game.weapon_count && !sparks; w++)
                if (s->game.weapons[w].vehicle && !s->game.weapons[w].travels)
                    sparks = hta_projectile_response_effect(&s->cache,
                        s->game.weapons[w].def.projectile_id, HTA_HULL_MATERIAL);
            if (sparks) s->spark_recipe = hta_particles_add(&s->parts, &s->cache, pbm, sparks);
        }
        /* Rounds into bodies: decoded now rather than on the first hit. */
        for (uint32_t w = 0; s->game_on && w < s->game.weapon_count; w++)
            for (uint8_t m = HTA_MATERIAL_CYBORG_ARMOR; m <= HTA_MATERIAL_CYBORG_SHIELD; m++) {
                uint32_t snd = s->game.weapons[w].def.projectile_id
                    ? hta_projectile_impact_sound(&s->cache, s->game.weapons[w].def.projectile_id, m) : 0;
                if (snd) bank_get(s, snd);
            }
        /* And whatever the bots' rounds throw when they go off. */
        for (uint32_t p = 0; p < HTA_GAME_MAX_POOLS; p++) {
            s->pool_recipe[p] = HTA_PART_NO_RECIPE;
            if (s->game_on && p < s->game.pool_count && s->game.pools[p].det_effect)
                s->pool_recipe[p] = hta_particles_add(&s->parts, &s->cache, pbm,
                                                      s->game.pools[p].det_effect);
        }
        if (hta_particles_build(&s->parts, perr, sizeof(perr))) {
            hta_log("[weapon] particles: %u type(s), %u recipe(s)",
                    s->parts.type_count, s->parts.recipe_count);
            if (s->gfx)
                s->gpu_parts = hta_gfx_mesh_upload_dynamic(s->gfx, &s->parts.mesh,
                                                           perr, sizeof(perr));
        }
    }

    hta_ammo_init(&s->ammo, &s->weap);
    /* A new weapon comes up unzoomed, and takes its own field of view. */
    s->zoom_level = 0;
    apply_zoom(s);

    /* Rebuild the viewmodel and everything hanging off it. */
    if (s->gpu_fp) { hta_gfx_mesh_free(s->gfx, s->gpu_fp); s->gpu_fp = NULL; }
    hta_viewmodel_free(&s->vm);
    s->have_fp = false;
    if (hta_viewmodel_load(&s->vm, &s->cache, bm, &s->weap, err, sizeof(err))) {
        s->have_fp = true;
        if (s->gfx)
            s->gpu_fp = hta_gfx_mesh_upload_dynamic(s->gfx, &s->vm.mesh, err, sizeof(err));
    } else {
        hta_log("[weapon] viewmodel: %s", err);
    }

    /* And the HUD, whose crosshair and ammo block are this weapon's. */
    if (s->gpu_hud) { hta_gfx_mesh_free(s->gfx, s->gpu_hud); s->gpu_hud = NULL; }
    hta_hud_free(&s->hud);
    hta_hud_load(&s->hud, &s->cache, bm, &s->weap, err, sizeof(err));
    hta_hud_set_shield(&s->hud, 1.0f);
    hta_hud_set_health(&s->hud, 1.0f);
    if (s->gfx && s->hud.elem_count)
        s->gpu_hud = hta_gfx_mesh_upload_dynamic(s->gfx, &s->hud.mesh, err, sizeof(err));

    hta_log("[weapon] %s: ROF %.1f/s  mag %d/%d  spread %.1f-%.1f deg  %s  %s  %s",
            s->weap.path, s->weap.rof, s->ammo.loaded, s->ammo.reserve,
            s->weap.error_angle[0] * 57.2957795f,
            s->weap.error_angle[1] * 57.2957795f,
            s->have_fp ? "viewmodel" : "NO viewmodel",
            s->vm.have_flash ? "flash" : "no flash",
            s->hud.have_cross ? "crosshair" : "no crosshair");
}

/* What the round hit. Halo keeps one response per material on the
 * projectile itself, each naming the effect -- so a bullet into sand and a
 * bullet into a base wall are the weapon's own two sounds, not one of
 * ours. */
static void play_impact_at(hta_android *s, uint8_t material, const float at[3])
{
    if (material >= 33u) return;
    if (!s->impact_known[material]) {
        s->impact_snd[material] =
            hta_projectile_impact_sound(&s->cache, s->weap.projectile_id, material);
        s->impact_known[material] = 1;
    }
    if (s->impact_snd[material]) play_tag_at(s, s->impact_snd[material], at, 0.8f);
}


/* The footstep for what you are standing on. Halo keeps these in the
 * biped's own `foot` tag, one sound per material, and plenty of materials
 * have none -- silence is the right answer there, so remember that too. */
static void play_footstep(hta_android *s, uint8_t material)
{
    if (material >= 33u || !s->player.phys.footsteps_id) return;
    if (!s->foot_known[material]) {
        s->foot_known[material] = 1;
        s->foot_snd[material] =
            hta_material_effect_sound(&s->cache, s->player.phys.footsteps_id, 0u, material);
        if (s->foot_snd[material]) bank_get(s, s->foot_snd[material]);
    }
    if (s->foot_snd[material]) play_tag(s, s->foot_snd[material], 0.7f);
}


static bool find_named(hta_android *s, const char *name, char *out, size_t outlen)
{
    if (!strncmp(s->map_path, HTA_APK_PREFIX, strlen(HTA_APK_PREFIX))) {
        char asset[128];
        snprintf(asset, sizeof(asset), "maps/%s", name);
        if (apk_has(s, asset)) { snprintf(out, outlen, HTA_APK_PREFIX "%s", asset); return true; }
    }
    /* Reuse the map search directories: same folder as the cache, plus Download. */
    char dir[512];
    if (s->map_path[0]) {
        snprintf(dir, sizeof(dir), "%s", s->map_path);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = 0;
            char cand[520];
            snprintf(cand, sizeof(cand), "%s/%s", dir, name);
            if (file_exists(cand)) { snprintf(out, outlen, "%s", cand); return true; }
        }
    }
    static const char *public_dirs[] = {
        "/sdcard/halo-trial/maps", "/sdcard/halo-trial",
        "/sdcard/Download/halo-trial", "/sdcard/Download",
        "/storage/emulated/0/Download",
    };
    for (unsigned i = 0; i < sizeof(public_dirs)/sizeof(public_dirs[0]); i++) {
        char cand[520];
        snprintf(cand, sizeof(cand), "%s/%s", public_dirs[i], name);
        if (file_exists(cand)) { snprintf(out, outlen, "%s", cand); return true; }
    }
    const char *ext = s->app->activity->externalDataPath;
    if (ext) {
        char cand[520];
        snprintf(cand, sizeof(cand), "%s/%s", ext, name);
        if (file_exists(cand)) { snprintf(out, outlen, "%s", cand); return true; }
    }
    return false;
}

static void start_game(hta_android *s);
static void start_gfx(hta_android *s);
static void stop_gfx(hta_android *s);
static void rebuild_gfx_if_size_changed(hta_android *s);
static void game_gpu_upload(hta_android *s);
static void game_gpu_free(hta_android *s);

static bool load_map(hta_android *s)
{
    if (!find_map(s)) return false;
    hta_log("[assets] found %s", s->map_path);

    /* mmap the cache read-only: no copy, and the OS pages it in lazily */
    if (!map_data_file(s, s->map_path, &s->map_data, &s->map_size)) {
        snprintf(s->status, sizeof(s->status), "cannot map %s", s->map_path);
        return false;
    }

    char err[HTA_ERRLEN];
    double t0 = hta_time_seconds();
    if (!hta_cache_open(&s->cache, s->map_data, s->map_size, err, sizeof(err))) {
        hta_log("[assets] cache rejected: %s", err);
        snprintf(s->status, sizeof(s->status), "bad cache: %s", err);
        return false;
    }
    hta_log("[assets] %s | engine %u (%s) | %u tags | %s layout",
            s->cache.name, s->cache.engine, hta_engine_name(s->cache.engine),
            s->cache.tag_count, s->cache.is_demo_layout ? "Trial" : "retail");

    if (!hta_bsp_load_first(&s->cache, &s->mesh, err, sizeof(err))) {
        hta_log("[assets] BSP extraction failed: %s", err);
        snprintf(s->status, sizeof(s->status), "bsp: %s", err);
        return false;
    }
    double t1 = hta_time_seconds();
    hta_log("[assets] BSP: %u verts, %u tris, %u submeshes in %.1f ms",
            s->mesh.vertex_count, s->mesh.index_count / 3, s->mesh.submesh_count,
            (t1 - t0) * 1000.0);
    hta_log("[assets] bounds (%.2f %.2f %.2f) .. (%.2f %.2f %.2f)",
            s->mesh.bounds_min[0], s->mesh.bounds_min[1], s->mesh.bounds_min[2],
            s->mesh.bounds_max[0], s->mesh.bounds_max[1], s->mesh.bounds_max[2]);

    /* sounds.map, same external-resource pattern as bitmaps but type 2. */
    if (s->sounds_data || find_named(s, "sounds.map", s->sounds_path, sizeof(s->sounds_path))) {
        {
            /* The menu may have mapped it already. */
            if (s->sounds_data ||
                map_data_file(s, s->sounds_path, &s->sounds_data, &s->sounds_size)) {
                if (hta_resource_open_typed(&s->sounds_rm, s->sounds_data, s->sounds_size,
                                            HTA_RESOURCE_SOUNDS, err, sizeof(err)))
                    hta_log("[assets] sounds.map %zu bytes from %s",
                            s->sounds_size, s->sounds_path);
                else
                    hta_log("[assets] sounds.map rejected: %s", err);
            }
        }
    } else {
        hta_log("[assets] no sounds.map -- the game will be silent. Copy it next to bloodgulch.map");
    }

    /* Kept on the state: swapping weapons re-decodes their art, so the
     * resource map has to outlive load_map. */
    memset(&s->bitmaps_rm, 0, sizeof(s->bitmaps_rm));
    if (s->bitmaps_data || find_named(s, "bitmaps.map", s->bitmaps_path, sizeof(s->bitmaps_path))) {
        {
            if (s->bitmaps_data ||
                map_data_file(s, s->bitmaps_path, &s->bitmaps_data, &s->bitmaps_size)) {
                if (hta_resource_open(&s->bitmaps_rm, s->bitmaps_data, s->bitmaps_size, err, sizeof(err)))
                    hta_log("[assets] bitmaps.map %zu bytes from %s", s->bitmaps_size, s->bitmaps_path);
                else
                    hta_log("[assets] bitmaps.map rejected: %s", err);
            }
        }
    } else {
        hta_log("[assets] no bitmaps.map — world will stay untextured. Copy it next to bloodgulch.map");
    }
    if (!hta_bsp_load_textures(&s->cache, s->bitmaps_rm.data ? &s->bitmaps_rm : NULL, &s->mesh, err, sizeof(err)))
        hta_log("[assets] texture load: %s", err);
    else
        hta_log("[assets] textures: %u unique (albedos+lightmaps)", s->mesh.texture_count);

    if (hta_vehicles_load(&s->vehicles, &s->cache,
            s->bitmaps_rm.data ? &s->bitmaps_rm : NULL, err, sizeof(err)))
        hta_log("[vehicles] %s", err);

    if (hta_bsp_load_collision(&s->cache, &s->coll_mesh, err, sizeof(err))) {
        s->have_coll = true;
        if (hta_scenario_add_collision_excluding(&s->coll_mesh, &s->cache,
                s->vehicles.skip, HTA_VEHICLE_PLACEMENTS, err, sizeof(err)))
            hta_log("[assets] %s", err);
        if (!hta_collision_build(&s->col, &s->coll_mesh))
            hta_log("[assets] collision BSP grid failed; %s", err);
        else
            hta_log("[assets] collision BSP %u verts / %u tris, grid %ux%u",
                    s->coll_mesh.vertex_count, s->coll_mesh.index_count / 3,
                    s->col.nx, s->col.ny);
    } else {
        hta_log("[assets] collision BSP: %s — using render mesh", err);
        if (!hta_collision_build(&s->col, &s->mesh))
            hta_log("[assets] collision grid failed to build; player will free-fly");
        else
            hta_log("[assets] collision grid %ux%u cells (render mesh)", s->col.nx, s->col.ny);
    }

    /* What the map leaves lying about. Every position, facing, respawn time
     * and weighted choice is the scenario's own. */
    if (hta_pickups_load(&s->items, &s->cache)) {
        char ierr[HTA_ERRLEN];
        if (hta_pickups_build(&s->items, &s->cache,
                              s->bitmaps_rm.data ? &s->bitmaps_rm : NULL,
                              ierr, sizeof(ierr)))
            hta_log("[items] %s", ierr);
        else
            hta_log("[items] %u placement(s) but no geometry: %s",
                    s->items.count, ierr);
    }

    /* Which materials this map is actually made of. Blood Gulch is four:
     * sand, stone, thick metal and a little plastic. Knowing them turns 33
     * impact effects per weapon into four, which is what makes per-material
     * impact particles affordable at all. */
    s->map_material_count = 0;
    if (s->col.built && s->col.tri_material) {
        uint8_t seen[33];
        memset(seen, 0, sizeof(seen));
        for (uint32_t i = 0; i < s->col.tri_count; i++) {
            uint8_t m = s->col.tri_material[i];
            if (m < 33u) seen[m] = 1;
        }
        for (uint32_t m = 0; m < 33u; m++)
            if (seen[m] && s->map_material_count < 33u)
                s->map_material[s->map_material_count++] = (uint8_t)m;
        hta_log("[assets] %u material(s) in this map", s->map_material_count);
    }

    if (hta_scenario_add_objects_excluding(&s->mesh, &s->cache, s->bitmaps_rm.data ? &s->bitmaps_rm : NULL,
            s->vehicles.skip, HTA_VEHICLE_PLACEMENTS, err, sizeof(err)))
        hta_log("[assets] %s  (now %u verts / %u submeshes)", err,
                s->mesh.vertex_count, s->mesh.submesh_count);
    if (!s->have_coll)
        hta_collision_rebind(&s->col, s->mesh.vertices, s->mesh.indices);

    /* Vehicles are placed rigid grids: everything that asks the world a
     * question -- feet, bullets, grenades, cameras -- sees them where they
     * are now, and moving one costs a matrix. */
    if (s->vehicles.loaded) {
        s->col.instances = s->vehicles.inst;
        s->col.instance_count = s->vehicles.count;
    }

    s->bitmaps_ok = (s->bitmaps_rm.data != NULL);
    s->weapon_count = hta_weapon_list_playable(&s->cache, s->weapons,
                                               (uint32_t)(sizeof(s->weapons)/sizeof(s->weapons[0])));
    hta_log("[weapon] %u playable weapon(s) in this cache", s->weapon_count);
    /* What the MAP says you spawn holding: Blood Gulch's starting equipment
     * names the assault rifle and the pistol, and the campaign map's player
     * starting profile agrees down to the magazines. Nothing here is
     * chosen by us. */
    s->start_count = hta_scenario_starting_weapons(&s->cache, s->start_weapon,
                                                   HTA_CARRY_MAX);
    if (!s->start_count && s->weapon_count) {
        /* A map with no loadout at all still has to arm you. */
        s->start_weapon[0] = s->weapons[0];
        s->start_count = 1;
        hta_log("[weapon] no starting equipment in this map; taking the first");
    }
    for (uint32_t i = 0; i < s->start_count; i++) {
        hta_weapon_def probe;
        if (hta_weapon_load_id(&s->cache, NULL, s->start_weapon[i], &probe,
                               NULL, NULL, 0))
            hta_log("[weapon] spawn with %s", probe.path);
    }
    s->held_count = s->start_count;
    for (uint32_t i = 0; i < s->start_count; i++) s->held[i] = s->start_weapon[i];
    s->held_slot = 0;
    if (s->held_count) equip_weapon(s, s->held[0]);
    else hta_log("[weapon] nothing to hold");
    if (hta_sky_load(&s->sky, &s->cache, s->bitmaps_rm.data ? &s->bitmaps_rm : NULL, err, sizeof(err))) {
        s->have_sky = true;
        hta_log("[assets] sky %u verts / %u submeshes", s->sky.vertex_count, s->sky.submesh_count);
    } else {
        hta_log("[assets] sky: %s", err);
    }

    /* spawn at a real player start if the scenario has one. Kept, because
     * dying means coming back at another one. */
    hta_spawn_point *sp = s->spawn;
    uint32_t nsp = hta_scenario_spawns(&s->cache, sp, 64);
    s->spawn_count = nsp;
    s->spawn_rng = 0x9E3779B9u;
    hta_player_init(&s->player);
    {
        hta_player_physics phys;
        if (hta_player_physics_load(&phys, &s->cache, err, sizeof(err))) {
            hta_player_apply_physics(&s->player, &phys);
            s->cam.fov_y = phys.fov_y;
            s->base_fov = phys.fov_y;
            /* The grenades. `globals` keeps a table of them at +296,
             * 68 bytes an entry: how many you may carry, how many you
             * spawn with in multiplayer, and the projectile itself. */
            {
                int32_t gi = hta_cache_find_tag_by_class(&s->cache, HTA_TAG_MATG);
                hta_tag_entry gt;
                uint32_t gb;
                if (gi >= 0 && hta_cache_tag(&s->cache, (uint32_t)gi, &gt) &&
                    hta_cache_ptr_to_offset(&s->cache, gt.tag_data_ptr, &gb)) {
                    uint32_t n = 0, p2 = 0, off = 0;
                    if (hta_read_reflexive(&s->cache, gb + 296u, &n, &p2) && n &&
                        hta_cache_ptr_to_offset(&s->cache, p2, &off)) {
                        int16_t mx = 0, sp = 0;
                        uint32_t proj = 0;
                        hta_rd_u16(&s->cache, off + 0u, (uint16_t *)&mx);
                        hta_rd_u16(&s->cache, off + 2u, (uint16_t *)&sp);
                        hta_rd_u32(&s->cache, off + 52u + 12u, &proj);
                        char gerr[HTA_ERRLEN];
                        if (proj && hta_projectiles_equip_projectile(
                                &s->nades, &s->cache,
                                s->bitmaps_rm.data ? &s->bitmaps_rm : NULL,
                                proj, gerr, sizeof(gerr))) {
                            s->nade_max = mx > 0 ? mx : 4;
                            s->nade_count = sp > 0 ? sp : 2;
                            s->nade_snd = s->nades.detonation_snd;
                            hta_log("[player] %d frag grenade(s) of %d, "
                                    "fuse %.2fs after the bounce, blast %.0f",
                                    s->nade_count, s->nade_max,
                                    s->nades.timer, s->nades.blast_damage);
                        }
                    }
                }
            }
            if (hta_vitals_load(s->vit, &s->cache))
                hta_log("[player] %.0f health, %.0f shield, back in %.1fs at %.0f%%/s"
                        "; a fall hurts past %.1f wu/s and kills at %.1f",
                        s->vit->max_health, s->vit->max_shield,
                        s->vit->recharge_delay, s->vit->recharge_rate * 100.0f,
                        s->vit->fall_harmful_min, s->vit->fall_fatal);
            /* Somebody to shoot at, out in front of the spawn. */
            {
                char berr[HTA_ERRLEN];
                uint32_t bip = 0;
                for (uint32_t i = 0; i < s->cache.tag_count && !bip; i++) {
                    hta_tag_entry t;
                    if (!hta_cache_tag(&s->cache, i, &t)) continue;
                    if (t.primary_class != HTA_FOURCC('b','i','p','d')) continue;
                    char p[128];
                    hta_cache_tag_path(&s->cache, &t, p, sizeof(p));
                    if (strstr(p, "cyborg_mp")) bip = t.tag_id;
                }
                s->melee_damage = hta_biped_melee_damage(&s->cache, bip);
                if (bip && hta_bot_load(&s->bot, &s->cache,
                                        s->bitmaps_ok ? &s->bitmaps_rm : NULL,
                                        bip, berr, sizeof(berr))) {
                    hta_log("[bot] %s; melee does %.0f", berr, s->melee_damage);
                    /* Something in its hands. `stand rifle idle` poses them
                     * to hold a rifle; without one it reads as a man
                     * standing with his arms out. */
                    uint32_t ar = 0;
                    for (uint32_t i = 0; i < s->cache.tag_count && !ar; i++) {
                        hta_tag_entry t;
                        if (!hta_cache_tag(&s->cache, i, &t)) continue;
                        if (t.primary_class != HTA_FOURCC('m','o','d','2')) continue;
                        char p[160];
                        hta_cache_tag_path(&s->cache, &t, p, sizeof(p));
                        if (strcmp(p, "weapons\\assault rifle\\assault rifle") == 0)
                            ar = t.tag_id;
                    }
                    if (ar && hta_bot_arm(&s->bot, &s->cache,
                                          s->bitmaps_ok ? &s->bitmaps_rm : NULL,
                                          ar, berr, sizeof(berr)))
                        hta_log("[bot] armed: %s", berr);
                    else
                        hta_log("[bot] unarmed (%s)", berr);
                    if (s->net_enabled) {
                        for (int slot=0;slot<2;slot++) {
                            uint32_t model=slot==0 ? ar : 0;
                            if (slot==1) {
                                for (uint32_t i=0;i<s->cache.tag_count && !model;i++) {
                                    hta_tag_entry t; char path[160];
                                    if (!hta_cache_tag(&s->cache,i,&t) ||
                                        t.primary_class!=HTA_FOURCC('m','o','d','2')) continue;
                                    hta_cache_tag_path(&s->cache,&t,path,sizeof(path));
                                    if (!strcmp(path,"weapons\\pistol\\pistol")) model=t.tag_id;
                                }
                            }
                            hta_actor *a=&s->remote[slot];
                            if (!hta_actor_load(a,&s->cache,
                                    s->bitmaps_ok ? &s->bitmaps_rm : NULL,
                                    bip,berr,sizeof(berr))) continue;
                            if (model) hta_actor_hold(a,&s->cache,
                                s->bitmaps_ok ? &s->bitmaps_rm : NULL,
                                model,"right hand",berr,sizeof(berr));
                            hta_actor_play(a,slot ? "stand pistol idle" : "stand rifle idle",false);
                        }
                        hta_log("[net] remote Spartan models: AR=%d pistol=%d",
                                s->remote[0].loaded,s->remote[1].loaded);
                        /* The stationary practice target is another Spartan.
                         * In a live session it is misleading, and its local
                         * damage is not server authority, so remove it. */
                        hta_bot_free(&s->bot);
                    }
                } else {
                    hta_log("[bot] none (%s)", berr);
                }
            }

            /* The shield's own voice. Every one of these is the tag's:
             * which sound, and which condition it is latched to. */
            {
                bool lp = false;
                s->shield_charge_snd =
                    hta_unit_hud_sound(&s->cache, HTA_HUDSND_SHIELD_RECHARGING, &lp);
                s->shield_hit_snd =
                    hta_unit_hud_sound(&s->cache, HTA_HUDSND_SHIELD_DAMAGED, &lp);
                s->shield_low_snd =
                    hta_unit_hud_sound(&s->cache, HTA_HUDSND_SHIELD_LOW, &lp);
                s->shield_empty_snd =
                    hta_unit_hud_sound(&s->cache, HTA_HUDSND_SHIELD_EMPTY, &lp);
                s->health_low_snd =
                    hta_unit_hud_sound(&s->cache, HTA_HUDSND_HEALTH_LOW, &lp);
                /* Four of the five are `lsnd`, and nothing downstream can
                 * play one -- a mixer clip comes from a `snd!`. Resolve
                 * each to its first track here, once. */
                uint32_t *snds[5] = {
                    &s->shield_charge_snd, &s->shield_hit_snd,
                    &s->shield_low_snd, &s->shield_empty_snd,
                    &s->health_low_snd
                };
                for (int q = 0; q < 5; q++) {
                    hta_loop_sound ls;
                    if (!*snds[q]) continue;
                    if (hta_loop_sound_track(&s->cache, *snds[q], &ls))
                        *snds[q] = ls.loop ? ls.loop : ls.start;
                    else
                        *snds[q] = 0;
                }
                if (s->shield_charge_snd) bank_get(s, s->shield_charge_snd);
                if (s->shield_hit_snd)    bank_get(s, s->shield_hit_snd);
                if (s->shield_empty_snd)  bank_get(s, s->shield_empty_snd);
                if (s->shield_low_snd)    bank_get(s, s->shield_low_snd);
                if (s->health_low_snd)    bank_get(s, s->health_low_snd);
                hta_log("[player] hud sounds: charge 0x%08X hit 0x%08X "
                        "low 0x%08X empty 0x%08X heartbeat 0x%08X",
                        s->shield_charge_snd, s->shield_hit_snd,
                        s->shield_low_snd, s->shield_empty_snd,
                        s->health_low_snd);
            }

            /* Your own body, for looking at once it is on the floor. */
            {
                char aerr[HTA_ERRLEN];
                uint32_t bip = 0;
                for (uint32_t i = 0; i < s->cache.tag_count && !bip; i++) {
                    hta_tag_entry t;
                    if (!hta_cache_tag(&s->cache, i, &t)) continue;
                    if (t.primary_class != HTA_FOURCC('b','i','p','d')) continue;
                    char p[128];
                    hta_cache_tag_path(&s->cache, &t, p, sizeof(p));
                    if (strstr(p, "cyborg_mp")) bip = t.tag_id;
                }
                if (bip && hta_actor_load(&s->corpse, &s->cache,
                                          s->bitmaps_ok ? &s->bitmaps_rm : NULL,
                                          bip, aerr, sizeof(aerr)))
                    hta_log("[player] body: %s", aerr);
                else
                    hta_log("[player] no body to leave behind (%s)", aerr);
            }

            /* What the Chief says on the way down. Blood Gulch carries one
             * dialogue tag and it is his. */
            {
                uint32_t udlg = hta_dialogue_tag(&s->cache);
                s->death_quiet_snd =
                    hta_dialogue_sound(&s->cache, udlg, HTA_DLG_DEATH_QUIET);
                s->death_violent_snd =
                    hta_dialogue_sound(&s->cache, udlg, HTA_DLG_DEATH_VIOLENT);
                s->death_falling_snd =
                    hta_dialogue_sound(&s->cache, udlg, HTA_DLG_DEATH_FALLING);
                if (s->death_quiet_snd) bank_get(s, s->death_quiet_snd);
                if (s->death_violent_snd) bank_get(s, s->death_violent_snd);
                hta_log("[player] death dialogue: quiet 0x%08X violent 0x%08X",
                        s->death_quiet_snd, s->death_violent_snd);
            }
            hta_collision_set_slope(&s->col, phys.max_slope);
            hta_log("[player] cyborg_mp run %.2f wu/s jump %.2f cam %.2f r %.2f slope %.0f deg",
                    phys.run_forward, phys.jump_speed, phys.cam_stand, phys.radius,
                    phys.max_slope * (180.0f / 3.14159265f));
            hta_log("[player] footsteps tag 0x%08X, step every %.2f wu",
                    phys.footsteps_id, HTA_STEP_LENGTH);
        } else {
            hta_log("[player] using fallback physics (%s)", err);
        }
    }
    if (nsp > 0) {
        hta_player_spawn(&s->player, &sp[0]);
        float gz;
        if (s->col.built &&
            hta_collision_ground(&s->col, s->player.pos[0], s->player.pos[1],
                                 s->player.pos[2] + 8.0f, &gz)) {
            s->player.pos[2] = gz;
            s->player.on_ground = true;
            hta_log("[assets] snapped spawn to ground z=%.2f", gz);
        }
        /* Eight world units in front of where you start, facing you --
         * far enough to shoot at, near enough to walk up and hit. */
        if (s->bot.loaded) {
            float bp[3] = {
                s->player.pos[0] + cosf(sp[0].facing) * 8.0f,
                s->player.pos[1] + sinf(sp[0].facing) * 8.0f,
                s->player.pos[2]
            };
            float gz;
            if (s->col.built &&
                hta_collision_ground(&s->col, bp[0], bp[1], bp[2] + 8.0f, &gz))
                bp[2] = gz;
            hta_bot_spawn(&s->bot, bp, sp[0].facing + 3.14159265f);
            hta_log("[bot] standing at (%.2f %.2f %.2f)", bp[0], bp[1], bp[2]);
        }
        hta_log("[assets] %u spawn points; spawning at (%.2f %.2f %.2f)",
                nsp, s->player.pos[0], s->player.pos[1], s->player.pos[2]);
        s->cam.yaw = sp[0].facing;
    } else {
        s->player.pos[0] = 0.5f * (s->mesh.bounds_min[0] + s->mesh.bounds_max[0]);
        s->player.pos[1] = 0.5f * (s->mesh.bounds_min[1] + s->mesh.bounds_max[1]);
        s->player.pos[2] = s->mesh.bounds_max[2] + 1.0f;
        hta_log("[assets] no spawn points; starting above the centre of the BSP");
    }

    hta_scene_light_from_bsp(&s->mesh, s->scene.light_dir, s->scene.light_color, s->scene.ambient);
    s->scene.clear[0] = 0.42f; s->scene.clear[1] = 0.55f; s->scene.clear[2] = 0.72f;  /* sky-ish */

    s->have_mesh = true;
    s->map_loaded = true;
    snprintf(s->status, sizeof(s->status), "loaded %s", s->cache.name);
    start_game(s);
    /* Again, now the game's rounds exist: their detonations need particle
     * recipes built alongside the held weapon's. */
    if (s->game_on && s->held_count) equip_weapon(s, s->held[s->held_slot]);
    return true;
}

/* ------------------------------- the game ------------------------------ */

/* What the Java HUD draws over the game: the announcer's banner, where you
 * stand, the kill feed and, at the end, the scoreboard. Sections are
 * separated by 0x1E, lines by '\n'. Written on the game thread, read by the
 * HUD's own redraw; a torn read shows one odd frame of text, nothing worse. */
static char g_game_text[1536];

/* How long a feed line and a banner stay up. Ours: Halo fades its kill
 * messages after a few seconds and the tag does not say how many. */
#define HTA_FEED_TIME    6.0f
#define HTA_BANNER_TIME  3.0f
/* Seconds the scoreboard shows after a game before the next begins. Ours. */
#define HTA_POSTGAME     10.0f

static uint32_t find_sound(const hta_cache *c, const char *path)
{
    for (uint32_t i = 0; i < c->tag_count; i++) {
        hta_tag_entry t;
        char p[256];
        if (!hta_cache_tag(c, i, &t) || t.primary_class != HTA_TAG_SND) continue;
        if (hta_cache_tag_path(c, &t, p, sizeof(p)) && !strcasecmp(p, path)) return t.tag_id;
    }
    return 0;
}

static int32_t held_roster(hta_android *s)
{
    if (!s->game_on || !s->held_count) return -1;
    return hta_game_weapon_index(&s->game, s->held[s->held_slot]);
}

static void feed_push(hta_android *s, const char *text)
{
    for (int i = 3; i > 0; i--) {
        memcpy(s->feed[i], s->feed[i - 1], sizeof(s->feed[i]));
        s->feed_age[i] = s->feed_age[i - 1];
    }
    snprintf(s->feed[0], sizeof(s->feed[0]), "%s", text);
    s->feed_age[0] = 0.0f;
    hta_log("[game] %s", text);
}

static void start_game(hta_android *s)
{
    char err[HTA_ERRLEN];
    const hta_resource_map *bm = s->bitmaps_ok ? &s->bitmaps_rm : NULL;
    if (!hta_game_load(&s->game, &s->cache, bm, &s->col, err, sizeof(err))) {
        hta_log("[game] not playable: %s", err);
        return;
    }
    hta_log("[game] %s", err);
    if (s->vehicles.loaded) {
        uint32_t before = s->game.weapon_count;
        hta_game_attach_vehicles(&s->game, &s->vehicles, bm);
        /* A client draws the host's vehicles and runs none of its own. */
        s->game.simulate_vehicles = !s->net_enabled || s->net_hosting;
        hta_vehicles_roster(&s->vehicles, s->game.simulate_vehicles ? s->vehicle_roster
                                                                    : HTA_VROSTER_NONE);
        uint32_t active = 0;
        for (uint32_t i = 0; i < s->vehicles.count; i++) active += s->vehicles.cars[i].active;
        hta_log("[vehicles] %u of %u placed (roster %d); %u vehicle trigger(s), %u pools",
                active, s->vehicles.count, s->vehicle_roster, s->game.weapon_count - before,
                s->game.pool_count);
        s->veh_in_snd = find_sound(&s->cache, "sound\\sfx\\vehicles\\warthog_7_in");
        s->veh_out_snd = find_sound(&s->cache, "sound\\sfx\\vehicles\\warthog_7_out");
        if (s->veh_in_snd) bank_get(s, s->veh_in_snd);
        if (s->veh_out_snd) bank_get(s, s->veh_out_snd);
        for (uint32_t t = 0; t < s->vehicles.type_count && t < HTA_VEHICLE_TYPES; t++) {
            hta_loop_sound ls;
            s->veh_engine[t] = 0;
            if (hta_object_loop_sound(&s->cache, s->vehicles.types[t].tag_id, "", &ls)) {
                s->veh_engine[t] = ls.loop ? ls.loop : ls.start;
                s->veh_engine_gain[t] = ls.gain > 0.0f ? ls.gain : 1.0f;
                if (s->veh_engine[t]) bank_get(s, s->veh_engine[t]);
            }
        }
    }
    s->my_car = s->my_seat = -1;
    s->game.simulate_drops = !s->net_enabled || s->net_hosting;
    if (!hta_game_set_mode(&s->game, (hta_game_mode)s->game_mode))
        hta_log("[game] mode %d is not playable on this map: Slayer", s->game_mode);
    s->carried_flag = -1;
    s->game.score_limit = s->score_limit;
    s->game.time_limit = (float)s->time_limit_min * 60.0f;
    s->game.respawn_time = s->respawn_delay;
    int bots = s->net_enabled && !s->net_hosting ? 0 : s->bot_count;
    if (bots > 0) {
        double t0 = hta_time_seconds();
        /* The walkable grid is the ground, not the vehicles parked on it:
         * they move, and bots walk round them as they find them. */
        hta_collision nav_col = s->col;
        nav_col.instances = NULL;
        nav_col.instance_count = 0;
        hta_nav_params prm = { s->game.phys.radius, s->game.phys.coll_stand,
                               s->game.phys.max_slope, 1.0f };
        /* Built once per map and physics, then read back from app storage. */
        char navpath[600] = "";
        const char *ext = s->app->activity->externalDataPath;
        uint32_t key = s->cache.crc32 ^ (uint32_t)(prm.radius * 1e4f) ^
                       ((uint32_t)(prm.height * 1e4f) << 8) ^ ((uint32_t)(prm.max_slope * 1e4f) << 16);
        if (ext) snprintf(navpath, sizeof(navpath), "%s/nav-%08x.bin", ext, key);
        if (navpath[0] && hta_nav_load(&s->nav, navpath, key)) {
            s->game.nav = &s->nav;
            hta_log("[game] nav: %u nodes from %s in %.0f ms", s->nav.node_count, navpath,
                    (hta_time_seconds() - t0) * 1000.0);
        } else if (hta_nav_build(&s->nav, &nav_col, s->mesh.bounds_min, s->mesh.bounds_max,
                          &prm, err, sizeof(err))) {
            if (navpath[0] && !hta_nav_save(&s->nav, navpath, key))
                hta_log("[game] could not keep the nav grid at %s", navpath);
            s->game.nav = &s->nav;
            hta_log("[game] nav: %s in %.0f ms", err, (hta_time_seconds() - t0) * 1000.0);
        } else {
            hta_log("[game] nav failed (%s): bots will stand still", err);
        }
    }
    if (s->items.loaded) s->game.items = &s->items;
    s->me = hta_game_add(&s->game, HTA_UNIT_LOCAL, "Player", HTA_TEAM_AUTO);
    for (int i = 0; i < bots && i < HTA_GAME_MAX_UNITS - 1; i++)
        hta_game_add(&s->game, HTA_UNIT_BOT, NULL, HTA_TEAM_AUTO);
    hta_game_set_skill(&s->game, (uint8_t)s->bot_skill);
    /* The local player's health and shield move into the game, carrying
     * what the tags already gave them. */
    s->game.units[s->me].vitals = *s->vit;
    s->vit = &s->game.units[s->me].vitals;
    /* Bodies for everyone -- ours too, for the seats watched from outside. */
    if (!hta_game_view_load(&s->gview, &s->game, bm,
                            s->net_enabled ? HTA_GAME_MAX_UNITS : s->game.unit_count,
                            err, sizeof(err)))
        hta_log("[game] bodies: %s", err);
    else
        hta_log("[game] %s", err);
    static const char *const LINES[HTA_LINE_COUNT] = {
        NULL,
        "sound\\dialog\\multiplayer1\\slayer",
        "sound\\dialog\\multiplayer1\\double_kill",
        "sound\\dialog\\multiplayer1\\triple_kill",
        "sound\\dialog\\multiplayer1\\killtacular",
        "sound\\dialog\\multiplayer1\\killing_spree",
        "sound\\dialog\\multiplayer1\\running_riot",
        "sound\\dialog\\multiplayer1\\game_over",
        "sound\\dialog\\multiplayer1\\team_slayer",
        "sound\\dialog\\multiplayer1\\capture_the_flag",
        "sound\\dialog\\multiplayer1\\red_team_has_the_flag",
        "sound\\dialog\\multiplayer1\\blue_team_has_the_flag",
        "sound\\dialog\\multiplayer1\\red_team_flag_returned",
        "sound\\dialog\\multiplayer1\\blue_team_flag_returned",
        "sound\\dialog\\multiplayer1\\red_team_score",
        "sound\\dialog\\multiplayer1\\blue_team_score",
    };
    for (int l = 1; l < HTA_LINE_COUNT; l++) {
        s->line_snd[l] = find_sound(&s->cache, LINES[l]);
        if (s->line_snd[l]) bank_get(s, s->line_snd[l]);
    }
    /* The flag's own pickup sound, for the moment you take it. */
    s->flag_take_snd = 0;
    if (s->game.flag_weapon >= 0) {
        hta_weapon_def fd;
        if (hta_weapon_load_id(&s->cache, NULL, s->game.weapons[s->game.flag_weapon].tag,
                               &fd, NULL, NULL, 0) && fd.pickup_snd_id) {
            s->flag_take_snd = fd.pickup_snd_id;
            bank_get(s, s->flag_take_snd);
        }
    }
    for (uint32_t p = 0; p < s->game.pool_count; p++)
        if (s->game.pools[p].detonation_snd) bank_get(s, s->game.pools[p].detonation_snd);
    s->game_on = true;
    if (s->net_enabled) {
        s->net.map_crc=s->cache.crc32;
        if (s->net_hosting) s->host_server.map_crc=s->cache.crc32;
    }
    hta_game_start(&s->game);
    s->world_round = 1;
    /* Every round's contrail, once: the roster's and the pools'. */
    hta_contrails_free(&s->trails);
    hta_contrails_init(&s->trails);
    for (uint32_t w = 0; w < HTA_GAME_MAX_WEAPONS; w++)
        s->wtrail[w] = w < s->game.weapon_count
            ? hta_contrails_for_projectile(&s->trails, &s->cache, bm,
                                           s->game.weapons[w].def.projectile_id)
            : HTA_CONT_NONE;
    for (uint32_t p = 0; p < HTA_GAME_MAX_POOLS; p++)
        s->ptrail[p] = p < s->game.pool_count
            ? hta_contrails_for_projectile(&s->trails, &s->cache, bm, s->game.pools[p].proj_tag_id)
            : HTA_CONT_NONE;
    if (hta_contrails_build(&s->trails, err, sizeof(err))) hta_log("[game] %s", err);
    /* The practice target is gone: there are real people to shoot now. */
    if (bots > 0) hta_bot_free(&s->bot);
    static const char *const MODES[HTA_MODE_COUNT] = { "Slayer", "Team Slayer", "CTF" };
    hta_log("[game] %s: you (team %d) and %d bot(s) at skill %d, first to %d, %d min, respawn %.0f s",
            MODES[s->game.mode], s->game.units[s->me].team, bots, s->bot_skill,
            s->game.score_limit, s->time_limit_min, s->respawn_delay);
}

static void game_gpu_upload(hta_android *s)
{
    if (!s->game_on || !s->gfx) return;
    char err[HTA_ERRLEN];
    for (uint32_t i = 0; i < s->game.unit_count; i++)
        if (s->gview.actor[i].loaded && !s->gpu_units[i])
            s->gpu_units[i] = hta_gfx_mesh_upload_dynamic_world(s->gfx,
                &s->gview.actor[i].mesh, err, sizeof(err));
    for (uint32_t w = 0; w < s->game.weapon_count; w++)
        if (s->gview.have_weapon[w] && !s->gpu_held[w])
            s->gpu_held[w] = hta_gfx_mesh_upload(s->gfx, &s->gview.weapon_mesh[w], err, sizeof(err));
    if (s->trails.loaded && !s->gpu_trails)
        s->gpu_trails = hta_gfx_mesh_upload_dynamic(s->gfx, &s->trails.mesh, err, sizeof(err));
    for (uint32_t p = 0; p < s->game.pool_count; p++)
        if (s->game.pools[p].mesh.index_count && !s->gpu_pools[p])
            s->gpu_pools[p] = hta_gfx_mesh_upload_dynamic_world(s->gfx,
                &s->game.pools[p].mesh, err, sizeof(err));
}

static void game_gpu_free(hta_android *s)
{
    for (uint32_t i = 0; i < HTA_GAME_MAX_UNITS; i++)
        if (s->gpu_units[i]) { hta_gfx_mesh_free(s->gfx, s->gpu_units[i]); s->gpu_units[i] = NULL; }
    for (uint32_t w = 0; w < HTA_GAME_MAX_WEAPONS; w++)
        if (s->gpu_held[w]) { hta_gfx_mesh_free(s->gfx, s->gpu_held[w]); s->gpu_held[w] = NULL; }
    for (uint32_t p = 0; p < HTA_GAME_MAX_POOLS; p++)
        if (s->gpu_pools[p]) { hta_gfx_mesh_free(s->gfx, s->gpu_pools[p]); s->gpu_pools[p] = NULL; }
    if (s->gpu_trails) { hta_gfx_mesh_free(s->gfx, s->gpu_trails); s->gpu_trails = NULL; }
}

/* A hitscan round's tracer, from where it left to the first thing in its
 * way, if its projectile carries a contrail. */
/* Halo draws one round in (between + 1) as a tracer: the rifle's trigger
 * says 3. `shooter` keeps count per unit; true when this round is one. */
static bool tracer_due(hta_android *s, int32_t shooter, int between)
{
    uint32_t k = shooter >= 0 && shooter < HTA_GAME_MAX_UNITS ? (uint32_t)shooter
                                                              : HTA_GAME_MAX_UNITS;
    if (s->since_tracer[k] < (uint8_t)between) { s->since_tracer[k]++; return false; }
    s->since_tracer[k] = 0;
    return true;
}

static void tracer(hta_android *s, int32_t shooter, int32_t weapon, const float from[3], const float dir[3])
{
    if (!s->trails.loaded || weapon < 0 || weapon >= (int32_t)s->game.weapon_count) return;
    uint32_t type = s->wtrail[weapon];
    if (type == HTA_CONT_NONE || s->game.weapons[weapon].travels) return;
    if (!tracer_due(s, shooter, s->game.weapons[weapon].def.between_contrails)) return;
    float t = 100.0f, end[3];
    if (s->col.built) hta_collision_ray(&s->col, from, dir, 100.0f, &t, NULL, NULL);
    for (int k = 0; k < 3; k++) end[k] = from[k] + dir[k] * t;
    hta_contrails_tracer(&s->trails, type, from, end, 300.0f);
}

/* Every blast near the camera shakes it, by the effect's own damage
 * effects: a tank shell kicks the view within 3.25 wu and shakes it out
 * to 8. */
static void shake_effect(hta_android *s, uint32_t effect, const float at[3])
{
    if (!effect || !at) return;
    hta_damage_shake d[4];
    uint32_t n = hta_effect_shakes(&s->cache, effect, d, 4);
    for (uint32_t i = 0; i < n; i++) hta_shake_add(&s->shake, &d[i], &s->cam, at);
}

/* A trigger's firing damage effect is felt by whoever pulled it. */
static void shake_fire(hta_android *s, uint32_t jpt)
{
    hta_damage_shake d;
    if (jpt && hta_damage_shake_read(&s->cache, jpt, &d))
        hta_shake_add(&s->shake, &d, &s->cam, NULL);
}

/* A vehicle blowing up: the shell's fireball twice, a ring of it around
 * the hull, its bang, the shake, the scorch. */
static void wreck_fx(hta_android *s, const float at[3])
{
    float up[3] = { 0, 0, 1 };
    if (s->wreck_recipe != HTA_PART_NO_RECIPE) {
        hta_particles_burst(&s->parts, s->wreck_recipe, at, up);
        for (int i = 0; i < 3; i++) {
            float a = (float)i * 2.094f;
            float p[3] = { at[0] + cosf(a) * 0.5f, at[1] + sinf(a) * 0.5f, at[2] + 0.2f };
            float d[3] = { cosf(a) * 0.5f, sinf(a) * 0.5f, 0.85f };
            hta_particles_burst(&s->parts, s->wreck_recipe, p, d);
        }
    }
    if (s->wreck_snd) play_tag_at(s, s->wreck_snd, at, 1.0f);
    shake_effect(s, s->game.wreck_effect, at);
    float down[3] = { 0, 0, 1 };
    hta_gun_add_mark(&s->gun, at, down, 1.5f);
}

/* The Trial's own words for what you just picked up: `hud_item_messages`
 * says "Picked up an assault rifle" and "Picked up %d rounds for ...".
 * Found by the item's own name in the message, since the list is not in
 * any order a tag points into. */
static void item_message(hta_android *s, uint32_t tag, int rounds)
{
    static uint32_t list;
    if (!list) list = hta_ustr_find(&s->cache, "ui\\hud\\hud_item_messages");
    int32_t ti = hta_cache_find_tag_by_id(&s->cache, tag);
    hta_tag_entry t;
    char path[256] = "";
    if (!list || ti < 0 || !hta_cache_tag(&s->cache, (uint32_t)ti, &t) ||
        !hta_cache_tag_path(&s->cache, &t, path, sizeof(path))) return;
    const char *name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    char want[64];
    snprintf(want, sizeof(want), "%s", name);
    if (strstr(want, "frag grenade")) snprintf(want, sizeof(want), "fragmentation grenade");
    if (!strncmp(want, "mp_", 3)) memmove(want, want + 3, strlen(want + 3) + 1);
    uint32_t n = hta_ustr_count(&s->cache, list);
    for (uint32_t i = 0; i < n; i++) {
        char line[96];
        if (!hta_ustr_get(&s->cache, list, i, line, sizeof(line))) continue;
        bool counts = strstr(line, "%d") != NULL;
        if (counts != (rounds > 0) || !strstr(line, want)) continue;
        if (counts) snprintf(s->item_msg, sizeof(s->item_msg), line, rounds);
        else snprintf(s->item_msg, sizeof(s->item_msg), "%s", line);
        s->item_msg_age = 0.0f;
        return;
    }
}

/* Everything the game did this frame, turned into sound, words and dust. */
static void game_events(hta_android *s)
{
    hta_game_event e;
    char buf[96];
    while (hta_game_pop(&s->game, &e)) {
        if (s->net_hosting && (e.a==-1 || (e.a>=0 && e.a<HTA_GAME_MAX_UNITS)) &&
            (e.kind==HTA_EV_FIRE || e.kind==HTA_EV_HIT_WORLD ||
             e.kind==HTA_EV_DETONATE)) {
            hta_net_fx fx={0};
            fx.kind=e.kind==HTA_EV_FIRE ? HTA_NET_FX_FIRE :
                    e.kind==HTA_EV_HIT_WORLD ? HTA_NET_FX_IMPACT : HTA_NET_FX_DETONATE;
            fx.entity=e.a<0 ? 255 : (uint8_t)e.a;
            fx.weapon=(uint8_t)(e.kind==HTA_EV_DETONATE ? e.pool : e.weapon);
            fx.material=e.material;
            for (int k=0;k<3;k++) { fx.pos[k]=e.pos[k]; fx.dir[k]=e.dir[k]; }
            hta_net_server_fx(&s->host_server,&fx);
        }
        switch (e.kind) {
        case HTA_EV_FIRE:
            if (e.weapon < 0 || e.weapon >= HTA_GAME_MAX_WEAPONS) break;
            if (e.a == s->me && s->game.weapons[e.weapon].vehicle)
                shake_fire(s, s->game.weapons[e.weapon].def.firing_damage_id);
            if (e.a != s->me || s->game.weapons[e.weapon].vehicle) tracer(s, e.a, e.weapon, e.pos, e.dir);
            /* Our own rifle speaks for itself; a vehicle gun is the game's. */
            if (e.a == s->me && !s->game.weapons[e.weapon].vehicle) break;
            if (s->game.weapons[e.weapon].vehicle &&
                s->vfire_recipe[e.weapon] != HTA_PART_NO_RECIPE)
                hta_particles_burst(&s->parts, s->vfire_recipe[e.weapon], e.pos, e.dir);
            if (!s->unit_fire_known[e.weapon]) {
                s->unit_fire_known[e.weapon] = 1;
                s->unit_fire_snd[e.weapon] = hta_effect_first_sound(&s->cache,
                    s->game.weapons[e.weapon].def.firing_fx_id);
            }
            if (s->unit_fire_snd[e.weapon]) play_tag_at(s, s->unit_fire_snd[e.weapon], e.pos, 1.0f);
            break;
        case HTA_EV_HIT_WORLD:
            if (e.weapon >= 0 && e.weapon < (int32_t)s->game.weapon_count &&
                s->game.weapons[e.weapon].vehicle) {
                uint32_t sound = hta_projectile_impact_sound(&s->cache,
                    s->game.weapons[e.weapon].def.projectile_id, e.material);
                if (sound) play_tag_at(s, sound, e.pos, 0.8f);
                if (e.material < 33u && s->impact_recipe[e.material] != HTA_PART_NO_RECIPE)
                    hta_particles_burst(&s->parts, s->impact_recipe[e.material], e.pos, e.dir);
            } else if (e.weapon >= 0 && e.weapon == held_roster(s)) {
                play_impact_at(s, e.material, e.pos);
                if (e.material < 33u && s->impact_recipe[e.material] != HTA_PART_NO_RECIPE)
                    hta_particles_burst(&s->parts, s->impact_recipe[e.material], e.pos, e.dir);
            }
            hta_gun_add_mark(&s->gun, e.pos, e.dir, HTA_MARK_SIZE);
            break;
        case HTA_EV_DETONATE:
            if (e.pool >= 0 && (uint32_t)e.pool < s->game.pool_count) {
                const hta_projectiles *pl = &s->game.pools[e.pool];
                if (pl->detonation_snd) play_tag_at(s, pl->detonation_snd, e.pos, 1.0f);
                shake_effect(s, pl->det_effect, e.pos);
                if (s->pool_recipe[e.pool] != HTA_PART_NO_RECIPE)
                    hta_particles_burst(&s->parts, s->pool_recipe[e.pool], e.pos, e.dir);
                if (pl->blast_radius > 0.0f)
                    hta_gun_add_mark(&s->gun, e.pos, e.dir, pl->blast_radius);
            }
            break;
        case HTA_EV_HIT_UNIT: {
            /* The body answers the round: the weapon's own impact on a
             * cyborg's shield while it holds, on armour after. Heard near
             * enough to matter. */
            if (e.a < 0 || e.a >= (int32_t)s->game.unit_count || e.a == s->me) break;
            const hta_unit *v = &s->game.units[e.a];
            int32_t w = -1;
            if (e.b >= 0 && e.b < (int32_t)s->game.unit_count) {
                const hta_unit *k = &s->game.units[e.b];
                const hta_game_weapon *hw = hta_game_held(&s->game, e.b);
                w = hw ? (int32_t)(hw - s->game.weapons) : -1;
                if (k->vehicle >= 0 && (uint32_t)k->vehicle < s->vehicles.count) {
                    uint16_t ty = s->vehicles.cars[k->vehicle].type;
                    const hta_vehicle_seat *st = hta_vehicles_seat(&s->vehicles,
                        (uint32_t)k->vehicle, (uint32_t)k->seat);
                    if (st && (st->flags & HTA_SEAT_GUNNER) && ty < HTA_VEHICLE_TYPES)
                        w = s->game.vweapon[ty][0];
                }
            }
            if (w < 0 || w >= (int32_t)s->game.weapon_count ||
                !s->game.weapons[w].def.projectile_id) break;
            uint8_t mat = v->vitals.shield > 0.0f ? HTA_MATERIAL_CYBORG_SHIELD
                                                   : HTA_MATERIAL_CYBORG_ARMOR;
            uint32_t snd = hta_projectile_impact_sound(&s->cache,
                s->game.weapons[w].def.projectile_id, mat);
            if (snd) play_tag_at(s, snd, e.pos, e.b == s->me ? 1.0f : 0.7f);
            break;
        }
        case HTA_EV_WRECK:
            if (s->net_hosting) {
                hta_net_fx fx = { .kind = HTA_NET_FX_WRECK,
                                  .entity = e.a >= 0 && e.a < HTA_GAME_MAX_UNITS ? (uint8_t)e.a : 255,
                                  .weapon = (uint8_t)(e.b & 31), .material = 0 };
                for (int k = 0; k < 3; k++) { fx.pos[k] = e.pos[k]; fx.dir[k] = e.dir[k]; }
                hta_net_server_fx(&s->host_server, &fx);
            }
            wreck_fx(s, e.pos);
            break;
        case HTA_EV_PICKUP:
            if (e.a != s->me) {
                hta_item_choice ch;
                char path[96];
                memset(&ch, 0, sizeof(ch));
                hta_item_describe(&s->cache, e.tag, path, sizeof(path), &ch);
                uint32_t snd = ch.pickup_snd;
                if (!snd) {
                    hta_weapon_def wd;
                    if (hta_weapon_load_id(&s->cache, NULL, e.tag, &wd, NULL, NULL, 0))
                        snd = wd.pickup_snd_id;
                }
                if (snd) play_tag_at(s, snd, e.pos, 0.8f);
            }
            break;
        case HTA_EV_KILL:
            if (s->net_hosting && e.a>=0 && e.a<HTA_GAME_MAX_UNITS) {
                hta_net_kill kill={0};
                kill.victim=(uint8_t)e.a;
                kill.killer=e.b>=0 && e.b<HTA_GAME_MAX_UNITS ? (uint8_t)e.b : 255;
                size_t k=0;
                while (k<sizeof(kill.text)-1 && e.text[k]) {
                    unsigned char ch=(unsigned char)e.text[k];
                    kill.text[k]=(char)(ch>=32 && ch<127 ? ch : '?'); k++;
                }
                kill.text[k]=0;
                hta_net_server_kill(&s->host_server,&kill,s->last_time);
            }
            if (e.b == s->me && e.a != s->me) {
                char fmt[64];
                if (!hta_ustr_get(&s->cache, s->game.text_tag, 88, fmt, sizeof(fmt)))
                    snprintf(fmt, sizeof(fmt), "You killed %%s");
                snprintf(buf, sizeof(buf), fmt, s->game.units[e.a].name);
                feed_push(s, buf);
            } else {
                feed_push(s, e.text);
            }
            break;
        case HTA_EV_ANNOUNCE:
            if (!e.for_local) break;
            snprintf(s->banner, sizeof(s->banner), "%s", e.text);
            s->banner_age = 0.0f;
            if (e.line > HTA_LINE_NONE && e.line < HTA_LINE_COUNT && s->line_snd[e.line])
                play_tag(s, s->line_snd[e.line], 1.0f);
            break;
        case HTA_EV_FLAG:
            if (e.text[0]) {
                snprintf(s->banner, sizeof(s->banner), "%s", e.text);
                s->banner_age = 0.0f;
            }
            if (e.line > HTA_LINE_NONE && e.line < HTA_LINE_COUNT && s->line_snd[e.line])
                play_tag(s, s->line_snd[e.line], 1.0f);
            if (e.pool == HTA_FLAG_TAKEN && s->flag_take_snd)
                play_tag_at(s, s->flag_take_snd, e.pos, 1.0f);
            break;
        case HTA_EV_ENTER:
            if (s->veh_in_snd) play_tag_at(s, s->veh_in_snd, e.pos, 1.0f);
            break;
        case HTA_EV_EXIT:
            if (s->veh_out_snd) play_tag_at(s, s->veh_out_snd, e.pos, 1.0f);
            break;
        case HTA_EV_GAME_OVER:
            snprintf(s->banner, sizeof(s->banner), "%s", e.text);
            s->banner_age = 0.0f;
            s->over_timer = HTA_POSTGAME;
            if (s->line_snd[HTA_LINE_GAME_OVER]) play_tag(s, s->line_snd[HTA_LINE_GAME_OVER], 1.0f);
            break;
        default:
            break;
        }
    }
}

/* The text the HUD draws, rebuilt a few times a second. */
static void game_text(hta_android *s, float dt)
{
    if (!s->game_on) { g_game_text[0] = 0; return; }
    s->banner_age += dt;
    for (int i = 0; i < 4; i++) s->feed_age[i] += dt;
    char place[96] = "";
    if (s->me >= 0 && s->game.unit_count > 1)
        hta_game_place_text(&s->game, s->me, place, sizeof(place));
    size_t n = 0;
    n += (size_t)snprintf(g_game_text + n, sizeof(g_game_text) - n, "%s\x1e%s\x1e",
                          s->banner_age < HTA_BANNER_TIME ? s->banner : "", place);
    for (int i = 3; i >= 0 && n < sizeof(g_game_text); i--)
        if (s->feed[i][0] && s->feed_age[i] < HTA_FEED_TIME)
            n += (size_t)snprintf(g_game_text + n, sizeof(g_game_text) - n, "%s\n", s->feed[i]);
    if (n < sizeof(g_game_text))
        n += (size_t)snprintf(g_game_text + n, sizeof(g_game_text) - n, "\x1e");
    size_t board_at = n;
    if (s->game.over && n < sizeof(g_game_text)) {
        int32_t order[HTA_GAME_MAX_UNITS];
        uint32_t k = hta_game_standings(&s->game, order, HTA_GAME_MAX_UNITS);
        for (uint32_t i = 0; i < k && n < sizeof(g_game_text); i++) {
            const hta_unit *u = &s->game.units[order[i]];
            n += (size_t)snprintf(g_game_text + n, sizeof(g_game_text) - n,
                                  "%u\t%s\t%d\t%d\t%d\t%d\n", i + 1, u->name, u->score,
                                  u->kills, u->assists, u->deaths);
        }
    }
    (void)board_at;
    s->item_msg_age += dt;
    if (n < sizeof(g_game_text))
        n += (size_t)snprintf(g_game_text + n, sizeof(g_game_text) - n, "\x1e%s",
                              s->item_msg_age < 2.5f ? s->item_msg : "");
    /* Waypoints: where each flag is, as screen fractions, its team, how far
     * in metres, and whether it is on screen (else pinned to the edge). */
    if (n < sizeof(g_game_text))
        n += (size_t)snprintf(g_game_text + n, sizeof(g_game_text) - n, "\x1e");
    if (s->game.mode == HTA_MODE_CTF && s->me >= 0 && !s->dead && !s->game.over) {
        hta_mat4 vp = hta_camera_view_proj(&s->cam);
        for (int t = 0; t < 2 && n < sizeof(g_game_text); t++) {
            const hta_game_flag *f = &s->game.flags[t];
            if (!f->present) continue;
            if (f->state == HTA_FLAG_CARRIED && f->carrier == s->me) continue;
            float at[4] = { f->pos[0], f->pos[1], f->pos[2] + 0.9f, 1.0f };
            if (f->state == HTA_FLAG_CARRIED && f->carrier >= 0 &&
                f->carrier < (int32_t)s->game.unit_count) {
                const hta_unit *cu = &s->game.units[f->carrier];
                at[0] = cu->body.pos[0]; at[1] = cu->body.pos[1]; at[2] = cu->body.pos[2] + 1.0f;
            }
            float clip[4];
            hta_mat4_transform(&vp, at, clip);
            float dx = at[0]-s->cam.pos[0], dy = at[1]-s->cam.pos[1], dz = at[2]-s->cam.pos[2];
            float metres = sqrtf(dx*dx + dy*dy + dz*dz) * 3.048f;
            float x, y;
            bool on = clip[3] > 0.05f;
            if (on) {
                x = 0.5f + 0.5f * clip[0] / clip[3];
                y = 0.5f + 0.5f * clip[1] / clip[3];
                on = x > 0.04f && x < 0.96f && y > 0.08f && y < 0.90f;
            } else {
                /* Behind: along the bottom, on the side it lies. */
                float right[3];
                hta_camera_right(&s->cam, right);
                x = dx*right[0] + dy*right[1] > 0.0f ? 0.96f : 0.04f;
                y = 0.90f;
            }
            if (x < 0.04f) x = 0.04f;
            if (x > 0.96f) x = 0.96f;
            if (y < 0.08f) y = 0.08f;
            if (y > 0.90f) y = 0.90f;
            n += (size_t)snprintf(g_game_text + n, sizeof(g_game_text) - n, "%.3f,%.3f,%d,%.0f,%d,%d;",
                                  x, y, t, metres, on ? 1 : 0, (int)f->state);
        }
    }
}

JNIEXPORT jstring JNICALL
Java_net_hta_halotrial_GameActivity_nativeGameText(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, g_game_text);
}

/* Rigid things drawn with a matrix each this frame: guns in hands and
 * vehicle parts. Handed to the renderer once, just before the draw. */
static hta_gfx_instance g_inst[HTA_GFX_MAX_INSTANCES];
static uint32_t g_inst_count;

/* The bodies, their guns and their rounds, onto the draw list. */
static uint32_t game_draw(hta_android *s, hta_gfx_dynamic *dyn, uint32_t n)
{
    if (!s->game_on || !s->gfx) return n;
    for (uint32_t i = 0; i < s->game.unit_count && n < HTA_GFX_MAX_DYNAMIC; i++) {
        if (((int32_t)i == s->me && !s->show_self) || !s->gview.shown[i] || !s->gpu_units[i])
            continue;
        dyn[n].mesh = s->gpu_units[i];
        dyn[n].vertices = s->gview.actor[i].posed;
        dyn[n].vertex_count = s->gview.actor[i].mesh.vertex_count;
        dyn[n].lit = true;
        if (s->game.teams) {
            dyn[n].change = true;
            hta_game_team_color(s->game.units[i].team, dyn[n].change_color);
        }
        n++;
    }
    for (uint32_t p = 0; p < s->game.pool_count && n < HTA_GFX_MAX_DYNAMIC; p++) {
        if (!s->gpu_pools[p]) continue;
        dyn[n].mesh = s->gpu_pools[p];
        dyn[n].vertices = s->game.pools[p].mesh.vertices;
        dyn[n].vertex_count = s->game.pools[p].mesh.vertex_count;
        dyn[n].lit = false;
        n++;
    }
    hta_game_held_weapon held[HTA_GAME_MAX_UNITS];
    uint32_t nh = hta_game_view_weapons(&s->gview, &s->game, s->show_self ? -1 : s->me,
                                        held, HTA_GAME_MAX_UNITS);
    for (uint32_t k = 0; k < nh && g_inst_count < HTA_GFX_MAX_INSTANCES; k++) {
        if (!s->gpu_held[held[k].weapon]) continue;
        hta_gfx_instance *in = &g_inst[g_inst_count++];
        in->mesh = s->gpu_held[held[k].weapon];
        memcpy(in->model, held[k].model, sizeof(in->model));
        in->first_submesh = held[k].first_submesh;
        in->submesh_count = held[k].submesh_count;
        in->lit = true;
    }
    /* The flags: upright on their stands, or lying where they fell. */
    for (int t = 0; t < 2 && s->game.flag_weapon >= 0; t++) {
        float fm[16];
        uint32_t first[2], count[2];
        if (!s->gpu_held[s->game.flag_weapon] || !hta_game_flag_model(&s->game, t, fm)) continue;
        uint32_t parts = hta_game_view_flag_parts(&s->gview, t, first, count);
        for (uint32_t p = 0; p < parts && g_inst_count < HTA_GFX_MAX_INSTANCES; p++) {
            hta_gfx_instance *in = &g_inst[g_inst_count++];
            in->mesh = s->gpu_held[s->game.flag_weapon];
            memcpy(in->model, fm, sizeof(fm));
            in->first_submesh = first[p];
            in->submesh_count = count[p];
            in->lit = true;
        }
    }
    /* Weapons on the ground, lying on their side. */
    for (int i = 0; i < HTA_GAME_MAX_DROPS && g_inst_count < HTA_GFX_MAX_INSTANCES; i++) {
        const hta_game_drop *d = &s->game.drops[i];
        if (!d->live || d->weapon < 0 || d->weapon >= HTA_GAME_MAX_WEAPONS ||
            !s->gpu_held[d->weapon]) continue;
        float cy = cosf(d->yaw), sy = sinf(d->yaw);
        hta_gfx_instance *in = &g_inst[g_inst_count++];
        in->mesh = s->gpu_held[d->weapon];
        /* Model +X along the yaw, +Y up (on its side), +Z to the side. */
        float m[16] = { cy, sy, 0, 0,   0, 0, 1, 0,   sy, -cy, 0, 0,
                        d->pos[0], d->pos[1], d->pos[2] + 0.05f, 1 };
        memcpy(in->model, m, sizeof(m));
        in->first_submesh = in->submesh_count = 0;
        in->lit = true;
    }
    return n;
}

/* Every vehicle, as rigid parts of its type's one mesh. */
static void vehicles_draw(hta_android *s)
{
    if (!s->vehicles.loaded || !s->gfx) return;
    static hta_vehicle_part parts[HTA_GFX_MAX_INSTANCES];
    uint32_t np = hta_vehicles_parts(&s->vehicles, parts, HTA_GFX_MAX_INSTANCES - g_inst_count);
    for (uint32_t i = 0; i < np && g_inst_count < HTA_GFX_MAX_INSTANCES; i++) {
        if (parts[i].type >= HTA_VEHICLE_TYPES || !s->gpu_vtypes[parts[i].type]) continue;
        hta_gfx_instance *in = &g_inst[g_inst_count++];
        in->mesh = s->gpu_vtypes[parts[i].type];
        memcpy(in->model, parts[i].model, sizeof(in->model));
        in->first_submesh = parts[i].first_submesh;
        in->submesh_count = parts[i].submesh_count;
        in->lit = true;
    }
}

/* ------------------------------- main menu ------------------------------ */

#define HTA_LOOP_MUSIC 10u
#define HTA_LOOP_MUSIC_IN 11u   /* the intro, stopped when it has played once */

/* A ui.map sound as a mixer clip, or HTA_AUDIO_NO_CLIP. `chain` joins a
 * long sound's segments -- the title music is cut into five-second pieces. */
static uint32_t menu_clip(hta_android *s, uint32_t tag, bool chain)
{
    if (!tag || !s->audio_ok || !s->sounds_rm.data || s->menu_pcm_count >= 8) return HTA_AUDIO_NO_CLIP;
    char err[HTA_ERRLEN];
    hta_pcm pcm;
    bool ok = chain ? hta_sound_decode_chain(&s->ui_cache, &s->sounds_rm, tag, &s->rng, &pcm, err, sizeof(err))
                    : hta_sound_decode(&s->ui_cache, &s->sounds_rm, tag, 0, &pcm, err, sizeof(err));
    if (!ok) { hta_log("[menu] sound 0x%08X: %s", tag, err); return HTA_AUDIO_NO_CLIP; }
    uint32_t clip = hta_audio_add_clip(&s->audio, pcm.samples, pcm.frame_count,
                                       pcm.sample_rate, pcm.channels);
    if (clip == HTA_AUDIO_NO_CLIP) { hta_pcm_free(&pcm); return clip; }
    s->menu_pcm[s->menu_pcm_count++] = pcm.samples;   /* the mixer reads it */
    return clip;
}

static bool menu_load(hta_android *s)
{
    char err[HTA_ERRLEN];
    char path[512];
    if (!find_map(s)) return false;           /* where the data is, not loaded */
    if (!find_named(s, "ui.map", path, sizeof(path)) ||
        !map_data_file(s, path, &s->ui_data, &s->ui_size) ||
        !hta_cache_open(&s->ui_cache, s->ui_data, s->ui_size, err, sizeof(err))) {
        hta_log("[menu] no usable ui.map; straight into the game");
        return false;
    }
    if (!s->bitmaps_data && find_named(s, "bitmaps.map", s->bitmaps_path, sizeof(s->bitmaps_path)) &&
        map_data_file(s, s->bitmaps_path, &s->bitmaps_data, &s->bitmaps_size))
        hta_resource_open(&s->bitmaps_rm, s->bitmaps_data, s->bitmaps_size, err, sizeof(err));
    if (!s->sounds_data && find_named(s, "sounds.map", s->sounds_path, sizeof(s->sounds_path)) &&
        map_data_file(s, s->sounds_path, &s->sounds_data, &s->sounds_size))
        hta_resource_open_typed(&s->sounds_rm, s->sounds_data, s->sounds_size,
                                HTA_RESOURCE_SOUNDS, err, sizeof(err));
    if (!hta_menu_load(&s->menu, &s->ui_cache, s->bitmaps_rm.data ? &s->bitmaps_rm : NULL,
                       err, sizeof(err))) {
        hta_log("[menu] %s", err);
        return false;
    }
    hta_log("[menu] %s", err);
    if (!atomic_load(&g_shell_ready) &&
        hta_shell_load(&g_shell, &s->ui_cache, s->bitmaps_rm.data ? &s->bitmaps_rm : NULL))
        atomic_store(&g_shell_ready, 1);
    g_shell_shown = -1;
    s->menu_clip[0] = menu_clip(s, s->menu.snd_cursor, false);
    s->menu_clip[1] = menu_clip(s, s->menu.snd_forward, false);
    s->menu_clip[2] = menu_clip(s, s->menu.snd_back, false);
    /* The title theme: its intro once, then its loop for as long as you
     * sit here. Both are the looping sound's own first track. */
    s->music_in = s->music_loop = HTA_AUDIO_NO_CLIP;
    hta_loop_sound ls;
    if (s->menu.music && hta_loop_sound_track(&s->ui_cache, s->menu.music, &ls)) {
        s->music_in = menu_clip(s, ls.start, true);
        s->music_loop = menu_clip(s, ls.loop, true);
    }
    if (s->music_in != HTA_AUDIO_NO_CLIP) {
        hta_audio_loop(&s->audio, HTA_LOOP_MUSIC_IN, s->music_in, 0.9f);
        const hta_audio_clip *cl = &s->audio.clips[s->music_in];
        s->music_left = cl->rate ? (float)cl->frames / (float)cl->rate : 0.0f;
    }
    s->menu_pressed = -1;
    return true;
}

static void menu_gpu_upload(hta_android *s)
{
    if (!s->menu_mode || !s->menu.loaded || !s->gfx) return;
    char err[HTA_ERRLEN];
    if (s->menu.scene.index_count)
        s->gpu_menu_scene = hta_gfx_mesh_upload(s->gfx, &s->menu.scene, err, sizeof(err));
    if (s->menu.sky.index_count)
        s->gpu_menu_sky = hta_gfx_mesh_upload(s->gfx, &s->menu.sky, err, sizeof(err));
    s->gpu_menu_ui = hta_gfx_mesh_upload_dynamic(s->gfx, &s->menu.overlay, err, sizeof(err));
}

static void menu_gpu_free(hta_android *s)
{
    if (s->gpu_menu_scene) { hta_gfx_mesh_free(s->gfx, s->gpu_menu_scene); s->gpu_menu_scene = NULL; }
    if (s->gpu_menu_sky) { hta_gfx_mesh_free(s->gfx, s->gpu_menu_sky); s->gpu_menu_sky = NULL; }
    if (s->gpu_menu_ui) { hta_gfx_mesh_free(s->gfx, s->gpu_menu_ui); s->gpu_menu_ui = NULL; }
}

/* A Java method on the activity with no arguments. */
static void call_activity(hta_android *s, const char *method)
{
    JavaVM *vm = s->app->activity->vm;
    JNIEnv *env = NULL;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || !env) return;
    jclass cls = (*env)->GetObjectClass(env, s->app->activity->clazz);
    jmethodID mid = cls ? (*env)->GetMethodID(env, cls, method, "()V") : NULL;
    if (mid) (*env)->CallVoidMethod(env, s->app->activity->clazz, mid);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (cls) (*env)->DeleteLocalRef(env, cls);
}

static void menu_sound(hta_android *s, int which)
{
    if (s->audio_ok && s->menu_clip[which] != HTA_AUDIO_NO_CLIP)
        hta_audio_play(&s->audio, s->menu_clip[which], 1.0f);
}

static void menu_activate(hta_android *s, int item)
{
    menu_sound(s, 1);
    switch (item) {
    /* The submenus are the Java overlay's; it tells us which is up. */
    case HTA_MENU_CAMPAIGN:    call_activity(s, "openSolo"); break;
    case HTA_MENU_MULTIPLAYER: call_activity(s, "openMultiplayer"); break;
    case HTA_MENU_SETTINGS:    call_activity(s, "openSettings"); break;
    case HTA_MENU_CREDITS:     call_activity(s, "showCredits"); break;
    case HTA_MENU_QUIT:        ANativeActivity_finish(s->app->activity); break;
    default: break;
    }
}

/* From the Java overlay: 0 down, 1 move, 2 up; x and y are 0..1 of the
 * screen. The HUD owns the touches, so the menu hears them through here. */
static _Atomic int g_menu_touch_action = -1;
static _Atomic int g_menu_touch_x, g_menu_touch_y;   /* x 10000ths */

JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativeMenuTouch(JNIEnv *env, jclass cls, jint action,
                                                    jfloat x, jfloat y)
{
    (void)env; (void)cls;
    atomic_store(&g_menu_touch_x, (int)(x * 10000.0f));
    atomic_store(&g_menu_touch_y, (int)(y * 10000.0f));
    /* An up must not be lost behind a move, so it is never overwritten. */
    if (atomic_load(&g_menu_touch_action) != 2 || action == 0)
        atomic_store(&g_menu_touch_action, action);
}

static _Atomic int g_menu_mode;

JNIEXPORT jint JNICALL
Java_net_hta_halotrial_GameActivity_nativePaused(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return atomic_load(&g_paused);
}

JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativePause(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    atomic_store(&g_paused, 1);
}

JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativeResume(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    atomic_store(&g_paused, 0);
}

JNIEXPORT jint JNICALL
Java_net_hta_halotrial_GameActivity_nativeMenuMode(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return atomic_load(&g_menu_mode);
}


/* ---- the submenus -------------------------------------------------
 * The Java overlay draws them over the ring, with ui.map's art and words,
 * which are copied out here once and kept for the life of the process, so
 * the UI thread can read them while this thread loads and frees levels. */
JNIEXPORT jstring JNICALL
Java_net_hta_halotrial_GameActivity_nativeShellText(JNIEnv *env, jclass cls)
{
    (void)cls;
    if (!atomic_load(&g_shell_ready) || !g_shell.text) return NULL;
    return (*env)->NewStringUTF(env, g_shell.text);
}

/* One piece of art as { width, height, ARGB... }, or null. */
JNIEXPORT jintArray JNICALL
Java_net_hta_halotrial_GameActivity_nativeShellArt(JNIEnv *env, jclass cls, jint which)
{
    (void)cls;
    if (!atomic_load(&g_shell_ready) || which < 0 || which >= HTA_SHELL_ART_COUNT) return NULL;
    const hta_shell_image *im = &g_shell.art[which];
    if (!im->rgba || !im->width || !im->height) return NULL;
    size_t n = (size_t)im->width * im->height;
    jint *px = (jint *)malloc((n + 2u) * sizeof(jint));
    if (!px) return NULL;
    px[0] = (jint)im->width; px[1] = (jint)im->height;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *c = im->rgba + i * 4u;
        px[i + 2] = (jint)(((uint32_t)c[3] << 24) | ((uint32_t)c[0] << 16) |
                           ((uint32_t)c[1] << 8) | c[2]);
    }
    jintArray out = (*env)->NewIntArray(env, (jsize)(n + 2u));
    if (out) (*env)->SetIntArrayRegion(env, out, 0, (jsize)(n + 2u), px);
    free(px);
    return out;
}

JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativeShellScreen(JNIEnv *env, jclass cls, jint screen)
{
    (void)env; (void)cls;
    atomic_store(&g_shell_screen, (int)screen);
}

/* The shell's clicks: 0 cursor, 1 forward, 2 back. */
static _Atomic int g_shell_sounds;

JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativeShellSound(JNIEnv *env, jclass cls, jint which)
{
    (void)env; (void)cls;
    if (which >= 0 && which < 3) atomic_fetch_or(&g_shell_sounds, 1 << which);
}

/* A match set up in the submenus, handed over once. */
typedef struct {
    int  mode;               /* 0 solo, 1 host, 2 join */
    int  bots, skill, kills, minutes, respawn, max_players, port, vehicles;
    int  gametype;           /* hta_game_mode; solo only for now */
    char host[64];
    char name[HTA_NET_NAME];
} match_setup;
static match_setup g_match;
static _Atomic int g_match_ready;

JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativeStartMatch(JNIEnv *env, jclass cls, jintArray cfg,
                                                     jstring host, jstring name)
{
    (void)cls;
    if (atomic_load(&g_match_ready)) return;     /* one is already on its way */
    match_setup m;
    memset(&m, 0, sizeof(m));
    jint v[10] = { 0, 3, 1, 25, 0, 5, 8, 32270, HTA_VROSTER_ALL, HTA_MODE_SLAYER };
    jsize n = cfg ? (*env)->GetArrayLength(env, cfg) : 0;
    if (n > 10) n = 10;
    if (n > 0) (*env)->GetIntArrayRegion(env, cfg, 0, n, v);
    m.mode = v[0]; m.bots = v[1]; m.skill = v[2]; m.kills = v[3];
    m.minutes = v[4]; m.respawn = v[5]; m.max_players = v[6]; m.port = v[7];
    m.vehicles = v[8];
    m.gametype = v[9];
    const char *u;
    if (host && (u = (*env)->GetStringUTFChars(env, host, NULL))) {
        snprintf(m.host, sizeof(m.host), "%s", u);
        (*env)->ReleaseStringUTFChars(env, host, u);
    }
    if (name && (u = (*env)->GetStringUTFChars(env, name, NULL))) {
        snprintf(m.name, sizeof(m.name), "%s", u);
        (*env)->ReleaseStringUTFChars(env, name, u);
    }
    g_match = m;
    atomic_store(&g_match_ready, 1);             /* publishes g_match */
}

/* Ask the LAN (or given addresses) who is hosting. `targets` is a comma
 * list of IPv4 addresses, broadcast ones included; each answer is a line
 * "ip \t port \t name \t players \t max \t kills \t minutes". Blocks for
 * `ms`; call it off the UI thread. */
JNIEXPORT jstring JNICALL
Java_net_hta_halotrial_GameActivity_nativeLanScan(JNIEnv *env, jclass cls, jstring targets,
                                                  jint port, jint ms)
{
    (void)cls;
    char list[512] = "", out[2048] = "";
    const char *u;
    if (targets && (u = (*env)->GetStringUTFChars(env, targets, NULL))) {
        snprintf(list, sizeof(list), "%s", u);
        (*env)->ReleaseStringUTFChars(env, targets, u);
    }
    hta_net_scan scan;
    if (!hta_net_scan_open(&scan)) return (*env)->NewStringUTF(env, "");
    if (port <= 0 || port > 65535) port = 32270;
    double t0 = hta_time_seconds(), next_send = t0;
    size_t len = 0;
    char seen[16][16];
    int nseen = 0;
    while (hta_time_seconds() - t0 < (double)ms / 1000.0) {
        if (hta_time_seconds() >= next_send) {
            /* A few tries: broadcasts get dropped on busy Wi-Fi. */
            char tmp[512];
            snprintf(tmp, sizeof(tmp), "%s", list);
            for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ","))
                hta_net_scan_send(&scan, tok, (uint16_t)port);
            next_send += 0.4;
        }
        hta_net_info info;
        char ip[16];
        uint16_t from = 0;
        while (hta_net_scan_recv(&scan, &info, ip, &from)) {
            int dup = 0;
            for (int i = 0; i < nseen; i++) dup |= !strcmp(seen[i], ip);
            if (dup || nseen >= 16) continue;
            snprintf(seen[nseen++], 16, "%s", ip);
            int w = snprintf(out + len, sizeof(out) - len, "%s\t%u\t%s\t%u\t%u\t%u\t%u\n",
                             ip, from, info.name, info.players, info.max_players,
                             info.score_limit, info.time_limit);
            if (w > 0 && (size_t)w < sizeof(out) - len) len += (size_t)w;
        }
        struct timespec nap = { 0, 20 * 1000 * 1000 };
        nanosleep(&nap, NULL);
    }
    hta_net_scan_close(&scan);
    return (*env)->NewStringUTF(env, out);
}

/* Open the network side of a match: host (a server here, and this phone
 * joins it through loopback) or join someone else's. */
static void net_begin(hta_android *s, const char *host, bool hosting, uint16_t port,
                      const hta_net_info *info)
{
    if (!host || !host[0]) return;
    if (hosting) {
        s->net_hosting = hta_net_server_open(&s->host_server, port);
        if (!s->net_hosting) hta_log("[net] could not bind LAN host UDP %u", port);
        else if (info) {
            uint8_t max = s->host_server.info.max_players;
            s->host_server.info = *info;
            if (!s->host_server.info.max_players) s->host_server.info.max_players = max;
        }
    }
    s->net_enabled = hta_net_client_open(&s->net, host, port);
    if (!s->net_enabled && s->net_hosting) {
        hta_net_server_close(&s->host_server);
        s->net_hosting = false;
    }
    if (hosting && !s->net_hosting && s->net_enabled) {
        hta_net_client_close(&s->net); s->net_enabled = false;
    }
    atomic_store(&g_net_status, s->net_enabled ? 1 : 5);
    hta_log("[net] %s %s:%u", s->net_enabled ? (hosting ? "hosting, joined" : "joining")
                                            : "bad address", host, port);
}

/* The submenus have set up a match: take it, and go. */
static void match_take(hta_android *s)
{
    match_setup m = g_match;
    s->bot_count = m.mode == 2 ? 0 : m.bots;
    if (s->bot_count < 0) s->bot_count = 0;
    if (s->bot_count > 7) s->bot_count = 7;
    s->bot_skill = m.skill < 0 ? 0 : m.skill > 3 ? 3 : m.skill;
    s->score_limit = m.kills > 0 ? m.kills : 0;
    s->time_limit_min = m.minutes > 0 ? m.minutes : 0;
    /* Halo's INSTANT still has to show you your body go down. Ours. */
    s->respawn_delay = m.respawn < HTA_RESPAWN_MIN ? HTA_RESPAWN_MIN : (float)m.respawn;
    s->vehicle_roster = m.vehicles >= 0 && m.vehicles < HTA_VROSTER_COUNT ? m.vehicles
                                                                           : HTA_VROSTER_ALL;
    /* A host chooses the game; a joiner learns it from the host's GAME. */
    s->game_mode = m.mode != 2 && m.gametype > 0 && m.gametype < HTA_MODE_COUNT
                 ? m.gametype : HTA_MODE_SLAYER;
    uint16_t port = (uint16_t)(m.port > 0 && m.port < 65536 ? m.port : 32270);
    if (m.mode == 1) {
        hta_net_info info;
        memset(&info, 0, sizeof(info));
        info.max_players = (uint8_t)(m.max_players < 2 ? 2 : m.max_players > (int)HTA_NET_MAX_PLAYERS
                                     ? (int)HTA_NET_MAX_PLAYERS : m.max_players);
        info.score_limit = (uint8_t)(s->score_limit ? s->score_limit : HTA_SLAYER_SCORE_LIMIT);
        info.time_limit = (uint8_t)s->time_limit_min;
        snprintf(info.name, sizeof(info.name), "%s", m.name[0] ? m.name : "Halo");
        net_begin(s, "127.0.0.1", true, port, &info);
    } else if (m.mode == 2) {
        net_begin(s, m.host, false, port, NULL);
    }
    hta_log("[menu] match: mode %d, game %d, %d bot(s) skill %d, %d to win, %d min, respawn %.0f s",
            m.mode, s->game_mode, s->bot_count, s->bot_skill, s->score_limit, s->time_limit_min,
            s->respawn_delay);
    atomic_store(&g_match_ready, 0);
    atomic_store(&g_shell_screen, 0);
    s->menu_go = true;
}

/* One menu frame: the camera drifts, the words answer the finger, the
 * music plays. */
static void menu_frame(hta_android *s, float dt)
{
    if (atomic_load(&g_match_ready)) { match_take(s); return; }
    int snd = atomic_exchange(&g_shell_sounds, 0);
    for (int k = 0; k < 3; k++) if (snd & (1 << k)) menu_sound(s, k);
    int screen = atomic_load(&g_shell_screen);
    if (screen != g_shell_shown) {
        /* The Trial's own camera points; `multiplayer` looks at the ring's
         * inner face, which we still draw washed out, so the dark shots. */
        static const char *const SHOT[3] = { "uicam", "new_campaign", "load_campaign" };
        hta_menu_focus(&s->menu, SHOT[screen >= 0 && screen < 3 ? screen : 0]);
        s->menu.shell = screen != 0;
        s->menu_pressed = -1;
        g_shell_shown = screen;
    }
    int action = atomic_exchange(&g_menu_touch_action, -1);
    if (s->menu.shell) action = -1;     /* the overlay has the finger */
    if (action >= 0 && s->gfx) {
        uint32_t w = 0, h = 0;
        hta_gfx_extent(s->gfx, &w, &h);
        float x = (float)atomic_load(&g_menu_touch_x) / 10000.0f * (float)w;
        float y = (float)atomic_load(&g_menu_touch_y) / 10000.0f * (float)h;
        int hit = hta_menu_hit(&s->menu, x, y);
        if (action == 0 || action == 1) {
            if (hit >= 0 && hit != s->menu.selected) { s->menu.selected = hit; menu_sound(s, 0); }
            if (action == 0) s->menu_pressed = hit;
        } else if (action == 2) {
            if (hit >= 0 && hit == s->menu_pressed) menu_activate(s, hit);
            s->menu_pressed = -1;
        }
    }
    if (s->music_in != HTA_AUDIO_NO_CLIP && !s->music_looping) {
        s->music_left -= dt;
        if (s->music_left <= 0.0f) {
            hta_audio_loop_stop(&s->audio, HTA_LOOP_MUSIC_IN);
            if (s->music_loop != HTA_AUDIO_NO_CLIP)
                hta_audio_loop(&s->audio, HTA_LOOP_MUSIC, s->music_loop, 0.9f);
            s->music_looping = true;
        }
    }
    hta_audio_android_poll(&s->audio);
    hta_menu_update(&s->menu, dt);
    if (!s->has_window || !s->gfx) return;
    rebuild_gfx_if_size_changed(s);
    if (!s->gfx) return;
    uint32_t w = 0, h = 0;
    hta_gfx_extent(s->gfx, &w, &h);
    hta_menu_layout(&s->menu, w, h);
    hta_camera cam;
    hta_menu_camera(&s->menu, &cam, h ? (float)w / (float)h : 1.777f);
    hta_scene sc = s->menu.light;
    hta_gfx_overlay ov = { s->gpu_menu_ui, s->menu.overlay.vertices, s->menu.overlay.vertex_count,
                           s->menu.overlay.submeshes, s->menu.overlay.submesh_count };
    if (!hta_gfx_draw(s->gfx, &cam, &sc, s->gpu_menu_scene, s->gpu_menu_sky, NULL, NULL, 0,
                      NULL, s->gpu_menu_ui ? &ov : NULL)) {
        stop_gfx(s);
        if (s->app->window) start_gfx(s);
    }
}

/* Leave the menu for the level: the music stops, the shell's meshes go,
 * and Blood Gulch loads behind the last menu frame. */
static void menu_leave(hta_android *s)
{
    hta_audio_loop_stop(&s->audio, HTA_LOOP_MUSIC_IN);
    hta_audio_loop_stop(&s->audio, HTA_LOOP_MUSIC);
    stop_gfx(s);
    hta_menu_free(&s->menu);
    s->menu_mode = false;
    atomic_store(&g_menu_mode, 0);
    if (!load_map(s)) {
        hta_log("[app] running without map data: %s", s->status);
        s->scene.clear[0] = 0.55f; s->scene.clear[1] = 0.05f; s->scene.clear[2] = 0.45f;
    }
    if (s->app->window) start_gfx(s);
}

/* ------------------------------- input ------------------------------- */

#define STICK_RADIUS_FRAC 0.12f   /* of the shorter screen edge */
#define LOOK_SENSITIVITY  0.006f
#define PAD_LOOK_SPEED    2.6f

static int32_t on_input(struct android_app *app, AInputEvent *event)
{
    hta_android *s = (hta_android *)app->userData;
    int32_t src  = AInputEvent_getSource(event);
    int32_t type = AInputEvent_getType(event);

    if (type == AINPUT_EVENT_TYPE_MOTION) {
        if ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
            float lx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0);
            float ly = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0);
            float rx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0);
            float ry = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0);
            const float DEAD = 0.18f;
            s->pad_move[0] = fabsf(lx) > DEAD ? lx : 0.0f;
            s->pad_move[1] = fabsf(ly) > DEAD ? ly : 0.0f;
            s->pad_look[0] = fabsf(rx) > DEAD ? rx : 0.0f;
            s->pad_look[1] = fabsf(ry) > DEAD ? ry : 0.0f;
            {
                float rt = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0);
                s->pad_fire = rt > 0.35f;
            }
            return 1;
        }

        int32_t action = AMotionEvent_getAction(event);
        int32_t code   = action & AMOTION_EVENT_ACTION_MASK;
        int32_t pindex = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                       >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
        if (!app->window) return 1;
        float w = (float)ANativeWindow_getWidth(app->window);
        float h = (float)ANativeWindow_getHeight(app->window);
        if (w <= 0 || h <= 0) return 1;

        size_t count = AMotionEvent_getPointerCount(event);

        if (code == AMOTION_EVENT_ACTION_DOWN || code == AMOTION_EVENT_ACTION_POINTER_DOWN) {
            if ((size_t)pindex < count) {
                int32_t id = AMotionEvent_getPointerId(event, (size_t)pindex);
                float x = AMotionEvent_getX(event, (size_t)pindex);
                float y = AMotionEvent_getY(event, (size_t)pindex);
                /* If the Java HUD is up it owns stick/fire/jump. Always keep
                 * native look so a missing overlay cannot freeze the camera. */
                if (!s->hud_ready && x < w * 0.5f && s->move_pointer < 0) {
                    s->move_pointer = id;
                    s->move_origin[0] = x; s->move_origin[1] = y;
                    s->move_cur[0] = x;    s->move_cur[1] = y;
                } else if (s->look_pointer < 0) {
                    if (!s->hud_ready && x > w * 0.82f && y > h * 0.72f)
                        s->jump_held = true;
                    else if (!s->hud_ready && x > w * 0.64f && y > h * 0.72f)
                        s->fire_held = true;
                    else {
                        s->look_pointer = id;
                        s->look_last[0] = x; s->look_last[1] = y;
                    }
                }
            }
        } else if (code == AMOTION_EVENT_ACTION_MOVE) {
            for (size_t i = 0; i < count; i++) {
                int32_t id = AMotionEvent_getPointerId(event, i);
                float x = AMotionEvent_getX(event, i);
                float y = AMotionEvent_getY(event, i);
                if (id == s->move_pointer) { s->move_cur[0] = x; s->move_cur[1] = y; }
                else if (id == s->look_pointer) {
                    s->pending_yaw   += -(x - s->look_last[0]) * LOOK_SENSITIVITY;
                    s->pending_pitch += -(y - s->look_last[1]) * LOOK_SENSITIVITY;
                    s->look_last[0] = x; s->look_last[1] = y;
                }
            }
        } else if (code == AMOTION_EVENT_ACTION_UP || code == AMOTION_EVENT_ACTION_POINTER_UP ||
                   code == AMOTION_EVENT_ACTION_CANCEL) {
            int32_t id = ((size_t)pindex < count) ? AMotionEvent_getPointerId(event, (size_t)pindex) : -1;
            if (code == AMOTION_EVENT_ACTION_CANCEL) {
                s->move_pointer = s->look_pointer = -1;
                s->jump_held = false;
                s->fire_held = false;
            } else {
                if (id == s->move_pointer) s->move_pointer = -1;
                if (id == s->look_pointer) s->look_pointer = -1;
                s->jump_held = false;
                s->fire_held = false;
            }
        }
        return 1;
    }

    if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t code = AKeyEvent_getKeyCode(event);
        bool down = (AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN);
        if (code == AKEYCODE_BACK) {
            /* In the menu, BACK leaves the app. In a game it pauses, the
             * way Halo's does; the pause screen offers the way out. */
            if (down) {
                if (s->menu_mode && atomic_load(&g_shell_screen)) {
                    /* A submenu is up: BACK goes up one, as Halo's does. */
                    call_activity(s, "shellBack");
                } else if (s->menu_mode || !s->map_loaded) {
                    hta_log("[input] BACK -> exit");
                    ANativeActivity_finish(app->activity);
                } else {
                    atomic_store(&g_paused, !atomic_load(&g_paused));
                    hta_log("[input] BACK -> %s", atomic_load(&g_paused) ? "paused" : "resumed");
                }
            }
            return 1;
        }
        if (code == AKEYCODE_BUTTON_A || code == AKEYCODE_SPACE) { s->jump_held = down; return 1; }
        if (code == AKEYCODE_BUTTON_R1 || code == AKEYCODE_BUTTON_R2 ||
            code == AKEYCODE_BUTTON_X) { s->fire_held = down; return 1; }
        if (code == AKEYCODE_BUTTON_Y && down) { s->hud_reload = true; return 1; }
        if (code == AKEYCODE_BUTTON_R1 && down) { s->hud_melee = true; return 1; }
        if (code == AKEYCODE_BUTTON_L1 && down) { s->hud_swap = true; return 1; }
        if (code == AKEYCODE_BUTTON_THUMBR && down) { s->hud_zoom = true; return 1; }
        if (code == AKEYCODE_BUTTON_L2 && down) { s->hud_grenade = true; return 1; }
        if (code == AKEYCODE_BUTTON_B && down) {
            s->player.noclip = !s->player.noclip;
            hta_log("[input] noclip %s", s->player.noclip ? "ON" : "OFF");
            return 1;
        }
        return 1;
    }
    return 0;
}

static void gather_input(hta_android *s, hta_player_input *in, float dt)
{
    memset(in, 0, sizeof(*in));

    /* Java HUD stick, or fallback invisible left-half stick */
    if (s->hud_ready) {
        in->move_right   += s->hud_move[0];
        in->move_forward += s->hud_move[1];
    } else if (s->move_pointer >= 0 && s->app->window) {
        float w = (float)ANativeWindow_getWidth(s->app->window);
        float h = (float)ANativeWindow_getHeight(s->app->window);
        float shorter = w < h ? w : h;
        float r = shorter * STICK_RADIUS_FRAC;
        if (r < 1.0f) r = 1.0f;
        float dx = (s->move_cur[0] - s->move_origin[0]) / r;
        float dy = (s->move_cur[1] - s->move_origin[1]) / r;
        float len = sqrtf(dx*dx + dy*dy);
        if (len > 1.0f) { dx /= len; dy /= len; }
        in->move_right   += dx;
        in->move_forward += -dy;   /* dragging up walks forward */
    }

    /* gamepad */
    in->move_right   += s->pad_move[0];
    in->move_forward += -s->pad_move[1];
    in->look_yaw   += -s->pad_look[0] * PAD_LOOK_SPEED * dt;
    in->look_pitch += -s->pad_look[1] * PAD_LOOK_SPEED * dt;

    /* accumulated touch look */
    in->look_yaw   += s->pending_yaw;
    in->look_pitch += s->pending_pitch;
    s->pending_yaw = s->pending_pitch = 0.0f;

    if (in->move_forward >  1.0f) in->move_forward =  1.0f;
    if (in->move_forward < -1.0f) in->move_forward = -1.0f;
    if (in->move_right   >  1.0f) in->move_right   =  1.0f;
    if (in->move_right   < -1.0f) in->move_right   = -1.0f;

    in->jump = s->jump_held || s->hud_jump;
    in->fire = s->fire_held || s->pad_fire || s->hud_fire;
    in->crouch = s->hud_crouch;
}

/* ------------------------------ lifecycle ------------------------------ */

static void start_gfx(hta_android *s)
{
    char err[HTA_ERRLEN];
    s->gfx = hta_gfx_create_window(s->app->window, err, sizeof(err));
    if (!s->gfx) { hta_log("[gfx] init FAILED: %s", err); s->has_window = false; return; }
    s->has_window = true;
    s->win_w = ANativeWindow_getWidth(s->app->window);
    s->win_h = ANativeWindow_getHeight(s->app->window);

    uint32_t w, h;
    hta_gfx_extent(s->gfx, &w, &h);
    hta_log("[gfx] ready: %s, %ux%u", hta_gfx_device_name(s->gfx), w, h);
    s->cam.aspect = h ? (float)w / (float)h : 1.777f;
    menu_gpu_upload(s);

    if (s->have_mesh) {
        double t0 = hta_time_seconds();
        s->gpu_mesh = hta_gfx_mesh_upload(s->gfx, &s->mesh, err, sizeof(err));
        if (!s->gpu_mesh) hta_log("[gfx] mesh upload FAILED: %s", err);
        else hta_log("[gfx] uploaded %u verts / %u tris in %.1f ms (device mem %.2f MiB)",
                     s->mesh.vertex_count, s->mesh.index_count/3,
                     (hta_time_seconds()-t0)*1000.0,
                     hta_gfx_device_memory_used(s->gfx)/(1024.0*1024.0));
        if (s->have_sky) {
            s->gpu_sky = hta_gfx_mesh_upload(s->gfx, &s->sky, err, sizeof(err));
            if (!s->gpu_sky) hta_log("[gfx] sky upload FAILED: %s", err);
        }
        if (s->gun.n) {
            hta_gun_build_mesh(&s->gun);
            s->gpu_fx = hta_gfx_mesh_upload(s->gfx, &s->gun.mesh, err, sizeof(err));
        }
        /* The world-space dynamic meshes. These are NOT conditional on
         * there being scorch marks -- they were, briefly, which meant
         * rockets and their smoke only appeared once you had shot a wall. */
        if (s->proj.loaded && s->proj.mesh.index_count)
            s->gpu_proj = hta_gfx_mesh_upload_dynamic_world(s->gfx, &s->proj.mesh,
                                                      err, sizeof(err));
        if (s->parts.loaded && s->parts.mesh.index_count)
            s->gpu_parts = hta_gfx_mesh_upload_dynamic(s->gfx, &s->parts.mesh,
                                                       err, sizeof(err));
        if (s->nades.loaded && s->nades.mesh.index_count)
            s->gpu_nades = hta_gfx_mesh_upload_dynamic_world(s->gfx, &s->nades.mesh,
                                                       err, sizeof(err));
        if (s->corpse.loaded && s->corpse.mesh.index_count) {
            s->gpu_corpse = hta_gfx_mesh_upload_dynamic_world(s->gfx, &s->corpse.mesh,
                                                        err, sizeof(err));
            if (!s->gpu_corpse) hta_log("[gfx] corpse upload FAILED: %s", err);
        }
        if (s->bot.loaded && s->bot.actor.mesh.index_count) {
            s->gpu_bot = hta_gfx_mesh_upload_dynamic_world(s->gfx, &s->bot.actor.mesh,
                                                     err, sizeof(err));
            if (!s->gpu_bot) hta_log("[gfx] bot upload FAILED: %s", err);
        }
        for (int slot=0;slot<2;slot++)
            if (s->remote[slot].loaded && s->remote[slot].mesh.index_count) {
                s->gpu_remote[slot]=hta_gfx_mesh_upload_dynamic_world(s->gfx,
                    &s->remote[slot].mesh,err,sizeof(err));
                if (!s->gpu_remote[slot]) hta_log("[gfx] remote upload FAILED: %s",err);
            }
        if (s->items.have_mesh && s->items.mesh.index_count) {
            s->gpu_items = hta_gfx_mesh_upload_dynamic_world(s->gfx, &s->items.mesh,
                                                       err, sizeof(err));
            if (!s->gpu_items) hta_log("[gfx] item upload FAILED: %s", err);
            s->items_upload = HTA_ITEMS_UPLOAD_FRAMES;
        }
        for (uint32_t t = 0; s->vehicles.loaded && t < s->vehicles.type_count; t++) {
            s->gpu_vtypes[t] = hta_gfx_mesh_upload(s->gfx, &s->vehicles.types[t].mesh,
                                                   err, sizeof(err));
            if (!s->gpu_vtypes[t]) hta_log("[gfx] vehicle %s upload FAILED: %s",
                                           s->vehicles.types[t].name, err);
        }
        game_gpu_upload(s);
        if (s->have_fp) {
            s->gpu_fp = hta_gfx_mesh_upload_dynamic(s->gfx, &s->vm.mesh, err, sizeof(err));
            if (!s->gpu_fp) hta_log("[gfx] fp weapon upload FAILED: %s", err);
        }
        if (s->hud.elem_count) {
            s->gpu_hud = hta_gfx_mesh_upload_dynamic(s->gfx, &s->hud.mesh, err, sizeof(err));
            if (!s->gpu_hud) hta_log("[gfx] hud upload FAILED: %s", err);
        }
        float span = s->mesh.bounds_max[0] - s->mesh.bounds_min[0];
        if (span < 1.0f) span = 1.0f;
        s->cam.zfar  = span * 6.0f;
        s->cam.znear = 0.02f;
    }
}

static void stop_gfx(hta_android *s)
{
    game_gpu_free(s);
    menu_gpu_free(s);
    for (int slot=0;slot<2;slot++)
        if (s->gpu_remote[slot]) { hta_gfx_mesh_free(s->gfx,s->gpu_remote[slot]); s->gpu_remote[slot]=NULL; }
    for (uint32_t t = 0; t < HTA_VEHICLE_TYPES; t++)
        if (s->gpu_vtypes[t]) { hta_gfx_mesh_free(s->gfx, s->gpu_vtypes[t]); s->gpu_vtypes[t] = NULL; }
    if (s->gpu_bot) { hta_gfx_mesh_free(s->gfx, s->gpu_bot); s->gpu_bot = NULL; }
    if (s->gpu_items) { hta_gfx_mesh_free(s->gfx, s->gpu_items); s->gpu_items = NULL; }
    if (s->gpu_corpse) { hta_gfx_mesh_free(s->gfx, s->gpu_corpse); s->gpu_corpse = NULL; }
    if (s->gpu_hud) { hta_gfx_mesh_free(s->gfx, s->gpu_hud); s->gpu_hud = NULL; }
    if (s->gpu_fp) { hta_gfx_mesh_free(s->gfx, s->gpu_fp); s->gpu_fp = NULL; }
    if (s->gpu_nades) { hta_gfx_mesh_free(s->gfx, s->gpu_nades); s->gpu_nades = NULL; }
    if (s->gpu_parts) { hta_gfx_mesh_free(s->gfx, s->gpu_parts); s->gpu_parts = NULL; }
    if (s->gpu_proj) { hta_gfx_mesh_free(s->gfx, s->gpu_proj); s->gpu_proj = NULL; }
    if (s->gpu_fx) { hta_gfx_mesh_free(s->gfx, s->gpu_fx); s->gpu_fx = NULL; }
    if (s->gpu_sky) { hta_gfx_mesh_free(s->gfx, s->gpu_sky); s->gpu_sky = NULL; }
    if (s->gpu_mesh) { hta_gfx_mesh_free(s->gfx, s->gpu_mesh); s->gpu_mesh = NULL; }
    if (s->gfx) { hta_gfx_destroy(s->gfx); s->gfx = NULL; }
    s->has_window = false;
}

/* SCREEN_ORIENTATION_SENSOR_LANDSCAPE = 6. SetupActivity is already locked;
 * NativeActivity must request it too or Samsung can keep the portrait window
 * that INIT_WINDOW first sees. */
static void request_landscape(struct android_app *app)
{
    JavaVM *vm = app->activity->vm;
    JNIEnv *env = NULL;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || !env) return;
    jclass cls = (*env)->GetObjectClass(env, app->activity->clazz);
    if (!cls) return;
    jmethodID mid = (*env)->GetMethodID(env, cls, "setRequestedOrientation", "(I)V");
    if (mid) {
        (*env)->CallVoidMethod(env, app->activity->clazz, mid, 6);
        hta_log("[app] requested SENSOR_LANDSCAPE");
    }
    (*env)->DeleteLocalRef(env, cls);
}

/* NativeActivity intent is read once at startup. Only numeric IPv4 is
 * accepted by the UDP layer; there is no DNS lookup on the game thread. */
static void read_net_host(struct android_app *app, char host[64], bool *hosting)
{
    host[0]=0; *hosting=false;
    JavaVM *vm=app->activity->vm; JNIEnv *env=NULL;
    if ((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK || !env) return;
    jobject activity=app->activity->clazz;
    jclass activity_class=(*env)->GetObjectClass(env,activity);
    jmethodID get_intent=(*env)->GetMethodID(env,activity_class,"getIntent","()Landroid/content/Intent;");
    jobject intent=get_intent ? (*env)->CallObjectMethod(env,activity,get_intent) : NULL;
    if (intent) {
        jclass intent_class=(*env)->GetObjectClass(env,intent);
        jmethodID get_string=(*env)->GetMethodID(env,intent_class,"getStringExtra",
                                                 "(Ljava/lang/String;)Ljava/lang/String;");
        jstring key=(*env)->NewStringUTF(env,"net_host");
        jstring value=get_string ? (jstring)(*env)->CallObjectMethod(env,intent,get_string,key) : NULL;
        if (value) {
            const char *utf=(*env)->GetStringUTFChars(env,value,NULL);
            if (utf) { snprintf(host,64,"%s",utf); (*env)->ReleaseStringUTFChars(env,value,utf); }
            (*env)->DeleteLocalRef(env,value);
        }
        (*env)->DeleteLocalRef(env,key);
        jmethodID get_bool=(*env)->GetMethodID(env,intent_class,"getBooleanExtra",
                                              "(Ljava/lang/String;Z)Z");
        jstring host_key=(*env)->NewStringUTF(env,"net_hosting");
        if (get_bool) *hosting=(*env)->CallBooleanMethod(env,intent,get_bool,host_key,JNI_FALSE);
        (*env)->DeleteLocalRef(env,host_key);
        (*env)->DeleteLocalRef(env,intent_class);
        (*env)->DeleteLocalRef(env,intent);
    }
    (*env)->DeleteLocalRef(env,activity_class);
}

/* An int extra on the launch intent, or `def`. The setup screen puts the
 * bot count and their skill there. */
static int intent_int(struct android_app *app, const char *key, int def)
{
    JavaVM *vm=app->activity->vm; JNIEnv *env=NULL;
    if ((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK || !env) return def;
    int out=def;
    jobject activity=app->activity->clazz;
    jclass ac=(*env)->GetObjectClass(env,activity);
    jmethodID gi=(*env)->GetMethodID(env,ac,"getIntent","()Landroid/content/Intent;");
    jobject intent=gi ? (*env)->CallObjectMethod(env,activity,gi) : NULL;
    if (intent) {
        jclass ic=(*env)->GetObjectClass(env,intent);
        jmethodID get=(*env)->GetMethodID(env,ic,"getIntExtra","(Ljava/lang/String;I)I");
        jstring k=(*env)->NewStringUTF(env,key);
        if (get) out=(*env)->CallIntMethod(env,intent,get,k,(jint)def);
        (*env)->DeleteLocalRef(env,k);
        (*env)->DeleteLocalRef(env,ic);
        (*env)->DeleteLocalRef(env,intent);
    }
    (*env)->DeleteLocalRef(env,ac);
    return out;
}

static void net_action(hta_android *s, uint8_t kind)
{
    if (kind==HTA_NET_EVENT_MELEE) s->net_melee_count++;
    if (kind==HTA_NET_EVENT_GRENADE) s->net_grenade_count++;
    if (!s->net_enabled || !s->net.connected) return;
    if (s->net_hosting && kind==HTA_NET_EVENT_FIRE && s->game_on) {
        int32_t weapon=held_roster(s);
        if (weapon>=0) {
            hta_net_fx fx={.kind=HTA_NET_FX_FIRE,.entity=(uint8_t)s->me,
                           .weapon=(uint8_t)weapon};
            for (int k=0;k<3;k++) fx.pos[k]=s->cam.pos[k];
            hta_camera_forward(&s->cam,fx.dir);
            hta_net_server_fx(&s->host_server,&fx);
        }
    }
    hta_net_event e={s->net.id,kind,(uint8_t)s->held_slot,++s->net_event_id};
    hta_net_client_event(&s->net,&e);
}

static void net_host_peers(hta_android *s, double now)
{
    if (!s->game_on) return;
    for (unsigned i=0;i<HTA_NET_MAX_PLAYERS;i++) {
        hta_net_peer *p=&s->host_server.peers[i];
        int32_t unit=s->peer_unit[i];
        if (!p->active) {
            if (unit>=0 && unit!=s->me) hta_game_remove(&s->game,unit);
            s->peer_unit[i]=-1;
            s->peer_melee_seen[i]=s->peer_grenade_seen[i]=0;
            s->peer_reload_seen[i]=s->peer_pickup_seen[i]=0;
            s->peer_action_seen[i]=0;
            continue;
        }
        if (p->player.id==s->net.id) {
            s->peer_unit[i]=(int8_t)s->me;
            continue;
        }
        if (unit<0) {
            char name[HTA_GAME_NAME];
            snprintf(name,sizeof(name),"Player %u",p->player.id);
            unit=hta_game_add(&s->game,HTA_UNIT_REMOTE,name,HTA_TEAM_AUTO);
            if (unit<0) continue;
            s->peer_unit[i]=(int8_t)unit;
            hta_game_spawn(&s->game,unit);
            game_gpu_upload(s);
            hta_log("[net] player %u joined game unit %d",p->player.id,unit);
        }
        hta_unit *u=&s->game.units[unit];
        hta_unit_input *in=&u->in;
        if (!p->has_control || now-p->last_control_at>0.3) {
            in->move.move_forward=in->move.move_right=0.0f;
            in->move.fire=in->move.jump=in->move.crouch=false;
            in->fire2=false;
            continue;
        }
        const hta_net_control *c=&p->control;
        in->move.move_forward=c->forward;
        in->move.move_right=c->right;
        in->move.jump=(c->flags&HTA_NET_JUMP)!=0;
        in->move.fire=(c->flags&HTA_NET_TRIGGER)!=0;
        in->move.crouch=(c->flags&HTA_NET_DUCK)!=0;
        in->fire2=(c->flags&HTA_NET_ALT)!=0;
        in->move.look_yaw=in->move.look_pitch=0.0f;
        if (s->peer_action_seen[i]!=c->action_count) {
            s->peer_action_seen[i]=c->action_count; in->action=true;
        }
        u->eye.yaw=c->yaw; u->eye.pitch=c->pitch;
        if (u->slot!=c->weapon_slot) in->swap=true;
        if (s->peer_melee_seen[i]!=c->melee_count) {
            s->peer_melee_seen[i]=c->melee_count; in->melee=true;
        }
        if (s->peer_grenade_seen[i]!=c->grenade_count) {
            s->peer_grenade_seen[i]=c->grenade_count; in->grenade=true;
        }
        if (s->peer_reload_seen[i]!=c->reload_count) {
            s->peer_reload_seen[i]=c->reload_count; in->reload=true;
        }
        if (s->peer_pickup_seen[i]!=c->pickup_count) {
            s->peer_pickup_seen[i]=c->pickup_count; in->pickup=true;
            /* A joiner puts the flag down with SWAP, which it sends as a
             * pickup: its gun slot does not change while it carries. */
            if (u->flag>=0) in->swap=true;
        }
    }
}

/* What this player carries, into the game's copy of them: the game drops
 * it when they die and tells everybody else what they hold. */
static void mirror_local(hta_android *s)
{
    if (!s->game_on || s->me<0 || s->me>=(int32_t)s->game.unit_count) return;
    hta_unit *local=&s->game.units[s->me];
    local->slot=s->held_slot&1u;
    for (unsigned slot=0;slot<2;slot++)
        local->carry[slot].weapon=slot<s->held_count ?
            hta_game_weapon_index(&s->game,s->held[slot]) : -1;
    local->carry[local->slot].ammo=s->ammo;
    unsigned other=local->slot^1u;
    if (other<HTA_CARRY_MAX && s->held_ammo_set[other]) local->carry[other].ammo=s->held_ammo[other];
    local->grenades=s->nade_count;
    local->powerup=s->powerup;
}

/* Put the gun in hand on the ground: swapped for another, it stays. */
static void drop_held(hta_android *s)
{
    if (!s->game_on || (s->net_enabled && !s->net_hosting)) return;
    int32_t w=held_roster(s);
    if (w<0) return;
    float at[3]={s->player.pos[0],s->player.pos[1],s->player.pos[2]+0.3f};
    float vel[3]={cosf(s->cam.yaw)*0.6f,sinf(s->cam.yaw)*0.6f,0.8f};
    hta_game_drop_weapon(&s->game,w,&s->ammo,at,s->cam.yaw,vel);
}

static void net_host_world(hta_android *s)
{
    if (!s->game_on || !s->net.connected) return;
    mirror_local(s);
    hta_net_world w={0};
    w.time=s->game.time; w.round=s->world_round;
    w.bot_count=(uint8_t)s->bot_count; w.over=s->game.over;
    w.winner=s->game.winner>=0 && s->game.winner<HTA_GAME_MAX_UNITS
        ? (uint8_t)s->game.winner : 255;
    w.score_limit=(uint8_t)s->game.score_limit;
    w.time_limit=(uint8_t)s->time_limit_min;
    w.respawn_time=(uint8_t)s->respawn_delay;
    if (s->items.loaded) {
        w.item_count=(uint8_t)s->items.count;
        for (uint8_t i=0;i<w.item_count;i++) {
            if (s->items.slot[i].present) w.item_present[i>>3]|=(uint8_t)(1u<<(i&7u));
            w.item_choice[i]=(uint8_t)s->items.slot[i].choice;
        }
    }
    for (uint32_t i=0;i<s->game.unit_count && w.count<HTA_NET_MAX_ENTITIES;i++) {
        const hta_unit *u=&s->game.units[i];
        if (u->kind==HTA_UNIT_NONE) continue;
        hta_net_entity *e=&w.entities[w.count++];
        e->id=(uint8_t)i;
        e->kind=u->kind==HTA_UNIT_BOT ? HTA_NET_ENTITY_BOT : HTA_NET_ENTITY_PLAYER;
        e->peer_id=u->kind==HTA_UNIT_LOCAL ? s->net.id : 0;
        if (u->kind==HTA_UNIT_REMOTE)
            for (unsigned p=0;p<HTA_NET_MAX_PLAYERS;p++)
                if (s->peer_unit[p]==(int8_t)i) e->peer_id=s->host_server.peers[p].player.id;
        if (u->alive) e->flags|=HTA_NET_ENTITY_ALIVE;
        if (u->body.on_ground) e->flags|=HTA_NET_ENTITY_GROUNDED;
        if (u->body.crouch_t>0.5f) e->flags|=HTA_NET_ENTITY_CROUCH;
        if (u->fired) e->flags|=HTA_NET_ENTITY_FIRE;
        if (u->meleed) e->flags|=HTA_NET_ENTITY_MELEE;
        if (u->threw) e->flags|=HTA_NET_ENTITY_GRENADE;
        if (s->game.teams && u->team==HTA_TEAM_BLUE) e->flags|=HTA_NET_ENTITY_BLUE;
        const hta_game_weapon *held=hta_game_held(&s->game,(int32_t)i);
        e->weapon=held ? (uint8_t)(held-s->game.weapons) : 255;
        for (int k=0;k<3;k++) e->pos[k]=u->body.pos[k];
        for (int k=0;k<2;k++) e->velocity[k]=u->body.velocity[k];
        e->yaw=u->eye.yaw; e->pitch=u->eye.pitch;
        e->health=u->vitals.health; e->shield=u->vitals.shield;
        e->score=(int16_t)u->score; e->kills=(int16_t)u->kills;
        e->deaths=(int16_t)u->deaths;
        for (int slot=0;slot<2;slot++)
            e->carry[slot]=u->carry[slot].weapon>=0 ?
                (uint8_t)u->carry[slot].weapon : 255;
        e->slot=(uint8_t)(u->slot&1u);
        e->grenades=(uint8_t)u->grenades;
        e->powerup=u->powerup;
        const hta_ammo *ammo=&u->carry[e->slot].ammo;
        e->ammo_loaded=(uint16_t)ammo->loaded;
        e->ammo_reserve=(uint16_t)ammo->reserve;
        size_t j=0;
        while (j<HTA_NET_ENTITY_NAME-1 && u->name[j]) {
            unsigned char ch=(unsigned char)u->name[j];
            e->name[j]=(char)(ch>=32 && ch<127 ? ch : '?'); j++;
        }
        e->name[j]=0;
    }
    hta_net_server_world(&s->host_server,&w);
    hta_net_projectiles projectiles={0};
    for (uint32_t p=0;p<s->game.pool_count;p++)
        for (uint32_t slot=0;slot<HTA_PROJ_MAX;slot++) {
            const hta_projectile *q=&s->game.pools[p].live[slot];
            if (!q->alive || projectiles.count>=HTA_NET_MAX_PROJECTILES) continue;
            hta_net_projectile *out=&projectiles.live[projectiles.count++];
            out->pool=(uint8_t)p; out->slot=(uint8_t)slot;
            for (int k=0;k<3;k++) { out->pos[k]=q->pos[k]; out->dir[k]=q->dir[k]; }
            out->speed=q->speed;
        }
    const hta_projectiles *local_pools[2]={&s->proj,&s->nades};
    for (uint8_t p=0;p<2;p++) {
        const hta_projectiles *pool=local_pools[p];
        if (!pool->loaded) continue;
        for (uint8_t slot=0;slot<HTA_PROJ_MAX;slot++) {
            const hta_projectile *q=&pool->live[slot];
            if (!q->alive || projectiles.count>=HTA_NET_MAX_PROJECTILES) continue;
            hta_net_projectile *out=&projectiles.live[projectiles.count++];
            out->pool=(uint8_t)(p ? HTA_NET_POOL_HOST_GRENADES : HTA_NET_POOL_HOST_WEAPON);
            out->slot=slot;
            for (int k=0;k<3;k++) { out->pos[k]=q->pos[k]; out->dir[k]=q->dir[k]; }
            out->speed=q->speed;
        }
    }
    hta_net_server_projectiles(&s->host_server,&projectiles);
    if (s->vehicles.loaded) {
        static hta_net_vehicles cars;
        memset(&cars,0,sizeof(cars));
        for (uint32_t i=0;i<s->vehicles.count && cars.count<HTA_NET_MAX_VEHICLES;i++) {
            const hta_vehicle *v=&s->vehicles.cars[i];
            hta_net_vehicle *o=&cars.cars[cars.count++];
            o->index=(uint8_t)i;
            o->flags=(uint8_t)((v->active ? HTA_NET_VEHICLE_ACTIVE : 0) |
                               (v->grounded ? HTA_NET_VEHICLE_GROUNDED : 0) |
                               (v->ctl.driven ? HTA_NET_VEHICLE_DRIVEN : 0));
            for (int k=0;k<3;k++) o->pos[k]=v->pos[k];
            o->yaw=v->yaw; o->pitch=v->pitch; o->roll=v->roll+v->bank;
            o->aim_yaw=v->aim_yaw; o->aim_pitch=v->aim_pitch;
            o->steering=v->steering; o->wheel_spin=v->wheel_spin;
            o->barrel_spin=v->barrel_spin; o->speed=v->speed;
            if (o->speed>300.0f) o->speed=300.0f;
            if (o->speed<-300.0f) o->speed=-300.0f;
            for (unsigned k=0;k<HTA_NET_VEHICLE_SEATS;k++)
                o->occupant[k]=k<HTA_VEHICLE_SEATS && v->occupant[k]>=0 &&
                    v->occupant[k]<(int8_t)HTA_NET_MAX_ENTITIES ? (uint8_t)v->occupant[k] : 255;
            unsigned w=0;
            for (uint32_t k=0;k<v->point_count && w<4;k++) {
                if (!v->points[k].wheel) continue;
                float t=v->points[k].travel;
                o->travel[w++]=t>0.25f ? 0.25f : t<-0.25f ? -0.25f : t;
            }
            for (int k=0;k<3;k++) {
                if (o->pos[k]>327.0f) o->pos[k]=327.0f;
                if (o->pos[k]<-327.0f) o->pos[k]=-327.0f;
            }
        }
        hta_net_server_vehicles(&s->host_server,&cars);
    }
    static hta_net_drops drops;
    memset(&drops,0,sizeof(drops));
    for (int i=0;i<HTA_GAME_MAX_DROPS && drops.count<HTA_NET_MAX_DROPS;i++) {
        const hta_game_drop *d=&s->game.drops[i];
        if (!d->live || d->weapon<0 || d->weapon>=(int32_t)HTA_NET_MAX_WEAPONS) continue;
        drops.drop[drops.count].weapon=(uint8_t)d->weapon;
        for (int k=0;k<3;k++) {
            float v=d->pos[k];
            drops.drop[drops.count].pos[k]=v>327.0f ? 327.0f : v<-327.0f ? -327.0f : v;
        }
        drops.drop[drops.count].yaw=d->yaw;
        drops.count++;
    }
    hta_net_server_drops(&s->host_server,&drops);
    /* The rules beside WORLD: mode, scores, both flags, every hull. */
    static hta_net_game gm;
    memset(&gm,0,sizeof(gm));
    gm.mode=(uint8_t)(s->game.mode<HTA_MODE_COUNT ? s->game.mode : 0);
    gm.score_limit=(uint8_t)(s->game.score_limit>255 ? 255 : s->game.score_limit<0 ? 0 : s->game.score_limit);
    for (int t=0;t<2;t++) {
        int sc=s->game.team_score[t];
        gm.team_score[t]=(int16_t)(sc>32767 ? 32767 : sc<-32767 ? -32767 : sc);
        const hta_game_flag *f=&s->game.flags[t];
        gm.flag[t].present=f->present ? 1 : 0;
        gm.flag[t].state=f->state<=HTA_FLAG_DROPPED ? f->state : HTA_FLAG_HOME;
        gm.flag[t].carrier=f->state==HTA_FLAG_CARRIED && f->carrier>=0 &&
            f->carrier<(int32_t)HTA_NET_MAX_ENTITIES ? (uint8_t)f->carrier : 255;
        for (int k=0;k<3;k++) {
            float v=f->pos[k];
            gm.flag[t].pos[k]=v>327.0f ? 327.0f : v<-327.0f ? -327.0f : v;
        }
        gm.flag[t].yaw=f->yaw;
    }
    gm.winner_team=s->game.over && s->game.winner_team>=0 ? (uint8_t)s->game.winner_team : 255;
    for (uint32_t i=0;i<s->vehicles.count && i<HTA_NET_MAX_VEHICLES;i++) {
        float h=hta_game_hull(&s->game,(int32_t)i);
        gm.hull[i]=!s->vehicles.cars[i].active ? 0 :
                   (uint8_t)(h*254.0f+1.0f>255.0f ? 255.0f : h*254.0f+1.0f);
    }
    hta_net_server_game(&s->host_server,&gm);
}

/* The host's vehicles, as of its last snapshot and eased between them,
 * and who is sitting where. A client runs no vehicle physics. */
static void net_client_vehicles(hta_android *s, double now)
{
    if (!s->game_on || !s->vehicles.loaded) return;
    if (s->net.have_vehicles && s->net.last_vehicle_tick!=s->vehicles_applied_tick) {
        s->vehicles_applied_tick=s->net.last_vehicle_tick;
        for (uint32_t i=0;i<s->vehicles.count;i++)
            for (uint32_t k=0;k<HTA_VEHICLE_SEATS;k++) s->vehicles.cars[i].occupant[k]=-1;
        for (uint32_t u=0;u<s->game.unit_count;u++) {
            s->game.units[u].vehicle=-1; s->game.units[u].seat=-1;
        }
        for (uint8_t n=0;n<s->net.vehicles.count;n++) {
            const hta_net_vehicle *in=&s->net.vehicles.cars[n];
            if (in->index>=s->vehicles.count) continue;
            uint32_t i=in->index;
            hta_vehicle *v=&s->vehicles.cars[i];
            bool was=v->active;
            v->active=(in->flags&HTA_NET_VEHICLE_ACTIVE)!=0;
            v->grounded=(in->flags&HTA_NET_VEHICLE_GROUNDED)!=0;
            v->ctl.driven=(in->flags&HTA_NET_VEHICLE_DRIVEN)!=0;
            s->vfrom[i]=s->vhave[i] && was ? s->vto[i] : *in;
            s->vto[i]=*in; s->vhave[i]=true;
            uint32_t seats=hta_vehicles_seat_count(&s->vehicles,i);
            for (uint32_t k=0;k<seats && k<HTA_NET_VEHICLE_SEATS;k++) {
                uint8_t u=in->occupant[k];
                if (u==255 || u>=s->game.unit_count) continue;
                v->occupant[k]=(int8_t)u;
                s->game.units[u].vehicle=(int16_t)i;
                s->game.units[u].seat=(int8_t)k;
            }
        }
        s->vsnap_time=now;
    }
    /* Between snapshots, a straight line: 20 a second on a LAN. */
    float t=(float)((now-s->vsnap_time)/0.05);
    if (t<0.0f) t=0.0f;
    if (t>1.5f) t=1.5f;
    for (uint32_t i=0;i<s->vehicles.count;i++) {
        if (!s->vhave[i]) continue;
        hta_vehicle *v=&s->vehicles.cars[i];
        const hta_net_vehicle *a=&s->vfrom[i], *b=&s->vto[i];
        for (int k=0;k<3;k++) v->pos[k]=a->pos[k]+(b->pos[k]-a->pos[k])*t;
        #define LERP_ANGLE(x) (a->x+hta_angle_wrap(b->x-a->x)*t)
        v->yaw=LERP_ANGLE(yaw); v->pitch=LERP_ANGLE(pitch); v->roll=LERP_ANGLE(roll);
        v->bank=0.0f;
        v->aim_yaw=LERP_ANGLE(aim_yaw); v->aim_pitch=LERP_ANGLE(aim_pitch);
        v->steering=LERP_ANGLE(steering); v->wheel_spin=LERP_ANGLE(wheel_spin);
        v->barrel_spin=LERP_ANGLE(barrel_spin);
        #undef LERP_ANGLE
        v->speed=b->speed;
        unsigned w=0;
        for (uint32_t k=0;k<v->point_count && w<4;k++)
            if (v->points[k].wheel) { v->points[k].travel=a->travel[w]+(b->travel[w]-a->travel[w])*t; w++; }
    }
    hta_vehicles_sync(&s->vehicles);
}

static void net_client_projectiles(hta_android *s)
{
    if (!s->game_on || !s->net.have_projectiles ||
        s->net.last_projectile_tick==s->projectile_applied_tick) return;
    s->projectile_applied_tick=s->net.last_projectile_tick;
    for (uint32_t p=0;p<s->game.pool_count;p++)
        for (uint32_t slot=0;slot<HTA_PROJ_MAX;slot++)
            s->game.pools[p].live[slot].alive=false;
    for (uint32_t slot=0;slot<HTA_PROJ_MAX;slot++) {
        s->proj.live[slot].alive=false;
        s->nades.live[slot].alive=false;
    }
    for (uint8_t i=0;i<s->net.projectiles.count;i++) {
        const hta_net_projectile *in=&s->net.projectiles.live[i];
        if (in->slot>=HTA_PROJ_MAX) continue;
        hta_projectiles *pool=in->pool==HTA_NET_POOL_HOST_WEAPON ? &s->proj :
                              in->pool==HTA_NET_POOL_HOST_GRENADES ? &s->nades :
                              in->pool<s->game.pool_count ? &s->game.pools[in->pool] : NULL;
        if (!pool || !pool->loaded) continue;
        hta_projectile *q=&pool->live[in->slot];
        q->alive=true; q->speed=in->speed; q->fuse=-1.0f;
        for (int k=0;k<3;k++) { q->pos[k]=in->pos[k]; q->dir[k]=in->dir[k]; }
    }
    for (uint32_t p=0;p<s->game.pool_count;p++)
        hta_projectiles_update(&s->game.pools[p],NULL,0.0f);
    if (s->proj.loaded) hta_projectiles_update(&s->proj,NULL,0.0f);
    if (s->nades.loaded) hta_projectiles_update(&s->nades,NULL,0.0f);
}

static void net_client_world(hta_android *s)
{
    if (!s->game_on || !s->net.have_world ||
        s->world_applied_tick==s->net.last_world_tick) return;
    const hta_net_world *w=&s->net.world;
    int32_t mine=-1;
    for (uint8_t i=0;i<w->count;i++)
        if (w->entities[i].peer_id==s->net.id) mine=w->entities[i].id;
    if (mine<0 || mine>=HTA_GAME_MAX_UNITS) return;
    if (!s->world_local_bound) {
        if (mine!=s->me) {
            s->game.units[mine]=s->game.units[s->me];
            s->game.units[s->me].kind=HTA_UNIT_NONE;
            s->game.units[s->me].alive=false;
            s->me=mine;
            s->game.local=mine;
            s->vit=&s->game.units[mine].vitals;
        }
        s->world_local_bound=true;
    }
    if (s->world_round!=w->round) {
        s->world_round=w->round;
        memset(s->feed,0,sizeof(s->feed));
        s->banner[0]=0; s->over_timer=0.0f;
    }
    s->world_applied_tick=s->net.last_world_tick;
    s->bot_count=w->bot_count;
    bool was_over=s->game.over;
    s->game.time=w->time; s->game.over=w->over!=0;
    s->game.winner=w->winner==255 ? HTA_GAME_NONE : w->winner;
    s->game.score_limit=w->score_limit;
    s->game.time_limit=(float)w->time_limit*60.0f;
    s->game.respawn_time=w->respawn_time;
    s->respawn_delay=w->respawn_time;
    if (s->items.loaded && s->items.count==w->item_count) {
        for (uint8_t i=0;i<w->item_count;i++) {
            bool shown=(w->item_present[i>>3]&(1u<<(i&7u)))!=0;
            if (s->items.slot[i].present!=shown ||
                s->items.slot[i].choice!=w->item_choice[i]) {
                s->items.slot[i].present=shown;
                s->items.slot[i].choice=w->item_choice[i];
                s->items.dirty=true;
            }
        }
    }
    bool present[HTA_GAME_MAX_UNITS]={0};
    for (uint8_t i=0;i<w->count;i++) {
        const hta_net_entity *e=&w->entities[i];
        int32_t idx=e->id;
        present[idx]=true;
        if (s->game.unit_count<=(uint32_t)idx) s->game.unit_count=(uint32_t)idx+1u;
        hta_unit *u=&s->game.units[idx];
        bool was_alive=u->kind!=HTA_UNIT_NONE && u->alive;
        if (u->kind==HTA_UNIT_NONE) {
            memset(u,0,sizeof(*u));
            hta_player_init(&u->body);
            hta_player_apply_physics(&u->body,&s->game.phys);
            hta_camera_init(&u->eye);
            u->vitals=s->game.vitals_template;
        }
        u->kind=idx==s->me ? HTA_UNIT_LOCAL :
                e->kind==HTA_NET_ENTITY_BOT ? HTA_UNIT_BOT : HTA_UNIT_REMOTE;
        snprintf(u->name,sizeof(u->name),"%s",e->name);
        u->alive=(e->flags&HTA_NET_ENTITY_ALIVE)!=0;
        if (was_alive && !u->alive) {
            u->dead_for=0.0f;
            u->death_yaw=e->yaw;
        } else if (!u->alive) u->dead_for+=0.05f;
        u->score=e->score; u->kills=e->kills; u->deaths=e->deaths;
        u->fired=(e->flags&HTA_NET_ENTITY_FIRE)!=0;
        /* The motion tracker's "fired lately", kept here from the flag. */
        u->since_shot=u->fired ? 0.0f : u->since_shot+0.05f;
        u->meleed=(e->flags&HTA_NET_ENTITY_MELEE)!=0;
        u->threw=(e->flags&HTA_NET_ENTITY_GRENADE)!=0;
        u->team=(e->flags&HTA_NET_ENTITY_BLUE)!=0 ? HTA_TEAM_BLUE : HTA_TEAM_RED;
        u->body.on_ground=(e->flags&HTA_NET_ENTITY_GROUNDED)!=0;
        u->body.crouch_t=(e->flags&HTA_NET_ENTITY_CROUCH)!=0 ? 1.0f : 0.0f;
        u->slot=0;
        for (int slot=0;slot<2;slot++)
            u->carry[slot].weapon=e->carry[slot]==255 ? -1 : e->carry[slot];
        u->slot=e->slot;
        u->grenades=e->grenades;
        u->powerup=e->powerup;
        if (idx!=s->me) {
            for (int k=0;k<3;k++) u->body.pos[k]=e->pos[k];
            for (int k=0;k<2;k++) u->body.velocity[k]=e->velocity[k];
            u->eye.yaw=e->yaw; u->eye.pitch=e->pitch;
            for (int k=0;k<3;k++) u->eye.pos[k]=e->pos[k];
            u->eye.pos[2]+=u->body.eye_height;
        }
        if (idx==s->me && !u->alive && !s->dead) u->vitals.died=true;
        if (u->alive &&
            e->health+e->shield < u->vitals.health+u->vitals.shield-0.01f) {
            u->hurt=true;
            if (idx==s->me) {
                u->vitals.took_damage=true;
                if (u->vitals.shield>0.0f && e->shield<=0.0f)
                    u->vitals.shield_broke=true;
            }
        }
        u->vitals.health=e->health; u->vitals.shield=e->shield;
        if (idx==s->me && u->alive) {
            if (s->dead) {
                respawn(s);
                s->dead=false;
                s->dead_timer=-HTA_DEATH_FADE_IN;
                s->cam.yaw=e->yaw; s->cam.pitch=e->pitch;
            }
            float dx=e->pos[0]-s->player.pos[0];
            float dy=e->pos[1]-s->player.pos[1];
            float dz=e->pos[2]-s->player.pos[2];
            float dist=sqrtf(dx*dx+dy*dy+dz*dz);
            float f=dist>0.5f || !s->net_spawned ? 1.0f : 0.12f;
            for (int k=0;k<3;k++) {
                float delta=(e->pos[k]-s->player.pos[k])*f;
                s->player.pos[k]+=delta;
                s->cam.pos[k]+=delta;
            }
            u->vitals.died=false;
        }
        if (idx==s->me) {
            uint32_t held[2]={0}; unsigned held_count=0;
            for (int slot=0;slot<2;slot++)
                if (u->carry[slot].weapon>=0 &&
                    (uint32_t)u->carry[slot].weapon<s->game.weapon_count)
                    held[held_count++]=s->game.weapons[u->carry[slot].weapon].tag;
            if (held_count) {
                unsigned slot=e->slot<held_count ? e->slot : 0;
                bool change=s->held_count!=held_count || s->held_slot!=slot;
                for (unsigned k=0;k<held_count;k++)
                    if (s->held[k]!=held[k]) change=true;
                s->held_count=held_count; s->held_slot=slot;
                for (unsigned k=0;k<held_count;k++) s->held[k]=held[k];
                if (change) equip_weapon(s,s->held[slot]);
            }
            s->ammo.loaded=e->ammo_loaded;
            s->ammo.reserve=e->ammo_reserve;
            s->nade_count=e->grenades;
            s->powerup=e->powerup;
        }
    }
    for (uint32_t i=0;i<s->game.unit_count;i++)
        if (!present[i] && (int32_t)i!=s->me) {
            s->game.units[i].kind=HTA_UNIT_NONE;
            s->game.units[i].alive=false;
        }
    if (s->game.over && !was_over && s->game.teams && s->me>=0) {
        int mine=s->game.units[s->me].team;
        char buf[96];
        uint32_t tx=s->game.winner_team<0 ? 55 : s->game.winner_team==mine ? 58 : 56;
        const char *fb=s->game.winner_team<0 ? "Game ends in a draw" :
                       s->game.winner_team==mine ? "Your team won" : "Your team lost";
        if (!hta_ustr_get(&s->cache,s->game.text_tag,tx,buf,sizeof(buf)))
            snprintf(buf,sizeof(buf),"%s",fb);
        snprintf(s->banner,sizeof(s->banner),"%s",buf);
        s->banner_age=0.0f;
        if (s->line_snd[HTA_LINE_GAME_OVER]) play_tag(s,s->line_snd[HTA_LINE_GAME_OVER],1.0f);
    } else if (s->game.over && !was_over) {
        const char *winner="Nobody";
        if (s->game.winner>=0 && s->game.winner<(int32_t)s->game.unit_count)
            winner=s->game.units[s->game.winner].name;
        snprintf(s->banner,sizeof(s->banner),"%s",s->game.winner==s->me ?
                 "You won" : winner);
        s->banner_age=0.0f;
    }
    game_gpu_upload(s);
}

static void net_frame(hta_android *s, double now, float dt, const hta_player_input *in)
{
    if (!s->net_enabled) return;
    if (s->net_hosting) hta_net_server_pump(&s->host_server,now);
    hta_net_client_pump(&s->net,now);
    hta_net_fx fx;
    while (hta_net_client_pop_fx(&s->net,&fx)) {
        if (s->net_hosting || !s->game_on) continue;
        /* Our own on-foot shots we heard already; a vehicle gun we did not. */
        if (fx.entity==(uint8_t)s->me &&
            !(fx.kind==HTA_NET_FX_FIRE && fx.weapon<s->game.weapon_count &&
              s->game.weapons[fx.weapon].vehicle)) continue;
        if (fx.kind==HTA_NET_FX_FIRE && fx.weapon<s->game.weapon_count) {
            tracer(s,fx.entity==255 ? -1 : (int32_t)fx.entity,fx.weapon,fx.pos,fx.dir);
            if (!s->unit_fire_known[fx.weapon]) {
                s->unit_fire_known[fx.weapon]=1;
                s->unit_fire_snd[fx.weapon]=hta_effect_first_sound(&s->cache,
                    s->game.weapons[fx.weapon].def.firing_fx_id);
            }
            if (s->unit_fire_snd[fx.weapon])
                play_tag_at(s,s->unit_fire_snd[fx.weapon],fx.pos,1.0f);
            if (s->game.weapons[fx.weapon].vehicle &&
                s->vfire_recipe[fx.weapon]!=HTA_PART_NO_RECIPE)
                hta_particles_burst(&s->parts,s->vfire_recipe[fx.weapon],fx.pos,fx.dir);
        } else if (fx.kind==HTA_NET_FX_IMPACT && fx.weapon<s->game.weapon_count) {
            uint32_t proj=s->game.weapons[fx.weapon].def.projectile_id;
            uint32_t sound=hta_projectile_impact_sound(&s->cache,proj,fx.material);
            if (sound) play_tag_at(s,sound,fx.pos,0.8f);
            hta_gun_add_mark(&s->gun,fx.pos,fx.dir,HTA_MARK_SIZE);
        } else if (fx.kind==HTA_NET_FX_WRECK) {
            wreck_fx(s,fx.pos);
        } else if (fx.kind==HTA_NET_FX_DETONATE && fx.weapon<s->game.pool_count) {
            const hta_projectiles *pool=&s->game.pools[fx.weapon];
            if (pool->detonation_snd)
                play_tag_at(s,pool->detonation_snd,fx.pos,1.0f);
            shake_effect(s,pool->det_effect,fx.pos);
            if (s->pool_recipe[fx.weapon]!=HTA_PART_NO_RECIPE)
                hta_particles_burst(&s->parts,s->pool_recipe[fx.weapon],fx.pos,fx.dir);
            if (pool->blast_radius>0.0f)
                hta_gun_add_mark(&s->gun,fx.pos,fx.dir,pool->blast_radius);
        }
    }
    hta_net_kill kill;
    while (hta_net_client_pop_kill(&s->net,&kill)) {
        if (s->net_hosting) continue;
        if (kill.killer==s->me && kill.victim!=s->me &&
            kill.victim<s->game.unit_count) {
            char line[96];
            snprintf(line,sizeof(line),"You killed %s",s->game.units[kill.victim].name);
            feed_push(s,line);
        } else feed_push(s,kill.text);
    }
    if (s->net_hosting) net_host_peers(s,now);
    if (!s->net.connected) {
        s->net_spawned=false; s->remote_visible=false;
        if (!s->net_hosting) {
            s->world_applied_tick=0;
            s->projectile_applied_tick=0;
            s->world_local_bound=false;
            for (uint32_t i=0;i<s->game.unit_count;i++)
                if ((int32_t)i!=s->me) s->game.units[i].kind=HTA_UNIT_NONE;
            for (uint32_t p=0;p<s->game.pool_count;p++)
                for (uint32_t slot=0;slot<HTA_PROJ_MAX;slot++)
                    s->game.pools[p].live[slot].alive=false;
        }
    }
    atomic_store(&g_net_status, !s->net.connected ?
                 s->net.reject_reason==HTA_NET_REJECT_MAP ? 7 :
                 s->net.reject_reason==HTA_NET_REJECT_FULL ? 8 : 1 :
                 !s->net_hosting ? (s->net.have_world ? 2 : 6) :
                 hta_net_server_count(&s->host_server)>1 ? 4 : 3);
    if (s->net.connected && !s->net_spawned && s->spawn_count) {
        hta_spawn_point *sp=&s->spawn[(s->net.id-1u)%s->spawn_count];
        hta_player_spawn(&s->player,sp); s->cam.yaw=sp->facing;
        float z;
        if (s->col.built && hta_collision_ground(&s->col,s->player.pos[0],
                s->player.pos[1],s->player.pos[2]+8.0f,&z)) {
            s->player.pos[2]=z; s->player.on_ground=true;
        }
        s->net_spawned=true;
        hta_log("[net] joined as player %u",s->net.id);
    }
    if (!s->net_hosting) net_client_world(s);
    if (!s->net_hosting) net_client_projectiles(s);
    if (!s->net_hosting) net_client_vehicles(s,now);
    if (!s->net_hosting && s->game_on && s->net.have_game &&
        s->net.last_game_tick!=s->game_applied_tick) {
        s->game_applied_tick=s->net.last_game_tick;
        const hta_net_game *gm=&s->net.game;
        hta_game_flag_state fl[2];
        for (int t=0;t<2;t++) {
            fl[t].present=gm->flag[t].present!=0;
            fl[t].state=gm->flag[t].state;
            fl[t].carrier=gm->flag[t].carrier==255 ? -1 : gm->flag[t].carrier;
            for (int k=0;k<3;k++) fl[t].pos[k]=gm->flag[t].pos[k];
            fl[t].yaw=gm->flag[t].yaw;
        }
        int sc[2]={gm->team_score[0],gm->team_score[1]};
        hta_game_mode mode=(hta_game_mode)(gm->mode<HTA_MODE_COUNT ? gm->mode : 0);
        if (mode!=s->game.mode) hta_log("[net] the host plays mode %d",(int)mode);
        hta_game_mirror_rules(&s->game,mode,gm->score_limit,sc,
                              gm->winner_team==255 ? -1 : gm->winner_team,fl);
        s->game_mode=(int)s->game.mode;
        /* Hulls as the host has them, for our HULL readout and sparks. */
        for (uint32_t i=0;i<s->vehicles.count && i<HTA_NET_MAX_VEHICLES;i++) {
            s->game.vgun[i].hull_max=1.0f;
            s->game.vgun[i].hull=gm->hull[i] ? (float)(gm->hull[i]-1)/254.0f : 0.0f;
        }
    }
    if (!s->net_hosting && s->game_on && s->net.have_drops) {
        memset(s->game.drops,0,sizeof(s->game.drops));
        for (uint8_t i=0;i<s->net.drops.count && i<HTA_GAME_MAX_DROPS;i++) {
            hta_game_drop *d=&s->game.drops[i];
            d->live=s->net.drops.drop[i].weapon<s->game.weapon_count;
            d->weapon=s->net.drops.drop[i].weapon;
            for (int k=0;k<3;k++) d->pos[k]=s->net.drops.drop[i].pos[k];
            d->yaw=s->net.drops.drop[i].yaw; d->rest=true;
        }
    }
    if (s->net.connected && now-s->net_last_send>=0.05) {
        hta_net_control c={0};
        c.id=s->net.id; c.weapon_slot=(uint8_t)(s->held_slot&1u);
        c.forward=in->move_forward; c.right=in->move_right;
        c.yaw=s->cam.yaw; c.pitch=s->cam.pitch;
        if (in->jump) c.flags|=HTA_NET_JUMP;
        if (in->fire || s->veh_fire) c.flags|=HTA_NET_TRIGGER;
        if (in->crouch) c.flags|=HTA_NET_DUCK;
        if (s->hud_alt) c.flags|=HTA_NET_ALT;
        c.action_count=s->net_action_count;
        c.melee_count=s->net_melee_count;
        c.grenade_count=s->net_grenade_count;
        c.reload_count=s->net_reload_count;
        c.pickup_count=s->net_pickup_count;
        hta_net_client_control(&s->net,&c);
        hta_net_player p={0}; p.id=s->net.id; p.weapon=(uint8_t)s->held_slot;
        for (int k=0;k<3;k++) { p.pos[k]=s->player.pos[k]; p.velocity[k]=s->player.velocity[k]; }
        p.yaw=s->cam.yaw; p.pitch=s->cam.pitch;
        if (s->player.on_ground) p.flags|=HTA_NET_GROUNDED;
        if (s->player.crouch_t>0.5f) p.flags|=HTA_NET_CROUCH;
        hta_net_client_state(&s->net,&p); s->net_last_send=now;
    }
    if (s->net.stats.snapshots_in!=s->net_last_snapshots) {
        s->net_last_snapshots=s->net.stats.snapshots_in;
        bool had_remote=s->remote_visible;
        s->remote_visible=false;
        for (unsigned i=0;i<HTA_NET_MAX_PLAYERS;i++) {
            if (!s->net.present[i] || i+1u==s->net.id) continue;
            hta_net_player next=s->net.players[i];
            s->remote_from=had_remote && s->remote_id==next.id
                ? s->remote_to : next;
            s->remote_to=next; s->remote_id=next.id;
            s->remote_snapshot_time=now; s->remote_visible=true;
            break; /* renderer currently has one remote actor slot */
        }
    }
    hta_net_event e;
    while (hta_net_client_pop_event(&s->net,&e)) {
        int slot=e.weapon==1 ? 1 : 0;
        hta_actor *a=&s->remote[slot];
        if (!a->loaded || e.actor!=s->remote_id) continue;
        const char *clip=NULL;
        if (e.kind==HTA_NET_EVENT_FIRE) clip=slot ? "stand pistol hp fire-1" : "stand rifle ar fire-1";
        if (e.kind==HTA_NET_EVENT_MELEE) clip=slot ? "stand pistol hp melee" : "stand rifle ar melee";
        if (e.kind==HTA_NET_EVENT_GRENADE) clip="stand rifle throw-grenade";
        if (clip && hta_actor_play(a,clip,false))
            s->remote_action_until=now+(e.kind==HTA_NET_EVENT_GRENADE ? 0.8 : 0.3);
        hta_log("[net] player %u event %u weapon %u",e.actor,e.kind,e.weapon);
    }
    if (s->remote_visible) {
        const hta_net_player *p=&s->remote_to;
        int slot=p->weapon==1 ? 1 : 0;
        hta_actor *a=&s->remote[slot];
        if (!a->loaded) goto net_after_remote;
        const char *clip=(p->flags&HTA_NET_CROUCH) ?
            (slot ? "crouch pistol idle" : "crouch rifle idle") :
            (slot ? "stand pistol idle" : "stand rifle idle");
        if (!(p->flags&HTA_NET_GROUNDED)) clip=(p->flags&HTA_NET_CROUCH)
             ? "crouch rifle airborne" : (slot ? "stand pistol airborne" : "stand rifle airborne");
        else if (hypotf(p->velocity[0],p->velocity[1])>0.2f)
            clip=(p->flags&HTA_NET_CROUCH) ?
                (slot ? "crouch pistol move-front" : "crouch rifle move-front") :
                (slot ? "stand pistol move-front" : "stand rifle move-front");
        if (now>=s->remote_action_until &&
            (a->clip<0 || strcmp(a->graph.anims[a->clip].name,clip)))
            hta_actor_play(a,clip,false);
        hta_net_player visible;
        if (hta_net_interpolate(&s->remote_from,p,
                (float)((now-s->remote_snapshot_time)/0.05),&visible)) {
            hta_actor_update(a,dt); hta_actor_place(a,visible.pos,visible.yaw);
        }
    }
net_after_remote:
    if (s->net_hosting) net_host_world(s);
}

static void rebuild_gfx_if_size_changed(hta_android *s)
{
    if (!s->app->window) return;
    int w = ANativeWindow_getWidth(s->app->window);
    int h = ANativeWindow_getHeight(s->app->window);
    if (w <= 0 || h <= 0) return;
    if (s->gfx && s->win_w == w && s->win_h == h) return;
    hta_log("[app] window %dx%d (was %dx%d) -> rebuild", w, h, s->win_w, s->win_h);
    stop_gfx(s);
    start_gfx(s);
}

static void on_cmd(struct android_app *app, int32_t cmd)
{
    hta_android *s = (hta_android *)app->userData;
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (app->window) {
            hta_log("[app] INIT_WINDOW %dx%d", ANativeWindow_getWidth(app->window),
                    ANativeWindow_getHeight(app->window));
            if (!s->probe_done) {
                /* Trial base first, then retail — see INVADER_ASSET_PIPELINE.md §2.3 */
                hta_probe_fixed_map(0x4BF10000ull, 23u * 1024u * 1024u);
                hta_probe_fixed_map(0x40440000ull, 23u * 1024u * 1024u);
                s->probe_done = true;
            }
            if (s->menu_mode && !s->menu.loaded && !menu_load(s)) {
                s->menu_mode = false;
                atomic_store(&g_menu_mode, 0);
            }
            if (!s->map_loaded && !s->menu_mode) {
                if (!load_map(s)) {
                    hta_log("[app] running without map data: %s", s->status);
                    /* Unmistakable on-screen signal: magenta means "no data".
                     * Sky blue means the map loaded. No text renderer yet. */
                    s->scene.clear[0] = 0.55f;
                    s->scene.clear[1] = 0.05f;
                    s->scene.clear[2] = 0.45f;
                }
            }
            start_gfx(s);
        }
        break;
    case APP_CMD_TERM_WINDOW: hta_log("[app] TERM_WINDOW"); stop_gfx(s); break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONTENT_RECT_CHANGED:
    case APP_CMD_CONFIG_CHANGED:
        hta_log("[app] size/config cmd %d", (int)cmd);
        rebuild_gfx_if_size_changed(s);
        break;
    case APP_CMD_GAINED_FOCUS: hta_log("[app] focus gained"); break;
    case APP_CMD_LOST_FOCUS:   hta_log("[app] focus lost");   break;
    default: break;
    }
}

/* Where the pawn is, for the HUD to draw. Chasing a collision bug from a
 * screenshot needs coordinates: without them you are guessing which doorway
 * out of a 126 x 145 world unit map the reporter was standing in. */
static char g_debug_text[128];
static char g_ammo_text[32];

JNIEXPORT jstring JNICALL
Java_net_hta_halotrial_GameActivity_nativeDebugText(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, g_debug_text);
}

JNIEXPORT jint JNICALL
Java_net_hta_halotrial_GameActivity_nativeVehicleMode(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return atomic_load(&g_vehicle_mode);
}

JNIEXPORT jstring JNICALL
Java_net_hta_halotrial_GameActivity_nativeVehicleText(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, g_vehicle_text);
}

JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativeHudAlt(JNIEnv *env, jclass cls, jboolean down)
{
    (void)env; (void)cls;
    if (g_android) g_android->hud_alt = down ? true : false;
}

JNIEXPORT jint JNICALL
Java_net_hta_halotrial_GameActivity_nativeDamageFlash(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return atomic_load(&g_damage_flash);
}

JNIEXPORT jint JNICALL
Java_net_hta_halotrial_GameActivity_nativeNetStatus(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return atomic_load(&g_net_status);
}

JNIEXPORT jstring JNICALL
Java_net_hta_halotrial_GameActivity_nativeAmmoText(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, g_ammo_text);
}

JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativeHudReady(JNIEnv *env, jclass cls, jboolean ready)
{
    (void)env; (void)cls;
    g_hud_wanted = ready ? true : false;
    if (g_android) g_android->hud_ready = g_hud_wanted;
}

JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativeHudMove(JNIEnv *env, jclass cls, jfloat x, jfloat y)
{
    (void)env; (void)cls;
    if (!g_android) return;
    g_android->hud_move[0] = x;
    g_android->hud_move[1] = y;
}

JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativeHudLook(JNIEnv *env, jclass cls, jfloat dx, jfloat dy)
{
    (void)env; (void)cls;
    if (!g_android) return;
    g_android->pending_yaw   += -dx * LOOK_SENSITIVITY;
    g_android->pending_pitch += -dy * LOOK_SENSITIVITY;
}

JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativeHudJump(JNIEnv *env, jclass cls, jboolean down)
{
    (void)env; (void)cls;
    if (g_android) g_android->hud_jump = down ? true : false;
}

JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativeHudFire(JNIEnv *env, jclass cls, jboolean down)
{
    (void)env; (void)cls;
    if (g_android) g_android->hud_fire = down ? true : false;
}

JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativeHudCrouch(JNIEnv *env, jclass cls, jboolean down)
{
    (void)env; (void)cls;
    if (g_android) g_android->hud_crouch = down ? true : false;
}

/* A request, not a held button: the game loop consumes and clears it. */
JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativeHudReload(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    if (g_android) g_android->hud_reload = true;
}

JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativeHudMelee(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    if (g_android) g_android->hud_melee = true;
}

JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativeHudSwap(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    if (g_android) g_android->hud_swap = true;
}

JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativeHudZoom(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    if (g_android) g_android->hud_zoom = true;
}
JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativeHudGrenade(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    if (g_android) g_android->hud_grenade = true;
}
JNIEXPORT void JNICALL
Java_net_hta_halotrial_GameActivity_nativeHudDebug(JNIEnv *env, jclass cls,
                                                   jint action)
{
    (void)env; (void)cls;
    if (g_android) g_android->hud_debug = (int)action + 1;   /* 0 = nothing pending */
}

/* ------------------------------ vehicles ------------------------------ */

/* The HUD that goes with where you sit: a gunner sees the vehicle gun's own
 * crosshair; everyone else the weapon they carry. */
static bool g_hud_vehicle;
static void seat_hud(hta_android *s, const hta_weapon_def *vdef)
{
    char err[HTA_ERRLEN];
    if (!vdef && !g_hud_vehicle) return;
    hta_resource_map *bm = s->bitmaps_ok ? &s->bitmaps_rm : NULL;
    if (s->gpu_hud) { hta_gfx_mesh_free(s->gfx, s->gpu_hud); s->gpu_hud = NULL; }
    hta_hud_free(&s->hud);
    hta_hud_load(&s->hud, &s->cache, bm, vdef ? vdef : &s->weap, err, sizeof(err));
    hta_hud_set_shield(&s->hud, s->vit ? hta_vitals_shield_fraction(s->vit) : 1.0f);
    hta_hud_set_health(&s->hud, s->vit ? hta_vitals_health_fraction(s->vit) : 1.0f);
    if (s->gfx && s->hud.elem_count)
        s->gpu_hud = hta_gfx_mesh_upload_dynamic(s->gfx, &s->hud.mesh, err, sizeof(err));
    g_hud_vehicle = vdef != NULL;
}

/* The Trial's own words for a vehicle and a seat: `hud_icon_messages`
 * says "Warthog", "Ghost", "driver", "gunner", "side". */
static void vehicle_words(hta_android *s, uint32_t car, uint32_t seat, char *out, size_t n)
{
    static uint32_t icons;
    if (!icons) icons = hta_ustr_find(&s->cache, "ui\\hud\\hud_icon_messages");
    const hta_vehicle *v = &s->vehicles.cars[car];
    const hta_vehicle_type *t = &s->vehicles.types[v->type];
    const hta_vehicle_seat *st = hta_vehicles_seat(&s->vehicles, car, seat);
    char name[32] = "", place[32] = "";
    if (!icons || t->hud_name < 0 || !hta_ustr_get(&s->cache, icons, (uint32_t)t->hud_name,
                                                    name, sizeof(name)))
        snprintf(name, sizeof(name), "%s", t->name);
    if (!st || !icons || st->hud_text < 0 ||
        !hta_ustr_get(&s->cache, icons, (uint32_t)st->hud_text, place, sizeof(place)))
        snprintf(place, sizeof(place), "%s", st ? st->label : "");
    /* The rocket Warthog is a Warthog to the HUD; say which. */
    bool rocket = v->kind == HTA_VK_JEEP && t->name[0] == 'r';
    snprintf(out, n, "%s%s %s", rocket ? "Rocket " : "", name, place);
}

/* Where you sit, and what the stick and the look ask of the vehicle.
 * Returns whether this seat lets you shoot what you carry. */
static bool vehicle_controls(hta_android *s, hta_player_input *in)
{
    hta_unit *u = &s->game.units[s->me];
    uint32_t car = (uint32_t)u->vehicle, seat = (uint32_t)u->seat;
    const hta_vehicle_seat *st = hta_vehicles_seat(&s->vehicles, car, seat);
    const hta_vehicle *v = &s->vehicles.cars[car];
    if (!st) return false;
    bool driver = (st->flags & HTA_SEAT_DRIVER) != 0;
    bool gunner = (st->flags & HTA_SEAT_GUNNER) != 0;
    bool armed = (st->flags & HTA_SEAT_ALLOWS_WEAPONS) != 0;
    /* A Warthog steers with the stick, so its driver's look swings with
     * the hull; everything else aims where you look, in the world. */
    if (driver && v->kind == HTA_VK_JEEP) {
        s->seat_look[0] += in->look_yaw;
        s->seat_look[1] += in->look_pitch;
        if (s->seat_look[0] > 2.6f) s->seat_look[0] = 2.6f;
        if (s->seat_look[0] < -2.6f) s->seat_look[0] = -2.6f;
        if (s->seat_look[1] > 0.5f) s->seat_look[1] = 0.5f;
        if (s->seat_look[1] < -0.8f) s->seat_look[1] = -0.8f;
        s->cam.yaw = v->yaw + s->seat_look[0];
        s->cam.pitch = s->seat_look[1];
    } else {
        hta_camera_look(&s->cam, in->look_yaw, in->look_pitch);
        if (s->cam.pitch > 1.2f) s->cam.pitch = 1.2f;
        if (s->cam.pitch < -1.2f) s->cam.pitch = -1.2f;
    }
    hta_vehicles_camera(&s->vehicles, &s->col, car, seat, s->cam.yaw, s->cam.pitch, &s->cam);
    hta_transform root;
    if (hta_game_seat_root(&s->game, s->me, &root))
        for (int k = 0; k < 3; k++) s->player.pos[k] = root.t[k];
    s->player.velocity[0] = cosf(v->yaw) * v->speed + v->lateral_vel[0];
    s->player.velocity[1] = sinf(v->yaw) * v->speed + v->lateral_vel[1];
    s->player.velocity[2] = 0.0f;
    s->player.footstep = s->player.landed = false;
    s->player.on_ground = true;
    if (!s->net_enabled || s->net_hosting) {
        u->in.move.move_forward = driver ? in->move_forward : 0.0f;
        u->in.move.move_right = driver ? in->move_right : 0.0f;
        u->in.move.jump = driver && in->jump;
        u->in.move.fire = gunner && in->fire;
        u->in.fire2 = gunner && s->hud_alt;
        u->in.move.look_yaw = u->in.move.look_pitch = 0.0f;
    }
    s->veh_fire = gunner && in->fire;
    if (!driver) { in->move_forward = in->move_right = 0.0f; }
    if (!armed) in->fire = false;
    in->jump = driver && in->jump;
    in->crouch = false;
    return armed;
}

/* After everything has moved this frame: the camera onto the seat again,
 * and whether our own body is in the picture. */
static void vehicle_camera(hta_android *s)
{
    s->show_self = false;
    if (!s->game_on || s->me < 0 || s->dead) return;
    hta_unit *u = &s->game.units[s->me];
    if (u->vehicle < 0 || !s->vehicles.loaded) return;
    uint32_t car = (uint32_t)u->vehicle, seat = (uint32_t)u->seat;
    const hta_vehicle *v = &s->vehicles.cars[car];
    const hta_vehicle_seat *st = hta_vehicles_seat(&s->vehicles, car, seat);
    if (st && (st->flags & HTA_SEAT_DRIVER) && v->kind == HTA_VK_JEEP)
        s->cam.yaw = v->yaw + s->seat_look[0];
    hta_vehicles_camera(&s->vehicles, &s->col, car, seat, s->cam.yaw, s->cam.pitch, &s->cam);
    hta_transform root;
    if (hta_game_seat_root(&s->game, s->me, &root))
        for (int k = 0; k < 3; k++) s->player.pos[k] = root.t[k];
    s->show_self = hta_vehicles_third_person(&s->vehicles, car, seat) &&
                   !(v->kind == HTA_VK_TANK && st && (st->flags & HTA_SEAT_DRIVER));
}

/* Engines: each running vehicle's own looping sound, from where it is,
 * faster as it goes faster. Idle below the rate. */
#define HTA_LOOP_ENGINE 40u
static void vehicle_sounds(hta_android *s)
{
    if (!s->vehicles.loaded || !s->audio_ok) return;
    for (uint32_t i = 0; i < s->vehicles.count && i < HTA_VEHICLE_MAX; i++) {
        const hta_vehicle *v = &s->vehicles.cars[i];
        uint32_t snd = v->type < HTA_VEHICLE_TYPES ? s->veh_engine[v->type] : 0;
        float d[3] = { v->pos[0]-s->cam.pos[0], v->pos[1]-s->cam.pos[1], v->pos[2]-s->cam.pos[2] };
        float dist = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        bool on = snd && v->active && v->ctl.driven && dist < HTA_SOUND_FAR * 0.6f;
        if (!on) {
            if (s->veh_engine_on[i]) hta_audio_loop_stop(&s->audio, HTA_LOOP_ENGINE + i);
            s->veh_engine_on[i] = false;
            continue;
        }
        int b = bank_get(s, snd);
        if (b < 0) continue;
        float speed = hta_vehicles_speed(&s->vehicles, i);
        float frac = v->forward > 0.1f ? speed / v->forward : 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        float g = s->veh_engine_gain[v->type] * (0.55f + 0.45f * frac);
        if (dist > HTA_SOUND_NEAR) g *= (HTA_SOUND_NEAR / dist) * (1.0f - dist / (HTA_SOUND_FAR * 0.6f));
        float pan = 0.0f;
        if (dist > 0.01f) {
            float right[3];
            hta_camera_right(&s->cam, right);
            pan = (d[0]*right[0] + d[1]*right[1] + d[2]*right[2]) / dist;
        }
        /* Revs: idle as recorded, up to half again at full speed. Ours. */
        hta_audio_loop_ex(&s->audio, HTA_LOOP_ENGINE + i, s->bank[b].clip[0], g, pan,
                          0.85f + 0.5f * frac);
        s->veh_engine_on[i] = true;
    }
}

/* In, out, or across: what changes for this device when its seat does. */
static void vehicle_transition(hta_android *s)
{
    if (!s->game_on || s->me < 0 || s->me >= (int32_t)s->game.unit_count) return;
    hta_unit *u = &s->game.units[s->me];
    int32_t car = u->alive && !s->dead ? u->vehicle : -1;
    int32_t seat = car >= 0 ? u->seat : -1;
    if (car == s->my_car && seat == s->my_seat) return;
    bool was = s->my_car >= 0;
    s->my_car = car;
    s->my_seat = seat;
    if (car >= 0) {
        s->zoom_level = 0;
        apply_zoom(s);
        s->hud_fire = false;
        s->throwing = false;
        fire_loop(s, false);
        if (s->vm.loaded) hta_viewmodel_play(&s->vm, HTA_VM_IDLE);
        const hta_vehicle *v = &s->vehicles.cars[car];
        if (!was) {
            s->seat_look[0] = 0.0f;
            s->seat_look[1] = -0.15f;
            s->cam.yaw = v->yaw;
            s->cam.pitch = -0.1f;
        }
        const hta_vehicle_seat *st = hta_vehicles_seat(&s->vehicles, (uint32_t)car, (uint32_t)seat);
        int32_t wi = st && (st->flags & HTA_SEAT_GUNNER) && v->type < HTA_VEHICLE_TYPES
                   ? s->game.vweapon[v->type][0] : -1;
        seat_hud(s, wi >= 0 ? &s->game.weapons[wi].def : NULL);
        char words[64];
        vehicle_words(s, (uint32_t)car, (uint32_t)seat, words, sizeof(words));
        hta_log("[vehicles] in %s (car %d seat %d)", words, car, seat);
    } else {
        /* Out: stand where the host put us, facing the way we looked. */
        if (!s->net_enabled || s->net_hosting) {
            for (int k = 0; k < 3; k++) s->player.pos[k] = u->body.pos[k];
            for (int k = 0; k < 3; k++) s->player.velocity[k] = u->body.velocity[k];
            s->player.on_ground = u->body.on_ground;
        }
        s->player.landed = false;
        s->player.crouch_t = 0.0f;
        s->player.eye_height = s->player.phys.cam_stand;
        for (int k = 0; k < 3; k++) s->cam.pos[k] = s->player.pos[k];
        s->cam.pos[2] += s->player.eye_height;
        if (s->cam.pitch > 1.2f || s->cam.pitch < -1.2f) s->cam.pitch = 0.0f;
        /* A held BRAKE is not a jump on the way out. */
        s->hud_jump = s->jump_held = false;
        s->hud_alt = false;
        seat_hud(s, NULL);
        hta_log("[vehicles] out at (%.2f %.2f %.2f)", s->player.pos[0], s->player.pos[1],
                s->player.pos[2]);
    }
}

/* What the HUD tells you about vehicles: a free seat in reach, or the
 * seat you are in. */
static void vehicle_status(hta_android *s, bool seated, int32_t near_car, int32_t near_seat)
{
    int mode = 0;
    char text[96] = "";
    if (seated) {
        const hta_unit *u = &s->game.units[s->me];
        const hta_vehicle *v = &s->vehicles.cars[u->vehicle];
        const hta_vehicle_seat *st = hta_vehicles_seat(&s->vehicles, (uint32_t)u->vehicle,
                                                       (uint32_t)u->seat);
        uint32_t f = st ? st->flags : 0;
        mode = (f & HTA_SEAT_DRIVER) ? 2 : (f & HTA_SEAT_GUNNER) ? 3 :
               (f & HTA_SEAT_ALLOWS_WEAPONS) ? 4 : 5;
        if ((f & HTA_SEAT_GUNNER) && v->type < HTA_VEHICLE_TYPES &&
            s->game.vweapon[v->type][1] >= 0) mode |= 16;
        char words[64];
        vehicle_words(s, (uint32_t)u->vehicle, (uint32_t)u->seat, words, sizeof(words));
        snprintf(text, sizeof(text), "%s", words);
    } else if (near_car >= 0 && near_seat >= 0) {
        mode = 1;
        char words[64];
        vehicle_words(s, (uint32_t)near_car, (uint32_t)near_seat, words, sizeof(words));
        snprintf(text, sizeof(text), "GET IN: %s", words);
    }
    snprintf(g_vehicle_text, sizeof(g_vehicle_text), "%s", text);
    atomic_store(&g_vehicle_mode, mode);
}

void android_main(struct android_app *app)
{
    static hta_android state;
    memset(&state, 0, sizeof(state));
    for (unsigned i=0;i<HTA_NET_MAX_PLAYERS;i++) state.peer_unit[i]=-1;
    state.app = app;
    state.vit = &state.vitals;
    state.move_pointer = state.look_pointer = -1;
    g_android = &state;
    state.hud_ready = g_hud_wanted;
    char net_host[64]; bool net_hosting=false;
    read_net_host(app,net_host,&net_hosting);
    /* The setup screen asks for the menu first; a LAN launch goes straight in. */
    state.menu_mode = intent_int(app, "menu", 0) != 0 && !net_host[0];
    atomic_store(&g_paused, 0);
    atomic_store(&g_damage_flash, 0);
    atomic_store(&g_net_status, 0);
    atomic_store(&g_menu_mode, state.menu_mode ? 1 : 0);
    /* Three bots at normal unless the setup screen says otherwise. Ours. */
    state.bot_count = intent_int(app, "bots", 3);
    state.bot_skill = intent_int(app, "skill", 1);
    if (state.bot_count < 0) state.bot_count = 0;
    if (state.bot_count > 7) state.bot_count = 7;
    if (state.bot_skill < 0) state.bot_skill = 0;
    if (state.bot_skill > 3) state.bot_skill = 3;
    state.score_limit = HTA_SLAYER_SCORE_LIMIT;
    state.respawn_delay = HTA_RESPAWN_DELAY;
    atomic_store(&g_shell_screen, 0);
    atomic_store(&g_match_ready, 0);
    /* The setup screen's own LAN buttons, for a build with no ui.map. */
    if (net_host[0]) net_begin(&state, net_host, net_hosting, 32270, NULL);

    app->userData     = &state;
    app->onAppCmd     = on_cmd;
    app->onInputEvent = on_input;

    hta_camera_init(&state.cam);
    hta_player_init(&state.player);
    hta_gun_init(&state.gun);
    state.rng = 0x9E3779B9u;
    /* Before any asset loading: the clip table lives in the mixer, and
     * hta_audio_init clears it, so the stream has to exist first. */
    state.audio_ok = hta_audio_android_start(&state.audio);
    if (state.audio_ok) {
        /* The AR fires 15/s and its gunshot is 0.70 s long, so ten shots
         * overlap in steady fire. At unity that sums straight into the
         * clamp and turns into a buzz; leave headroom instead. */
        state.audio.master_gain = 0.45f;
    } else {
        hta_log("[audio] no output stream; running silent");
    }
    state.my_car = state.my_seat = -1;
    state.vehicle_roster = intent_int(app, "vehicles", HTA_VROSTER_ALL);
    state.last_time = hta_time_seconds();

    hta_log("[app] android_main; pointer size %zu bytes", sizeof(void *));
    hta_log("[app] external data path: %s",
            app->activity->externalDataPath ? app->activity->externalDataPath : "(null)");
    request_landscape(app);

    while (1) {
        int events;
        struct android_poll_source *source;
        int timeout = state.has_window ? 0 : -1;
        while (ALooper_pollOnce(timeout, NULL, &events, (void **)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) goto done;
            timeout = 0;
        }

        double now = hta_time_seconds();
        float dt = (float)(now - state.last_time);
        state.last_time = now;

        if (state.menu_mode) {
            if (!state.menu_go) { menu_frame(&state, dt); continue; }
            state.menu_go = false;
            menu_leave(&state);
            state.last_time = hta_time_seconds();   /* the load is not a frame */
            continue;
        }

        hta_player_input in;
        gather_input(&state, &in, dt);
        if (atomic_load(&g_paused)) {
            /* Solo holds the world. A live LAN match keeps simulating while
             * this player's input is blank, like an online pause menu. */
            memset(&in, 0, sizeof(in));
            if (!state.net_enabled) dt = 0.0f;
            state.hud_swap = state.hud_zoom = state.hud_melee = false;
            state.hud_reload = state.hud_grenade = false;
            state.hud_debug = 0;
            fire_loop(&state, false);
        }
        /* A corpse does not steer, shoot or jump. The body still falls --
         * hta_player_update with a blank input keeps gravity and the ground
         * query -- so dying on a slope still slides you down it. */
        if (state.dead) memset(&in, 0, sizeof(in));

        /* The flag: the game puts it in this player's hands, and takes it
         * away on a capture, a death or a drop. The gun goes to the belt
         * with what was in it, and the flag's own first-person model comes
         * up; nothing but a swing works until it is gone, and the swap
         * button puts it down. */
        if (state.game_on && state.me >= 0) {
            int8_t fl = state.game.units[state.me].flag;
            if ((fl >= 0) != (state.carried_flag >= 0) && state.game.flag_weapon >= 0) {
                if (fl >= 0) {
                    state.held_ammo[state.held_slot] = state.ammo;
                    state.held_ammo_set[state.held_slot] = true;
                    fire_loop(&state, false);
                    state.zoom_level = 0;
                    apply_zoom(&state);
                    equip_weapon(&state, state.game.weapons[state.game.flag_weapon].tag);
                } else if (state.held_count) {
                    equip_weapon(&state, state.held[state.held_slot]);
                    if (state.held_ammo_set[state.held_slot]) {
                        state.ammo = state.held_ammo[state.held_slot];
                        hta_ammo_cancel_reload(&state.ammo);
                    }
                }
            }
            state.carried_flag = fl;
            if (fl >= 0) {
                if (state.hud_swap && !state.dead) {
                    if (state.net_enabled && !state.net_hosting) state.net_pickup_count++;
                    else hta_game_drop_flag(&state.game, state.me);
                }
                state.hud_swap = state.hud_zoom = false;
                state.hud_reload = state.hud_grenade = false;
                in.fire = false;
            }
        }

        /* Vehicles: get in when a free seat is in reach, out when seated.
         * The game decides -- on a client, the host does. */
        hta_unit *mine = state.game_on && state.me >= 0 &&
                         state.me < (int32_t)state.game.unit_count
                       ? &state.game.units[state.me] : NULL;
        bool seated = mine && mine->vehicle >= 0 && mine->alive && !state.dead &&
                      state.vehicles.loaded;
        int32_t near_seat = -1;
        int32_t near_car = mine && !seated && !state.dead && state.vehicles.loaded
                         ? hta_game_seat_near(&state.game, state.me, &near_seat) : -1;
        if (!state.dead && state.hud_swap && (seated || near_car >= 0)) {
            state.hud_swap = false;
            if (state.net_enabled && !state.net_hosting) state.net_action_count++;
            else mine->in.action = true;
        }
        bool armed_seat = true;
        state.veh_fire = false;
        if (seated) {
            armed_seat = vehicle_controls(&state, &in);
            state.hud_swap = state.hud_melee = state.hud_grenade = false;
            state.hud_debug = 0;
            if (!armed_seat) state.hud_zoom = state.hud_reload = false;
        } else {
            hta_player_update(&state.player, &state.cam,
                state.col.built ? &state.col : NULL, &in, dt);
        }
        vehicle_status(&state, seated, near_car, near_seat);
        bool driving = seated;
        /* Everyone else sees, aims at and is hit by where you are now. */
        if (state.game_on) {
            hta_game_sync_local(&state.game, &state.player, &state.cam, held_roster(&state));
            if (!state.net_enabled || state.net_hosting) mirror_local(&state);
        }
        if (state.player.footstep && state.col.built) {
            uint8_t mat = hta_collision_ground_material(&state.col,
                                                        state.player.pos[0],
                                                        state.player.pos[1],
                                                        state.player.pos[2] + 0.1f);
            play_footstep(&state, mat);
        }
        hta_gun_update(&state.gun, dt);

        /* What the fall cost, and the shield growing back afterwards. */
        if (state.player.landed && state.vit->loaded &&
            (!state.net_enabled || state.net_hosting)) {
            float cost = hta_vitals_land(state.vit, state.player.land_speed);
            if (cost > 0.0f)
                hta_log("[player] landed at %.1f wu/s for %.0f damage "
                        "(%.0f shield, %.0f health left)",
                        state.player.land_speed, cost,
                        state.vit->shield, state.vit->health);
        }
        /* The one-shots have to be read BEFORE the update clears them. */
        if (state.vit->loaded && !state.dead) {
            if (state.vit->took_damage) state.damage_flash_left = HTA_DAMAGE_FLASH_TIME;
            if (state.vit->shield_broke)
                play_tag(&state, state.shield_empty_snd, 1.0f);
            else if (state.vit->took_damage)
                play_tag(&state, state.shield_hit_snd, 1.0f);
        }
        if (!state.net_enabled || state.net_hosting || !state.net.have_world)
            hta_vitals_update(state.vit, dt);
        else {
            state.vit->took_damage=false;
            state.vit->shield_broke=false;
        }
        if (state.damage_flash_left > 0.0f) {
            state.damage_flash_left -= dt;
            if (state.damage_flash_left < 0.0f) state.damage_flash_left = 0.0f;
        }
        atomic_store(&g_damage_flash, (int)(255.0f * state.damage_flash_left / HTA_DAMAGE_FLASH_TIME));
        if (state.game_on && state.me >= 0) {
            hta_game_contact con[HTA_HUD_MAX_BLIPS];
            hta_hud_blip blips[HTA_HUD_MAX_BLIPS];
            uint32_t nc = state.dead ? 0 : hta_game_sensor(&state.game, state.me, con, HTA_HUD_MAX_BLIPS);
            for (uint32_t i = 0; i < nc; i++) {
                blips[i].x = con[i].x; blips[i].y = con[i].y;
                blips[i].friendly = con[i].friendly;
                blips[i].size = con[i].vehicle ? 1.8f : 1.0f;
            }
            hta_hud_set_blips(&state.hud, blips, nc);
            /* The red reticle: an enemy under the crosshair, or inside the
             * weapon's own autoaim cone and range. A gunner's is the
             * vehicle gun's. */
            int32_t aimw = held_roster(&state);
            if (state.my_car >= 0 && (uint32_t)state.my_car < state.vehicles.count) {
                const hta_vehicle_seat *st = hta_vehicles_seat(&state.vehicles,
                    (uint32_t)state.my_car, (uint32_t)state.my_seat);
                uint16_t ty = state.vehicles.cars[state.my_car].type;
                aimw = st && (st->flags & HTA_SEAT_GUNNER) && ty < HTA_VEHICLE_TYPES
                     ? state.game.vweapon[ty][0]
                     : (st && (st->flags & HTA_SEAT_ALLOWS_WEAPONS) ? aimw : -1);
            }
            float fwd[3];
            hta_camera_forward(&state.cam, fwd);
            bool on = !state.dead && aimw >= 0 &&
                hta_game_aim_target(&state.game, state.me, aimw, state.cam.pos, fwd, NULL) >= 0;
            if (on != state.hud.cross_on_target) hta_hud_set_on_target(&state.hud, on);
        }
        if (state.vit->loaded) {
            hta_hud_set_shield(&state.hud, hta_vitals_shield_fraction(state.vit));
            hta_hud_set_health(&state.hud, hta_vitals_health_fraction(state.vit));

            /* The recharge hum. Its condition is the tag's own and nothing
             * of ours: the shield is growing back exactly when the delay
             * since the last hit has elapsed and it is not yet full. */
            float sf = hta_vitals_shield_fraction(state.vit);
            float hf = hta_vitals_health_fraction(state.vit);
            bool charging = !state.dead &&
                            state.vit->since_damage >= state.vit->recharge_delay &&
                            state.vit->shield < state.vit->max_shield;
            hud_loop(&state, HTA_LOOP_SHIELD_CHARGE, state.shield_charge_snd,
                     charging, &state.shield_charge_on);
            hud_loop(&state, HTA_LOOP_SHIELD_LOW, state.shield_low_snd,
                     !state.dead && !charging && sf <= HTA_VITALS_LOW && sf > 0.0f,
                     &state.shield_low_on);
            hud_loop(&state, HTA_LOOP_HEALTH_LOW, state.health_low_snd,
                     !state.dead && hf <= HTA_VITALS_LOW,
                     &state.health_low_on);
        }

        /* Dying, and coming back. */
        if (state.vit->loaded && state.vit->died && !state.dead) {
            state.dead = true;
            atomic_store(&g_vehicle_mode, 0);
            state.dead_timer = state.respawn_delay;
            for (int k = 0; k < 3; k++) state.death_pos[k] = state.player.pos[k];
            fire_loop(&state, false);
            hud_loop(&state, HTA_LOOP_SHIELD_CHARGE, state.shield_charge_snd,
                     false, &state.shield_charge_on);
            hud_loop(&state, HTA_LOOP_SHIELD_LOW, state.shield_low_snd,
                     false, &state.shield_low_on);
            hud_loop(&state, HTA_LOOP_HEALTH_LOW, state.health_low_snd,
                     false, &state.health_low_on);
            state.zoom_level = 0;
            apply_zoom(&state);
            /* A fall is its own kind of death and the tag has a line for
             * it; a blast is violent; anything else is the quiet one. */
            uint32_t snd = state.death_quiet_snd;
            if (state.player.landed && state.player.land_speed >= state.vit->fall_fatal)
                snd = state.death_falling_snd ? state.death_falling_snd
                                              : state.death_violent_snd;
            else if (state.vit->shield <= 0.0f && state.vit->health <= 0.0f)
                snd = state.death_violent_snd ? state.death_violent_snd : snd;
            play_tag(&state, snd, 1.0f);
            /* The body stays where it fell and the camera goes to look at
             * it. Halo does this and it is the whole reason dying reads as
             * an event rather than a fade. */
            if (state.corpse.loaded &&
                hta_actor_play_death(&state.corpse, &state.spawn_rng)) {
                state.corpse_up = true;
                hta_actor_place(&state.corpse, state.death_pos, state.cam.yaw);
                hta_log("[player] body playing '%s'",
                        state.corpse.graph.anims[state.corpse.clip].name);
            }
            hta_log("[player] died at (%.2f %.2f %.2f)",
                    state.death_pos[0], state.death_pos[1], state.death_pos[2]);
        }
        float fade = 0.0f;
        if (state.dead) {
            state.dead_timer -= dt;
            /* Clear while you watch; black only over the last moment. */
            if (state.dead_timer < HTA_DEATH_FADE_OUT) {
                fade = 1.0f - state.dead_timer / HTA_DEATH_FADE_OUT;
                if (fade > 1.0f) fade = 1.0f;
                if (fade < 0.0f) fade = 0.0f;
            }
            if (state.dead_timer <= 0.0f &&
                (!state.net_enabled || state.net_hosting)) {
                respawn(&state);
                state.dead = false;
                state.dead_timer = -HTA_DEATH_FADE_IN;   /* counts the fade back */
            }
        } else if (state.dead_timer < 0.0f) {
            state.dead_timer += dt;
            if (state.dead_timer > 0.0f) state.dead_timer = 0.0f;
            fade = -state.dead_timer / HTA_DEATH_FADE_IN;
            if (fade < 0.0f) fade = 0.0f;
        }
        hta_hud_set_fade(&state.hud, fade);
        /* Outside yourself, watching the body. */
        if (state.dead && state.corpse_up) {
            hta_actor_update(&state.corpse, dt);
            hta_actor_place(&state.corpse, state.death_pos, state.corpse.yaw);

            float gone = state.respawn_delay - state.dead_timer;
            float f = gone / HTA_DEATH_PULLBACK;
            if (f > 1.0f) f = 1.0f;

            /* Aim at the chest rather than the feet, and pull back along
             * the way the body is facing so you see its front. */
            float look[3] = { state.death_pos[0], state.death_pos[1],
                              state.death_pos[2] + HTA_DEATH_LOOK_AT };
            float back = HTA_DEATH_CAM_BACK * f;
            float want[3] = {
                look[0] + cosf(state.corpse.yaw) * back,
                look[1] + sinf(state.corpse.yaw) * back,
                look[2] + HTA_DEATH_CAM_UP * f
            };
            /* Do not go through a wall to get there. The ray query is a
             * grid walk now, so this costs nothing. */
            if (state.col.built) {
                float d[3] = { want[0]-look[0], want[1]-look[1], want[2]-look[2] };
                float len = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                if (len > 1e-4f) {
                    for (int k = 0; k < 3; k++) d[k] /= len;
                    float t = 0.0f, hit[3], nrm[3];
                    if (hta_collision_ray(&state.col, look, d, len, &t, hit, nrm)) {
                        float keep = t * 0.8f;   /* stop short of the surface */
                        for (int k = 0; k < 3; k++) want[k] = look[k] + d[k] * keep;
                    }
                }
            }
            for (int k = 0; k < 3; k++) state.cam.pos[k] = want[k];

            float to[3] = { look[0]-state.cam.pos[0], look[1]-state.cam.pos[1],
                            look[2]-state.cam.pos[2] };
            float flat = sqrtf(to[0]*to[0] + to[1]*to[1]);
            if (flat > 1e-4f || fabsf(to[2]) > 1e-4f) {
                state.cam.yaw = atan2f(to[1], to[0]);
                state.cam.pitch = atan2f(to[2], flat);
            }
        } else if (state.dead) {
            /* No body to watch -- sink and tip forward instead. */
            float gone = state.respawn_delay - state.dead_timer;
            float f = gone / HTA_DEATH_PULLBACK;
            if (f > 1.0f) f = 1.0f;
            state.cam.pos[2] -= HTA_DEATH_EYE_DROP * f;
            float want = -1.2f;
            state.cam.pitch += (want - state.cam.pitch) * f;
        }
        /* Ammo gates the shot: hta_gun_fire spends the cooldown whether or
         * not the magazine could pay, so ask before pulling. */
        hta_ammo_update(&state.ammo, dt);
        /* A shell-at-a-time reload chains on its own, and every shell after
         * the first was being loaded silently with no animation -- which is
         * most of a shotgun reload from empty. Each one replays the clip. */
        if (state.ammo.reload_began && state.vm.loaded)
            hta_viewmodel_play(&state.vm, HTA_VM_RELOAD);
        if (state.dry_cooldown > 0.0f) state.dry_cooldown -= dt;

        /* What you are standing on.
         *
         * Halo takes grenades, health and powerups as you walk over them
         * and makes you ASK for a weapon, which is the difference between
         * topping up and losing the gun you wanted. SWAP is that ask: on an
         * item it picks it up, and off one it cycles as before. */
        if (state.items.loaded && (!state.net_enabled || state.net_hosting))
            hta_pickups_update(&state.items, dt);
        if (state.items.loaded && state.net_enabled && !state.net_hosting &&
            state.hud_swap && !state.dead) {
            int32_t slot=hta_pickups_at_kind(&state.items,state.player.pos,HTA_ITEM_WEAPON);
            const hta_item_choice *item=hta_pickups_item(&state.items,slot);
            bool carrying=false;
            for (uint32_t i=0;item && i<state.held_count;i++)
                if (state.held[i]==item->tag_id) carrying=true;
            /* Or one somebody dropped: the host decides. */
            int32_t dr=state.game_on ? hta_game_drop_near(&state.game,state.player.pos,HTA_DROP_REACH) : -1;
            bool drop_new=false;
            if (dr>=0) {
                drop_new=true;
                for (uint32_t i=0;i<state.held_count;i++)
                    if (hta_game_weapon_index(&state.game,state.held[i])==state.game.drops[dr].weapon)
                        drop_new=false;
            }
            if ((item && !carrying) || drop_new) {
                state.net_pickup_count++;
                state.hud_swap=false;
            }
        }
        if (state.items.loaded && (!state.net_enabled || state.net_hosting) &&
            !state.dead && !driving) {
            const float *feet = state.player.pos;

            int32_t got = hta_pickups_at(&state.items, feet);
            const hta_item_choice *item = hta_pickups_item(&state.items, got);
            if (item) {
                bool taken = false;
                switch (item->kind) {
                case HTA_ITEM_GRENADE:
                    if (state.nade_count < state.nade_max) {
                        state.nade_count++;
                        taken = true;
                    }
                    break;
                case HTA_ITEM_HEALTH:
                    if (state.vit->loaded &&
                        state.vit->health < state.vit->max_health) {
                        state.vit->health = state.vit->max_health;
                        taken = true;
                    }
                    break;
                case HTA_ITEM_OVERSHIELD:
                    if (state.vit->loaded) {
                        state.vit->shield = state.vit->max_shield *
                                              HTA_OVERSHIELD_MULT;
                        state.powerup = HTA_ITEM_OVERSHIELD;
                        state.powerup_timer = item->powerup_time;
                        taken = true;
                    }
                    break;
                case HTA_ITEM_CAMOUFLAGE:
                    state.powerup = HTA_ITEM_CAMOUFLAGE;
                    state.powerup_timer = item->powerup_time;
                    taken = true;
                    break;
                default:
                    break;   /* a weapon waits to be asked for */
                }
                if (taken) {
                    play_tag(&state, item->pickup_snd, 1.0f);
                    item_message(&state, item->tag_id, 0);
                    hta_log("[items] picked up %s", item->path);
                    hta_pickups_take(&state.items, got);
                }
            }

            /* A dropped weapon: its ammunition if we carry one like it,
             * and the gun itself -- with what was left in it -- on SWAP. */
            if (state.game_on) {
                int32_t dr = hta_game_drop_near(&state.game, feet, HTA_DROP_REACH);
                if (dr >= 0) {
                    hta_game_drop *d = &state.game.drops[dr];
                    int32_t in_hand = held_roster(&state);
                    bool carried = false;
                    for (uint32_t i = 0; i < state.held_count; i++)
                        if (hta_game_weapon_index(&state.game, state.held[i]) == d->weapon) carried = true;
                    if (carried && d->weapon == in_hand && state.ammo.reserve < state.ammo.reserve_max) {
                        state.ammo.reserve += d->ammo.loaded + d->ammo.reserve;
                        if (state.ammo.reserve > state.ammo.reserve_max)
                            state.ammo.reserve = state.ammo.reserve_max;
                        d->live = false;
                        play_tag(&state, state.weap.pickup_snd_id, 0.8f);
                    } else if (!carried && state.hud_swap) {
                        state.hud_swap = false;
                        int32_t wi;
                        hta_ammo am;
                        hta_game_take_drop(&state.game, dr, &wi, &am);
                        uint32_t tag = state.game.weapons[wi].tag;
                        if (state.held_count < HTA_CARRY_MAX) {
                            state.held_ammo[state.held_slot] = state.ammo;
                            state.held_ammo_set[state.held_slot] = true;
                            state.held_slot = state.held_count;
                            state.held[state.held_count++] = tag;
                        } else {
                            drop_held(&state);
                            state.held[state.held_slot] = tag;
                        }
                        state.held_ammo_set[state.held_slot] = false;
                        equip_weapon(&state, tag);
                        state.ammo.loaded = am.loaded;
                        state.ammo.reserve = am.reserve;
                        play_tag(&state, state.weap.pickup_snd_id, 1.0f);
                        item_message(&state, tag, 0);
                        hta_log("[items] picked up a dropped %s (%d/%d)", state.weap.path,
                                am.loaded, am.reserve);
                    }
                }
            }
            /* A map weapon we already carry: its ammunition, as Halo gives
             * it, for whichever hand has that gun. */
            {
                int32_t ws = hta_pickups_at_kind(&state.items, feet, HTA_ITEM_WEAPON);
                const hta_item_choice *w = hta_pickups_item(&state.items, ws);
                for (uint32_t k = 0; w && k < state.held_count; k++) {
                    if (state.held[k] != w->tag_id) continue;
                    hta_weapon_def fd;
                    if (!hta_weapon_load_id(&state.cache, NULL, w->tag_id, &fd, NULL, NULL, 0)) break;
                    hta_ammo fresh;
                    hta_ammo_init(&fresh, &fd);
                    hta_ammo *a = k == state.held_slot ? &state.ammo : &state.held_ammo[k];
                    if (k != state.held_slot && !state.held_ammo_set[k]) break;   /* full already */
                    if (a->reserve >= a->reserve_max) break;
                    int before = a->reserve;
                    a->reserve += fresh.loaded + fresh.reserve;
                    if (a->reserve > a->reserve_max) a->reserve = a->reserve_max;
                    hta_pickups_take(&state.items, ws);
                    play_tag(&state, w->pickup_snd ? w->pickup_snd : state.pickup_snd_ammo, 0.8f);
                    item_message(&state, w->tag_id, a->reserve - before);
                    break;
                }
            }
            /* SWAP on a weapon PICKS IT UP; off one it switches between the
             * two you are carrying. Halo splits these across two actions and
             * we have one button, so standing on a gun means you want it. */
            if (state.hud_swap) {
                int32_t wslot = hta_pickups_at_kind(&state.items, feet,
                                                    HTA_ITEM_WEAPON);
                const hta_item_choice *w = hta_pickups_item(&state.items, wslot);
                bool already = false;
                for (uint32_t i = 0; w && i < state.held_count; i++)
                    if (state.held[i] == w->tag_id) already = true;
                if (w && !already) {
                    state.hud_swap = false;
                    if (state.held_count < HTA_CARRY_MAX) {
                        /* A free hand: take it and hold it. */
                        state.held_ammo[state.held_slot] = state.ammo;
                        state.held_ammo_set[state.held_slot] = true;
                        state.held_slot = state.held_count;
                        state.held[state.held_count++] = w->tag_id;
                    } else {
                        /* Full: it replaces the one you are holding, which
                         * is the one you were looking at when you chose --
                         * and that one goes on the ground. */
                        drop_held(&state);
                        state.held[state.held_slot] = w->tag_id;
                    }
                    state.held_ammo_set[state.held_slot] = false;
                    equip_weapon(&state, w->tag_id);
                    hta_pickups_take(&state.items, wslot);
                    play_tag(&state, w->pickup_snd, 1.0f);
                    item_message(&state, w->tag_id, 0);
                    hta_log("[items] picked up %s (holding %u)",
                            w->path, state.held_count);
                }
            }

        }
        if (state.items.loaded && hta_pickups_dirty(&state.items)) {
            hta_pickups_pose(&state.items);
            state.items_upload = HTA_ITEMS_UPLOAD_FRAMES;
        }

        /* A powerup running out. The overshield BLEEDS down rather than
         * vanishing: in Halo you watch the extra bars drain, and a cliff
         * edge at sixty seconds would make it impossible to judge. */
        if (state.powerup_timer > 0.0f &&
            (!state.net_enabled || state.net_hosting || !state.net.have_world)) {
            float was = state.powerup_timer;
            state.powerup_timer -= dt;
            if (state.powerup == HTA_ITEM_OVERSHIELD && state.vit->loaded &&
                state.vit->shield > state.vit->max_shield && was > 0.0f) {
                float extra = state.vit->max_shield *
                              (HTA_OVERSHIELD_MULT - 1.0f);
                state.vit->shield -= extra * (dt / was);
                if (state.vit->shield < state.vit->max_shield)
                    state.vit->shield = state.vit->max_shield;
            }
            if (state.powerup_timer <= 0.0f) {
                state.powerup_timer = 0.0f;
                if (state.powerup == HTA_ITEM_OVERSHIELD &&
                    state.vit->loaded &&
                    state.vit->shield > state.vit->max_shield)
                    state.vit->shield = state.vit->max_shield;
                hta_log("[items] powerup over");
                state.powerup = HTA_ITEM_NONE;
            }
        }

        /* The debug pad. Not part of the game: it exists because the map's
         * own item layout is the authority now, and Blood Gulch places
         * neither the needler nor the plasma pistol. */
        if (state.hud_debug) {
            int action = state.hud_debug - 1;
            state.hud_debug = 0;
            if (action == 0 && state.weapon_count && !state.dead) {
                /* Hand over the next weapon in the cache's roster, into the
                 * hand you are using. Walking the whole roster one tap at a
                 * time reaches everything without breaking the two-weapon
                 * rule the rest of the game plays by. */
                state.debug_weapon = (state.debug_weapon + 1u) % state.weapon_count;
                uint32_t give = state.weapons[state.debug_weapon];
                if (state.held_count < HTA_CARRY_MAX) {
                    state.held_slot = state.held_count;
                    state.held[state.held_count++] = give;
                } else {
                    state.held[state.held_slot] = give;
                }
                equip_weapon(&state, give);
                hta_log("[debug] gave %s (%u of %u)", state.weap.path,
                        state.debug_weapon + 1u, state.weapon_count);
            }
        }

        /* None of the buttons do anything to a corpse. They are consumed
         * rather than left pending, or every press made while dead would
         * fire at once on respawn. */
        if (state.dead) {
            state.hud_swap = state.hud_zoom = state.hud_melee = false;
            state.hud_reload = state.hud_grenade = false;
            state.hud_debug = 0;
        }

        /* Swapping rebuilds the viewmodel and the HUD, so do it before
         * anything this frame reads either. */
        if (state.hud_swap) {
            state.hud_swap = false;
            if (state.held_count > 1) {
                state.held_ammo[state.held_slot] = state.ammo;
                state.held_ammo_set[state.held_slot] = true;
                state.held_slot = (state.held_slot + 1u) % state.held_count;
                equip_weapon(&state, state.held[state.held_slot]);
                if (state.held_ammo_set[state.held_slot]) {
                    state.ammo = state.held_ammo[state.held_slot];
                    hta_ammo_cancel_reload(&state.ammo);
                }
                net_action(&state,HTA_NET_EVENT_WEAPON);
            }
        }

        if (state.hud_zoom) {
            state.hud_zoom = false;
            cycle_zoom(&state);
        }

        /* A swing takes the weapon out of the fight until it finishes, so
         * the rest of this frame's trigger work has to know about it. */
        bool swinging = state.vm.loaded && state.vm.state == HTA_VM_MELEE;
        if (state.hud_melee) {
            state.hud_melee = false;
            if (!swinging && state.ammo.phase != HTA_AMMO_RELOADING) {
                /* A swing connects with whatever is within arm's reach in
                 * front of you. The damage is the cyborg's own `melee
                 * damage` tag -- 1000, at a x1.00 multiplier against both
                 * armour and shield, so it kills outright. Halo's front /
                 * back distinction is engine logic, not tag data. */
                if (state.game_on && (!state.net_enabled || state.net_hosting)) {
                    /* The held weapon's own `player melee damage` -- 56 --
                     * and a kill from behind, for everyone alike. */
                    if (hta_game_melee(&state.game, state.me) >= 0)
                        hta_log("[game] melee connected");
                } else if (state.bot.loaded && state.melee_damage > 0.0f) {
                    float fwd[3];
                    hta_camera_forward(&state.cam, fwd);
                    float reach[3];
                    for (int k = 0; k < 3; k++)
                        reach[k] = state.player.pos[k] + fwd[k] * HTA_MELEE_REACH;
                    if (hta_bot_near(&state.bot, reach, HTA_MELEE_REACH)) {
                        float ctr[3];
                        hta_bot_centre(&state.bot, ctr);
                        hta_bot_damage(&state.bot, state.melee_damage, ctr);
                        hta_log("[bot] melee connected");
                    }
                }
                hta_viewmodel_play(&state.vm, HTA_VM_MELEE);
                net_action(&state,HTA_NET_EVENT_MELEE);
                swinging = state.vm.state == HTA_VM_MELEE;
            }
        }

        if (state.hud_reload) {
            state.hud_reload = false;
            if (!swinging && hta_ammo_reload(&state.ammo)) {
                state.net_reload_count++;
                hta_viewmodel_play(&state.vm, HTA_VM_RELOAD);
            }
        }
        if (in.fire && !swinging && hta_gun_ready(&state.gun)) {
            if (hta_ammo_shoot(&state.ammo)) {
                shake_fire(&state, state.weap.firing_damage_id);
                /* Autoaim: the weapon's own cone bends the round toward an
                 * enemy near the crosshair (leading one that moves, for a
                 * round that flies). */
                hta_camera aimcam = state.cam;
                if (state.game_on && state.me >= 0) {
                    float fwd[3], pt[3];
                    hta_camera_forward(&state.cam, fwd);
                    if (hta_game_aim_target(&state.game, state.me, held_roster(&state),
                                            state.cam.pos, fwd, pt) >= 0) {
                        float d[3] = { pt[0]-state.cam.pos[0], pt[1]-state.cam.pos[1],
                                       pt[2]-state.cam.pos[2] };
                        aimcam.yaw = atan2f(d[1], d[0]);
                        aimcam.pitch = atan2f(d[2], hypotf(d[0], d[1]));
                    }
                }
                if (state.proj.loaded) {
                    /* An object round does its own collision on the way, so
                     * there is no hitscan to trace and no impact yet. */
                    float dir[3];
                    if (hta_gun_launch(&state.gun, &aimcam, dir)) {
                        float muzzle[3];
                        for (int k = 0; k < 3; k++)
                            muzzle[k] = state.cam.pos[k] + dir[k] * 0.35f;
                        hta_projectiles_fire(&state.proj, muzzle, dir);
                    }
                } else {
                    /* Two halves, so a body can stop the round before the
                     * wall does: aim picks the direction out of the error
                     * cone, then whatever is nearest takes it. */
                    float dir[3];
                    if (hta_gun_aim(&state.gun, &aimcam, dir)) {
                        float bt = -1.0f, bhit[3];
                        int32_t who = -1;
                        bool onbot;
                        if (state.game_on) {
                            float wt = HTA_GUN_RANGE, wh[3], wn[3];
                            bool wall = state.col.built &&
                                hta_collision_ray(&state.col, state.cam.pos, dir,
                                                  HTA_GUN_RANGE, &wt, wh, wn);
                            who = hta_game_ray(&state.game, state.cam.pos, dir,
                                               wall ? wt : HTA_GUN_RANGE, state.me, &bt, bhit);
                            onbot = who >= 0;
                        } else {
                            onbot = hta_bot_ray(&state.bot, state.cam.pos, dir,
                                                HTA_GUN_RANGE, &bt, bhit);
                        }
                        hta_gun_impact(&state.gun,
                                       state.col.built ? &state.col : NULL,
                                       &state.cam, dir, onbot ? bt : -1.0f);
                        /* Our own tracer leaves from just under the eye,
                         * where the barrel is, toward what we hit. */
                        int32_t mine_w = held_roster(&state);
                        if (state.trails.loaded && mine_w >= 0 &&
                            state.wtrail[mine_w] != HTA_CONT_NONE &&
                            tracer_due(&state, -1, state.weap.between_contrails)) {
                            float fwd[3], right[3], up[3], from[3], end[3];
                            hta_camera_forward(&state.cam, fwd);
                            hta_camera_right(&state.cam, right);
                            hta_camera_up(&state.cam, up);
                            float reach = onbot ? bt : HTA_GUN_RANGE;
                            float wt;
                            if (!onbot && state.col.built &&
                                hta_collision_ray(&state.col, state.cam.pos, dir, HTA_GUN_RANGE,
                                                  &wt, NULL, NULL)) reach = wt;
                            if (reach > 100.0f) reach = 100.0f;
                            for (int k = 0; k < 3; k++) {
                                from[k] = state.cam.pos[k] + fwd[k] * 0.4f + right[k] * 0.08f - up[k] * 0.1f;
                                end[k] = state.cam.pos[k] + dir[k] * reach;
                            }
                            hta_contrails_tracer(&state.trails, state.wtrail[mine_w], from, end, 300.0f);
                        }
                        if (onbot && who >= 0 && (!state.net_enabled || state.net_hosting)) {
                            int pellets = state.weap.projectiles_per_shot > 0
                                        ? state.weap.projectiles_per_shot : 1;
                            hta_game_hurt_jpt(&state.game, who, state.me,
                                              state.impact_jpt, pellets, bhit);
                        } else if (onbot && !state.game_on) {
                            /* What a round does depends on WHAT it hits:
                             * the same shotgun pellet is 8 into armour and
                             * 4 into a shield, and a plasma bolt is the
                             * other way round. The tag knows. */
                            uint8_t mat = state.bot.vitals.shield > 0.0f
                                        ? HTA_MATERIAL_CYBORG_SHIELD
                                        : HTA_MATERIAL_CYBORG_ARMOR;
                            float dmg = hta_damage_vs(&state.cache,
                                                      state.impact_jpt, mat);
                            /* PROJECTILES per shot, not rounds: the
                             * shotgun spends one shell and throws eight
                             * pellets, and at 4 a pellet into a shield the
                             * difference is a weapon that works and one
                             * that does not. */
                            int pellets = state.weap.projectiles_per_shot > 0
                                        ? state.weap.projectiles_per_shot : 1;
                            if (pellets > 32) pellets = 32;
                            for (int r = 0; r < pellets; r++)
                                hta_bot_damage(&state.bot, dmg, bhit);
                        }
                    }
                    play_impact_at(&state, state.gun.hit_material,
                                   state.gun.last_hit);
                    if (state.net_hosting && state.gun.hit_material<33u) {
                        int32_t weapon=held_roster(&state);
                        if (weapon>=0) {
                            hta_net_fx fx={.kind=HTA_NET_FX_IMPACT,
                                .entity=(uint8_t)state.me,.weapon=(uint8_t)weapon,
                                .material=state.gun.hit_material};
                            for (int k=0;k<3;k++) {
                                fx.pos[k]=state.gun.last_hit[k];
                                fx.dir[k]=state.gun.last_nrm[k];
                            }
                            hta_net_server_fx(&state.host_server,&fx);
                        }
                    }
                    /* And the dust the round kicks off that surface. */
                    if (state.gun.hit_material < 33u &&
                        state.impact_recipe[state.gun.hit_material]
                            != HTA_PART_NO_RECIPE)
                        hta_particles_burst(&state.parts,
                                            state.impact_recipe[state.gun.hit_material],
                                            state.gun.last_hit, state.gun.last_nrm);
                }
                hta_viewmodel_play(&state.vm, HTA_VM_FIRE);
                hta_viewmodel_flash(&state.vm);
                /* Eject the spent casing from the gun's own marker. The
                 * viewmodel poses it in its own space, so it takes the
                 * same basis the renderer builds the weapon with. */
                if (state.casing_recipe != HTA_PART_NO_RECIPE &&
                    state.vm.have_eject) {
                    float fwd[3], right[3], up[3];
                    hta_camera_forward(&state.cam, fwd);
                    hta_camera_right(&state.cam, right);
                    hta_camera_up(&state.cam, up);
                    const float *e = state.vm.eject_pos;
                    float at[3], dir[3];
                    for (int k = 0; k < 3; k++) {
                        at[k] = state.cam.pos[k] + fwd[k]*e[0]
                              - right[k]*e[1] + up[k]*e[2];
                        /* Out to the right and a little up, which is where
                         * every one of these guns throws it. */
                        dir[k] = right[k] + up[k] * 0.35f;
                    }
                    hta_particles_burst(&state.parts, state.casing_recipe, at, dir);
                }
                play_tag(&state, state.fire_snd, 1.0f);
                net_action(&state,HTA_NET_EVENT_FIRE);
            } else if (state.ammo.dry && state.dry_cooldown <= 0.0f) {
                /* Click, then reload by itself, the way Halo does. */
                play_tag(&state, state.empty_snd, 1.0f);
                state.dry_cooldown = 0.35f;
                if (hta_ammo_reload(&state.ammo)) {
                    state.net_reload_count++;
                    hta_viewmodel_play(&state.vm, HTA_VM_RELOAD);
                }
            }
        }
        /* A continuous weapon sounds while the trigger is actually doing
         * something, and goes quiet the moment it is released, the magazine
         * runs out, or a swing takes the weapon out of the fight. */
        bool spraying = in.fire && !swinging &&
                        state.ammo.phase == HTA_AMMO_READY &&
                        state.ammo.loaded >= state.ammo.per_shot;
        fire_loop(&state, spraying);

        /* The jet comes out of the marker the muzzle flash hangs off, which
         * on the flamethrower is `spawn fire` itself, and goes where the
         * player is looking. Emitting is rate-based, not per-shot: the
         * flamethrower's 0.1 s between rounds would otherwise give it a
         * stutter the real weapon does not have. */
        if (spraying && state.jet_recipe != HTA_PART_NO_RECIPE) {
            float fwd[3], right[3], up[3];
            hta_camera_forward(&state.cam, fwd);
            hta_camera_right(&state.cam, right);
            hta_camera_up(&state.cam, up);
            const float *m = state.vm.flash_pos;
            float at[3];
            for (int k = 0; k < 3; k++)
                at[k] = state.cam.pos[k] + fwd[k]*m[0]
                      - right[k]*m[1] + up[k]*m[2];
            hta_particles_emit(&state.parts, state.jet_recipe, at, fwd, dt);
        }

        /* The gun carries its own round counter, and the HUD carries the
         * same magazine as a grid of pips. */
        hta_viewmodel_set_counter(&state.vm, (uint32_t)state.ammo.loaded);
        if (state.ammo.mag_max > 0) {
            float full = (float)state.ammo.loaded / (float)state.ammo.mag_max;
            hta_hud_set_ammo(&state.hud, full);
            /* Halo's HUD counter is the TOTAL carried: the magazine plus
             * the reserve. The pips are the magazine, the gun's own readout
             * is the magazine. */
            hta_hud_set_number(&state.hud, state.ammo.loaded + state.ammo.reserve);
            /* And the needler wears its magazine: its needles fold away as
             * it empties and spring back on the reload. */
            hta_viewmodel_set_ammo(&state.vm, full);
        }
        if (state.ammo.reload_done)
            hta_log("[weapon] reloaded: %d / %d", state.ammo.loaded, state.ammo.reserve);
        /* The animation graph fires a snd! id when a clip crosses its sound
         * frame -- reload clacks, the weapon-ready rack. Nothing consumed it
         * until now. */
        if (state.vm.sound_cue) {
            play_tag(&state, state.vm.sound_cue, 1.0f);
            state.vm.sound_cue = 0;
        }
        hta_audio_android_poll(&state.audio);
        hta_viewmodel_update(&state.vm, dt);
        /* Throw a grenade.
         *
         * The arm goes first. Every weapon carries a `first-person
         * throw-grenade` clip of about 1.2 s, and the grenade leaves at the
         * clip's key frame -- so the button STARTS the throw and the
         * projectile appears when your hand does. Letting it go on the press
         * put a grenade out of the player's chest with the weapon still
         * sitting there, which is what it looked like. */
        if (state.hud_grenade) {
            state.hud_grenade = false;
            if (state.nades.loaded && state.nade_count > 0 && !swinging &&
                !state.throwing && state.ammo.phase != HTA_AMMO_RELOADING) {
                if (state.vm.loaded && state.vm.clip[HTA_VM_THROW] >= 0) {
                    hta_viewmodel_play(&state.vm, HTA_VM_THROW);
                    state.throwing = true;
                } else {
                    state.throwing = true;
                    state.vm.key_frame_hit = true;   /* no clip: go at once */
                }
            }
        }
        if (state.throwing &&
            (state.vm.key_frame_hit || state.vm.state != HTA_VM_THROW)) {
            /* Either the hand reached the release, or the clip was
             * interrupted -- a throw that is cut short still throws, the
             * same way Halo will not swallow the grenade. */
            state.throwing = false;
            if (state.nades.loaded && state.nade_count > 0) {
                float fwd[3], up[3];
                hta_camera_forward(&state.cam, fwd);
                hta_camera_up(&state.cam, up);
                float at[3], dir[3];
                for (int k = 0; k < 3; k++) {
                    at[k] = state.cam.pos[k] + fwd[k] * 0.4f;
                    dir[k] = fwd[k] + up[k] * 0.25f;
                }
                hta_projectiles_throw(&state.nades, at, dir, HTA_GRENADE_THROW);
                net_action(&state,HTA_NET_EVENT_GRENADE);
                state.nade_count--;
                hta_log("[player] grenade away, %d left", state.nade_count);
            }
        }
        if (state.nades.loaded &&
            (!state.net_enabled || state.net_hosting || !state.net.have_world)) {
            hta_projectiles_update(&state.nades,
                                   state.col.built ? &state.col : NULL, dt);
            if (state.nades.detonated) {
                if (state.net_hosting && state.game.grenade_pool>=0) {
                    hta_net_fx fx={.kind=HTA_NET_FX_DETONATE,
                        .entity=(uint8_t)state.me,
                        .weapon=(uint8_t)state.game.grenade_pool,
                        .material=state.nades.hit_material};
                    for (int k=0;k<3;k++) {
                        fx.pos[k]=state.nades.hit[k];
                        fx.dir[k]=state.nades.hit_normal[k];
                    }
                    hta_net_server_fx(&state.host_server,&fx);
                }
                hta_gun_add_mark(&state.gun, state.nades.hit,
                                 state.nades.hit_normal,
                                 state.nades.blast_radius);
                if (state.nade_snd)
                    play_tag_at(&state, state.nade_snd, state.nades.hit, 1.0f);
                shake_effect(&state, state.nades.det_effect, state.nades.hit);
                if (state.nade_recipe != HTA_PART_NO_RECIPE)
                    hta_particles_burst(&state.parts, state.nade_recipe,
                                        state.nades.hit, state.nades.hit_normal);
                if (state.game_on && (!state.net_enabled || state.net_hosting) &&
                    state.nades.blast_damage > 0.0f) {
                    /* Everyone in it, you included, and it is yours. */
                    hta_game_blast(&state.game, state.me, state.nades.hit,
                                   state.nades.blast_damage, state.nades.blast_core,
                                   state.nades.blast_damage_radius);
                }
                if (!state.game_on && state.bot.loaded && state.nades.blast_damage > 0.0f) {
                    float ctr[3];
                    hta_bot_centre(&state.bot, ctr);
                    float f = blast_falloff(state.nades.hit, ctr,
                                            state.nades.blast_core,
                                            state.nades.blast_damage_radius);
                    if (f > 0.0f)
                        hta_bot_damage(&state.bot,
                                       state.nades.blast_damage * f, ctr);
                }
                if (!state.game_on && state.vit->loaded && state.nades.blast_damage > 0.0f) {
                    float dx = state.cam.pos[0] - state.nades.hit[0];
                    float dy = state.cam.pos[1] - state.nades.hit[1];
                    float dz = state.cam.pos[2] - state.nades.hit[2];
                    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                    float r = state.nades.blast_damage_radius;
                    if (dist < r) {
                        float core = state.nades.blast_core;
                        float f = 1.0f;
                        if (dist > core && r > core)
                            f = 1.0f - (dist - core) / (r - core);
                        if (f > 0.0f)
                            hta_vitals_damage(state.vit,
                                              state.nades.blast_damage * f);
                    }
                }
            }
        }

        /* The body standing out there: animation, dying, coming back. */
        if (state.bot.loaded) hta_bot_update(&state.bot, dt);

        /* Rounds in flight. A detonation leaves the same scorch and plays
         * the same material impact a hitscan round would. */
        if (state.proj.loaded &&
            (!state.net_enabled || state.net_hosting || !state.net.have_world)) {
            hta_projectiles_update(&state.proj,
                                   state.col.built ? &state.col : NULL, dt);
            /* A round that reaches the body stops there. The projectile
             * layer only knows about the world, so this is the one place a
             * flying round is asked whether it has hit somebody. */
            if (state.game_on) {
                for (uint32_t q = 0; q < HTA_PROJ_MAX; q++) {
                    hta_projectile *pr = &state.proj.live[q];
                    if (!pr->alive) continue;
                    int32_t who = hta_game_near(&state.game, pr->pos, 0.02f, state.me);
                    if (who < 0) continue;
                    if (!state.net_enabled || state.net_hosting)
                        hta_game_hurt_jpt(&state.game, who, state.me, state.impact_jpt, 1, pr->pos);
                    if ((!state.net_enabled || state.net_hosting) && state.proj.blast_damage > 0.0f)
                        hta_game_blast(&state.game, state.me, pr->pos, state.proj.blast_damage,
                                       state.proj.blast_core, state.proj.blast_damage_radius);
                    float up[3] = { 0.0f, 0.0f, 1.0f };
                    if (state.det_recipe != HTA_PART_NO_RECIPE)
                        hta_particles_burst(&state.parts, state.det_recipe, pr->pos, up);
                    if (state.proj.detonation_snd)
                        play_tag_at(&state, state.proj.detonation_snd, pr->pos, 1.0f);
                    if (state.net_hosting)
                        for (uint32_t pool=0;pool<state.game.pool_count;pool++)
                            if (state.game.pools[pool].proj_tag_id==state.proj.proj_tag_id) {
                                hta_net_fx fx={.kind=HTA_NET_FX_DETONATE,
                                    .entity=(uint8_t)state.me,.weapon=(uint8_t)pool};
                                for (int k=0;k<3;k++) fx.pos[k]=pr->pos[k];
                                fx.dir[2]=1.0f;
                                hta_net_server_fx(&state.host_server,&fx);
                                break;
                            }
                    pr->alive = false;
                }
            } else if (state.bot.loaded && state.bot.state == HTA_BOT_ALIVE) {
                for (uint32_t q = 0; q < HTA_PROJ_MAX; q++) {
                    hta_projectile *pr = &state.proj.live[q];
                    if (!pr->alive) continue;
                    if (!hta_bot_near(&state.bot, pr->pos, state.bot.radius))
                        continue;
                    uint8_t mat = state.bot.vitals.shield > 0.0f
                                ? HTA_MATERIAL_CYBORG_SHIELD
                                : HTA_MATERIAL_CYBORG_ARMOR;
                    float dmg = hta_damage_vs(&state.cache, state.impact_jpt, mat);
                    hta_bot_damage(&state.bot, dmg, pr->pos);
                    /* It goes off where it stopped, not where it would
                     * have reached. */
                    float up[3] = { 0.0f, 0.0f, 1.0f };
                    if (state.det_recipe != HTA_PART_NO_RECIPE)
                        hta_particles_burst(&state.parts, state.det_recipe,
                                            pr->pos, up);
                    if (state.proj.detonation_snd)
                        play_tag_at(&state, state.proj.detonation_snd,
                                    pr->pos, 1.0f);
                    pr->alive = false;
                }
            }
            if (state.proj.detonated) {
                if (state.net_hosting) {
                    for (uint32_t pool=0;pool<state.game.pool_count;pool++)
                        if (state.game.pools[pool].proj_tag_id==state.proj.proj_tag_id) {
                            hta_net_fx fx={.kind=HTA_NET_FX_DETONATE,
                                .entity=(uint8_t)state.me,.weapon=(uint8_t)pool,
                                .material=state.proj.hit_material};
                            for (int k=0;k<3;k++) {
                                fx.pos[k]=state.proj.hit[k];
                                fx.dir[k]=state.proj.hit_normal[k];
                            }
                            hta_net_server_fx(&state.host_server,&fx);
                            break;
                        }
                }
                hta_gun_add_mark(&state.gun, state.proj.hit, state.proj.hit_normal,
                                 state.proj.blast_radius);
                /* An explosion has a bang of its own; a round that does not
                 * falls back to what the surface it hit sounds like. */
                if (state.proj.detonation_snd)
                    play_tag_at(&state, state.proj.detonation_snd,
                                state.proj.hit, 1.0f);
                else
                    play_impact_at(&state, state.proj.hit_material,
                                   state.proj.hit);
                /* Thrown out along the surface it hit. */
                hta_particles_burst(&state.parts, state.det_recipe,
                                    state.proj.hit, state.proj.hit_normal);
                shake_effect(&state, state.proj.det_effect, state.proj.hit);

                /* And it can catch you. A rocket is 80 at the centre,
                 * full inside 0.6 world units and gone by 2.0 -- which is
                 * why firing one at your own feet is a bad idea in Halo
                 * and now here too. */
                if (state.game_on && (!state.net_enabled || state.net_hosting) &&
                    state.proj.blast_damage > 0.0f)
                    hta_game_blast(&state.game, state.me, state.proj.hit,
                                   state.proj.blast_damage, state.proj.blast_core,
                                   state.proj.blast_damage_radius);
                if (!state.game_on && state.bot.loaded && state.proj.blast_damage > 0.0f) {
                    float ctr[3];
                    hta_bot_centre(&state.bot, ctr);
                    float f = blast_falloff(state.proj.hit, ctr,
                                            state.proj.blast_core,
                                            state.proj.blast_damage_radius);
                    if (f > 0.0f)
                        hta_bot_damage(&state.bot,
                                       state.proj.blast_damage * f, ctr);
                }
                if (!state.game_on && state.vit->loaded && state.proj.blast_damage > 0.0f) {
                    float dx = state.cam.pos[0] - state.proj.hit[0];
                    float dy = state.cam.pos[1] - state.proj.hit[1];
                    float dz = state.cam.pos[2] - state.proj.hit[2];
                    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                    float r = state.proj.blast_damage_radius;
                    if (dist < r) {
                        float core = state.proj.blast_core;
                        float f = 1.0f;
                        if (dist > core && r > core)
                            f = 1.0f - (dist - core) / (r - core);
                        if (f > 0.0f) {
                            hta_vitals_damage(state.vit,
                                              state.proj.blast_damage * f);
                            hta_log("[player] caught the blast at %.1f wu for %.0f",
                                    dist, state.proj.blast_damage * f);
                        }
                    }
                }
            }
        }

        /* The weapon sways with the walk, from the weapon's own `moving`
         * overlay, scaled by how fast the player is actually going. */
        if (state.vm.loaded) {
            float run = state.player.phys.run_forward > 0.1f
                      ? state.player.phys.run_forward : 2.25f;
            float vx = state.player.velocity[0], vy = state.player.velocity[1];
            float speed = sqrtf(vx * vx + vy * vy);
            hta_viewmodel_set_move(&state.vm, speed / run);
        }

        /* Everybody else: the bots think and fight, their rounds fly, the
         * dead come back and the score is kept. Before the particles, so a
         * bot's explosion bursts this frame. */
        if (state.game_on) {
            if (!state.net_enabled || state.net_hosting) {
                hta_game_update(&state.game, dt);
                game_events(&state);
            }
            hta_game_view_update(&state.gview, &state.game, state.show_self ? -1 : state.me, dt);
            if (state.net_enabled && !state.net_hosting)
                for (uint32_t i=0;i<state.game.unit_count;i++)
                    state.game.units[i].fired=state.game.units[i].meleed=
                    state.game.units[i].threw=state.game.units[i].hurt=false;
            if ((!state.net_enabled || state.net_hosting) && state.over_timer > 0.0f) {
                state.over_timer -= dt;
                if (state.over_timer <= 0.0f) {
                    hta_game_start(&state.game);
                    if (state.net_hosting) state.world_round++;
                    if (!state.dead) respawn(&state);
                    hta_log("[game] a new game");
                }
            }
            game_text(&state, dt);
        }
        hta_particles_update(&state.parts, state.col.built ? &state.col : NULL,
                             &state.cam, dt);
        net_frame(&state,now,dt,&in);
        vehicle_transition(&state);
        vehicle_camera(&state);
        vehicle_sounds(&state);
        if (state.trails.loaded) {
            for (uint32_t p = 0; p < state.game.pool_count; p++) {
                if (state.ptrail[p] == HTA_CONT_NONE) continue;
                for (uint32_t k = 0; k < HTA_PROJ_MAX; k++) {
                    const hta_projectile *q = &state.game.pools[p].live[k];
                    if (q->alive) hta_contrails_feed(&state.trails, state.ptrail[p], p * 16u + k,
                                                     q->pos, q->age);
                }
            }
            uint32_t lt = state.proj.loaded
                ? hta_contrails_for_projectile(&state.trails, &state.cache, NULL, state.proj.proj_tag_id)
                : HTA_CONT_NONE;
            for (uint32_t k = 0; lt != HTA_CONT_NONE && k < HTA_PROJ_MAX; k++)
                if (state.proj.live[k].alive)
                    hta_contrails_feed(&state.trails, lt, 1000u + k, state.proj.live[k].pos,
                                       state.proj.live[k].age);
            hta_contrails_update(&state.trails, &state.cam, dt);
        }
        hta_shake_update(&state.shake, dt);
        /* A hull on its last third throws sparks, faster as it goes. */
        if (state.game_on && state.spark_recipe != HTA_PART_NO_RECIPE &&
            state.vehicles.loaded) {
            state.spark_clock += dt;
            if (state.spark_clock >= 0.12f) {
                state.spark_clock = 0.0f;
                for (uint32_t i = 0; i < state.vehicles.count && i < HTA_VEHICLE_MAX; i++) {
                    const hta_vehicle *c = &state.vehicles.cars[i];
                    float hull = hta_game_hull(&state.game, (int32_t)i);
                    if (!c->active || hull > 0.35f) continue;
                    if ((float)(rand() % 100) / 100.0f > 0.35f + (0.35f - hull) * 2.0f) continue;
                    float a = (float)(rand() % 628) / 100.0f;
                    float at[3] = { c->pos[0] + cosf(a) * c->body_radius * 0.4f,
                                    c->pos[1] + sinf(a) * c->body_radius * 0.4f,
                                    c->pos[2] + 0.35f };
                    float up[3] = { cosf(a) * 0.3f, sinf(a) * 0.3f, 0.9f };
                    hta_particles_burst(&state.parts, state.spark_recipe, at, up);
                }
            }
        }

        if (state.gun.dirty && state.gfx) {
            char err[HTA_ERRLEN];
            hta_gun_build_mesh(&state.gun);
            if (state.gpu_fx) { hta_gfx_mesh_free(state.gfx, state.gpu_fx); state.gpu_fx = NULL; }
            if (state.gun.mesh.index_count)
                state.gpu_fx = hta_gfx_mesh_upload(state.gfx, &state.gun.mesh, err, sizeof(err));
        }

        if (state.has_window && state.gfx) {
            rebuild_gfx_if_size_changed(&state);
            if (!state.gfx) continue;
            hta_gfx_viewmodel vmdraw;
            memset(&vmdraw, 0, sizeof(vmdraw));
            hta_gfx_overlay huddraw;
            memset(&huddraw, 0, sizeof(huddraw));
            if (state.gpu_hud) {
                uint32_t ew = 0, eh = 0;
                hta_gfx_extent(state.gfx, &ew, &eh);
                hta_hud_layout(&state.hud, ew, eh);
                huddraw.mesh = state.gpu_hud;
                huddraw.vertices = state.hud.mesh.vertices;
                huddraw.vertex_count = state.hud.mesh.vertex_count;
                huddraw.submeshes = state.hud.mesh.submeshes;
                huddraw.submesh_count = state.hud.mesh.submesh_count;
            }
            /* Scoped means looking THROUGH the weapon, so Halo takes it
             * off the screen entirely while zoomed. Without this the sniper
             * reads as a magnified view with a rifle in front of it. */
            /* And a corpse is not holding it either. */
            bool fp_weapon = !driving || (armed_seat && !state.show_self);
            if (state.gpu_fp && state.zoom_level == 0 && !state.dead && fp_weapon) {
                vmdraw.mesh = state.gpu_fp;
                vmdraw.vertices = state.vm.posed;
                vmdraw.vertex_count = state.vm.mesh.vertex_count;
                for (int k = 0; k < 3; k++) vmdraw.offset[k] = state.weap.fp_offset[k];
            }
            hta_gfx_dynamic dynlist[HTA_GFX_MAX_DYNAMIC];
            memset(dynlist, 0, sizeof(dynlist));   /* `lit` defaults off */
            uint32_t dyncount = 0;
            if (state.gpu_proj) {
                dynlist[dyncount].mesh = state.gpu_proj;
                dynlist[dyncount].vertices = state.proj.mesh.vertices;
                dynlist[dyncount].vertex_count = state.proj.mesh.vertex_count;
                dyncount++;
            }
            if (state.gpu_nades) {
                dynlist[dyncount].mesh = state.gpu_nades;
                dynlist[dyncount].vertices = state.nades.mesh.vertices;
                dynlist[dyncount].vertex_count = state.nades.mesh.vertex_count;
                dyncount++;
            }
            if (state.gpu_parts) {
                dynlist[dyncount].mesh = state.gpu_parts;
                dynlist[dyncount].vertices = state.parts.mesh.vertices;
                dynlist[dyncount].vertex_count = state.parts.mesh.vertex_count;
                dyncount++;
            }
            if (state.bot.loaded && state.gpu_bot &&
                dyncount < HTA_GFX_MAX_DYNAMIC) {
                dynlist[dyncount].mesh = state.gpu_bot;
                dynlist[dyncount].vertices = state.bot.actor.posed;
                dynlist[dyncount].vertex_count = state.bot.actor.mesh.vertex_count;
                dynlist[dyncount].lit = true;     /* a body, not a spark */
                dyncount++;
            }
            if (state.gpu_items && dyncount < HTA_GFX_MAX_DYNAMIC) {
                dynlist[dyncount].mesh = state.gpu_items;
                /* NULL skips the copy. The items do not move, so they are
                 * only written into the vertex slots after something is
                 * taken or comes back -- 23,000 vertices every frame to
                 * keep thirty-seven still objects still would be 900 KB a
                 * frame of nothing. */
                dynlist[dyncount].vertices =
                    state.items_upload > 0 ? state.items.posed : NULL;
                dynlist[dyncount].vertex_count = state.items.mesh.vertex_count;
                if (state.items_upload > 0) state.items_upload--;
                dyncount++;
            }
            if (state.corpse_up && state.gpu_corpse &&
                dyncount < HTA_GFX_MAX_DYNAMIC) {
                dynlist[dyncount].mesh = state.gpu_corpse;
                dynlist[dyncount].vertices = state.corpse.posed;
                dynlist[dyncount].vertex_count = state.corpse.mesh.vertex_count;
                dynlist[dyncount].lit = true;
                if (state.game_on && state.game.teams && state.me >= 0) {
                    dynlist[dyncount].change = true;
                    hta_game_team_color(state.game.units[state.me].team,
                                        dynlist[dyncount].change_color);
                }
                dyncount++;
            }
            int remote_slot=state.remote_to.weapon==1 ? 1 : 0;
            if (state.remote_visible && !state.net_hosting && !state.world_applied_tick &&
                state.gpu_remote[remote_slot] &&
                dyncount < HTA_GFX_MAX_DYNAMIC) {
                dynlist[dyncount].mesh = state.gpu_remote[remote_slot];
                dynlist[dyncount].vertices = state.remote[remote_slot].posed;
                dynlist[dyncount].vertex_count = state.remote[remote_slot].mesh.vertex_count;
                dynlist[dyncount].lit = true;
                dyncount++;
            }
            if (state.gpu_trails && dyncount < HTA_GFX_MAX_DYNAMIC) {
                dynlist[dyncount].mesh = state.gpu_trails;
                dynlist[dyncount].vertices = state.trails.mesh.vertices;
                dynlist[dyncount].vertex_count = state.trails.mesh.vertex_count;
                dynlist[dyncount].vertex_color = true;
                dyncount++;
            }
            g_inst_count = 0;
            dyncount = game_draw(&state, dynlist, dyncount);
            vehicles_draw(&state);
            hta_gfx_set_instances(state.gfx, g_inst, g_inst_count);
            hta_camera drawcam = state.cam;
            hta_shake_apply(&state.shake, &drawcam);
            if (!hta_gfx_draw(state.gfx, &drawcam, &state.scene, state.gpu_mesh,
                              state.gpu_sky, state.gpu_fx,
                              dynlist, dyncount,
                              vmdraw.mesh ? &vmdraw : NULL,
                              state.gpu_hud ? &huddraw : NULL)) {
                hta_log("[app] surface lost; rebuilding renderer");
                stop_gfx(&state);
                if (app->window) start_gfx(&state);
            }
            state.frames++;
            state.fps_accum += dt;
            state.fps_frames++;
            if (driving && state.my_car >= 0) {
                /* World units are ten feet: wu/s x 3.048 x 3.6 is km/h. */
                float kmh = hta_vehicles_speed(&state.vehicles, (uint32_t)state.my_car)
                          * 3.048f * 3.6f;
                const hta_game_vgun *gun = &state.game.vgun[state.my_car];
                bool loading = (!state.net_enabled || state.net_hosting) &&
                               (gun->chamber[0] > 0.0f || gun->chamber[1] > 0.0f);
                float hull = hta_game_hull(&state.game, state.my_car);
                char hulls[16] = "";
                if (hull < 0.995f)
                    snprintf(hulls, sizeof(hulls), "  HULL %d%%", (int)(hull * 100.0f + 0.5f));
                snprintf(g_ammo_text, sizeof(g_ammo_text), "%.0f km/h%s%s", kmh,
                         loading ? "  LOADING" : "", hulls);
            }
            else if (state.ammo.phase == HTA_AMMO_RELOADING)
                snprintf(g_ammo_text, sizeof(g_ammo_text), "-- / %d", state.ammo.reserve);
            else
                snprintf(g_ammo_text, sizeof(g_ammo_text), "%d / %d",
                         state.ammo.loaded, state.ammo.reserve);
            snprintf(g_debug_text, sizeof(g_debug_text),
                     "%.2f %.2f %.2f  %s  %.0f fps  net:%u/%u %.0fms",
                     state.player.pos[0], state.player.pos[1], state.player.pos[2],
                     state.player.on_ground ? "ground" : "air",
                     state.fps_accum > 0.05 ? state.fps_frames / state.fps_accum : 0.0,
                     state.net_enabled ? state.net.id : 0,
                     state.remote_visible ? state.remote_id : 0,
                     state.net_enabled ? state.net.stats.ping_ms : 0.0);
            if (state.fps_accum >= 2.0) {
                hta_log("[perf] %.1f fps | pos (%.2f %.2f %.2f) %s | tris %u"
                        " | audio %s %u voices %u started %u dropped",
                        state.fps_frames / state.fps_accum,
                        state.player.pos[0], state.player.pos[1], state.player.pos[2],
                        state.player.on_ground ? "grounded" : "airborne",
                        state.have_mesh ? state.mesh.index_count / 3 : 0,
                        hta_audio_android_running() ? "on" : "off",
                        hta_audio_active_voices(&state.audio),
                        state.audio.started,
                        (unsigned)atomic_load(&state.audio.dropped));
                hta_log("[weapon] spread %.2f deg", hta_gun_spread(&state.gun) * 57.2957795f);
                hta_log("[weapon] ammo %d / %d%s", state.ammo.loaded,
                        state.ammo.reserve,
                        state.ammo.phase == HTA_AMMO_RELOADING ? " (reloading)" : "");
                if (state.net_enabled) {
                    const hta_net_stats *n=&state.net.stats, *old=&state.net_stats_prev;
                    double seconds=state.fps_accum;
                    hta_log("[net] id %u remote %u ping %.1f ms | %.1f/%.1f pkt/s "
                            "%.0f/%.0f B/s in/out | %.1f snapshots/s | invalid %llu dropped %llu",
                            state.net.id,state.remote_visible ? state.remote_id : 0,
                            n->ping_ms,
                            (n->packets_in-old->packets_in)/seconds,
                            (n->packets_out-old->packets_out)/seconds,
                            (n->bytes_in-old->bytes_in)/seconds,
                            (n->bytes_out-old->bytes_out)/seconds,
                            (n->snapshots_in-old->snapshots_in)/seconds,
                            (unsigned long long)n->invalid,
                            (unsigned long long)n->dropped);
                    state.net_stats_prev=*n;
                }
                state.fps_accum = 0.0;
                state.fps_frames = 0;
            }
        }
    }

done:
    hta_log("[app] shutting down after %llu frames", (unsigned long long)state.frames);
    /* Stop the stream before freeing the PCM its voices point at. */
    hta_audio_android_stop();
    if (state.net_enabled) hta_net_client_close(&state.net);
    if (state.net_hosting) hta_net_server_close(&state.host_server);
    hta_hud_free(&state.hud);
    for (uint32_t i = 0; i < state.bank_count; i++)
        for (uint32_t k = 0; k < state.bank[i].count; k++)
            free(state.bank[i].pcm[k]);
    state.bank_count = 0;
    stop_gfx(&state);
    hta_game_view_free(&state.gview);
    hta_game_free(&state.game);
    hta_nav_free(&state.nav);
    hta_collision_free(&state.col);
    hta_vehicles_free(&state.vehicles);
    hta_contrails_free(&state.trails);
    hta_gun_free(&state.gun);
    hta_projectiles_free(&state.proj);
    hta_projectiles_free(&state.nades);
    hta_particles_free(&state.parts);
    hta_bsp_free(&state.mesh);
    hta_bsp_free(&state.sky);
    hta_bsp_free(&state.coll_mesh);
    hta_viewmodel_free(&state.vm);
    for (int slot=0;slot<2;slot++) hta_actor_free(&state.remote[slot]);
    for (uint32_t i = 0; i < state.menu_pcm_count; i++) free(state.menu_pcm[i]);
    hta_menu_free(&state.menu);
    for (uint32_t i = 0; i < state.mapped_count; i++)
        munmap(state.mapped_base[i], state.mapped_len[i]);
}
