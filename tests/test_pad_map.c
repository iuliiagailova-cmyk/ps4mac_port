/* SPARTAN_PAD_MAP / SPARTAN_KEY_MAP parsing (src/runtime_pad.c). */
#define _GNU_SOURCE
#include <assert.h>
#include "../src/runtime_pad.c"

int spartanportgpu_overlay_captures_input(void) { return 0; }
int spartanportgpu_vibration_percent(void) { return 100; }
static int pause_keys;
void spartanportgpu_mouse_pause_key(void) { ++pause_keys; }
static int aim_style;
int spartanportgpu_mouse_aim_style(void) { return aim_style; }
static int aim_sensitivity=100;
int spartanportgpu_mouse_aim_sensitivity(void) { return aim_sensitivity; }
void spartanportgpu_engine_fps_cycle(void) {}
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

/* PS4 buttons a host source presses in `map`. */
static uint32_t pressed_by(const ButtonMap *map, int source) {
    uint32_t ps=0;
    for (size_t i=0;i<map->count;++i) if (map->entries[i].source==source) ps|=map->entries[i].ps;
    return ps;
}

int main(void) {
    ButtonMap keys=key_map;
    assert(apply_button_map(&keys,NULL,true)==0 && apply_button_map(&keys,"",true)==0);
    assert(keys.count==key_map.count);

    /* Swap: Q presses Cross, Space presses Triangle; Space no longer presses Cross. */
    assert(apply_button_map(&keys," cross = Q , triangle=Space",true)==2);
    assert(pressed_by(&keys,SDL_SCANCODE_Q)==BTN_CROSS);
    assert(pressed_by(&keys,SDL_SCANCODE_SPACE)==BTN_TRIANGLE);
    assert(pressed_by(&keys,SDL_SCANCODE_E)==BTN_SQUARE);

    /* Names with spaces, any case; the last entry for a button wins. */
    ButtonMap again=key_map;
    assert(apply_button_map(&again,"CIRCLE=left shift,circle=Left Ctrl",true)==2);
    assert(pressed_by(&again,SDL_SCANCODE_LCTRL)==BTN_CIRCLE);
    assert(pressed_by(&again,SDL_SCANCODE_LSHIFT)==0);

    /* Bad entries are skipped and leave the defaults alone. */
    ButtonMap bad=key_map;
    assert(apply_button_map(&bad,"bogus=Q,cross=NotAKey,circle,=Q,square=,,",true)==0);
    assert(bad.count==key_map.count && pressed_by(&bad,SDL_SCANCODE_SPACE)==BTN_CROSS);

    /* Gamepad: Xbox layout swap; 'touchpad' is the left click, Back stays the right click and
     * the real touchpad click keeps its fingers. */
    ButtonMap pad=pad_map;
    assert(apply_button_map(&pad,"cross=b,circle=a,touchpad=misc1",false)==3);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_EAST)==BTN_CROSS);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_SOUTH)==BTN_CIRCLE);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_MISC1)==CLICK_LEFT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_BACK)==CLICK_RIGHT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_TOUCHPAD)==BTN_TOUCHPAD);
    assert(apply_button_map(&pad,"touchpad_right=guide,touchpad_left=paddle1",false)==2);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_BACK)==0 && pressed_by(&pad,SDL_GAMEPAD_BUTTON_GUIDE)==CLICK_RIGHT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_MISC1)==0 && pressed_by(&pad,SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1)==CLICK_LEFT);
    assert(apply_button_map(&pad,"cross=Space",false)==0); /* a key name is no gamepad button */
    /* Keyboard: each side has a key of its own. */
    assert(pressed_by(&key_map,SDL_SCANCODE_TAB)==CLICK_LEFT && pressed_by(&key_map,SDL_SCANCODE_BACKSPACE)==CLICK_RIGHT);
    ButtonMap sides=key_map;
    assert(apply_button_map(&sides,"touchpad_right=Delete",true)==1);
    assert(pressed_by(&sides,SDL_SCANCODE_BACKSPACE)==0 && pressed_by(&sides,SDL_SCANCODE_DELETE)==CLICK_RIGHT);
    /* Comma and Equals stand for the two separator keys; Escape is an ordinary key. */
    ButtonMap marks=key_map;
    assert(apply_button_map(&marks,"cross=Comma,circle=equals,square=Escape",true)==3);
    assert(pressed_by(&marks,SDL_SCANCODE_COMMA)==BTN_CROSS && pressed_by(&marks,SDL_SCANCODE_EQUALS)==BTN_CIRCLE);
    assert(pressed_by(&marks,SDL_SCANCODE_ESCAPE)==BTN_SQUARE);

    /* Mouse names on the keyboard map (any case); they take the button from its default key. */
    ButtonMap mouse=key_map;
    assert(apply_button_map(&mouse,"square=Mouse Left,triangle=mouse right,circle=Mouse Middle,"
                                   "l1=Wheel Up,r1=wheel down,l2=Mouse Back,r2=Mouse Forward",true)==7);
    assert(pressed_by(&mouse,MOUSE_LEFT)==BTN_SQUARE && pressed_by(&mouse,SDL_SCANCODE_E)==0);
    assert(pressed_by(&mouse,MOUSE_RIGHT)==BTN_TRIANGLE && pressed_by(&mouse,MOUSE_MIDDLE)==BTN_CIRCLE);
    assert(pressed_by(&mouse,WHEEL_UP)==BTN_L1 && pressed_by(&mouse,WHEEL_DOWN)==BTN_R1);
    assert(pressed_by(&mouse,MOUSE_BACK)==BTN_L2 && pressed_by(&mouse,MOUSE_FORWARD)==BTN_R2);
    assert(apply_button_map(&pad_map,"cross=Mouse Left",false)==0); /* the gamepad map has no mouse */

    /* Sources: mouse masks, wheel pulses, and keys only inside the scancode range. */
    bool held[SDL_SCANCODE_COUNT]={0};
    held[SDL_SCANCODE_E]=true;
    const bool no_wheel[2]={false,false}, up[2]={true,false};
    assert(source_down(SDL_SCANCODE_E,held,0,no_wheel) && !source_down(SDL_SCANCODE_Q,held,0,no_wheel));
    assert(source_down(MOUSE_LEFT,held,SDL_BUTTON_LMASK,no_wheel));
    assert(!source_down(MOUSE_RIGHT,held,SDL_BUTTON_LMASK,no_wheel));
    assert(source_down(MOUSE_BACK,held,SDL_BUTTON_X1MASK,no_wheel));
    assert(source_down(MOUSE_FORWARD,held,SDL_BUTTON_X2MASK,no_wheel));
    assert(source_down(WHEEL_UP,held,0,up) && !source_down(WHEEL_DOWN,held,0,up));

    /* Two notches up: press, gap, press, then idle; the other direction stays up. */
    SDL_SetAtomicInt(&wheel_ticks[0],2);
    const uint64_t t0=1000;
    assert(wheel_held(0,t0) && wheel_held(0,t0+WHEEL_PRESS_US-1));
    assert(!wheel_held(0,t0+WHEEL_PRESS_US));
    const uint64_t t1=t0+WHEEL_PRESS_US+WHEEL_GAP_US;
    assert(wheel_held(0,t1) && !wheel_held(0,t1+WHEEL_PRESS_US));
    assert(!wheel_held(0,t1+WHEEL_PRESS_US+WHEEL_GAP_US));
    assert(!wheel_held(1,t1));
    /* Notches beyond the limit are dropped, and the menu drops the rest. */
    SDL_Event spin={0};
    spin.type=SDL_EVENT_MOUSE_WHEEL; spin.wheel.integer_y=-10;
    mouse_watch(NULL,&spin);
    assert(SDL_GetAtomicInt(&wheel_ticks[1])==WHEEL_MAX_PENDING);
    drop_wheel();
    assert(SDL_GetAtomicInt(&wheel_ticks[1])==0 && !wheel_held(1,t1));
    /* Rage: one source for L3 + R3; "None" clears L3 and R3 so their keys press nothing. */
    ButtonMap rage=key_map;
    assert(apply_button_map(&rage,"l3=None,r3=none,rage=B",true)==3);
    assert(pressed_by(&rage,SDL_SCANCODE_Z)==0 && pressed_by(&rage,SDL_SCANCODE_C)==0);
    assert(pressed_by(&rage,SDL_SCANCODE_B)==RAGE);
    PadData sparta={0};
    sparta.buttons=RAGE|BTN_SQUARE;
    apply_combos(&sparta);
    assert(sparta.buttons==(BTN_L3|BTN_R3|BTN_SQUARE));
    sparta.buttons=BTN_L3;
    apply_combos(&sparta);
    assert(sparta.buttons==BTN_L3);

    /* The pause key tells the window once per press; L1 + touchpad right (photo mode) does not. */
    PadData menu={0};
    menu.buttons=CLICK_RIGHT; note_pause_key(&menu); note_pause_key(&menu);
    assert(pause_keys==1);
    menu.buttons=0; note_pause_key(&menu);
    menu.buttons=CLICK_RIGHT|BTN_L1; note_pause_key(&menu);
    assert(pause_keys==1);
    menu.buttons=0; note_pause_key(&menu);
    menu.buttons=CLICK_RIGHT; note_pause_key(&menu);
    assert(pause_keys==2);

    /* L2 aim: the right stick follows the mouse's movement while L2 is held, then lets go. */
    SDL_Event glide={0};
    glide.type=SDL_EVENT_MOUSE_MOTION;
    PadData item={0};
    item.right_x=item.right_y=128; item.timestamp=50000000;
    glide.motion.xrel=500.0f; mouse_watch(NULL,&glide);
    apply_mouse_aim(&item);                                   /* no L2: untouched, motion dropped */
    assert(item.right_x==128 && item.right_y==128);
    item.buttons=BTN_L2; item.timestamp+=16000;
    glide.motion.xrel=10.0f; glide.motion.yrel=-5.0f; mouse_watch(NULL,&glide);
    apply_mouse_aim(&item);                                   /* right and up, partly tilted */
    assert(item.right_x>128 && item.right_x<255 && item.right_y<128 && item.right_y>0);
    const uint8_t tilt=item.right_x;
    item.right_x=item.right_y=128; item.timestamp+=16000;     /* a gap between mouse reports */
    apply_mouse_aim(&item);
    assert(item.right_x==tilt);
    item.right_x=item.right_y=128; item.timestamp+=AIM_HOLD_US; /* stopped: the stick is let go */
    apply_mouse_aim(&item);
    assert(item.right_x==128 && item.right_y==128);
    glide.motion.xrel=-400.0f; glide.motion.yrel=0; mouse_watch(NULL,&glide);
    item.timestamp+=16000;
    apply_mouse_aim(&item);                                   /* fast to the left: full tilt */
    assert(item.right_x==0 && item.right_y==128);

    /* Absolute: the offset from where the mouse was at the L2 press, kept while the mouse rests. */
    aim_style=1;
    item.buttons=0; item.timestamp+=16000;
    apply_mouse_aim(&item);                                   /* L2 released: forget the centre */
    item.buttons=BTN_L2; item.right_x=item.right_y=128;
    glide.motion.xrel=100.0f; glide.motion.yrel=0; mouse_watch(NULL,&glide);
    item.timestamp+=16000;
    apply_mouse_aim(&item);                                   /* half way to full tilt */
    assert(item.right_x==128+63 && item.right_y==128);
    item.right_x=item.right_y=128; item.timestamp+=AIM_HOLD_US*4;
    apply_mouse_aim(&item);                                   /* resting: still pointing there */
    assert(item.right_x==128+63);
    glide.motion.xrel=1000.0f; mouse_watch(NULL,&glide);
    apply_mouse_aim(&item);                                   /* far past the edge: full tilt */
    assert(item.right_x==255);
    item.right_x=128;
    glide.motion.xrel=-200.0f; mouse_watch(NULL,&glide);
    apply_mouse_aim(&item);                                   /* back by the radius: centred, let go */
    assert(item.right_x==128 && item.right_y==128);
    aim_sensitivity=50;                                       /* half: twice the mouse for the same stick */
    glide.motion.xrel=100.0f; mouse_watch(NULL,&glide);
    apply_mouse_aim(&item);
    assert(item.right_x==128+31);
    aim_sensitivity=100;
    aim_style=0;

    /* Grab: L1 + Circle from one source, alongside rage. */
    ButtonMap grab=key_map;
    assert(apply_button_map(&grab,"r3=X,grab=C,rage=V",true)==3); /* C was R3's default key */
    assert(pressed_by(&grab,SDL_SCANCODE_C)==GRAB && pressed_by(&grab,SDL_SCANCODE_X)==BTN_R3);
    sparta.buttons=GRAB|RAGE;
    apply_combos(&sparta);
    assert(sparta.buttons==(BTN_L1|BTN_CIRCLE|BTN_L3|BTN_R3));

    /* Dodge: a host-only source; the right stick follows the left one, or goes back when still. */
    ButtonMap evade=key_map;
    assert(apply_button_map(&evade,"dodge=Left Alt",true)==1 && pressed_by(&evade,SDL_SCANCODE_LALT)==DODGE);
    PadData p={0};
    p.left_x=p.left_y=p.right_x=p.right_y=128; p.timestamp=1000000;
    p.buttons=DODGE|BTN_CROSS; p.left_x=0;                      /* A held: dodge left */
    apply_dodge(&p);
    assert(p.right_x==0 && p.right_y==128 && p.buttons==BTN_CROSS);
    p.buttons=DODGE; p.left_x=255; p.left_y=0; p.timestamp+=10000; /* W+D: up and right */
    apply_dodge(&p);
    assert(p.right_x==255 && p.right_y==0);
    p.buttons=0; p.right_x=p.right_y=128; p.timestamp+=10000;  /* released early: held to the minimum */
    apply_dodge(&p);
    assert(p.right_x==255 && p.right_y==0);
    p.right_x=p.right_y=128; p.timestamp=1000000+DODGE_MIN_US;
    apply_dodge(&p);
    assert(p.right_x==128 && p.right_y==128);
    p.buttons=DODGE; p.left_x=p.left_y=140; p.timestamp+=1000000; /* standing still: back */
    apply_dodge(&p);
    assert(p.right_x==128 && p.right_y==255);
    reset_dodge();
    p.buttons=0; p.right_x=p.right_y=128; p.timestamp+=1000;
    apply_dodge(&p);
    assert(p.right_x==128 && p.right_y==128);
    /* Spin: the left stick turns at full tilt from where it points, never through the centre.
     * Checked the way games read a turn (God of War III's rotation prompts): four directions, a
     * direction once a component reaches 0.6, and a sequence that breaks on any other direction,
     * the centre included. Up180CW is left, up, right; 360CCW from up is up, left, down, right. */
    assert(pressed_by(&key_map,SDL_SCANCODE_X)==SPIN_CW && pressed_by(&key_map,SDL_SCANCODE_V)==SPIN_CCW);
    static const struct { int dir, start_x, start_y; int sequence[5]; } turns[]={
        {SPIN_CW,0,128,{4,3,2}},          /* A held, X: Up180CW */
        {SPIN_CW,255,128,{2,1,4}},        /* D held, X: Down180CW */
        {SPIN_CCW,255,128,{2,3,4}},       /* D held, V: Up180CCW */
        {SPIN_CCW,128,128,{3,4,1,2,3}},   /* nothing held, V: a full turn from up */
    };
    const uint64_t rates[]={16667,33333,50000};  /* the game reading the pad at 60, 30, 20 Hz */
    for (size_t t=0;t<sizeof(turns)/sizeof(*turns);++t)
        for (size_t r=0;r<sizeof(rates)/sizeof(*rates);++r) {
            reset_spin();
            unsigned step=0; int last=-1; bool broken=false;
            const unsigned length=turns[t].sequence[4] ? 5 : 3;
            for (uint64_t now=1000000; step<length && now<1000000+2*SPIN_TURN_US; now+=rates[r]) {
                PadData q={.buttons=(uint32_t)turns[t].dir,.left_x=(uint8_t)turns[t].start_x,
                           .left_y=(uint8_t)turns[t].start_y,.timestamp=now};
                apply_spin(&q);
                assert(!(q.buttons & (SPIN_CW|SPIN_CCW)));
                const float x=(q.left_x-128)/127.5f, y=(q.left_y-128)/127.5f;
                assert(x*x+y*y>0.95f);
                const int sector=fabsf(x)<0.6f && fabsf(y)<0.6f ? 0
                                 : fabsf(y)>fabsf(x) ? (y>0 ? 1 : 3) : (x>0 ? 2 : 4);
                if (sector==last) continue;
                last=sector;
                if (sector==turns[t].sequence[step]) ++step;
                else broken=true;
            }
            assert(step==length && !broken);
        }
    /* Released, both held, or the menu open: the stick is the player's again. */
    PadData q={.buttons=SPIN_CW|SPIN_CCW,.left_x=128,.left_y=128,.timestamp=5000000};
    apply_spin(&q);
    assert(q.left_x==128 && q.left_y==128 && !q.buttons);
    q.buttons=0; apply_spin(&q);
    assert(q.left_x==128 && q.left_y==128);

    puts("PASS: pad and key maps");
}
