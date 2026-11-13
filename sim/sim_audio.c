/*
 * sim_audio.c -- audio/audio.h for the simulator: the play queue's tracks decoded by ffmpeg on the
 * host and played through SDL, in place of core B's decoders and the I2S DAC.
 *
 * The firmware still decides everything it does on the device: what plays next (services/queue.c
 * via queue_take, so shuffle and the repeat modes are the firmware's), skip/seek/pause, the volume
 * in dB, what the Now Playing screen shows (audio_status). Each track is read out of the disk image
 * through the firmware's own file system (fs_open_ref/fs_read) into a temp file that ffprobe
 * describes and ffmpeg decodes to 48 kHz stereo; a decoder thread fills a ring that SDL's audio
 * callback drains. The EQ is not applied (core B's EQ isn't simulated).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <SDL.h>

#include "sim.h"
#include "audio/audio.h"
#include "audio/audio_shared.h"
#include "services/fs.h"
#include "services/log.h"
#include "services/queue.h"

extern char **environ;

#define DEV_RATE 48000u
#define RING_FRAMES (DEV_RATE * 2u)

/* ---- the ring: decoder thread -> SDL callback ------------------------------------------------- */

static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cv = PTHREAD_COND_INITIALIZER;
static int16_t s_ring[RING_FRAMES * 2];
static uint32_t s_wr, s_rd;            /* frames, free-running */
static int s_eof;                      /* the decoder is done with this track */
static int s_quit;                     /* the decoder thread should stop */
static uint64_t s_consumed;            /* device frames of this track played */
static int s_paused;
static float s_gain = 0.1f;            /* -40 dB, +20 dB for PC speakers (sim_audio_gain_db) */
static int s_boost_db = 20;

static SDL_AudioDeviceID s_dev;
static int s_no_dev;                   /* no audio device: consumed in real time by the tick */

static void consume(int16_t *out, uint32_t frames)
{
    uint32_t have, n, i;

    pthread_mutex_lock(&s_mx);
    have = s_wr - s_rd;
    n = s_paused ? 0 : frames < have ? frames : have;
    for (i = 0; i < n; i++) {
        uint32_t k = (s_rd + i) % RING_FRAMES * 2u;
        if (out) {
            float l = s_ring[k] * s_gain, r = s_ring[k + 1] * s_gain;
            out[2 * i] = (int16_t)(l > 32767.f ? 32767 : l < -32768.f ? -32768 : lrintf(l));
            out[2 * i + 1] = (int16_t)(r > 32767.f ? 32767 : r < -32768.f ? -32768 : lrintf(r));
        }
    }
    s_rd += n;
    s_consumed += n;
    pthread_cond_broadcast(&s_cv);
    pthread_mutex_unlock(&s_mx);
    if (out && n < frames)
        memset(out + 2 * n, 0, (frames - n) * 4u);
}

static void sdl_cb(void *u, Uint8 *stream, int len) { consume((int16_t *)stream, (uint32_t)len / 4u); }

/* The 10 ms tick: with no audio device, play the ring silently in real time. */
void sim_audio_tick(void)
{
    static uint64_t s_last;
    uint64_t now = sim_now_us();

    if (s_no_dev && s_last)
        consume(NULL, (uint32_t)((now - s_last) * DEV_RATE / 1000000u));
    s_last = now;
}

static void dev_open(void)
{
    SDL_AudioSpec want = { 0 }, got;

    if (s_dev || s_no_dev)
        return;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        goto none;
    want.freq = DEV_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = sdl_cb;
    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!s_dev)
        goto none;
    SDL_PauseAudioDevice(s_dev, 0);
    return;
none:
    sim_log("no audio output (%s): tracks play silently", SDL_GetError());
    s_no_dev = 1;
}

/* ---- the decoder: ffmpeg -> ring ------------------------------------------------------------- */

static pid_t s_pid;
static FILE *s_pipe;
static pthread_t s_thr;
static int s_thr_on;

static void *decoder(void *u)
{
    int16_t buf[4096 * 2];
    size_t n;

    while ((n = fread(buf, 4, 4096, s_pipe)) > 0) {
        size_t i = 0;

        pthread_mutex_lock(&s_mx);
        while (i < n && !s_quit) {
            while (s_wr - s_rd >= RING_FRAMES && !s_quit)
                pthread_cond_wait(&s_cv, &s_mx);
            for (; i < n && s_wr - s_rd < RING_FRAMES; i++, s_wr++)
                memcpy(&s_ring[s_wr % RING_FRAMES * 2u], &buf[2 * i], 4);
        }
        pthread_mutex_unlock(&s_mx);
        if (s_quit)
            break;
    }
    pthread_mutex_lock(&s_mx);
    s_eof = 1;
    pthread_mutex_unlock(&s_mx);
    return NULL;
}

static void decoder_stop(void)
{
    if (!s_thr_on)
        return;
    pthread_mutex_lock(&s_mx);
    s_quit = 1;
    pthread_cond_broadcast(&s_cv);
    pthread_mutex_unlock(&s_mx);
    kill(s_pid, SIGTERM);
    pthread_join(s_thr, NULL);
    fclose(s_pipe);
    waitpid(s_pid, NULL, 0);
    s_thr_on = 0;
    pthread_mutex_lock(&s_mx);
    s_quit = 0;
    s_wr = s_rd = 0;
    s_eof = 0;
    s_consumed = 0;
    pthread_mutex_unlock(&s_mx);
}

static int decoder_start(const char *path, uint32_t start_ms)
{
    char ss[32], *argv[24];
    int fd[2], i = 0;
    posix_spawn_file_actions_t fa;

    snprintf(ss, sizeof ss, "%u.%03u", start_ms / 1000u, start_ms % 1000u);
    argv[i++] = "ffmpeg";
    argv[i++] = "-nostdin";
    argv[i++] = "-v";
    argv[i++] = "fatal";   /* (not the broken pipe of a stop) */
    if (start_ms) {
        argv[i++] = "-ss";
        argv[i++] = ss;
    }
    argv[i++] = "-i";
    argv[i++] = (char *)path;
    argv[i++] = "-map";
    argv[i++] = "0:a:0";
    argv[i++] = "-f";
    argv[i++] = "s16le";
    argv[i++] = "-ac";
    argv[i++] = "2";
    argv[i++] = "-ar";
    argv[i++] = "48000";
    argv[i++] = "-";
    argv[i] = NULL;
    if (pipe(fd))
        return -1;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_addclose(&fa, fd[0]);
    posix_spawn_file_actions_addclose(&fa, fd[1]);
    if (posix_spawnp(&s_pid, "ffmpeg", &fa, NULL, argv, environ) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        close(fd[0]);
        close(fd[1]);
        sim_log("ffmpeg not found: install it to hear tracks (sudo apt install ffmpeg)");
        return -1;
    }
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    s_pipe = fdopen(fd[0], "rb");
    s_thr_on = 1;
    pthread_create(&s_thr, NULL, decoder, NULL);
    return 0;
}

/* ---- tracks: out of the image into a temp file, described by ffprobe ------------------------- */

static char s_tmp[64];
static fs_ref_t s_tmp_ref;
static int s_tmp_ok;

static int extract(const fs_ref_t *ref)
{
    static uint8_t buf[256 * 1024];
    fs_file_t *f;
    FILE *out;
    long n;

    if (s_tmp_ok && !memcmp(ref, &s_tmp_ref, sizeof *ref))
        return 0;                      /* (a seek replays the same file) */
    s_tmp_ok = 0;
    if (!s_tmp[0]) {
        int fd;

        strcpy(s_tmp, "/tmp/rechord_sim_XXXXXX");
        fd = mkstemp(s_tmp);
        if (fd < 0)
            return -1;
        close(fd);
    }
    f = fs_open_ref(ref);
    if (!f)
        return -1;
    out = fopen(s_tmp, "wb");
    if (!out) {
        fs_close(f);
        return -1;
    }
    while ((n = fs_read(f, buf, sizeof buf)) > 0)
        fwrite(buf, 1, (size_t)n, out);
    fclose(out);
    fs_close(f);
    if (n < 0)
        return -1;
    s_tmp_ref = *ref;
    s_tmp_ok = 1;
    return 0;
}

static uint32_t codec_of(const char *c, const char *fmt)
{
    static const struct { const char *name; uint32_t codec; } k[] = {
        { "flac", AS_CODEC_FLAC }, { "mp3", AS_CODEC_MP3 }, { "aac", AS_CODEC_AAC },
        { "alac", AS_CODEC_ALAC }, { "vorbis", AS_CODEC_VORBIS }, { "opus", AS_CODEC_OPUS },
        { "wavpack", AS_CODEC_WAVPACK }, { "ape", AS_CODEC_APE }, { "wmav1", AS_CODEC_WMA },
        { "wmav2", AS_CODEC_WMA }, { "wmapro", AS_CODEC_WMAPRO }, { "wmalossless", AS_CODEC_WMALOSSLESS },
    };

    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strcmp(c, k[i].name))
            return k[i].codec;
    if (!strncmp(c, "dsd_", 4))
        return AS_CODEC_DSD;
    if (!strncmp(c, "pcm_f", 5))
        return AS_CODEC_WAVF;
    if (!strncmp(c, "pcm_", 4))
        return strstr(fmt, "aiff") ? (strstr(c, "le") ? AS_CODEC_AIFF_LE : AS_CODEC_AIFF) : AS_CODEC_WAV;
    return 0;
}

static int probe(const char *path, audio_track_t *t)
{
    char cmd[256], line[256], codec[32] = "", fmt[64] = "", sfmt[16] = "";
    double dur = 0;
    unsigned long br = 0;
    unsigned bits = 0, bps = 0;
    FILE *p;

    snprintf(cmd, sizeof cmd, "ffprobe -v error -select_streams a:0 -show_entries "
             "stream=codec_name,sample_rate,channels,bits_per_raw_sample,bits_per_sample,sample_fmt,"
             "bit_rate:format=format_name,duration -of default=nw=1 '%s' 2>/dev/null", path);
    p = popen(cmd, "r");
    if (!p)
        return -1;
    while (fgets(line, sizeof line, p)) {
        char *v = strchr(line, '=');

        if (!v)
            continue;
        *v++ = 0;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(line, "codec_name")) snprintf(codec, sizeof codec, "%s", v);
        else if (!strcmp(line, "sample_rate")) t->rate = (uint32_t)atoi(v);
        else if (!strcmp(line, "channels")) t->channels = (uint32_t)atoi(v);
        else if (!strcmp(line, "bits_per_raw_sample")) bits = (unsigned)atoi(v);
        else if (!strcmp(line, "bits_per_sample")) bps = (unsigned)atoi(v);
        else if (!strcmp(line, "sample_fmt")) snprintf(sfmt, sizeof sfmt, "%s", v);
        else if (!strcmp(line, "bit_rate")) br = strtoul(v, NULL, 10);
        else if (!strcmp(line, "format_name")) snprintf(fmt, sizeof fmt, "%s", v);
        else if (!strcmp(line, "duration")) dur = atof(v);
    }
    if (pclose(p) != 0 || !codec[0] || !t->rate)
        return -1;
    t->codec = codec_of(codec, fmt);
    if (!bits)
        bits = bps;
    if (!bits)
        bits = strstr(sfmt, "s32") || strstr(sfmt, "flt") ? 24 : 16;
    if (t->codec == AS_CODEC_DSD) {    /* the device plays DSD as 176.4 kHz PCM */
        t->rate = 176400;
        bits = 24;
    }
    t->bits = bits > 24 ? 24 : bits;
    switch (t->codec) {                /* what audio.c's open_* report for these */
    case AS_CODEC_MP3: case AS_CODEC_VORBIS: case AS_CODEC_OPUS: case AS_CODEC_AAC: case AS_CODEC_WMA:
        t->bits = 16;
        break;
    case AS_CODEC_WAVPACK:
        t->bits = 24;
        break;
    }
    t->frames = (uint32_t)(dur * t->rate);
    t->kbps = t->codec == AS_CODEC_MP3 ? (uint32_t)(br / 1000u) : 0;
    return 0;
}

/* ---- audio.h -------------------------------------------------------------------------------- */

static int s_playing;
static uint32_t s_seq;
static audio_track_t s_track;
static int s_vol_db = -40;
static uint32_t s_seek_ms;

/* Open what queue_take hands out next (skipping what doesn't open, as audio.c's open_next). */
static int start_next(uint32_t start_ms)
{
    for (int tries = 0; tries < Q_MAX; tries++) {
        audio_track_t t = { 0 };
        fs_ref_t ref;
        int kind, pos;

        if (queue_take(&ref, &kind, &pos) != 0)
            return -3;
        if (fs_ref_name(&ref, t.name, sizeof t.name) != 0)
            strcpy(t.name, "?");
        if (extract(&ref) != 0) {
            log_printf("audio: cannot open %s", t.name);
            continue;
        }
        if (probe(s_tmp, &t) != 0) {
            log_printf("audio: %s: not a playable stream", t.name);
            continue;
        }
        t.ref = ref;
        t.index = (uint32_t)pos;
        t.seq = ++s_seq;
        if (start_ms && t.frames && (uint64_t)start_ms * t.rate / 1000u >= t.frames)
            start_ms = (uint32_t)((uint64_t)(t.frames - 1u) * 1000u / t.rate);
        t.base = (uint32_t)((uint64_t)start_ms * t.rate / 1000u);
        if (decoder_start(s_tmp, start_ms) != 0)
            return -1;
        s_track = t;
        log_printf("audio: %s %u Hz %u-bit %uch", t.name, (unsigned)t.rate, (unsigned)t.bits,
                   (unsigned)t.channels);
        return 0;
    }
    return -3;
}

int audio_play(int pos)
{
    uint32_t ms = s_seek_ms;

    s_seek_ms = 0;
    decoder_stop();
    s_playing = 0;
    pthread_mutex_lock(&s_mx);
    s_paused = 0;
    pthread_mutex_unlock(&s_mx);
    dev_open();
    queue_seek(pos);
    if (start_next(ms) != 0) {
        memset(&s_track, 0, sizeof s_track);
        return -3;
    }
    s_playing = 1;
    return 0;
}

void audio_stop(void)
{
    decoder_stop();
    s_playing = 0;
    memset(&s_track, 0, sizeof s_track);
}

/* The end of a track: the next one the queue hands out, or the end of playing. */
void audio_poll(void)
{
    int done;

    if (!s_playing)
        return;
    pthread_mutex_lock(&s_mx);
    done = s_eof && s_wr == s_rd;
    pthread_mutex_unlock(&s_mx);
    if (!done)
        return;
    decoder_stop();
    if (start_next(0) != 0) {
        s_playing = 0;
        memset(&s_track, 0, sizeof s_track);
    }
}

int audio_status(audio_status_t *st)
{
    uint64_t played;

    memset(st, 0, sizeof *st);
    st->playing = s_playing;
    if (!s_playing || !s_track.seq)
        return 0;
    st->seq = s_track.seq;
    st->track = s_track;
    queue_heard((int)s_track.index);
    pthread_mutex_lock(&s_mx);
    played = s_consumed;
    pthread_mutex_unlock(&s_mx);
    st->frames = s_track.base + (uint32_t)(played * s_track.rate / DEV_RATE);
    if (s_track.frames && st->frames > s_track.frames)
        st->frames = s_track.frames;
    return 0;
}

int audio_set_volume_db(int db)
{
    s_vol_db = db < -60 ? -60 : db > 0 ? 0 : db;
    pthread_mutex_lock(&s_mx);
    s_gain = s_vol_db <= -60 ? 0.0f : powf(10.0f, (float)(s_vol_db + s_boost_db) / 20.0f);
    pthread_mutex_unlock(&s_mx);
    return s_vol_db;
}

int audio_get_volume_db(void) { return s_vol_db; }
void audio_eq(void) { }

int audio_skip(int dir)
{
    audio_status_t st;
    int pos;

    audio_status(&st);
    if (!st.seq)
        return -3;
    if (dir > 0)
        pos = queue_next_pos();
    else if (st.frames <= 3u * st.track.rate)
        pos = queue_prev_pos();
    else
        pos = queue_pos();
    return pos < 0 ? -3 : audio_play(pos);
}

int audio_seekable(uint32_t codec)
{
    switch (codec) {
    case AS_CODEC_FLAC: case AS_CODEC_WAV: case AS_CODEC_WAVF: case AS_CODEC_AIFF: case AS_CODEC_AIFF_LE:
    case AS_CODEC_MP3: case AS_CODEC_AAC: case AS_CODEC_ALAC: case AS_CODEC_WAVPACK: case AS_CODEC_DSD:
        return 1;
    default:
        return 0;
    }
}

int audio_seek(uint32_t ms)
{
    audio_status_t st;
    int r, paused;

    audio_status(&st);
    if (!s_playing || !st.seq || !st.track.rate)
        return -3;
    if (!audio_seekable(st.track.codec))
        return -2;
    paused = s_paused;
    s_seek_ms = ms;
    r = audio_play((int)st.track.index);
    if (r == 0 && paused)
        audio_pause(1);
    return r;
}

int audio_pause(int on)
{
    pthread_mutex_lock(&s_mx);
    if (s_playing)
        s_paused = on;
    pthread_mutex_unlock(&s_mx);
    return s_paused;
}

int audio_paused(void) { return s_playing && s_paused; }
int audio_set_output(int usb) { return 0; }
int audio_output_usb(void) { return 0; }
int audio_write_ok(void) { return 1; }

/* Extra gain on top of the firmware's volume: the device's -40 dB default is a headphone level. */
void sim_audio_gain_db(int db)
{
    s_boost_db = db;
    audio_set_volume_db(s_vol_db);
}

void sim_audio_shutdown(void)
{
    decoder_stop();
    if (s_dev)
        SDL_CloseAudioDevice(s_dev);
    if (s_tmp[0])
        unlink(s_tmp);
}
