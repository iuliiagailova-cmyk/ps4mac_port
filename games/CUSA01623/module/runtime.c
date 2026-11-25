// SPDX-License-Identifier: GPL-2.0-or-later
/* God of War III Remastered's module (SpartanPort's features): cheats, orb multipliers, the live
 * engine frame rate, the enemy health bar, QTE answers and the pause menu's Exit to Desktop.
 * Compiled into the runtime with the other games' modules (build.sh, out/game_modules.inc, included
 * by src/probe.c after its loader helpers, which it uses), run only for a profile with
 * modules = ["god_of_war"]. */
#include "orb_hook.h"
#include "cheat_hook.h"
#include "qte_hook.h"
#include "actor_hook.h"
#include "damage_hook.h"
#include "menu_hook.h"

extern ABI float spartanport_orbs_gain(void *player, float requested);
extern void spartanport_orbs_status(int supported);
extern uintptr_t spartanport_cheat_flag(unsigned id);
extern uintptr_t spartanport_actor_slots_address(void);
extern void spartanport_actor_status(int supported);
extern void spartanport_cheat_status(unsigned id);
extern uintptr_t spartanport_damage_multiplier(int taken);
extern uintptr_t spartanport_orb_pickup_multiplier(unsigned kind);
extern void spartanport_stat_multipliers_status(int damage, int orb_pickup);
extern uintptr_t spartanport_qte_state_address(void);
extern void spartanport_qte_status(int supported);
extern uintptr_t spartanport_auto_qte_flag(void);

/* The hooks below find their sites by signature (src/signature.h), so they work in any region
 * and version of the game whose code matches; a site that is not found leaves its feature off. */
static void install_cheats(Segment *segments, uint64_t count, uint64_t size) {
    const char *names[] = {"health", "magic", "item meter", "Rage of Sparta", "red orbs",
                           "Very Hard unlock", "infinite double jumps"};
    uint64_t at[SPARTAN_CHEAT_HOOK_COUNT];
    for (unsigned i = 0; i < SPARTAN_CHEAT_HOOK_COUNT; ++i)
        at[i] = spartanport_signature_find(image, size, &spartanport_cheat_hooks[i].site);
    for (unsigned id = 0; id < SPARTAN_CHEAT_COUNT; ++id) {
        int valid = 1;
        for (unsigned i = 0; i < SPARTAN_CHEAT_HOOK_COUNT; ++i) {
            const SpartanCheatHook *hook = &spartanport_cheat_hooks[i];
            if (hook->id != id) continue;
            if (at[i] == UINT64_MAX || !mapped(segments, count, at[i], hook->length) ||
                !spartanport_cheat_signature_match(hook, image + at[i], size - at[i])) valid = 0;
        }
        if (!valid) {
            printf("Cheats: %s unavailable (code not found in this eboot.bin)\n", names[id]);
            continue;
        }
        unsigned char *stubs = allocate(page_size);
        int32_t relative[SPARTAN_CHEAT_HOOK_COUNT] = {0};
        for (unsigned i = 0; i < SPARTAN_CHEAT_HOOK_COUNT; ++i) {
            const SpartanCheatHook *hook = &spartanport_cheat_hooks[i];
            if (hook->id != id) continue;
            unsigned char *stub = stubs + i * 256;
            int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at[i] + 5);
            if (delta < INT32_MIN || delta > INT32_MAX) { valid = 0; break; }
            relative[i] = (int32_t)delta;
            spartanport_cheat_build_stub(stub, hook, spartanport_cheat_flag(id),
                                 (uintptr_t)(image + at[i] + hook->length));
        }
        if (!valid) {
            printf("Cheats: %s unavailable (hook outside relative-jump range)\n", names[id]);
            continue;
        }
        protect(stubs, page_size, 5);
        for (unsigned i = 0; i < SPARTAN_CHEAT_HOOK_COUNT; ++i) {
            const SpartanCheatHook *hook = &spartanport_cheat_hooks[i];
            if (hook->id != id) continue;
            image[at[i]] = 0xe9;
            memcpy(image + at[i] + 1, &relative[i], 4);
            memset(image + at[i] + 5, 0x90, hook->length - 5);
        }
        spartanport_cheat_status(id);
        printf("Cheats: %s ready (live toggle, initially disabled unless saved)\n", names[id]);
    }
}

/* The engine's frame rate setter, called once at start-up with 59.94 (the frame rate patches
 * change that constant): it stores the maximum frame rate as a float and rounded, and the frame
 * timing reads both every frame. Writing them changes the engine rate live (SyncEngineFps). */
extern void spartanport_engine_fps_globals(uintptr_t fps, uintptr_t rounded);
static const SpartanSignature engine_fps_setter={
    "48 8d 05 ?? ?? ?? ?? c5 fa 11 00 c5 fa 58 0d ?? ?? ?? ?? c5 fa 2c c1 48 8d 0d ?? ?? ?? ?? 89 01",0};
static uint64_t engine_fps_at, engine_rounded_at; /* image offsets; 0: not found */
/* Before the image is protected: the search reads the gaps between segments too. */
static void find_engine_fps(Segment *segments, uint64_t count, uint64_t size) {
    const uint64_t at=spartanport_signature_find(image,size,&engine_fps_setter);
    if (at==UINT64_MAX) {
        puts("Engine FPS: live frame rate unavailable (code not found in this eboot.bin)");
        return;
    }
    int32_t fps_displacement, rounded_displacement;
    memcpy(&fps_displacement,image+at+3,4);      /* lea rax, [rip+fps] */
    memcpy(&rounded_displacement,image+at+26,4); /* lea rcx, [rip+rounded] */
    const uint64_t fps=(uint64_t)((int64_t)at+7+fps_displacement);
    const uint64_t rounded=(uint64_t)((int64_t)at+30+rounded_displacement);
    if (!mapped(segments,count,fps,4) || !mapped(segments,count,rounded,4)) {
        puts("Engine FPS: live frame rate unavailable (globals outside the image)");
        return;
    }
    engine_fps_at=fps;
    engine_rounded_at=rounded;
}
/* After it: the present thread reads the globals every vblank, and the image is not
 * accessible between the two protect passes. */
static void publish_engine_fps(void) {
    if (!engine_fps_at) return;
    spartanport_engine_fps_globals((uintptr_t)(image+engine_fps_at),(uintptr_t)(image+engine_rounded_at));
    puts("Engine FPS: live frame rate ready (menu, R3 + L1 + R1 or F9)");
}

/* Damage hook for the enemy health bar (src/actor_hook.h). Always
 * installed when the code matches: it only records values; the overlay decides what to show. */
static void install_actor_watch(Segment *segments, uint64_t count, uint64_t size) {
    const uint64_t at = spartanport_signature_find(image, size, &spartanport_actor_site);
    if (at == UINT64_MAX || !mapped(segments, count, at, SPARTAN_ACTOR_HOOK_LENGTH) ||
        !spartanport_actor_signature_match(image + at, size - at)) {
        printf("Health display: unavailable (code not found in this eboot.bin)\n");
        return;
    }
    unsigned char *stub = allocate(page_size);
    const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) {
        printf("Health display: unavailable (hook outside relative-jump range)\n");
        return;
    }
    spartanport_actor_build_stub(stub, spartanport_actor_slots_address(), (uintptr_t)(image + at + SPARTAN_ACTOR_HOOK_LENGTH));
    protect(stub, page_size, 5);
    const int32_t relative = (int32_t)delta;
    image[at] = 0xe9;
    memcpy(image + at + 1, &relative, 4);
    memset(image + at + 5, 0x90, SPARTAN_ACTOR_HOOK_LENGTH - 5);
    spartanport_actor_status(1);
    printf("Health display: damage hook ready (enemy health bar in the menu)\n");
}

/* QTE prompts (src/qte_hook.h): a frame count in the prompt's update and an answer in its
 * rotation check, for the stick prompts in runtime_pad.c and Automatic QTE. Both or neither;
 * they do nothing until one of those answers a prompt. Returns 1 when installed. */
static int install_qte_hooks(Segment *segments, uint64_t count, uint64_t size) {
    const uint64_t sites[2] = {spartanport_signature_find(image, size, &spartanport_qte_update_site),
                               spartanport_signature_find(image, size, &spartanport_qte_rotation_site)};
    const unsigned char *originals[2] = {spartanport_qte_update_original, spartanport_qte_rotation_original};
    for (unsigned i = 0; i < 2; ++i) {
        if (sites[i] == UINT64_MAX || !mapped(segments, count, sites[i], SPARTAN_QTE_BLOCK_NEXT) ||
            memcmp(image + sites[i], originals[i], SPARTAN_QTE_HOOK_LENGTH)) {
            printf("Stick prompts: unavailable (code not found in this eboot.bin)\n");
            return 0;
        }
    }
    const uint64_t block = spartanport_qte_block(image + sites[1], sites[1]);
    if (!mapped(segments, count, block, SPARTAN_QTE_TYPE_OFFSET + 2)) {
        printf("Stick prompts: unavailable (prompt state outside the image)\n");
        return 0;
    }
    unsigned char *stubs = allocate(page_size);
    int32_t relative[2];
    for (unsigned i = 0; i < 2; ++i) {
        const int64_t delta = (int64_t)(uintptr_t)(stubs + i * 128) - (int64_t)(uintptr_t)(image + sites[i] + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) {
            printf("Stick prompts: unavailable (hook outside relative-jump range)\n");
            return 0;
        }
        relative[i] = (int32_t)delta;
    }
    SpartanQteState *state = (SpartanQteState *)spartanport_qte_state_address();
    spartanport_qte_build_update_stub(stubs, (uintptr_t)&state->heartbeat,
                               (uintptr_t)(image + sites[0] + SPARTAN_QTE_HOOK_LENGTH));
    spartanport_qte_build_rotation_stub(stubs + 128, (uintptr_t)&state->pass, spartanport_auto_qte_flag(),
                                 (uintptr_t)(image + sites[1] + SPARTAN_QTE_HOOK_LENGTH));
    protect(stubs, page_size, 5);
    for (unsigned i = 0; i < 2; ++i) {
        image[sites[i]] = 0xe9;
        memcpy(image + sites[i] + 1, &relative[i], 4);
        memset(image + sites[i] + 5, 0x90, SPARTAN_QTE_HOOK_LENGTH - 5);
    }
    state->block = (uint64_t)(uintptr_t)(image + block);
    printf("Stick prompts: rotation prompt hooks ready\n");
    return 1;
}

/* Automatic QTE (src/qte_hook.h): the button check of timed prompts, the press query of repeated
 * presses, the held buttons of charged presses and the Circle struggle; rotations go through the
 * rotation stub above. Each site on its own: one that is not found leaves that kind to the player. */
enum { AUTO_QTE_SITES = 4 };
static void install_auto_qte_hooks(Segment *segments, uint64_t count, uint64_t size, int rotation) {
    const SpartanSignature *signatures[AUTO_QTE_SITES] = {&spartanport_qte_timed_site, &spartanport_qte_repeat_site,
                                                       &spartanport_qte_charged_site, &spartanport_qte_circle_site};
    const unsigned lengths[AUTO_QTE_SITES] = {SPARTAN_QTE_TIMED_LENGTH, 5, SPARTAN_QTE_CHARGED_LENGTH,
                                              SPARTAN_QTE_CIRCLE_LENGTH};
    const char *names[AUTO_QTE_SITES] = {"button", "repeated press", "held press", "Circle struggle"};
    const uintptr_t automatic = spartanport_auto_qte_flag();
    unsigned char *stubs = allocate(page_size);
    uint64_t sites[AUTO_QTE_SITES];
    int32_t relative[AUTO_QTE_SITES] = {0};
    int ready = 0;
    for (unsigned i = 0; i < AUTO_QTE_SITES; ++i) {
        sites[i] = spartanport_signature_find(image, size, signatures[i]);
        if (sites[i] == UINT64_MAX || !mapped(segments, count, sites[i], lengths[i])) {
            printf("Automatic QTE: %s prompts unavailable (code not found in this eboot.bin)\n", names[i]);
            sites[i] = UINT64_MAX;
            continue;
        }
        unsigned char *stub = stubs + i * 128;
        const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + sites[i] + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) {
            printf("Automatic QTE: %s prompts unavailable (hook outside relative-jump range)\n", names[i]);
            sites[i] = UINT64_MAX;
            continue;
        }
        relative[i] = (int32_t)delta;
        const uintptr_t resume = (uintptr_t)(image + sites[i] + lengths[i]);
        if (i == 0) spartanport_qte_build_timed_stub(stub, automatic, resume);
        else if (i == 1) spartanport_qte_build_repeat_stub(stub, automatic,
                                                    (uintptr_t)(image + spartanport_qte_call_target(image + sites[i], sites[i])));
        else if (i == 2) spartanport_qte_build_charged_stub(stub, automatic, resume);
        else spartanport_qte_build_circle_stub(stub, automatic, resume, (uintptr_t)(image + sites[i] + SPARTAN_QTE_CIRCLE_SKIP));
    }
    protect(stubs, page_size, 5);
    for (unsigned i = 0; i < AUTO_QTE_SITES; ++i) {
        if (sites[i] == UINT64_MAX) continue;
        image[sites[i]] = i == 1 ? 0xe8 : 0xe9;  /* the repeat site stays a call */
        memcpy(image + sites[i] + 1, &relative[i], 4);
        memset(image + sites[i] + 5, 0x90, lengths[i] - 5);
        ++ready;
    }
    if (!ready && !rotation) return;
    spartanport_qte_status(1);
    printf("Automatic QTE: ready for %d of %d prompt kinds (menu, Game)\n", ready + rotation, AUTO_QTE_SITES + 1);
}

static void install_orb_multiplier(Segment *segments, uint64_t count, uint64_t size) {
    uint64_t calls[3];
    const uint64_t setter_at = spartanport_orb_find(image, size, calls);
    if (setter_at == UINT64_MAX) {
        printf("Red orbs: multiplier unavailable (code not found in this eboot.bin)\n");
        return;
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!mapped(segments, count, calls[i], 5)) return;
    }
    /* SysV call sites: preserve the receiver across the helper, then tail-call the original
     * setter. Its cap and upgrade notifications remain in the game's own code. */
    unsigned char *stub = allocate(page_size);
    uintptr_t helper = (uintptr_t)spartanport_orbs_gain, setter = (uintptr_t)(image + setter_at);
    unsigned char code[26];
    spartanport_orb_build_stub(code, helper, setter);
    int32_t displacements[3];
    for (unsigned i = 0; i < 3; ++i) {
        int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + calls[i] + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) {
            printf("Red orbs: multiplier unavailable (hook outside relative-call range)\n");
            return;
        }
        displacements[i] = (int32_t)delta;
    }
    memcpy(stub, code, sizeof(code));
    protect(stub, page_size, 5);
    for (unsigned i = 0; i < 3; ++i)
        memcpy(image + calls[i] + 1, &displacements[i], 4);
    spartanport_orbs_status(1);
    printf("Red orbs: live multiplier ready (3 validated gain paths, 0.1x to 100x)\n");
}
/* Writes a jump from image+at to `stub`; the rest of the replaced bytes become nops (the stub
 * replays them). 0 when the stub is out of relative-jump range. */
static int install_jump(uint64_t at, unsigned length, unsigned char *stub) {
    const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) return 0;
    const int32_t relative = (int32_t)delta;
    image[at] = 0xe9;
    memcpy(image + at + 1, &relative, 4);
    memset(image + at + 5, 0x90, length - 5);
    return 1;
}

/* Damage dealt/taken and green/blue/gold orb multipliers (src/damage_hook.h, src/orb_hook.h):
 * the stubs read the menu's floats directly, so changes apply on the next hit or pickup. */
static void install_stat_multipliers(Segment *segments, uint64_t count, uint64_t size) {
    const uint64_t damage_at = spartanport_signature_find(image, size, &spartanport_damage_site);
    const uint64_t pickup_at = spartanport_signature_find(image, size, &spartanport_orb_pickup_site);
    int damage = damage_at != UINT64_MAX && mapped(segments, count, damage_at, SPARTAN_DAMAGE_HOOK_LENGTH) &&
                 spartanport_damage_signature_match(image + damage_at, size - damage_at);
    int pickup = pickup_at != UINT64_MAX && mapped(segments, count, pickup_at, SPARTAN_ORB_PICKUP_LENGTH) &&
                 spartanport_orb_pickup_signature_match(image + pickup_at, size - pickup_at);
    unsigned char *stubs = damage || pickup ? allocate(page_size) : NULL;
    if (damage) {
        spartanport_damage_build_stub(stubs, spartanport_damage_multiplier(1), spartanport_damage_multiplier(0),
                               (uintptr_t)(image + damage_at + SPARTAN_DAMAGE_HOOK_LENGTH));
    }
    if (pickup) {
        spartanport_orb_pickup_build_stub(stubs + 256, spartanport_orb_pickup_multiplier(0), spartanport_orb_pickup_multiplier(1),
                                   spartanport_orb_pickup_multiplier(2),
                                   (uintptr_t)(image + pickup_at + SPARTAN_ORB_PICKUP_LENGTH));
    }
    if (stubs) protect(stubs, page_size, 5);
    damage = damage && install_jump(damage_at, SPARTAN_DAMAGE_HOOK_LENGTH, stubs);
    pickup = pickup && install_jump(pickup_at, SPARTAN_ORB_PICKUP_LENGTH, stubs + 256);
    spartanport_stat_multipliers_status(damage, pickup);
    printf("Multipliers: damage %s, green/blue/gold orbs %s\n", damage ? "ready" : "unavailable",
           pickup ? "ready" : "unavailable");
}
/* The pause menu's "Exit to Desktop" row (src/menu_hook.h). Ends the process as the restart
 * does: GPU, audio and guest threads are still running, so no destructors. */
static void menu_quit(void) {
    puts("Pause menu: Exit to Desktop");
    fflush(NULL);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(),0);
#endif
    _exit(0);
}
/* The row comes with the hook: its copy of R_UI.WAD (scripts/pause_menu.py) is mounted over the
 * game's, and over the glyphs' copy it was made from, only once the hook is in. */
static void install_menu_exit(Segment *segments, uint64_t count, uint64_t size, const char *wad) {
    const uint64_t at = spartanport_signature_find(image, size, &spartanport_menu_site);
    if (at == UINT64_MAX || !mapped(segments, count, at, SPARTAN_MENU_HOOK_LENGTH) ||
        !spartanport_menu_signature_match(image + at, size - at)) {
        printf("Pause menu: Exit to Desktop unavailable (code not found in this eboot.bin)\n");
        return;
    }
    unsigned char *stub = allocate(page_size);
    spartanport_menu_build_stub(stub, (uintptr_t)menu_quit, (uintptr_t)(image + at + SPARTAN_MENU_HOOK_LENGTH));
    protect(stub, page_size, 5);
    if (!install_jump(at, SPARTAN_MENU_HOOK_LENGTH, stub)) {
        puts("Pause menu: Exit to Desktop unavailable (hook outside relative-jump range)");
        return;
    }
    runtime_file_mount("/app0/EXEC/WAD/PS4_LE/R_UI.WAD", wad);
    puts("Pause menu: Exit to Desktop ready");
}
static void god_of_war_install(Segment *segments, uint64_t ns, uint64_t size, const char *pause_menu, const char *app0) {
    install_orb_multiplier(segments, ns, size);
    install_cheats(segments, ns, size);
    find_engine_fps(segments, ns, size);
    install_actor_watch(segments, ns, size);
    install_stat_multipliers(segments, ns, size);
    install_auto_qte_hooks(segments, ns, size, install_qte_hooks(segments, ns, size));
    if (pause_menu && app0) install_menu_exit(segments, ns, size, pause_menu);
}
GAME_MODULE(god_of_war)
