/*
 * sim_hw.c -- the hardware under the firmware, for the desktop simulator.
 *
 * Real behaviour where the UI can see it: the clock, the keys (from the window's keyboard), the
 * 10 ms tick, the backlight, the screen-off timer and power-off. Everything the simulator has no
 * hardware for (USB, the DAC's registers, the Bluetooth module, the firmware slots, the watchdog)
 * answers the way the device does when that part is idle or absent: no cable, no module, nothing to
 * update. Storage is sim_storage.c, audio sim_audio.c, overlays sim_ovl.c.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sim.h"

#include "drivers/board.h"
#include "drivers/clock.h"
#include "drivers/panel_io.h"
#include "drivers/dac_cs43131.h"
#include "drivers/bt_jieli.h"
#include "drivers/usb_msc.h"
#include "drivers/usb_cdc.h"
#include "drivers/usb_out.h"
#include "drivers/usb_probe.h"
#include "drivers/usb_uac.h"
#include "drivers/text.h"

#include "services/input.h"
#include "services/power.h"
#include "services/config.h"
#include "services/bootcrumb.h"
#include "services/bootguard.h"
#include "services/watchdog.h"
#include "services/cpuclk.h"
#include "services/fw_update.h"
#include "services/log.h"
#include "ui/nav.h"

volatile int g_sim_adc = SIM_ADC_IDLE;
volatile int g_sim_power;
volatile int g_sim_backlight = 1;
volatile int g_sim_off;
volatile int g_sim_quit;

void sim_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fputs("[sim] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---- time ------------------------------------------------------------------------------------ */

static struct timespec s_t0;
static pthread_once_t s_t0_once = PTHREAD_ONCE_INIT;

static void t0_init(void) { clock_gettime(CLOCK_MONOTONIC, &s_t0); }

uint64_t sim_now_us(void)
{
    struct timespec t;

    pthread_once(&s_t0_once, t0_init);
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)(t.tv_sec - s_t0.tv_sec) * 1000000u + (uint64_t)((t.tv_nsec - s_t0.tv_nsec) / 1000);
}

uint32_t sim_now_ms(void) { return (uint32_t)(sim_now_us() / 1000u); }

void sim_sleep_us(uint64_t us)
{
    struct timespec ts = { (time_t)(us / 1000000u), (long)(us % 1000000u) * 1000L };

    while (nanosleep(&ts, &ts) && errno == EINTR) { }
}

/* The firmware's clock (drivers/clock.h). Asking for the time is also where the tick runs. */
uint32_t time_ms(void) { sim_tick_maybe(); return sim_now_ms(); }
uint64_t time_us(void) { sim_tick_maybe(); return sim_now_us(); }
uint32_t panel_io_now_us(void) { return (uint32_t)sim_now_us(); }
uint32_t panel_io_now_ms(void) { return sim_now_ms(); }
uint32_t clock_core_hz(void) { return 96000000u; }
uint32_t clock_core_b_hz(void) { return 0; }
unsigned cpu_load_permille(void) { return 0; }
void cpuclk_boot(void) { }

void delay_ms(uint32_t ms)
{
    uint32_t end = sim_now_ms() + ms;

    while ((int32_t)(end - sim_now_ms()) > 0) {
        sim_sleep_us(1000);
        sim_tick_maybe();
    }
}

void panel_io_delay_ms(uint16_t ms) { delay_ms(ms); }

/* The main loop's WFI until the next 10 ms tick. */
void cpu_wait_tick(void)
{
    uint64_t now = sim_now_us(), next = (now / 10000u + 1u) * 10000u;

    sim_sleep_us(next - now);
    sim_tick_maybe();
    if (g_sim_quit) {                  /* the window closed: as a power-off, the log on the disk */
        log_flush("sim quit");
        g_sim_off = 1;
        pthread_exit(NULL);
    }
}

/* ---- the 10 ms tick (ap_tick.c on the device: keys_tick + audio_tick) ------------------------ */

static int s_keys_isr;                 /* keys_isr_start() ran */
static int s_key_now = SIM_ADC_IDLE;   /* the ladder's stable reading */
static uint32_t s_press_ms, s_release_ms;

void keys_tick(void)
{
    int adc = g_sim_adc;
    uint32_t now = sim_now_ms();

    if (!s_keys_isr)
        return;
    if (adc != s_key_now) {            /* an edge: when the voltage left idle / came back */
        if (s_key_now == SIM_ADC_IDLE)
            s_press_ms = now | 1u;
        else if (adc == SIM_ADC_IDLE)
            s_release_ms = now | 1u;
        s_key_now = adc;
    }
    input_sample_edges(s_key_now, g_sim_power, now, s_press_ms, s_release_ms);
}

void sim_tick_maybe(void)
{
    static uint64_t s_next;
    static int s_in;
    uint64_t now;

    if (s_in)
        return;                        /* (the tick asks for the time too) */
    now = sim_now_us();
    if (now < s_next)
        return;
    s_in = 1;
    s_next = (now / 10000u + 1u) * 10000u;
    keys_tick();
    sim_audio_tick();
    s_in = 0;
}

void keys_isr_start(void) { s_keys_isr = 1; input_isr_sampling(1); }
int keys_adc(void) { return s_keys_isr ? s_key_now : g_sim_adc; }
int keys_power(void) { return g_sim_power; }
uint32_t keys_release_stats(void) { return 0; }
int adc_value(unsigned ch) { return ch == 1 ? keys_adc() : SIM_ADC_IDLE; }
const uint32_t *adc_diag(void) { static uint32_t d[ADC_DIAG_N]; return d; }
int board_adc_ch2(void) { return SIM_ADC_IDLE; }

/* ---- panel, backlight, power ----------------------------------------------------------------- */

void backlight_on(void) { g_sim_backlight = 1; }
void backlight_set(int duty) { g_sim_backlight = duty; }
int panel_io_select(int path) { return 0; }
void panel_io_poll(void) { }
void panel_te_probe(void) { log_printf("sim: no panel TE line"); }

static int s_screen = 1;
static uint32_t s_last_key_ms;

void power_init(void) { s_last_key_ms = sim_now_ms(); power_brightness_changed(); }
void power_activity(void) { s_last_key_ms = sim_now_ms(); }
uint32_t power_key_idle_ms(void) { return sim_now_ms() - s_last_key_ms; }
int power_screen_on(void) { return s_screen; }
void power_usb_supply_off(int off) { }

void power_screen(int on)
{
    if (on == s_screen)
        return;
    s_screen = on;
    g_sim_backlight = on ? (g_cfg.brightness ? g_cfg.brightness : 8) : 0;
    log_printf("power: screen %s", on ? "on" : "off");
}

void power_brightness_changed(void)
{
    if (s_screen)
        g_sim_backlight = g_cfg.brightness ? g_cfg.brightness : 8;
}

/* The screen-off timer as on the device. Auto power-off is left out: the window would close. */
int power_poll(int playing, int usb_session)
{
    uint32_t off_s = config_screen_off_s();

    if (s_screen && off_s && power_key_idle_ms() >= off_s * 1000u)
        power_screen(0);
    return POWER_NONE;
}

const power_battery_t *power_battery(void)
{
    static const power_battery_t b = { .mv = 4100, .pct10 = 1000, .level = 4 };

    return &b;
}

const char *power_battery_text(void) { return "100 % (simulated)"; }

void board_power_off(void)
{
    log_printf("sim: powered off");
    sim_log("the firmware powered off");
    g_sim_off = 1;
    pthread_exit(NULL);
}

void board_reboot_rockusb(void)
{
    sim_log("the firmware asked for the USB loader (rockusb): not simulated, closing");
    g_sim_off = 1;
    pthread_exit(NULL);
}

int board_check(uint32_t emmc_sectors) { return 1; }
const char *board_name(void) { return BOARD == BOARD_8G ? "8 GB (sim)" : "512 MB (sim)"; }
int board_usb_supply(int mode) { return 0; }

/* ---- boot trail, crash record, watchdog: nothing to keep between runs ------------------------ */

void bootcrumb(uint32_t step) { }
void bootcrumb_kv(uint32_t key, uint32_t val) { }
void bootcrumb_set(uint32_t key, uint32_t val) { }
int bootguard_crashed(bootguard_rec_t *rec) { return 0; }
void bootguard_clear(void) { }
void bootguard_poll(void) { }
void watchdog_feed(void) { }
void watchdog_start(void) { }

/* ---- firmware slots: no update image, nothing to repair -------------------------------------- */

int fw_update_find(uint32_t vol_lba, fw_image_t *img, fwu_progress_t progress) { return FWU_NONE; }
int fw_update_install(const fw_image_t *img, fwu_progress_t progress) { return FWU_EWRITE; }
int fw_update_repair(fwu_progress_t progress) { return 0; }
int fw_update_retire(const fw_image_t *img) { return 0; }

/* As services/fw_update.c (the library's index CRC uses it). */
uint32_t fwu_crc(uint32_t crc, const uint8_t *p, uint32_t n)
{
    static uint32_t tab[256];

    if (!tab[1]) {
        for (uint32_t i = 0; i < 256u; i++) {
            uint32_t r = i << 24;
            for (int k = 0; k < 8; k++)
                r = r & 0x80000000u ? r << 1 ^ 0x04C10DB7u : r << 1;
            tab[i] = r;
        }
    }
    while (n--)
        crc = crc << 8 ^ tab[(crc >> 24 ^ *p++) & 0xFFu];
    return crc;
}

/* ---- fonts: the blob linked in (on the device: the fonts module from the firmware slot) ------- */

extern const uint32_t rechord_fonts_blob[];
extern const uint32_t rechord_fonts_len;

int text_fonts_load(void) { return text_fonts_init(rechord_fonts_blob, rechord_fonts_len); }

/* ---- the DAC: playback goes through sim_audio.c; the register tools see a quiet chip --------- */

int dac_init(uint32_t rate_hz) { return 0; }
void dac_output_off(void) { }
void dac_i2s_start(void) { }
void dac_i2s_stop(void) { }
int dac_i2s_feed(const int16_t *pcm, int frames) { return frames; }
void dac_i2s_fifo_range(unsigned *lo, unsigned *hi) { *lo = *hi = 0; }
int dac_last_step(void) { return 0; }
int dac_scan_result(int cand, int chip) { return 0; }
int dac_read_reg(int chan, uint32_t map24, uint8_t *out) { *out = 0; return -1; }
int dac_write_reg(int chan, uint32_t map24, uint8_t val) { return -1; }
void dac_get_hp_status(uint8_t out[2]) { out[0] = out[1] = 0; }
void hifi_probe_test(void) { log_printf("sim: no HIFI accelerator"); }
void sd_selftest(void) { log_printf("sim: SD self-test not simulated"); }

/* ---- Bluetooth: no module ---------------------------------------------------------------------- */

void bt_app_init(void) { }
void bt_app_tick(void) { }
void bt_app_external(int streaming) { }
bt_state_t bt_state(void) { return BT_OFF; }
const char *bt_state_text(bt_state_t st) { return "Off"; }
const bt_dev_t *bt_peer(void) { return NULL; }
int bt_routed(void) { return 0; }
int bt_listen(bt_listen_t *r, uint32_t *buf, unsigned words, uint32_t ms) { return -1; }

static int bt_count(void) { return 1; }

static void bt_row(int i, row_t *r)
{
    snprintf(r->text, sizeof r->text, "Bluetooth isn't simulated");
    r->flags = R_OFF;
}

const page_t pg_bt = {
    .name = "bluetooth", .kind = PK_LIST, .icon = ICON_BT, .keys = k_list, .title = "Bluetooth",
    .count = bt_count, .row = bt_row,
};

/* ---- USB: no cable (the disk, DAC and USB-C out sessions need one) ---------------------------- */

void (*volatile usb_irq_hook)(void);
uint32_t usb_msc_diag;
uint32_t usb_probe_phy[15];
const char *const usb_detect_names[USB_DETECT_STATES] = { 0 };

int usb_cable(void) { return 0; }
int usb_cable_gone(void) { return 1; }
void usb_cable_state(uint32_t w[3]) { w[0] = w[1] = w[2] = 0; }
void usb_detect_state(int n) { }
void usb_dwc2_idle_tick(void) { }
void usb_dwc2_park(void) { }
void usb_dwc2_stop(void) { }
void usb_regs_read(uint32_t r[10]) { memset(r, 0, 10 * sizeof r[0]); }
int usb_msc_run(const usb_lun_t *luns, unsigned nluns, int (*user_exit)(void),
                uint32_t *rd_sectors, uint32_t *wr_sectors) { return -1; }
const struct usb_cdc_diag_t *usb_cdc_diag(void) { static struct usb_cdc_diag_t d; return &d; }
int usb_cdc_getc(void) { return -1; }
void usb_cdc_init(void) { }
void usb_cdc_poll(void) { }
int usb_cdc_puts(const char *str) { return 0; }
void usb_cdc_request_exit(void) { }
int usb_probe_run(usb_probe_step_t *out, void (*progress)(int step, const char *name)) { return -1; }
int usb_uac_run(int (*tick)(void), void (*status)(const usb_uac_status_t *)) { return -1; }
void usb_uac_key(int key) { }
void usb_uac_self_powered(int on) { }
int usb_uac_streaming(void) { return 0; }

static usb_out_status_t s_uout;
int usb_out_active(void) { return 0; }
int usb_out_poll(void) { return 0; }
int usb_out_start(int supply, uint32_t budget_ma) { return -1; }
const usb_out_status_t *usb_out_status(void) { return &s_uout; }
void usb_out_stop(void) { }

/* The overlay gates (startup/ap_ovl.c) call the real functions by these names (the device's link
 * uses --wrap); the simulator links them straight to the stubs above. */
int __real_usb_out_active(void) { return 0; }
void __real_usb_out_pause(int on) { }
int __real_usb_out_poll(void) { return 0; }
void __real_usb_out_release(void) { }
int __real_usb_out_set_format(uint32_t rate, uint32_t bits) { return -1; }
int __real_usb_out_set_volume(int db) { return 0; }
int __real_usb_out_start(int supply, uint32_t budget_ma) { return -1; }
const usb_out_status_t *__real_usb_out_status(void) { return &s_uout; }
void __real_usb_out_stop(void) { }
void __real_usb_out_stream_start(void) { }
void __real_usb_out_stream_stop(void) { }
int __real_usb_out_streaming(void) { return 0; }
void __real_usb_uac_key(int key) { }
