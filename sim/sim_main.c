/*
 * sim_main.c -- ReChord's firmware UI in a desktop window.
 *
 * The firmware runs as on the device (startup/ap_main.c's Main: log, display, ap_ui_loop) on its
 * own thread; this thread owns the SDL window, turns the keyboard into the key ladder's voltage and
 * the power key, and shows the panel's frame buffer (drivers/panel_io_sim.c) 60 times a second.
 *
 *   rechord_sim [options] internal.img
 *     --sd FILE        a microSD card image (FAT32/exFAT, as sim/mkimage.sh makes)
 *     --scale N        window scale (default 3)
 *     --script FILE    run keys from a file, then quit (see sim/README.md); with --headless no window
 *     --headless       no window (SDL's dummy video driver)
 *     --gain DB        extra output gain over the firmware's volume (default 20: -40 dB on the
 *                      device's scale is a headphone level)
 *
 * Keys: Up = VOL+, Down = VOL-, Enter/Space/Right = PLAY, Esc/Backspace/Left = BACK, M/Tab = MENU,
 *       P = POWER, F12 = screenshot (BMP next to the image), Q with Ctrl = quit.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL.h>

#include "sim.h"
#include "drivers/display.h"
#include "drivers/panel_io_sim.h"
#include "services/config.h"
#include "services/log.h"

void ap_ui_loop(void);

#define W PANEL_SIM_W
#define H PANEL_SIM_H

/* ---- the firmware thread ---------------------------------------------------------------------- */

static unsigned long log_clock(void) { return time_ms(); }

static void *firmware(void *u)
{
    log_init(log_clock);
    display_set_logger(log_printf);
    display_init();
    ap_ui_loop();
    g_sim_off = 1;
    return NULL;
}

/* ---- keys: a stack of held keys, the ladder shows the newest (one voltage at a time) --------- */

enum { K_UP, K_DOWN, K_PLAY, K_BACK, K_MENU, K_N };
static const int k_adc[K_N] = { SIM_ADC_VOLUP, SIM_ADC_VOLDN, SIM_ADC_PLAY, SIM_ADC_BACK, SIM_ADC_MENU };
static int s_held[K_N + 1], s_nheld;

static void ladder_update(void) { g_sim_adc = s_nheld ? k_adc[s_held[s_nheld - 1]] : SIM_ADC_IDLE; }

static void key_down(int k)
{
    for (int i = 0; i < s_nheld; i++)
        if (s_held[i] == k)
            return;
    s_held[s_nheld++] = k;
    ladder_update();
}

static void key_up(int k)
{
    for (int i = 0; i < s_nheld; i++)
        if (s_held[i] == k) {
            memmove(&s_held[i], &s_held[i + 1], (size_t)(--s_nheld - i) * sizeof s_held[0]);
            break;
        }
    ladder_update();
}

static int key_of_name(const char *n)
{
    static const char *const names[K_N] = { "UP", "DOWN", "PLAY", "BACK", "MENU" };

    for (int i = 0; i < K_N; i++)
        if (!strcasecmp(n, names[i]))
            return i;
    if (!strcasecmp(n, "VOL+")) return K_UP;
    if (!strcasecmp(n, "VOL-")) return K_DOWN;
    if (!strcasecmp(n, "SELECT")) return K_PLAY;
    if (!strcasecmp(n, "POWER")) return K_N;
    return -1;
}

static int key_of_sdl(SDL_Keycode k)
{
    switch (k) {
    case SDLK_UP: return K_UP;
    case SDLK_DOWN: return K_DOWN;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: case SDLK_RIGHT: return K_PLAY;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_LEFT: return K_BACK;
    case SDLK_m: case SDLK_TAB: return K_MENU;
    case SDLK_p: return K_N;           /* power */
    default: return -1;
    }
}

static void press(int k, int down)
{
    if (k == K_N)
        g_sim_power = down;
    else if (down)
        key_down(k);
    else
        key_up(k);
}

/* ---- the picture -------------------------------------------------------------------------------- */

static uint16_t s_frame[H][W];

static void grab(void)
{
    int lit = g_sim_backlight != 0;

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            s_frame[y][x] = lit ? panel_io_sim_pixel(x, y) : 0;
}

/* The frame as a 24-bit BMP (or, for a .ppm name, a PPM), at scale 1. */
static int save_shot(const char *path)
{
    SDL_Surface *s;
    int r;

    grab();
    s = SDL_CreateRGBSurfaceWithFormatFrom(s_frame, W, H, 16, W * 2, SDL_PIXELFORMAT_RGB565);
    if (!s)
        return -1;
    r = SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
    if (r == 0)
        sim_log("screenshot: %s", path);
    return r;
}

/* ---- scripts: one command a line ---------------------------------------------------------------
 *   wait MS            let the firmware run
 *   press KEY [MS]     down, held MS (default 80), up, then 150 ms
 *   hold KEY MS        the same, for a long press
 *   down KEY / up KEY
 *   shot FILE.bmp      the screen now
 *   quit
 * KEY: UP DOWN PLAY BACK MENU POWER (also VOL+ VOL- SELECT). # starts a comment. */

static FILE *s_script;
static uint32_t s_script_wait;         /* sim_now_ms() when the next line may run */
static int s_quit;

static void script_step(void)
{
    char line[512], cmd[32], arg[400];
    int ms, n;

    while (s_script && sim_now_ms() >= s_script_wait) {
        if (!fgets(line, sizeof line, s_script)) {
            s_quit = 1;
            return;
        }
        line[strcspn(line, "#\r\n")] = 0;
        ms = 0;
        n = sscanf(line, "%31s %399s %d", cmd, arg, &ms);
        if (n < 1)
            continue;
        if (!strcmp(cmd, "wait") && n >= 2) {
            s_script_wait = sim_now_ms() + (uint32_t)atoi(arg);
        } else if ((!strcmp(cmd, "press") || !strcmp(cmd, "hold")) && n >= 2) {
            int k = key_of_name(arg);

            if (k < 0) {
                sim_log("script: unknown key %s", arg);
                continue;
            }
            press(k, 1);
            SDL_Delay((Uint32)(n >= 3 ? ms : 80));
            press(k, 0);
            s_script_wait = sim_now_ms() + 150u;
        } else if ((!strcmp(cmd, "down") || !strcmp(cmd, "up")) && n >= 2) {
            int k = key_of_name(arg);

            if (k >= 0)
                press(k, cmd[0] == 'd');
        } else if (!strcmp(cmd, "shot") && n >= 2) {
            save_shot(arg);
        } else if (!strcmp(cmd, "quit")) {
            s_quit = 1;
            return;
        } else {
            sim_log("script: ? %s", line);
        }
    }
}

/* ---- main --------------------------------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr, "usage: rechord_sim [--sd sd.img] [--scale N] [--script FILE] [--headless] [--gain DB] internal.img\n"
                    "  make an image from a music folder: sim/mkimage.sh ~/Music internal.img\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *img = NULL, *sd = NULL, *script = NULL;
    int scale = 3, headless = 0, shots = 0;
    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL;
    pthread_t fw;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sd") && i + 1 < argc) sd = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--gain") && i + 1 < argc) sim_audio_gain_db(atoi(argv[++i]));
        else if (argv[i][0] == '-') usage();
        else img = argv[i];
    }
    if (!img)
        usage();
    if (scale < 1 || scale > 8)
        scale = 3;
    signal(SIGPIPE, SIG_IGN);
    if (headless)
        SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    if (script && !(s_script = fopen(script, "r"))) {
        perror(script);
        return 1;
    }
    if (sim_storage_open(img, sd) != 0)
        return 1;
    if (!headless) {
        win = SDL_CreateWindow("ReChord simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               W * scale, H * scale, SDL_WINDOW_RESIZABLE);
        if (!win) {
            fprintf(stderr, "SDL window: %s\n", SDL_GetError());
            return 1;
        }
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
        if (!ren)
            ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        SDL_RenderSetLogicalSize(ren, W, H);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, W, H);
    }

    pthread_create(&fw, NULL, firmware, NULL);

    while (!s_quit && !g_sim_off) {
        SDL_Event e;

        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                s_quit = 1;
            } else if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
                int down = e.type == SDL_KEYDOWN;
                SDL_Keycode kc = e.key.keysym.sym;

                if (down && kc == SDLK_q && (e.key.keysym.mod & KMOD_CTRL)) {
                    s_quit = 1;
                } else if (down && kc == SDLK_F12) {
                    char path[1024];

                    snprintf(path, sizeof path, "%s.shot%02d.bmp", img, shots++);
                    save_shot(path);
                } else {
                    int k = key_of_sdl(kc);

                    if (k >= 0)
                        press(k, down);
                }
            }
        }
        script_step();
        if (ren) {
            grab();
            SDL_UpdateTexture(tex, NULL, s_frame, W * 2);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
        }
        SDL_Delay(16);
    }

    /* The firmware thread stops at its next tick (flushing its log, as a power-off does); a thread
     * stuck in a long job is left behind after 2 s (its writes are synchronous: the image holds). */
    g_sim_quit = 1;
    for (int i = 0; i < 200 && !g_sim_off; i++)
        SDL_Delay(10);
    sim_audio_shutdown();
    sim_storage_flush();
    SDL_Quit();
    _exit(0);
}
