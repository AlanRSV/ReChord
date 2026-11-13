/*
 * sim.h -- what the simulator's parts share (sim_main.c, sim_hw.c, sim_storage.c, sim_audio.c).
 *
 * Two threads: the firmware thread runs the firmware's own loop (ap_ui_loop) exactly as on the
 * device; the main thread runs the SDL window, reads the keyboard into the key state below, and
 * shows the simulated panel. The 10 ms tick the device takes from SysTick (keys, audio_tick) runs
 * on the firmware thread whenever it asks for the time or waits (sim_tick_maybe), like an
 * interrupt between two statements.
 */
#ifndef RECHORD_SIM_H
#define RECHORD_SIM_H
#include <stdint.h>

/* The key ladder: the ADC value of the key held (input.c's bands), 1023 = none. */
#define SIM_ADC_IDLE   1023
#define SIM_ADC_VOLDN  30
#define SIM_ADC_VOLUP  130
#define SIM_ADC_PLAY   270
#define SIM_ADC_MENU   360
#define SIM_ADC_BACK   500

extern volatile int g_sim_adc;        /* written by the window thread */
extern volatile int g_sim_power;      /* power key held */
extern volatile int g_sim_backlight;  /* 0 = off, else the duty the firmware set */
extern volatile int g_sim_off;        /* the firmware powered off (the window closes) */
extern volatile int g_sim_quit;       /* the window closed: the firmware thread stops at its next tick */

uint32_t sim_now_ms(void);
uint64_t sim_now_us(void);
void sim_sleep_us(uint64_t us);
void sim_tick_maybe(void);            /* the 10 ms tick, if one is due */
void sim_log(const char *fmt, ...);   /* to stderr, prefixed */

/* sim_storage.c */
int sim_storage_open(const char *internal_img, const char *sd_img);
void sim_storage_flush(void);

/* sim_audio.c */
void sim_audio_tick(void);            /* from the 10 ms tick (as audio_tick) */
void sim_audio_shutdown(void);
void sim_audio_gain_db(int db);       /* extra gain for PC speakers (default +20) */

#endif
