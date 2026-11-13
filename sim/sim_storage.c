/*
 * sim_storage.c -- the internal storage and the microSD card, backed by disk-image files.
 *
 * The internal image is what the device's user volume looks like from LBA 2048 on (the firmware
 * area in front of it is not simulated: emmc_layout() answers 2048 and the image's first 1 MiB is
 * padding). sim/mkimage.sh makes one from a folder. Writes go straight to the file, so the
 * library index, RECHORD.CFG and playlists the firmware writes are there on the next run.
 * The SD image is optional; without one the card slot is empty.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sim.h"
#include "drivers/emmc.h"

#define SIM_USER_START 2048u

typedef struct {
    int fd;
    uint32_t sectors;
    uint32_t written;
} disk_t;

static disk_t s_emmc = { -1 }, s_sd = { -1 };
static emmc_wq_stats_t s_stats;

static int disk_open(disk_t *d, const char *path)
{
    struct stat st;

    d->fd = open(path, O_RDWR);
    if (d->fd < 0) {
        perror(path);
        return -1;
    }
    fstat(d->fd, &st);
    d->sectors = (uint32_t)(st.st_size / 512);
    return 0;
}

int sim_storage_open(const char *internal_img, const char *sd_img)
{
    if (disk_open(&s_emmc, internal_img))
        return -1;
    if (s_emmc.sectors <= SIM_USER_START) {
        sim_log("%s: too small for a volume at LBA %u (make one with sim/mkimage.sh)", internal_img,
                SIM_USER_START);
        return -1;
    }
    if (sd_img && disk_open(&s_sd, sd_img))
        return -1;
    return 0;
}

void sim_storage_flush(void)
{
    if (s_emmc.fd >= 0)
        fsync(s_emmc.fd);
    if (s_sd.fd >= 0)
        fsync(s_sd.fd);
}

static int disk_read(disk_t *d, uint32_t lba, uint32_t count, void *buf)
{
    if (d->fd < 0 || lba >= d->sectors || count > d->sectors - lba)
        return -1;
    return pread(d->fd, buf, (size_t)count * 512, (off_t)lba * 512) == (ssize_t)count * 512 ? 0 : -1;
}

static int disk_write(disk_t *d, uint32_t lba, uint32_t count, const void *buf)
{
    if (d->fd < 0 || lba >= d->sectors || count > d->sectors - lba)
        return -1;
    if (pwrite(d->fd, buf, (size_t)count * 512, (off_t)lba * 512) != (ssize_t)count * 512)
        return -1;
    d->written += count;
    s_stats.queued++;
    s_stats.written++;
    return 0;
}

/* ---- internal storage (drivers/emmc.h): writes are done when they return, the queue is empty -- */

int emmc_init(void) { return s_emmc.fd >= 0 ? 0 : -1; }
const char *emmc_kind(void) { return "image file"; }
int emmc_width(void) { return 8; }
uint32_t emmc_sectors(void) { return s_emmc.sectors; }
int emmc_layout(uint32_t *user_start) { *user_start = SIM_USER_START; return 0; }
int emmc_read(uint32_t lba, uint32_t count, void *buf) { return disk_read(&s_emmc, lba, count, buf); }
int emmc_write(uint32_t lba, uint32_t count, const void *buf) { return disk_write(&s_emmc, lba, count, buf); }
int emmc_write_async(uint32_t lba, uint32_t count, const void *buf) { return disk_write(&s_emmc, lba, count, buf); }
int emmc_wq_step(int may_start) { return 0; }
int emmc_busy(void) { return 0; }
unsigned emmc_wq_pending(void) { return 0; }
const emmc_wq_stats_t *emmc_wq_stats(void) { return &s_stats; }
int emmc_sync(void) { s_stats.syncs++; return 0; }

/* ---- the card ---------------------------------------------------------------------------------- */

static int s_sd_init;

int sd_present(void) { return s_sd.fd >= 0; }
int sd_init(void) { s_sd_init = s_sd.fd >= 0; return s_sd_init ? 0 : SD_ERR_NOCARD; }
uint32_t sd_sectors(void) { return s_sd_init ? s_sd.sectors : 0; }
int sd_is_hc(void) { return 1; }
int sd_bus_width(void) { return 4; }
int sd_read(uint32_t lba, uint32_t count, void *buf) { return s_sd_init ? disk_read(&s_sd, lba, count, buf) : SD_ERR_NOCARD; }
int sd_write_async(uint32_t lba, uint32_t count, const void *buf) { return s_sd_init ? disk_write(&s_sd, lba, count, buf) : SD_ERR_NOCARD; }
uint32_t sd_written(void) { return s_sd.written; }
