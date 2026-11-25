// SPDX-License-Identifier: GPL-2.0-or-later
/* God of War III Remastered's module (SpartanPort's features): the live engine frame rate, the
 * stick-rotation prompts on a keyboard (runtime_pad.c) and the pause menu's Exit to Desktop.
 * SpartanPort's cheats, orb and damage multipliers, enemy health bar and Automatic QTE were removed
 * on 25.11.2026: their switches live in the service's menu, which the two-process build never
 * passed to the game.
 * Compiled into the runtime with the other games' modules (build.sh, out/game_modules.inc, included
 * by src/probe.c after its loader helpers, which it uses), run only for a profile with
 * modules = ["god_of_war"]. */
#include "qte_hook.h"
#include "menu_hook.h"

extern uintptr_t spartanport_qte_state_address(void);

/* The hooks below find their sites by signature (src/signature.h), so they work in any region
 * and version of the game whose code matches; a site that is not found leaves its feature off. */
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
static void god_of_war_mapped(void) {
    if (!engine_fps_at) return;
    spartanport_engine_fps_globals((uintptr_t)(image+engine_fps_at),(uintptr_t)(image+engine_rounded_at));
    puts("Engine FPS: live frame rate ready (menu, R3 + L1 + R1 or F9)");
}

/* QTE prompts (src/qte_hook.h): a frame count in the prompt's update and an answer in its
 * rotation check, for the stick prompts in runtime_pad.c. Both or neither;
 * they do nothing until one of those answers a prompt. Returns 1 when installed. */
/* The rotation stub also reads an Automatic QTE flag; the feature was removed (25.11.2026), the
 * flag stays 0. */
static uint8_t auto_qte_off;
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
    spartanport_qte_build_rotation_stub(stubs + 128, (uintptr_t)&state->pass, (uintptr_t)&auto_qte_off,
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
    find_engine_fps(segments, ns, size);
    install_qte_hooks(segments, ns, size);
    if (pause_menu && app0) install_menu_exit(segments, ns, size, pause_menu);
}
GAME_MODULE(god_of_war)
