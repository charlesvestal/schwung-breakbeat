#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include "plugin_api_v1.h"
#include "slice_select.h"
#include "perf.h"
#include <time.h>
#include <math.h>

/* WAV audio format codes */
#define WAV_FORMAT_PCM   1
#define WAV_FORMAT_FLOAT 3

#define BB_PATH_MAX 512

typedef struct {
    int preset_idx;
    /* All paths stored as absolute. relative inputs get resolved via module_dir. */
    char alt_sample_path[BB_PATH_MAX];   /* alt sample for phrase swap */
    char main_sample_path[BB_PATH_MAX];  /* main loop user choice; restored at top of phrase */
    char pending_sample_path[BB_PATH_MAX]; /* deferred load; "" = nothing pending */
    char module_dir[BB_PATH_MAX];        /* base for relative paths */
    float length;
    float complexity;
    float anchor;
    float roll;
    int   phrase_bars;
    float fill;
    int   bar_counter;
    int   reseed_pending;

    /* Live performance layer (momentary MIDI-pad overrides). */
    bb_perf_t perf;
    float perf_trig_acc;   /* accumulator for ½×/2× trigger-cadence */

    // WAV file state
    int fd;
    void *map;
    size_t map_size;
    void *data;
    uint32_t total_frames;
    float play_pos;
    int num_channels;
    int audio_format;
    int bits_per_sample;
    int playing;

    // Slice state
    uint32_t slice_starts[8];
    uint32_t slice_lengths[8];
    int current_slice;
    float retrig_p[4];        /* [0]=2x [1]=3x [2]=4x [3]=8x, each 0..1 */
    int sub_slice_active;
    int sub_slice_counter;
    int retrigger_divisions;
    int preview_frames;       /* >0 = play preview even while transport stopped */

    uint64_t sample_counter;

    /* Timing — tick-triggered.
     *
     * Triggers fire directly from MIDI 0xF8 clock ticks in on_midi(), not from
     * a sample-counting phase accumulator. This gives inherently correct sync:
     * there is no rate calculation, no BPM measurement, no drift.
     *
     * Trigger rate: one trigger every ticks_per_trigger ticks.
     *   ticks_per_trigger = 12 * active_length
     *   (96 ticks/bar ÷ 8 slices/bar = 12 ticks/slice at 1-bar length)
     *
     * Playback rate: rate = slice_length / samples_per_trigger
     *   samples_per_trigger is measured from the actual sample-counter distance
     *   between two consecutive trigger ticks — no BPM math required.
     *
     * When MIDI Sync = Out is not configured the module falls back to
     * get_bpm()-derived timing (same as before), which is reliable while
     * transport is stopped but may drift slightly while running.
     */
    int      ticks_per_trigger;    /* 0xF8 ticks between slice triggers (12 × active_length) */
    int      midi_tick;            /* ticks since last 0xFA; resets every 96 ticks (1 bar) */
    int      fa_received;          /* set by on_midi(0xFA); cleared by the T_run handler */

    /* Pending events set from on_midi(), applied at the top of render_block(). */
    int      pending_trigger;      /* 1 = a trigger arrived since last render block */
    int      pending_beat_pos;     /* beat_position for the pending trigger */
    int      pending_bar;          /* 1 = a bar boundary crossed since last render block */

    /* Rate measurement. */
    uint64_t last_trigger_sample;  /* sample_counter at the previous trigger tick */
    float    samples_per_trigger;  /* measured inter-trigger duration in samples (0 = unknown) */

    /* Phase accumulators — used as fallback when no MIDI clock is available
     * (MIDI Sync = Off / CLOCK_STATUS_UNAVAILABLE). */
    float    trigger_phase;
    float    bar_phase;

    int      trigger_count;
    float    active_length;
    float    pending_main_length;
    int      was_running;
    int      just_reset;

    /* get_bpm() value tracked while stopped; used as rate fallback before the
     * first tick measurement arrives. */
    float    stable_bpm;
    int      bpm_low_count;

    /* Diagnostics */
    uint64_t dbg_last_trigger_sample; /* sample_counter at last trigger, for interval check */
    int      dbg_triggers_to_log;     /* countdown: log this many more triggers in detail */

    float swap_prob;
    float alt_length;
    float main_length;

    int alt_loop_idx;
    int main_loop_idx;

    char current_loop;   /* loop actually playing right now ('A' or 'B') */
    char pending_loop;   /* loop that will play when pending_sample_path loads */
    char status_str[32];

    /* Diagnostics — log key events once rather than every block. */
    int dbg_first_render;     /* 1 until first render_block call is logged */
    int dbg_silence_reason;   /* last silence reason code logged (0 = none yet) */
    int dbg_last_clock_status;/* last value returned by get_clock_status */
    uint64_t dbg_last_heartbeat; /* sample_counter at last periodic status log */

} breakbeat_t;

static const host_api_v1_t *g_host = NULL;
static char g_preset_filenames[32][64];
static int g_total_presets = 0;

static void wp_log(const char *msg) {
    if (g_host && g_host->log) g_host->log(msg);
}

static void scan_presets(const char *module_dir) {
    g_total_presets = 0;
    char path[512];
    snprintf(path, sizeof(path), "%s/presets", module_dir);
    
    DIR *dir = opendir(path);
    if (!dir) {
        wp_log("breakbeat: failed to open presets dir");
        return;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && g_total_presets < 32) {
        const char *ext = strrchr(entry->d_name, '.');
        if (ext && strcmp(ext, ".json") == 0) {
            strncpy(g_preset_filenames[g_total_presets], entry->d_name, 63);
            g_preset_filenames[g_total_presets][63] = '\0';
            g_total_presets++;
        }
    }
    closedir(dir);

    /* Sort alphabetically so index 0 is always the lowest-numbered preset. */
    for (int i = 0; i < g_total_presets - 1; i++) {
        for (int j = i + 1; j < g_total_presets; j++) {
            if (strcmp(g_preset_filenames[i], g_preset_filenames[j]) > 0) {
                char tmp[64];
                memcpy(tmp, g_preset_filenames[i], 64);
                memcpy(g_preset_filenames[i], g_preset_filenames[j], 64);
                memcpy(g_preset_filenames[j], tmp, 64);
            }
        }
    }

    char dbg[64];
    snprintf(dbg, sizeof(dbg), "breakbeat: found %d presets", g_total_presets);
    wp_log(dbg);
}

static float bb_rand(void *ctx) {
    (void)ctx;
    return (float)rand() / (float)RAND_MAX;
}

/* JSON helpers for state parsing */
static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    if (!json || !key || !out || out_len < 1) return 0;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return 0;
    const char *colon = strchr(pos, ':');
    if (!colon) return 0;
    while (*colon && (*colon == ':' || *colon == ' ' || *colon == '\t')) colon++;
    if (*colon != '"') return 0;
    colon++;
    const char *end = strchr(colon, '"');
    if (!end) return 0;
    int len = (int)(end - colon);
    if (len >= out_len) len = out_len - 1;
    strncpy(out, colon, len);
    out[len] = '\0';
    return len;
}

static int json_get_int(const char *json, const char *key, int *out) {
    if (!json || !key || !out) return 0;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return 0;
    const char *colon = strchr(pos, ':');
    if (!colon) return 0;
    colon++;
    while (*colon && (*colon == ' ' || *colon == '\t')) colon++;
    *out = atoi(colon);
    return 1;
}

static char* read_file_to_string(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char*)malloc(size + 1);
    if (buf) {
        fread(buf, 1, size, f);
        buf[size] = '\0';
    }
    fclose(f);
    return buf;
}

static int json_get_float(const char *json, const char *key, float *out) {
    if (!json || !key || !out) return 0;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return 0;
    const char *colon = strchr(pos, ':');
    if (!colon) return 0;
    colon++;
    while (*colon && (*colon == ' ' || *colon == '\t')) colon++;
    *out = atof(colon);
    return 1;
}

static void close_file(breakbeat_t *wp) {
    if (wp->map && wp->map != MAP_FAILED) {
        munmap(wp->map, wp->map_size);
    }
    if (wp->fd >= 0) {
        close(wp->fd);
    }
    wp->fd = -1;
    wp->map = NULL;
    wp->map_size = 0;
    wp->data = NULL;
    wp->total_frames = 0;
    wp->play_pos = 0;
    wp->num_channels = 0;
    wp->audio_format = 0;
    wp->bits_per_sample = 0;
    wp->playing = 0;
}

/* True if extension (case-insensitive) matches .wav */
static int has_wav_ext(const char *path) {
    if (!path) return 0;
    size_t n = strlen(path);
    if (n < 4) return 0;
    const char *e = path + n - 4;
    return (e[0] == '.')
        && (e[1] == 'w' || e[1] == 'W')
        && (e[2] == 'a' || e[2] == 'A')
        && (e[3] == 'v' || e[3] == 'V');
}

/* Resolve possibly-relative `path` against `module_dir` into `out`.
 * Absolute paths copy as-is; relative paths get prefixed with module_dir. */
static void resolve_sample_path(const char *module_dir, const char *path, char *out, size_t out_len) {
    if (!path || !path[0]) { if (out_len > 0) out[0] = '\0'; return; }
    if (path[0] == '/') {
        snprintf(out, out_len, "%s", path);
    } else {
        snprintf(out, out_len, "/data/UserData/breakbeat-samples/%s", path);
    }
}

#define BB_PORTAL_DIR      "/data/UserData/breakbeat-samples"
#define BB_USER_LIB_TARGET "/data/UserData/UserLibrary/Samples"
#define BB_BUILTIN_LINK    BB_PORTAL_DIR "/Built-in"
#define BB_USER_LIB_LINK   BB_PORTAL_DIR "/User Library"

/* Returns the directory part of a sample path, or BB_PORTAL_DIR as fallback. */
static void sample_dirname(const char *path, char *dir, size_t dir_len) {
    const char *slash = (path && path[0]) ? strrchr(path, '/') : NULL;
    if (!slash || slash == path) { snprintf(dir, dir_len, "%s", BB_PORTAL_DIR); return; }
    size_t n = (size_t)(slash - path);
    if (n >= dir_len) n = dir_len - 1;
    memcpy(dir, path, n);
    dir[n] = '\0';
}

static void build_ui_hierarchy(const breakbeat_t *bb, char *out, int out_len) {
    (void)bb;
    const char *ar = BB_PORTAL_DIR;
    const char *br = BB_PORTAL_DIR;
    snprintf(out, out_len,
        "{\"modes\":null,\"levels\":{\"root\":{"
        "\"list_param\":\"preset\",\"count_param\":\"preset_count\",\"name_param\":\"preset_name\","
        "\"knobs\":[\"preset\",\"A_sample_path\",\"A_sample_length\",\"B_sample_path\",\"B_sample_length\","
        "\"B_chance\",\"complexity\",\"phrase\",\"anchor\",\"roll\",\"fill\","
        "\"retrig_2x\",\"retrig_3x\",\"retrig_4x\",\"retrig_8x\",\"save_preset\",\"status\"],"
        "\"params\":["
        "{\"key\":\"preset\",\"label\":\"Preset\",\"type\":\"int\",\"min\":0,\"max\":10},"
        "{\"key\":\"A_sample_path\",\"label\":\"A Sample\",\"type\":\"filepath\",\"root\":\"%s\",\"filter\":\".wav\"},"
        "{\"key\":\"A_sample_length\",\"label\":\"A Length\",\"type\":\"enum\",\"options\":[\"1/4 bar\",\"1/2 bar\",\"1 bar\",\"2 bars\",\"4 bars\",\"8 bars\"]},"
        "{\"key\":\"B_sample_path\",\"label\":\"B Sample\",\"type\":\"filepath\",\"root\":\"%s\",\"filter\":\".wav\"},"
        "{\"key\":\"B_sample_length\",\"label\":\"B Length\",\"type\":\"enum\",\"options\":[\"1/4 bar\",\"1/2 bar\",\"1 bar\",\"2 bars\",\"4 bars\",\"8 bars\"]},"
        "{\"key\":\"B_chance\",\"label\":\"B Chance\",\"type\":\"int\",\"min\":0,\"max\":100},"
        "{\"key\":\"complexity\",\"label\":\"Complexity\",\"type\":\"int\",\"min\":0,\"max\":100},"
        "{\"key\":\"phrase\",\"label\":\"Phrase\",\"type\":\"enum\",\"options\":[\"Off\",\"2 bars\",\"4 bars\",\"8 bars\",\"16 bars\"]},"
        "{\"key\":\"anchor\",\"label\":\"Anchor\",\"type\":\"int\",\"min\":0,\"max\":100},"
        "{\"key\":\"roll\",\"label\":\"Roll\",\"type\":\"int\",\"min\":0,\"max\":100},"
        "{\"key\":\"fill\",\"label\":\"Fill\",\"type\":\"int\",\"min\":0,\"max\":100},"
        "{\"key\":\"retrig_2x\",\"label\":\"Retrig 2x\",\"type\":\"int\",\"min\":0,\"max\":100},"
        "{\"key\":\"retrig_3x\",\"label\":\"Retrig 3x\",\"type\":\"int\",\"min\":0,\"max\":100},"
        "{\"key\":\"retrig_4x\",\"label\":\"Retrig 4x\",\"type\":\"int\",\"min\":0,\"max\":100},"
        "{\"key\":\"retrig_8x\",\"label\":\"Retrig 8x\",\"type\":\"int\",\"min\":0,\"max\":100},"
        "{\"key\":\"save_preset\",\"label\":\"Save Preset\",\"type\":\"int\",\"min\":0,\"max\":1},"
        "{\"key\":\"status\",\"label\":\"Status\",\"type\":\"enum\",\"options\":[\"-\"]}"
        "]}}}",
        ar, br);
}

static void build_chain_params(const breakbeat_t *bb, char *out, int out_len) {
    (void)bb;
    const char *ar = BB_PORTAL_DIR;
    const char *br = BB_PORTAL_DIR;
    snprintf(out, out_len,
        "["
        "{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"int\",\"min\":0,\"max\":10},"
        "{\"key\":\"A_sample_path\",\"name\":\"A Sample\",\"type\":\"filepath\",\"root\":\"%s\",\"filter\":\".wav\"},"
        "{\"key\":\"A_sample_length\",\"name\":\"A Length\",\"type\":\"enum\",\"options\":[\"1/4 bar\",\"1/2 bar\",\"1 bar\",\"2 bars\",\"4 bars\",\"8 bars\"]},"
        "{\"key\":\"B_sample_path\",\"name\":\"B Sample\",\"type\":\"filepath\",\"root\":\"%s\",\"filter\":\".wav\"},"
        "{\"key\":\"B_sample_length\",\"name\":\"B Length\",\"type\":\"enum\",\"options\":[\"1/4 bar\",\"1/2 bar\",\"1 bar\",\"2 bars\",\"4 bars\",\"8 bars\"]},"
        "{\"key\":\"B_chance\",\"name\":\"B Chance\",\"type\":\"int\",\"min\":0,\"max\":100},"
        "{\"key\":\"complexity\",\"name\":\"Complexity\",\"type\":\"int\",\"min\":0,\"max\":100},"
        "{\"key\":\"phrase\",\"name\":\"Phrase\",\"type\":\"enum\",\"options\":[\"Off\",\"2 bars\",\"4 bars\",\"8 bars\",\"16 bars\"]},"
        "{\"key\":\"anchor\",\"name\":\"Anchor\",\"type\":\"int\",\"min\":0,\"max\":100},"
        "{\"key\":\"roll\",\"name\":\"Roll\",\"type\":\"int\",\"min\":0,\"max\":100},"
        "{\"key\":\"fill\",\"name\":\"Fill\",\"type\":\"int\",\"min\":0,\"max\":100},"
        "{\"key\":\"retrig_2x\",\"name\":\"Retrig 2x\",\"type\":\"int\",\"min\":0,\"max\":100},"
        "{\"key\":\"retrig_3x\",\"name\":\"Retrig 3x\",\"type\":\"int\",\"min\":0,\"max\":100},"
        "{\"key\":\"retrig_4x\",\"name\":\"Retrig 4x\",\"type\":\"int\",\"min\":0,\"max\":100},"
        "{\"key\":\"retrig_8x\",\"name\":\"Retrig 8x\",\"type\":\"int\",\"min\":0,\"max\":100},"
        "{\"key\":\"save_preset\",\"name\":\"Save Preset\",\"type\":\"int\",\"min\":0,\"max\":1},"
        "{\"key\":\"status\",\"name\":\"Status\",\"type\":\"enum\",\"options\":[\"-\"]}"
        "]",
        ar, br);
}

/* Create the filepath browser "portal" with symlinks to bundled samples and
 * to the user's sample library. Idempotent — safe to run on every instance
 * creation. Needed because Module Store installs unpack the tarball but
 * don't run install.sh, so the symlinks must be set up at runtime. */

static void ensure_portal_exists(const char *module_dir) {
    if (mkdir(BB_PORTAL_DIR, 0755) != 0 && errno != EEXIST) {
        wp_log("breakbeat: could not create portal dir");
        /* Continue anyway — symlink calls will surface their own errors. */
    }

    char target[BB_PATH_MAX];
    snprintf(target, sizeof(target), "%s/samples", module_dir);

    /* Replace any pre-existing link to make sure it points where we want. */
    struct stat st;
    if (lstat(BB_BUILTIN_LINK, &st) == 0) (void)unlink(BB_BUILTIN_LINK);
    if (symlink(target, BB_BUILTIN_LINK) != 0 && errno != EEXIST) {
        wp_log("breakbeat: could not create Built-in symlink");
    }

    /* Only expose User Library if it actually exists on this device. */
    if (stat(BB_USER_LIB_TARGET, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (lstat(BB_USER_LIB_LINK, &st) == 0) (void)unlink(BB_USER_LIB_LINK);
        if (symlink(BB_USER_LIB_TARGET, BB_USER_LIB_LINK) != 0 && errno != EEXIST) {
            wp_log("breakbeat: could not create User Library symlink");
        }
    }
}

/* Open and validate a WAV file before destroying the previously-loaded one.
 * On any failure, returns -1 and leaves wp's existing sample state untouched. */
static int open_wav(breakbeat_t *wp, const char *path, float expected_length) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        wp_log("breakbeat: failed to open file");
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 44) {
        wp_log("breakbeat: file too small for WAV header");
        close(fd);
        return -1;
    }

    size_t map_size = (size_t)st.st_size;
    void *map = mmap(NULL, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        wp_log("breakbeat: mmap failed");
        close(fd);
        return -1;
    }
    /* Pre-fault all pages now, while we're still in the MIDI callback (not
     * the RT render thread). Without this, render_block would trigger OS page
     * faults on first access, causing latency spikes that can trip Schwung's
     * render watchdog and kill the module. */
    madvise(map, map_size, MADV_WILLNEED);

    const uint8_t *raw = (const uint8_t *)map;
    if (memcmp(raw, "RIFF", 4) != 0 || memcmp(raw + 8, "WAVE", 4) != 0) {
        wp_log("breakbeat: not a RIFF/WAVE file");
        munmap(map, map_size);
        close(fd);
        return -1;
    }

    uint32_t offset = 12;
    uint16_t audio_format = 0;
    uint16_t num_channels = 0;
    uint16_t bits_per_sample = 0;
    int found_fmt = 0;
    int found_data = 0;
    uint32_t data_offset = 0;
    uint32_t data_size = 0;

    while (offset + 8 <= map_size) {
        const uint8_t *chunk = raw + offset;
        uint32_t chunk_size = chunk[4] | (chunk[5] << 8) | (chunk[6] << 16) | (chunk[7] << 24);

        if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
            audio_format    = chunk[8]  | (chunk[9]  << 8);
            num_channels    = chunk[10] | (chunk[11] << 8);
            bits_per_sample = chunk[22] | (chunk[23] << 8);
            found_fmt = 1;
        } else if (memcmp(chunk, "data", 4) == 0) {
            data_offset = offset + 8;
            data_size = chunk_size;
            found_data = 1;
            break;
        }
        offset += 8 + chunk_size;
        if (chunk_size & 1) offset++;
    }

    if (!found_fmt || !found_data) {
        wp_log("breakbeat: missing fmt or data chunk");
        munmap(map, map_size);
        close(fd);
        return -1;
    }

    /* WAVE_FORMAT_EXTENSIBLE (0xFFFE) lives in the subformat GUID; many DAWs
     * write 24-bit/32-bit PCM with this format code. Treat it as PCM. */
    int is_float = (audio_format == WAV_FORMAT_FLOAT);
    int is_pcm   = (audio_format == WAV_FORMAT_PCM) || (audio_format == 0xFFFE);
    int bytes_per_sample = 0;
    if (is_pcm) {
        if      (bits_per_sample == 16) bytes_per_sample = 2;
        else if (bits_per_sample == 24) bytes_per_sample = 3;
        else if (bits_per_sample == 32) bytes_per_sample = 4;
    } else if (is_float && bits_per_sample == 32) {
        bytes_per_sample = 4;
    }

    if (bytes_per_sample == 0 || num_channels == 0 || num_channels > 2) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "breakbeat: unsupported format (audio_format=%u, bits=%u, ch=%u)",
                 audio_format, bits_per_sample, num_channels);
        wp_log(buf);
        munmap(map, map_size);
        close(fd);
        return -1;
    }

    /* Validation passed — now destroy old and commit new. */
    close_file(wp);

    wp->fd = fd;
    wp->map = map;
    wp->map_size = map_size;
    /* Keep audio_format as the canonical "float vs PCM" hint for render_block. */
    wp->audio_format = is_float ? WAV_FORMAT_FLOAT : WAV_FORMAT_PCM;
    wp->bits_per_sample = bits_per_sample;
    wp->num_channels = num_channels;
    wp->data = (void *)(raw + data_offset);
    wp->total_frames = data_size / (num_channels * bytes_per_sample);
    wp->play_pos = 0;
    /* Scale samples_per_trigger proportionally when active_length changes so the
     * playback rate is correct immediately on the first block of the new sample,
     * without waiting for a full new tick-trigger measurement to arrive. */
    {
        int new_tpt = (int)(12.0f * expected_length + 0.5f);
        if (new_tpt < 1) new_tpt = 1;
        if (wp->ticks_per_trigger > 0 && wp->samples_per_trigger > 0.0f)
            wp->samples_per_trigger *= (float)new_tpt / (float)wp->ticks_per_trigger;
        wp->ticks_per_trigger = new_tpt;
    }
    wp->active_length = expected_length;

    uint32_t slice_size = wp->total_frames / 8;
    for (int i = 0; i < 8; i++) {
        wp->slice_starts[i] = i * slice_size;
        wp->slice_lengths[i] = slice_size;
    }

    char logbuf[160];
    snprintf(logbuf, sizeof(logbuf),
             "breakbeat: loaded %u frames (%uch, %ubit, %s)",
             wp->total_frames, num_channels, bits_per_sample,
             is_float ? "float" : "PCM");
    wp_log(logbuf);

    return 0;
}

/* Load the sample at `path` (relative or absolute). Returns 0 on success. */
static int apply_sample_path(breakbeat_t *bb, const char *path, float expected_length) {
    if (!bb || !path || !path[0]) return -1;
    if (!has_wav_ext(path)) {
        char buf[BB_PATH_MAX + 64];
        snprintf(buf, sizeof(buf), "breakbeat: rejected non-.wav path: %s", path);
        wp_log(buf);
        return -1;
    }
    char resolved[1024];
    resolve_sample_path(bb->module_dir, path, resolved, sizeof(resolved));

    char log_buf[BB_PATH_MAX + 64];
    snprintf(log_buf, sizeof(log_buf), "breakbeat: applying sample %s", resolved);
    wp_log(log_buf);

    return open_wav(bb, resolved, expected_length);
}



/* Parse a preset JSON blob and apply all fields to bb. Does not load audio. */
static void apply_preset_json(breakbeat_t *bb, const char *json) {
    char str_val[256];
    float fval;
    int ival;

    if (json_get_string(json, "A_sample_path", str_val, sizeof(str_val)))
        resolve_sample_path(bb->module_dir, str_val, bb->main_sample_path, sizeof(bb->main_sample_path));
    else if (json_get_string(json, "sample_path", str_val, sizeof(str_val)))
        resolve_sample_path(bb->module_dir, str_val, bb->main_sample_path, sizeof(bb->main_sample_path));

    if (json_get_string(json, "B_sample_path", str_val, sizeof(str_val)))
        resolve_sample_path(bb->module_dir, str_val, bb->alt_sample_path, sizeof(bb->alt_sample_path));
    else if (json_get_string(json, "alt_sample_path", str_val, sizeof(str_val)))
        resolve_sample_path(bb->module_dir, str_val, bb->alt_sample_path, sizeof(bb->alt_sample_path));

    const char *length_names[] = {"1/4 bar", "1/2 bar", "1 bar", "2 bars", "4 bars", "8 bars"};
    float length_vals[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};

    if (json_get_string(json, "A_sample_length", str_val, sizeof(str_val))) {
        for (int i = 0; i < 6; i++) {
            if (strcmp(length_names[i], str_val) == 0) { bb->length = bb->main_length = length_vals[i]; break; }
        }
    } else if (json_get_string(json, "length", str_val, sizeof(str_val))) {
        for (int i = 0; i < 6; i++) {
            if (strcmp(length_names[i], str_val) == 0) { bb->length = bb->main_length = length_vals[i]; break; }
        }
    }

    if (json_get_string(json, "B_sample_length", str_val, sizeof(str_val))) {
        for (int i = 0; i < 6; i++) {
            if (strcmp(length_names[i], str_val) == 0) { bb->alt_length = length_vals[i]; break; }
        }
    }

    if (json_get_float(json, "complexity", &fval)) bb->complexity = fval / 100.0f;
    if (json_get_float(json, "anchor",     &fval)) bb->anchor     = fval / 100.0f;
    if (json_get_float(json, "roll",       &fval)) bb->roll       = fval / 100.0f;
    if (json_get_float(json, "fill",       &fval)) bb->fill       = fval / 100.0f;
    if (json_get_string(json, "phrase", str_val, sizeof(str_val))) {
        const char *phrases[] = {"Off", "2 bars", "4 bars", "8 bars", "16 bars"};
        static const int phrase_values[] = {0, 2, 4, 8, 16};
        for (int i = 0; i < 5; i++) {
            if (strcmp(phrases[i], str_val) == 0) { bb->phrase_bars = phrase_values[i]; break; }
        }
    } else if (json_get_int(json, "phrase", &ival)) {
        static const int phrase_values[] = {0, 2, 4, 8, 16};
        if (ival >= 0 && ival < 5) bb->phrase_bars = phrase_values[ival];
    }

    /* Per-rate retrigger probabilities (new format). */
    {
        int has_new = 0;
        if (json_get_float(json, "retrig_2x", &fval)) { bb->retrig_p[0] = fval / 100.0f; has_new = 1; }
        if (json_get_float(json, "retrig_3x", &fval)) { bb->retrig_p[1] = fval / 100.0f; has_new = 1; }
        if (json_get_float(json, "retrig_4x", &fval)) { bb->retrig_p[2] = fval / 100.0f; has_new = 1; }
        if (json_get_float(json, "retrig_8x", &fval)) { bb->retrig_p[3] = fval / 100.0f; has_new = 1; }
        /* Backward-compat: old retrigger + retrigger_rate → new per-rate. */
        if (!has_new) {
            float prob = 0.0f;
            json_get_float(json, "retrigger", &prob);
            prob /= 100.0f;
            const char *rates[] = {"2x", "3x", "4x", "8x", "Rand"};
            int rate_idx = 0;
            if (json_get_string(json, "retrigger_rate", str_val, sizeof(str_val)))
                for (int i = 0; i < 5; i++)
                    if (strcmp(rates[i], str_val) == 0) { rate_idx = i; break; }
            if (rate_idx < 4) {
                bb->retrig_p[rate_idx] = prob;
            } else {
                for (int i = 0; i < 4; i++) bb->retrig_p[i] = prob;
            }
        }
    }

    if (json_get_int(json, "B_chance", &ival))
        bb->swap_prob = (float)ival / 100.0f;
    else if (json_get_float(json, "swap_prob", &fval))
        bb->swap_prob = fval / 100.0f;
}

/* Load preset idx, parse its JSON, and apply the sample immediately.
 * Used at init time and can be called any time the transport is stopped. */
static void load_preset_idx(breakbeat_t *bb, int idx) {
    if (idx < 0) idx = 0;
    if (idx >= g_total_presets) idx = g_total_presets - 1;
    bb->preset_idx = idx;
    if (g_total_presets == 0) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/presets/%s", bb->module_dir, g_preset_filenames[idx]);
    char *json = read_file_to_string(path);
    if (!json) {
        wp_log("breakbeat: load_preset_idx: failed to read preset file");
        return;
    }
    apply_preset_json(bb, json);
    free(json);

    apply_sample_path(bb, bb->main_sample_path, bb->main_length);
    bb->pending_sample_path[0] = '\0';
}

static void* bb_create_instance(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;
    breakbeat_t *bb = calloc(1, sizeof(breakbeat_t));
    if (!bb) return NULL;

    bb->preset_idx = 0;
    bb->retrig_p[0] = bb->retrig_p[1] = bb->retrig_p[2] = bb->retrig_p[3] = 0.0f;
    bb->sub_slice_active = 0;
    bb->sub_slice_counter = 0;
    bb->retrigger_divisions = 2;
    bb->preview_frames = 0;
    bb->length = 1.0f;
    bb->main_length = 1.0f;
    bb->alt_length = 1.0f;
    bb->current_loop = 'A';
    bb->pending_loop = 'A';
    strcpy(bb->status_str, "A_0_1x");
    bb->sample_counter = 0;
    bb->trigger_phase = 0.0f;
    bb->bar_phase = 0.0f;
    bb->trigger_count = 0;
    bb->active_length = 1.0f;
    bb->pending_main_length = 1.0f;
    bb->was_running = 0;
    bb->ticks_per_trigger  = 12;   /* 1-bar default; updated when active_length changes */
    bb->midi_tick          = 0;
    bb->fa_received        = 0;
    bb->pending_trigger    = 0;
    bb->pending_beat_pos   = 0;
    bb->pending_bar        = 0;
    bb->last_trigger_sample = 0;
    bb->samples_per_trigger = 0.0f;
    bb->trigger_phase = 0.0f;
    bb->bar_phase     = 0.0f;
    bb->stable_bpm = 0.0f;
    bb->bpm_low_count = 0;
    bb->swap_prob = 0.0f;
    bb->complexity = 0.5f;
    bb->anchor = 0.0f;
    bb->roll = 0.0f;
    bb->phrase_bars = 0;
    bb->fill = 0.0f;
    bb->bar_counter = 0;
    bb->reseed_pending = 0;
    bb_perf_init(&bb->perf);
    bb->perf_trig_acc = 0.0f;
    bb->fd = -1;
    bb->dbg_first_render = 1;
    bb->dbg_silence_reason = 0;
    bb->dbg_last_clock_status = -1;
    bb->dbg_last_heartbeat = 0;

    /* Capture module_dir for relative-path resolution. */
    if (module_dir && module_dir[0]) {
        snprintf(bb->module_dir, sizeof(bb->module_dir), "%s", module_dir);
    } else {
        /* Fallback to the install path if host doesn't provide module_dir. */
        snprintf(bb->module_dir, sizeof(bb->module_dir),
                 "/data/UserData/schwung/modules/sound_generators/breakbeat");
    }

    /* Set up the filepath browser portal (Built-in + User Library symlinks). */
    ensure_portal_exists(bb->module_dir);
    
    scan_presets(bb->module_dir);

    /* Fallback paths — overwritten immediately by load_preset_idx below. */
    snprintf(bb->main_sample_path, sizeof(bb->main_sample_path),
             "/data/UserData/breakbeat-samples/Built-in/amen01.wav");
    snprintf(bb->alt_sample_path,  sizeof(bb->alt_sample_path),
             "/data/UserData/breakbeat-samples/Built-in/amen09.wav");
    bb->pending_sample_path[0] = '\0';

    if (g_host && g_host->log) g_host->log("breakbeat: instance created");

    /* Load the first preset (1_calm) immediately so the module is ready to
     * make sound as soon as the transport starts, with no extra steps needed. */
    load_preset_idx(bb, 0);
    bb->playing = 1;

    return bb;
}

static void bb_destroy_instance(void *instance) {
    breakbeat_t *bb = (breakbeat_t *)instance;
    if (!bb) return;
    close_file(bb);
    free(bb);
    if (g_host && g_host->log) g_host->log("breakbeat: instance destroyed");
}

static void bb_start_preview(breakbeat_t *bb) {
    float bpm = (bb->stable_bpm > 20.0f) ? bb->stable_bpm : 120.0f;
    float spb = ((float)MOVE_SAMPLE_RATE * 60.0f / bpm) * 4.0f;
    bb->preview_frames      = (int)(spb * bb->active_length);
    bb->play_pos            = (float)bb->slice_starts[0];
    bb->current_slice       = 0;
    bb->trigger_phase       = 0.0f;
    bb->bar_phase           = 0.0f;
    bb->trigger_count       = 0;
    bb->bar_counter         = 0;
    bb->samples_per_trigger = 0.0f;  /* force BPM-based timing; MIDI ticks not running */
}

static void bb_reset_transport(breakbeat_t *bb) {
    bb->trigger_count     = 0;
    bb->bar_counter       = 0;
    bb->current_slice     = 0;
    bb->sub_slice_active  = 0;
    bb->sub_slice_counter = 0;
    bb->just_reset        = 1;
    bb->bpm_low_count     = 0;
    bb->midi_tick         = 0;   /* tick count starts fresh from this 0xFA */
    bb->fa_received       = 1;   /* tells the T_run handler to use tick-based offset */
    bb->pending_trigger   = 0;
    bb->pending_bar       = 0;
    bb->last_trigger_sample = bb->sample_counter;
    bb->trigger_phase = 0.0f;
    bb->bar_phase     = 0.0f;
    /* For phrase=2, bar_in_phrase=phrase_bars-2=0 is never hit as a real bar
     * boundary (bar_counter starts at 0 and first boundary makes it 1).
     * Pre-schedule B here so it loads correctly at bar 1. */
    bb->pending_sample_path[0] = '\0';
    if (bb->phrase_bars == 2 && bb->alt_sample_path[0] &&
        (float)rand() / (float)RAND_MAX < bb->swap_prob) {
        snprintf(bb->pending_sample_path, sizeof(bb->pending_sample_path),
                 "%s", bb->alt_sample_path);
        bb->pending_main_length = bb->alt_length;
        bb->pending_loop = 'B';
    }
    bb->dbg_last_trigger_sample = bb->sample_counter;
    bb->dbg_triggers_to_log = 10;
    srand((unsigned int)(time(NULL) ^ bb->sample_counter));
    wp_log("breakbeat: transport reset");
}

static void bb_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    breakbeat_t *bb = (breakbeat_t *)instance;
    if (!bb || len < 1) return;

    /* Transport start / continue — reset phase so slice 0 lands on beat 1. */
    if (msg[0] == 0xFA || msg[0] == 0xFB) {
        bb_reset_transport(bb);
        return;
    }

    /* 0xF8 MIDI clock tick — fire triggers directly from the clock.
     *
     * Rather than measuring BPM and deriving a rate, we simply fire a slice
     * trigger every ticks_per_trigger ticks. The playback rate is then measured
     * from the actual sample-counter distance between triggers, which is inherently
     * correct with no calculation, no filter, and no startup jitter.
     *
     * Only arrives when MIDI Sync = Out is configured on the device.
     * When unavailable the phase-accumulator fallback runs in render_block. */
    if (msg[0] == 0xF8) {
        bb->midi_tick++;

        /* Slice trigger. */
        if (bb->ticks_per_trigger > 0 &&
            bb->midi_tick % bb->ticks_per_trigger == 0) {

            uint64_t now = bb->sample_counter;
            /* Measure the inter-trigger interval for rate calculation.
             * Sanity: at 30 BPM with 8-bar length, max interval ≈ 2.8M samples.
             * At 300 BPM with 1/4-bar length, min interval ≈ 1100 samples. */
            if (bb->last_trigger_sample > 0) {
                uint64_t elapsed = now - bb->last_trigger_sample;
                if (elapsed >= 1100 && elapsed <= 2822400)
                    bb->samples_per_trigger = (float)elapsed;
            }
            bb->last_trigger_sample = now;

            bb->pending_beat_pos = bb->trigger_count % 8;
            bb->trigger_count++;
            bb->pending_trigger  = 1;
        }

        /* Bar boundary every 96 ticks. */
        if (bb->midi_tick >= 96) {
            bb->midi_tick = 0;
            bb->pending_bar = 1;
        }

        return;
    }

    if (len < 3) return;

    uint8_t status = msg[0] & 0xF0;
    uint8_t note   = msg[1];
    uint8_t vel    = msg[2];

    /* Live performance pads — momentary overrides of the generative engine.
     * Note-on engages an effect; note-off releases it. Note-off arrives as 0x80
     * or as 0x90 with velocity 0 (running status). */
    int is_note_on  = (status == 0x90 && vel > 0);
    int is_note_off = (status == 0x80) || (status == 0x90 && vel == 0);
    if (!is_note_on && !is_note_off) return;

    bb_pad_t pad = bb_perf_decode(note);
    if (pad.kind == BB_PAD_NONE) return;

    if (pad.kind == BB_PAD_A_SLICE || pad.kind == BB_PAD_B_SLICE) {
        /* Phase 1: single buffer — B-slice pads play from the A bank. */
        int slice = pad.index;
        if (is_note_on) {
            bb_perf_slice_push(&bb->perf, slice);
            /* Instant hit: jump now so playing feels responsive, and consume any
             * pending clock trigger so the next tick doesn't fight the jump.
             * Rate-related behavior stays locked to the clock in render_block. */
            bb->current_slice     = slice;
            bb->play_pos          = (float)bb->slice_starts[slice];
            bb->sub_slice_active  = 0;
            bb->sub_slice_counter = 0;
            bb->pending_trigger   = 0;
            bb->playing           = 1;
            /* Audition instantly even while the transport is stopped. */
            bb->preview_frames    = MOVE_SAMPLE_RATE / 2;
        } else {
            /* Pop from the held-slice stack; the next trigger reverts to the new
             * top-of-stack slice, or to the engine when nothing is held. */
            bb_perf_slice_release(&bb->perf, slice);
        }
        return;
    }

    /* Macro row. */
    if (is_note_on) {
        bb_perf_macro_on(&bb->perf, pad.index, vel);
        if (bb->perf.reseed_request) {
            bb->perf.reseed_request = 0;
            /* "Throw the dice": reseed the RNG and force one immediate re-pick. */
            srand((unsigned int)(time(NULL) ^ bb->sample_counter ^ (unsigned)note));
            bb->pending_trigger  = 1;
            bb->pending_beat_pos = bb->trigger_count % 8;
        }
    } else {
        bb_perf_macro_off(&bb->perf, pad.index);
    }
}

static void bb_set_param(void *instance, const char *key, const char *val) {
    breakbeat_t *bb = (breakbeat_t *)instance;
    if (!bb || !key || !val) return;
    
    if (g_host && g_host->log) {
        char log_buf[256];
        snprintf(log_buf, sizeof(log_buf), "BB: set_param key=%s, val=%s", key, val);
        g_host->log(log_buf);
    }
    
    if (strcmp(key, "preset") == 0 || strcmp(key, "preset_index") == 0) {
        bb->preset_idx = atoi(val);
        
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "breakbeat: set preset=%d, total=%d", bb->preset_idx, g_total_presets);
        wp_log(dbg);
        
        if (bb->preset_idx < 0) bb->preset_idx = 0;
        if (bb->preset_idx >= g_total_presets) bb->preset_idx = g_total_presets - 1;
        
        if (g_total_presets == 0) return;
        
        char path[512];
        snprintf(path, sizeof(path), "%s/presets/%s", bb->module_dir, g_preset_filenames[bb->preset_idx]);
        
        char *json = read_file_to_string(path);
        if (json) {
            apply_preset_json(bb, json);
            free(json);
        }

        int is_running = 0;
        if (g_host && g_host->get_clock_status) {
            is_running = (g_host->get_clock_status() == 2);
        }

        if (!is_running) {
            apply_sample_path(bb, bb->main_sample_path, bb->main_length);
            bb->pending_sample_path[0] = '\0';
            bb_start_preview(bb);
        } else {
            snprintf(bb->pending_sample_path, sizeof(bb->pending_sample_path),
                     "%s", bb->main_sample_path);
            bb->pending_main_length = bb->main_length;
        }
    }
    else if (strcmp(key, "A_sample_path") == 0) {
        resolve_sample_path(bb->module_dir, val, bb->main_sample_path, sizeof(bb->main_sample_path));

        int is_running = 0;
        if (g_host && g_host->get_clock_status) {
            is_running = (g_host->get_clock_status() == 2);
        }

        if (!is_running) {
            apply_sample_path(bb, bb->main_sample_path, bb->main_length);
            bb->pending_sample_path[0] = '\0';
            bb_start_preview(bb);
        } else {
            snprintf(bb->pending_sample_path, sizeof(bb->pending_sample_path),
                     "%s", bb->main_sample_path);
            bb->pending_main_length = bb->main_length;
        }
    }
    else if (strcmp(key, "B_sample_path") == 0) {
        resolve_sample_path(bb->module_dir, val, bb->alt_sample_path, sizeof(bb->alt_sample_path));
    }
    else if (strcmp(key, "A_sample_length") == 0 || strcmp(key, "length") == 0) {
        int idx = atoi(val);
        float lengths[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
        if (idx >= 0 && idx < 6) {
            bb->length = lengths[idx];
            bb->main_length = lengths[idx];
            /* active_length only updates when the sample actually loads,
             * so the rate stays correct until the bar boundary. */
        }
    }
    else if (strcmp(key, "B_sample_length") == 0 || strcmp(key, "alt_length") == 0) {
        int idx = atoi(val);
        float lengths[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
        if (idx >= 0 && idx < 6) {
            bb->alt_length = lengths[idx];
        }
    }
    else if (strcmp(key, "complexity") == 0) {
        bb->complexity = atof(val) / 100.0f;
        if (bb->complexity < 0.0f) bb->complexity = 0.0f;
        if (bb->complexity > 1.0f) bb->complexity = 1.0f;
    }
    else if (strcmp(key, "anchor") == 0) {
        bb->anchor = atof(val) / 100.0f;
        if (bb->anchor < 0.0f) bb->anchor = 0.0f;
        if (bb->anchor > 1.0f) bb->anchor = 1.0f;
    }
    else if (strcmp(key, "roll") == 0) {
        bb->roll = atof(val) / 100.0f;
        if (bb->roll < 0.0f) bb->roll = 0.0f;
        if (bb->roll > 1.0f) bb->roll = 1.0f;
    }
    else if (strcmp(key, "phrase") == 0) {
        int idx = -1;
        const char *phrase_names[] = {"Off", "2 bars", "4 bars", "8 bars", "16 bars"};
        for (int i = 0; i < 5; i++) {
            if (strcmp(phrase_names[i], val) == 0) {
                idx = i;
                break;
            }
        }
        if (idx < 0) idx = atoi(val);
        
        static const int phrase_values[] = {0, 2, 4, 8, 16};
        if (idx >= 0 && idx < 5) {
            bb->phrase_bars = phrase_values[idx];
        }
    }
    else if (strcmp(key, "fill") == 0) {
        bb->fill = atof(val) / 100.0f;
        if (bb->fill < 0.0f) bb->fill = 0.0f;
        if (bb->fill > 1.0f) bb->fill = 1.0f;
    }
    else if (strcmp(key, "retrig_2x") == 0) {
        bb->retrig_p[0] = atof(val) / 100.0f;
        if (bb->retrig_p[0] < 0.0f) bb->retrig_p[0] = 0.0f;
        if (bb->retrig_p[0] > 1.0f) bb->retrig_p[0] = 1.0f;
    }
    else if (strcmp(key, "retrig_3x") == 0) {
        bb->retrig_p[1] = atof(val) / 100.0f;
        if (bb->retrig_p[1] < 0.0f) bb->retrig_p[1] = 0.0f;
        if (bb->retrig_p[1] > 1.0f) bb->retrig_p[1] = 1.0f;
    }
    else if (strcmp(key, "retrig_4x") == 0) {
        bb->retrig_p[2] = atof(val) / 100.0f;
        if (bb->retrig_p[2] < 0.0f) bb->retrig_p[2] = 0.0f;
        if (bb->retrig_p[2] > 1.0f) bb->retrig_p[2] = 1.0f;
    }
    else if (strcmp(key, "retrig_8x") == 0) {
        bb->retrig_p[3] = atof(val) / 100.0f;
        if (bb->retrig_p[3] < 0.0f) bb->retrig_p[3] = 0.0f;
        if (bb->retrig_p[3] > 1.0f) bb->retrig_p[3] = 1.0f;
    }
    else if (strcmp(key, "B_chance") == 0) {
        bb->swap_prob = atof(val) / 100.0f;
        if (bb->swap_prob < 0.0f) bb->swap_prob = 0.0f;
        if (bb->swap_prob > 1.0f) bb->swap_prob = 1.0f;
    }
    else if (strcmp(key, "save_preset") == 0) {
        int trigger = atoi(val);
        if (trigger == 1) {
            int len_idx = 2;
            if (bb->length == 0.25f) len_idx = 0;
            else if (bb->length == 0.5f) len_idx = 1;
            else if (bb->length == 1.0f) len_idx = 2;
            else if (bb->length == 2.0f) len_idx = 3;
            else if (bb->length == 4.0f) len_idx = 4;
            else if (bb->length == 8.0f) len_idx = 5;

            int phrase_idx = 0;
            if (bb->phrase_bars == 2) phrase_idx = 1;
            else if (bb->phrase_bars == 4) phrase_idx = 2;
            else if (bb->phrase_bars == 8) phrase_idx = 3;
            else if (bb->phrase_bars == 16) phrase_idx = 4;

            int n = 4;
            char filename[64];
            char path[512];
            while (n < 100) {
                snprintf(filename, sizeof(filename), "%d_preset.json", n);
                snprintf(path, sizeof(path), "%s/presets/%s", bb->module_dir, filename);
                if (access(path, F_OK) == -1) {
                    break;
                }
                n++;
            }

            FILE *f = fopen(path, "w");
            if (f) {
                const char *lengths[] = {"1/4 bar", "1/2 bar", "1 bar", "2 bars", "4 bars", "8 bars"};
                const char *phrases[] = {"Off", "2 bars", "4 bars", "8 bars", "16 bars"};

                int alt_len_idx = 2;
                if (bb->alt_length == 0.25f) alt_len_idx = 0;
                else if (bb->alt_length == 0.5f) alt_len_idx = 1;
                else if (bb->alt_length == 1.0f) alt_len_idx = 2;
                else if (bb->alt_length == 2.0f) alt_len_idx = 3;
                else if (bb->alt_length == 4.0f) alt_len_idx = 4;
                else if (bb->alt_length == 8.0f) alt_len_idx = 5;

                fprintf(f, "{\n");
                fprintf(f, "  \"name\": \"Preset %d\",\n", n);
                fprintf(f, "  \"A_sample_path\": \"%s\",\n", bb->main_sample_path);
                fprintf(f, "  \"A_sample_length\": \"%s\",\n", lengths[len_idx]);
                fprintf(f, "  \"B_sample_path\": \"%s\",\n", bb->alt_sample_path);
                fprintf(f, "  \"B_sample_length\": \"%s\",\n", lengths[alt_len_idx]);
                fprintf(f, "  \"B_chance\": %d,\n", (int)(bb->swap_prob * 100.0f));
                fprintf(f, "  \"complexity\": %d,\n", (int)(bb->complexity * 100.0f));
                fprintf(f, "  \"phrase\": \"%s\",\n", phrases[phrase_idx]);
                fprintf(f, "  \"anchor\": %d,\n", (int)(bb->anchor * 100.0f));
                fprintf(f, "  \"roll\": %d,\n", (int)(bb->roll * 100.0f));
                fprintf(f, "  \"fill\": %d,\n", (int)(bb->fill * 100.0f));
                fprintf(f, "  \"retrig_2x\": %d,\n", (int)(bb->retrig_p[0] * 100.0f));
                fprintf(f, "  \"retrig_3x\": %d,\n", (int)(bb->retrig_p[1] * 100.0f));
                fprintf(f, "  \"retrig_4x\": %d,\n", (int)(bb->retrig_p[2] * 100.0f));
                fprintf(f, "  \"retrig_8x\": %d\n",  (int)(bb->retrig_p[3] * 100.0f));
                fprintf(f, "}\n");
                fclose(f);
                
                wp_log("breakbeat: saved preset file");
                
                // Rescan presets!
                scan_presets(bb->module_dir);
            } else {
                wp_log("breakbeat: failed to save preset file");
            }
        }
    }
    else if (strcmp(key, "state") == 0) {
        json_get_int(val, "preset_index", &bb->preset_idx);
        if (g_total_presets > 0) {
            if (bb->preset_idx < 0) bb->preset_idx = 0;
            if (bb->preset_idx >= g_total_presets) bb->preset_idx = g_total_presets - 1;
        } else {
            bb->preset_idx = 0;
        }

        char path_str[BB_PATH_MAX];
        if (json_get_string(val, "sample_path", path_str, sizeof(path_str))) {
            resolve_sample_path(bb->module_dir, path_str, bb->main_sample_path, sizeof(bb->main_sample_path));
            snprintf(bb->pending_sample_path, sizeof(bb->pending_sample_path), "%s", bb->main_sample_path);
            char dbg[BB_PATH_MAX + 64];
            snprintf(dbg, sizeof(dbg), "breakbeat: state restore sample_path=%s", bb->main_sample_path);
            wp_log(dbg);
        }

        if (json_get_string(val, "alt_sample_path", path_str, sizeof(path_str))) {
            resolve_sample_path(bb->module_dir, path_str, bb->alt_sample_path, sizeof(bb->alt_sample_path));
        }

        int i;
        if (json_get_int(val, "length", &i)) {
            static const float lengths[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
            if (i >= 0 && i < 6) bb->length = lengths[i];
        }
        if (json_get_int(val, "complexity", &i)) {
            bb->complexity = (float)i / 100.0f;
            if (bb->complexity < 0.0f) bb->complexity = 0.0f;
            if (bb->complexity > 1.0f) bb->complexity = 1.0f;
        }
        if (json_get_int(val, "anchor", &i)) {
            bb->anchor = (float)i / 100.0f;
            if (bb->anchor < 0.0f) bb->anchor = 0.0f;
            if (bb->anchor > 1.0f) bb->anchor = 1.0f;
        }
        if (json_get_int(val, "roll", &i)) {
            bb->roll = (float)i / 100.0f;
            if (bb->roll < 0.0f) bb->roll = 0.0f;
            if (bb->roll > 1.0f) bb->roll = 1.0f;
        }
        if (json_get_int(val, "phrase", &i)) {
            static const int phrase_values[] = {0, 2, 4, 8, 16};
            if (i < 0) i = 0;
            if (i > 4) i = 4;
            bb->phrase_bars = phrase_values[i];
        }
        if (json_get_int(val, "fill", &i)) {
            bb->fill = (float)i / 100.0f;
            if (bb->fill < 0.0f) bb->fill = 0.0f;
            if (bb->fill > 1.0f) bb->fill = 1.0f;
        }
        if (json_get_int(val, "retrig_2x", &i)) bb->retrig_p[0] = (float)i / 100.0f;
        if (json_get_int(val, "retrig_3x", &i)) bb->retrig_p[1] = (float)i / 100.0f;
        if (json_get_int(val, "retrig_4x", &i)) bb->retrig_p[2] = (float)i / 100.0f;
        if (json_get_int(val, "retrig_8x", &i)) bb->retrig_p[3] = (float)i / 100.0f;
        if (json_get_int(val, "swap_prob", &i)) {
            bb->swap_prob = (float)i / 100.0f;
            if (bb->swap_prob < 0.0f) bb->swap_prob = 0.0f;
            if (bb->swap_prob > 1.0f) bb->swap_prob = 1.0f;
        }

        /* Apply the sample immediately when the transport is not running.
         * Deferring via pending_sample_path requires a MIDI clock bar boundary
         * that may never arrive if the module is just loading. */
        {
            int is_running = 0;
            if (g_host && g_host->get_clock_status)
                is_running = (g_host->get_clock_status() == 2);
            if (!is_running) {
                apply_sample_path(bb, bb->main_sample_path, bb->main_length);
                bb->pending_sample_path[0] = '\0';
            }
        }
    }
    else {
        /* Log any key the host sends that we don't handle — helps spot mute,
         * bypass, or other host signals that might affect audio output. */
        char buf[128];
        snprintf(buf, sizeof(buf), "breakbeat: unhandled set_param key='%s' val='%.64s'", key, val);
        wp_log(buf);
    }
}

static int bb_get_param(void *instance, const char *key, char *buf, int buf_len) {
    breakbeat_t *bb = (breakbeat_t *)instance;
    if (!bb || !key || !buf || buf_len < 2) return -1;
    

    if (strcmp(key, "ui_hierarchy") == 0) {
        build_ui_hierarchy(bb, buf, buf_len);
        return (int)strlen(buf);
    }
    if (strcmp(key, "chain_params") == 0) {
        build_chain_params(bb, buf, buf_len);
        return (int)strlen(buf);
    }
    if (strcmp(key, "preset_count") == 0) {
        return snprintf(buf, buf_len, "%d", g_total_presets);
    }
    else if (strcmp(key, "preset") == 0 || strcmp(key, "preset_index") == 0) {
        return snprintf(buf, buf_len, "%d", bb->preset_idx);
    }
    else if (strcmp(key, "preset_name") == 0) {
        int idx = bb->preset_idx;
        if (idx >= 0 && idx < g_total_presets) {
            char name[64];
            strncpy(name, g_preset_filenames[idx], sizeof(name));
            char *ext = strrchr(name, '.');
            if (ext) *ext = '\0'; // Strip .json
            return snprintf(buf, buf_len, "%s", name);
        }
        return snprintf(buf, buf_len, "unknown");
    }
    else if (strcmp(key, "save_preset") == 0) {
        return snprintf(buf, buf_len, "0");
    }
    else if (strcmp(key, "status") == 0) {
        /* While a performance pad/macro is held, show the live readout (e.g.
         * "A:3 .5x REV") in the visible Status param; otherwise fall back to the
         * engine's own status (e.g. "A_3_1x"). get_param is polled live, so this
         * updates in real time without any host-side overlay. */
        int n = bb_perf_status_str(&bb->perf, bb->current_slice,
                                   bb->current_loop, buf, buf_len);
        if (n > 0) return n;
        return snprintf(buf, buf_len, "%s", bb->status_str);
    }
    else if (strcmp(key, "perf_status") == 0) {
        /* Same live readout under its own key, for a future host-side overlay;
         * "" when nothing is being manually triggered. */
        return bb_perf_status_str(&bb->perf, bb->current_slice,
                                  bb->current_loop, buf, buf_len);
    }
    else if (strcmp(key, "A_sample_path") == 0 || strcmp(key, "loop") == 0) {
        char dbg[BB_PATH_MAX + 64];
        snprintf(dbg, sizeof(dbg), "breakbeat: get_param sample_path -> %s", bb->main_sample_path);
        wp_log(dbg);
        return snprintf(buf, buf_len, "%s", bb->main_sample_path);
    }
    else if (strcmp(key, "A_sample_length") == 0) {
        int idx = 2; // Default to 1.0
        if (bb->length == 0.25f) idx = 0;
        else if (bb->length == 0.5f) idx = 1;
        else if (bb->length == 1.0f) idx = 2;
        else if (bb->length == 2.0f) idx = 3;
        else if (bb->length == 4.0f) idx = 4;
        else if (bb->length == 8.0f) idx = 5;
        return snprintf(buf, buf_len, "%d", idx);
    }
    else if (strcmp(key, "B_sample_length") == 0) {
        int idx = 2; // Default to 1.0
        if (bb->alt_length == 0.25f) idx = 0;
        else if (bb->alt_length == 0.5f) idx = 1;
        else if (bb->alt_length == 1.0f) idx = 2;
        else if (bb->alt_length == 2.0f) idx = 3;
        else if (bb->alt_length == 4.0f) idx = 4;
        else if (bb->alt_length == 8.0f) idx = 5;
        return snprintf(buf, buf_len, "%d", idx);
    }
    else if (strcmp(key, "complexity") == 0) {
        return snprintf(buf, buf_len, "%d", (int)(bb->complexity * 100.0f));
    }
    else if (strcmp(key, "anchor") == 0) {
        return snprintf(buf, buf_len, "%d", (int)(bb->anchor * 100.0f));
    }
    else if (strcmp(key, "roll") == 0) {
        return snprintf(buf, buf_len, "%d", (int)(bb->roll * 100.0f));
    }
    else if (strcmp(key, "phrase") == 0) {
        int idx = 0;
        if (bb->phrase_bars == 2) idx = 1;
        else if (bb->phrase_bars == 4) idx = 2;
        else if (bb->phrase_bars == 8) idx = 3;
        else if (bb->phrase_bars == 16) idx = 4;
        return snprintf(buf, buf_len, "%d", idx);
    }
    else if (strcmp(key, "B_sample_path") == 0 || strcmp(key, "alt_loop") == 0) {
        return snprintf(buf, buf_len, "%s", bb->alt_sample_path);
    }
    else if (strcmp(key, "fill") == 0) {
        return snprintf(buf, buf_len, "%d", (int)(bb->fill * 100.0f));
    }
    else if (strcmp(key, "retrig_2x") == 0) {
        return snprintf(buf, buf_len, "%d", (int)(bb->retrig_p[0] * 100.0f));
    }
    else if (strcmp(key, "retrig_3x") == 0) {
        return snprintf(buf, buf_len, "%d", (int)(bb->retrig_p[1] * 100.0f));
    }
    else if (strcmp(key, "retrig_4x") == 0) {
        return snprintf(buf, buf_len, "%d", (int)(bb->retrig_p[2] * 100.0f));
    }
    else if (strcmp(key, "retrig_8x") == 0) {
        return snprintf(buf, buf_len, "%d", (int)(bb->retrig_p[3] * 100.0f));
    }
    else if (strcmp(key, "B_chance") == 0) {
        return snprintf(buf, buf_len, "%d", (int)(bb->swap_prob * 100.0f));
    }
    else if (strcmp(key, "state") == 0) {
        int len_idx = 2; // Default to 1.0
        if (bb->length == 0.25f) len_idx = 0;
        else if (bb->length == 0.5f) len_idx = 1;
        else if (bb->length == 1.0f) len_idx = 2;
        else if (bb->length == 2.0f) len_idx = 3;
        else if (bb->length == 4.0f) len_idx = 4;
        else if (bb->length == 8.0f) len_idx = 5;

        int phrase_idx = 0;
        if (bb->phrase_bars == 2) phrase_idx = 1;
        else if (bb->phrase_bars == 4) phrase_idx = 2;
        else if (bb->phrase_bars == 8) phrase_idx = 3;
        else if (bb->phrase_bars == 16) phrase_idx = 4;

        return snprintf(buf, buf_len,
            "{\"preset\":%d,\"sample_path\":\"%s\",\"alt_sample_path\":\"%s\","
            "\"length\":%d,\"complexity\":%d,\"anchor\":%d,\"roll\":%d,\"phrase\":%d,\"fill\":%d,"
            "\"retrig_2x\":%d,\"retrig_3x\":%d,\"retrig_4x\":%d,\"retrig_8x\":%d,\"swap_prob\":%d}",
            bb->preset_idx,
            bb->main_sample_path, bb->alt_sample_path,
            len_idx,
            (int)(bb->complexity * 100.0f), (int)(bb->anchor * 100.0f),
            (int)(bb->roll * 100.0f), phrase_idx, (int)(bb->fill * 100.0f),
            (int)(bb->retrig_p[0] * 100.0f), (int)(bb->retrig_p[1] * 100.0f),
            (int)(bb->retrig_p[2] * 100.0f), (int)(bb->retrig_p[3] * 100.0f),
            (int)(bb->swap_prob * 100.0f));
    }
    
    return -1;
}

static void bb_render_block(void *instance, int16_t *out_lr, int frames) {
    breakbeat_t *bb = (breakbeat_t *)instance;

    /* Log once on the very first render call so we can see the initial state
     * in the log even when the user never hears audio. */
    if (bb && bb->dbg_first_render) {
        bb->dbg_first_render = 0;
        char buf[BB_PATH_MAX + 96];
        int clock_status = (g_host && g_host->get_clock_status) ? g_host->get_clock_status() : -1;
        snprintf(buf, sizeof(buf),
                 "breakbeat: first render — data=%s frames=%u playing=%d clock_status=%d sample=%s",
                 bb->data ? "ok" : "NULL",
                 bb->total_frames,
                 bb->playing,
                 clock_status,
                 bb->main_sample_path);
        wp_log(buf);
    }

    if (!bb || !bb->data || bb->total_frames == 0) {
        if (bb && bb->dbg_silence_reason != 1) {
            bb->dbg_silence_reason = 1;
            wp_log("breakbeat: silence — no sample loaded");
        }
        memset(out_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    /* Transport state via get_clock_status().
     *
     * UNAVAILABLE (0) means MIDI Sync is not configured for output — the host
     * cannot report transport state. Treat as always running so the module
     * produces audio; transport sync requires MIDI Sync = Out on the device. */
    int clock_status = (g_host && g_host->get_clock_status)
                     ? g_host->get_clock_status() : MOVE_CLOCK_STATUS_RUNNING;
    int running = (clock_status == MOVE_CLOCK_STATUS_RUNNING)
               || (clock_status == MOVE_CLOCK_STATUS_UNAVAILABLE);

    if (clock_status != bb->dbg_last_clock_status) {
        char dbgbuf[64];
        snprintf(dbgbuf, sizeof(dbgbuf), "breakbeat: clock_status %d -> %d",
                 bb->dbg_last_clock_status, clock_status);
        wp_log(dbgbuf);
        bb->dbg_last_clock_status = clock_status;
    }

    /* While stopped, track the set tempo from get_bpm() directly.
     * With no MIDI clock running, get_bpm() returns the Move's set tempo knob
     * value — stable and accurate. Updating every block keeps stable_bpm current
     * so that if the user changes tempo while stopped, the first bar after
     * transport start uses the correct rate without waiting for MIDI clock. */
    if (!running && g_host && g_host->get_bpm) {
        float set_bpm = g_host->get_bpm();
        if (set_bpm >= 20.0f && set_bpm <= 400.0f) {
            bb->stable_bpm = set_bpm;
            /* If the cached samples_per_trigger implies a tempo that differs
             * >5% from the set tempo, it's stale (tempo changed while stopped).
             * Clear it so the first bar uses stable_bpm instead. */
            if (bb->samples_per_trigger > 0.0f) {
                /* spt → bpm: spt = spb*active_length/8, spb = SR*4*60/bpm
                 * → bpm = SR*4*60*active_length / (8*spt) */
                float cached_bpm = (MOVE_SAMPLE_RATE * 4.0f * 60.0f * bb->active_length)
                                 / (8.0f * bb->samples_per_trigger);
                float ratio = cached_bpm / set_bpm;
                if (ratio < 0.95f || ratio > 1.05f)
                    bb->samples_per_trigger = 0.0f;
            }
        }
    }

    /* Detect transport start. */
    if (running && !bb->was_running) {
        if (bb->fa_received) {
            /* 0xFA already reset everything. midi_tick has been counting since
             * then, so we know exactly how many ticks elapsed in the T_fa→T_run
             * gap. Use that to jump straight to the correct bar position instead
             * of always starting from slice 0. */
            bb->fa_received = 0;

            /* Estimate samples_per_trigger for the position calculation.
             * Prefer the cached measured value (accurate); fall back to
             * stable_bpm (= Move set-tempo knob, always correct while stopped). */
            float spt_est = bb->samples_per_trigger;
            if (spt_est <= 0.0f) {
                float spb = (MOVE_SAMPLE_RATE * 4.0f * 60.0f / bb->stable_bpm);
                spt_est = spb * bb->active_length / 8.0f;
            }

            /* How many complete trigger intervals elapsed during the gap? */
            int n = (bb->ticks_per_trigger > 0)
                  ? (bb->midi_tick / bb->ticks_per_trigger) : 0;
            int offset_ticks = (bb->ticks_per_trigger > 0)
                              ? (bb->midi_tick % bb->ticks_per_trigger) : 0;

            bb->trigger_count = n + 1; /* next tick-trigger gives beat n+1 */
            bb->current_slice = n % 8;
            bb->pending_trigger = 0;   /* handled by direct position set */

            /* Start play_pos part-way into the current slice based on tick offset. */
            float frac = (bb->ticks_per_trigger > 0)
                       ? ((float)offset_ticks / (float)bb->ticks_per_trigger) : 0.0f;
            bb->play_pos = (float)bb->slice_starts[bb->current_slice]
                         + frac * (float)bb->slice_lengths[bb->current_slice];
            bb->just_reset = 0; /* play_pos already set correctly */

            bb->dbg_last_trigger_sample = bb->sample_counter;
            bb->dbg_triggers_to_log = 10;

            char dbgbuf[96];
            snprintf(dbgbuf, sizeof(dbgbuf),
                     "breakbeat: T_run ticks=%d n=%d offset=%d → slice=%d frac=%.2f",
                     bb->midi_tick, n, offset_ticks, bb->current_slice, frac);
            wp_log(dbgbuf);
        } else {
            /* No 0xFA received (MIDI Sync = Off or firmware variant). */
            bb_reset_transport(bb);
        }
    }
    bb->was_running = running;

    int previewing = (bb->preview_frames > 0);
    if (!running && !previewing) {
        if (bb->dbg_silence_reason != 2) {
            bb->dbg_silence_reason = 2;
            wp_log("breakbeat: silence — transport stopped");
        }
        memset(out_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }
    if (previewing) {
        bb->preview_frames -= frames;
        if (bb->preview_frames < 0) bb->preview_frames = 0;
    }
    bb->dbg_silence_reason = 0;

    /* BPM stability filter — only runs while transport is running.
     *
     * get_bpm() returns bad (too-low) values at transport start due to MIDI
     * clock jitter; it also returns unreliable values while stopped. By only
     * filtering while running we avoid accumulating bpm_low_count across
     * stop/start cycles, which was poisoning stable_bpm after rapid restarts.
     *
     * Rules while running:
     *   - Snap up immediately to any higher reading (correct tempo or increase).
     *   - Drift down slowly if within 20% (normal fluctuation).
     *   - Reject >20% below stable unless sustained 200+ blocks (~580ms),
     *     indicating a genuine large tempo decrease.
     */
    /* ------------------------------------------------------------------ *
     * Timing                                                             *
     * ------------------------------------------------------------------ *
     * Primary mode  — MIDI clock (MIDI Sync = Out):                     *
     *   Triggers arrive via pending_trigger set in on_midi(0xF8).       *
     *   Rate = slice_length / samples_per_trigger (measured from ticks) *
     *                                                                    *
     * Fallback mode — no MIDI clock (MIDI Sync = Off / first block):    *
     *   Phase accumulator driven by stable_bpm (= Move set-tempo knob). *
     * ------------------------------------------------------------------ */

    /* samples_per_trigger for rate calculation.
     * Use the tick-measured value when available; otherwise derive from
     * the set tempo (accurate while stopped, stable while running). */
    float spt; /* samples per trigger interval */
    int tick_mode = (bb->samples_per_trigger > 0.0f);
    if (tick_mode) {
        spt = bb->samples_per_trigger;
    } else {
        float spb = ((float)MOVE_SAMPLE_RATE * 60.0f / bb->stable_bpm) * 4.0f;
        spt = spb * bb->active_length / 8.0f;
    }

    /* Heartbeat log every ~10 seconds. */
    if (bb->sample_counter - bb->dbg_last_heartbeat >= MOVE_SAMPLE_RATE * 10) {
        bb->dbg_last_heartbeat = bb->sample_counter;
        float dbg_bpm = (spt > 0.0f)
                      ? (MOVE_SAMPLE_RATE * 60.0f * 4.0f * bb->active_length / 8.0f / spt)
                      : bb->stable_bpm;
        char hbbuf[128];
        snprintf(hbbuf, sizeof(hbbuf),
                 "breakbeat: heartbeat bpm=%.1f loop=%c slice=%d ticks=%s",
                 dbg_bpm, bb->current_loop, bb->current_slice,
                 tick_mode ? "yes" : "no");
        wp_log(hbbuf);
    }

    /* Bar boundary ---------------------------------------------------- */
    if (tick_mode) {
        if (bb->pending_bar) {
            bb->pending_bar = 0;
            bb->bar_counter++;
            if (bb->pending_sample_path[0]) {
                /* Apply the queued sample and snap to slice 0 immediately.
                 * current_loop is updated from pending_loop HERE (not when
                 * scheduled) so the status never shows the next loop early.
                 * pending_trigger is cleared because the bar-boundary tick fires
                 * simultaneously and would otherwise overwrite play_pos. */
                bb->current_loop = bb->pending_loop;
                apply_sample_path(bb, bb->pending_sample_path, bb->pending_main_length);
                bb->pending_sample_path[0] = '\0';
                bb->current_slice = 0;
                bb->play_pos      = (float)bb->slice_starts[0];
                bb->trigger_count = 1;
                bb->pending_trigger = 0;
                bb->sub_slice_active  = 0;
                bb->sub_slice_counter = 0;
                snprintf(bb->status_str, sizeof(bb->status_str),
                         "%c_0_1x", bb->current_loop);
            }
            if (bb->phrase_bars > 0) {
                int bar_in_phrase = bb->bar_counter % bb->phrase_bars;
                /* Schedule B two bars before the end (loads on last bar).
                 * Schedule A return on the last bar (loads on first bar of next phrase). */
                if (bar_in_phrase == bb->phrase_bars - 2) {
                    if ((float)rand() / (float)RAND_MAX < bb->swap_prob && bb->alt_sample_path[0]) {
                        snprintf(bb->pending_sample_path, sizeof(bb->pending_sample_path),
                                 "%s", bb->alt_sample_path);
                        bb->pending_main_length = bb->alt_length;
                        bb->pending_loop = 'B';
                    }
                } else if (bar_in_phrase == bb->phrase_bars - 1) {
                    snprintf(bb->pending_sample_path, sizeof(bb->pending_sample_path),
                             "%s", bb->main_sample_path);
                    bb->pending_main_length = bb->main_length;
                    bb->pending_loop = 'A';
                }
            }
        }
    } else {
        /* Fallback: sample-counting bar boundary. */
        float spb = spt * 8.0f / bb->active_length;
        bb->bar_phase += (float)frames;
        while (bb->bar_phase >= spb) {
            bb->bar_phase -= spb;
            bb->bar_counter++;
            if (bb->pending_sample_path[0]) {
                bb->current_loop = bb->pending_loop;
                apply_sample_path(bb, bb->pending_sample_path, bb->pending_main_length);
                bb->pending_sample_path[0] = '\0';
                bb->trigger_phase = 0.0f;
                bb->current_slice = 0;
                bb->play_pos      = (float)bb->slice_starts[0];
                bb->trigger_count = 1;
                bb->sub_slice_active  = 0;
                bb->sub_slice_counter = 0;
                snprintf(bb->status_str, sizeof(bb->status_str),
                         "%c_0_1x", bb->current_loop);
            }
            if (bb->phrase_bars > 0) {
                int bar_in_phrase = bb->bar_counter % bb->phrase_bars;
                if (bar_in_phrase == bb->phrase_bars - 1) {
                    if ((float)rand() / (float)RAND_MAX < bb->swap_prob) {
                        snprintf(bb->pending_sample_path, sizeof(bb->pending_sample_path),
                                 "%s", bb->alt_sample_path);
                        bb->pending_main_length = bb->alt_length;
                        bb->pending_loop = 'B';
                    }
                } else if (bar_in_phrase == 0 && bb->bar_counter > 0) {
                    snprintf(bb->pending_sample_path, sizeof(bb->pending_sample_path),
                             "%s", bb->main_sample_path);
                    bb->pending_main_length = bb->main_length;
                    bb->pending_loop = 'A';
                }
            }
        }
    }

    /* Slice triggers -------------------------------------------------- */
    if (bb->just_reset) {
        bb->just_reset = 0;
        bb->play_pos = (float)bb->slice_starts[0];
    }

/* Shared trigger-fire logic used by both tick and fallback modes. */
#define BB_FIRE_TRIGGER(beat_pos) do {                                       \
    slice_inputs_t _in = {                                                   \
        .current_slice = bb->current_slice,                                  \
        .beat_position = (beat_pos),                                         \
        .complexity    = bb->complexity,                                     \
        .anchor        = bb->anchor,                                         \
        .roll          = bb->roll,                                           \
        .phrase_bars   = bb->phrase_bars,                                    \
        .fill          = bb->fill,                                           \
        .bar_in_phrase = bb->phrase_bars > 0                                 \
                       ? bb->bar_counter % bb->phrase_bars : 0,             \
    };                                                                       \
    int _bb_engine = (bb->complexity == 0.0f)                            \
                      ? (beat_pos)                                           \
                      : slice_select_next(&_in, bb_rand, NULL); \
    {   /* Live performance override of the engine's slice pick. */          \
        int _bb_held = -1;                                                   \
        bb_resolve_mode_t _bb_rm = bb_perf_resolve(&bb->perf, &_bb_held);    \
        if      (_bb_rm == BB_RESOLVE_HELD)   bb->current_slice = _bb_held;  \
        else if (_bb_rm == BB_RESOLVE_FREEZE) { /* keep current_slice */ }   \
        else if (_bb_rm == BB_RESOLVE_RANDOM) bb->current_slice =            \
                                          ((int)(bb_rand(NULL) * 8.0f)) & 7; \
        else                                  bb->current_slice = _bb_engine;\
    }             \
    bb->sub_slice_counter = 0;                                               \
    {   /* Roll each retrigger rate independently; pick one if any fire.      \
         * retrig_p[i] is a per-bar probability set by the user.             \
         * Convert to per-trigger using the exact inverse binomial formula:  \
         *   p_trig = 1 - (1 - p_bar)^(1/N)  where N = triggers per bar     \
         * This ensures 100% always fires and 0% never fires, and all        \
         * intermediate values are perceptually proportional to bar odds.    \
         * N = 8 / active_length (e.g. 8 for 1-bar, 4 for 2-bar loops).    */ \
        static const int _rdivs[4] = {2, 3, 4, 8};                          \
        float _tpb = (bb->active_length > 0.0f)                             \
                   ? (8.0f / bb->active_length) : 8.0f;                     \
        float _inv_tpb = 1.0f / _tpb;                                       \
        int _fired[4], _n = 0;                                               \
        for (int _i = 0; _i < 4; _i++) {                                    \
            float _pb = bb->retrig_p[_i];                                   \
            if (_pb <= 0.0f) continue;                                       \
            float _p = (_pb >= 1.0f) ? 1.0f                                 \
                                     : 1.0f - powf(1.0f - _pb, _inv_tpb);  \
            if ((float)rand() / (float)RAND_MAX < _p)                       \
                _fired[_n++] = _i;                                           \
        }                                                                    \
        if (_n > 0) {                                                        \
            bb->sub_slice_active = 1;                                        \
            bb->retrigger_divisions = _rdivs[_fired[rand() % _n]];          \
        } else {                                                              \
            bb->sub_slice_active = 0;                                        \
        }                                                                    \
    }                                                                        \
    if (bb->perf.stutter_div > 0) { /* Stutter macro forces sub-slice retrigger. */ \
        bb->sub_slice_active   = 1;                                          \
        bb->retrigger_divisions = bb->perf.stutter_div;                      \
    }                                                                        \
    snprintf(bb->status_str, sizeof(bb->status_str), "%c_%d_%dx",           \
             bb->current_loop, bb->current_slice,                            \
             bb->sub_slice_active ? bb->retrigger_divisions : 1);           \
    bb->play_pos = (float)bb->slice_starts[bb->current_slice];              \
    {   /* diagnostic log */                                                 \
        uint64_t _now = bb->sample_counter;                                  \
        uint64_t _iv  = _now - bb->dbg_last_trigger_sample;                 \
        bb->dbg_last_trigger_sample = _now;                                  \
        int _log = (bb->dbg_triggers_to_log > 0);                           \
        if (!_log && spt > 0.0f) {                                           \
            _log = (_iv < (uint64_t)(spt*0.9f) || _iv > (uint64_t)(spt*1.1f)); \
        }                                                                    \
        if (_log) {                                                          \
            if (bb->dbg_triggers_to_log > 0) bb->dbg_triggers_to_log--;    \
            char _tb[128];                                                   \
            snprintf(_tb, sizeof(_tb),                                       \
                "breakbeat: trig beat=%d slice=%d spt=%.0f interval=%llu",  \
                (beat_pos), bb->current_slice, spt,                         \
                (unsigned long long)_iv);                                    \
            wp_log(_tb);                                                     \
        }                                                                    \
    }                                                                        \
} while(0)

    if (tick_mode) {
        if (bb->pending_trigger) {
            bb->pending_trigger = 0;
            /* ½× gates triggers so the slice plays across two clock intervals
             * (half-time); skip means let the current slice keep sounding. 2×
             * still fires every trigger and repeats the slice within the
             * interval (intra-slice loop below). */
            if (bb_perf_trigger_fires(&bb->perf, &bb->perf_trig_acc))
                BB_FIRE_TRIGGER(bb->pending_beat_pos);
        }
    } else {
        /* Fallback: phase accumulator. */
        bb->trigger_phase += (float)frames;
        while (bb->trigger_phase >= spt) {
            bb->trigger_phase -= spt;
            int bp = bb->trigger_count % 8;
            bb->trigger_count++;
            if (bb_perf_trigger_fires(&bb->perf, &bb->perf_trig_acc))
                BB_FIRE_TRIGGER(bp);
        }
    }
#undef BB_FIRE_TRIGGER

    /* Rate: slice_length / samples_per_trigger
     * This is algebraically identical to total_frames / (samples_per_bar * active_length)
     * but derived purely from measured tick timing with no BPM math. */
    float slice_len = (bb->current_slice < 8)
                    ? (float)bb->slice_lengths[bb->current_slice] : 0.0f;
    float rate = (slice_len > 0.0f && spt > 0.0f) ? slice_len / spt : 1.0f;
    rate *= bb->perf.rate_mult;   /* live ½×/2× macros */

    const int nch = bb->num_channels;
    const int is_float = (bb->audio_format == WAV_FORMAT_FLOAT);
    const int bits = bb->bits_per_sample;
    
    for (int i = 0; i < frames; i++) {
        uint32_t idx = (uint32_t)bb->play_pos;
        if (idx >= bb->total_frames) {
            bb->play_pos = 0; // Loop always for now
            idx = 0;
        }
        
        float fL, fR;
        if (is_float) {
            /* 32-bit IEEE float */
            const float *fdata = (const float *)bb->data;
            if (nch == 1) {
                fL = fR = fdata[idx];
            } else {
                fL = fdata[idx * 2];
                fR = fdata[idx * 2 + 1];
            }
        } else if (bits == 16) {
            const int16_t *sdata = (const int16_t *)bb->data;
            if (nch == 1) {
                fL = fR = sdata[idx] / 32768.0f;
            } else {
                fL = sdata[idx * 2]     / 32768.0f;
                fR = sdata[idx * 2 + 1] / 32768.0f;
            }
        } else if (bits == 24) {
            /* 24-bit PCM, 3 bytes per sample, little-endian signed */
            const uint8_t *bdata = (const uint8_t *)bb->data;
            uint32_t base = idx * (uint32_t)nch * 3u;
            int32_t l = bdata[base] | (bdata[base + 1] << 8) | (bdata[base + 2] << 16);
            if (l & 0x800000) l |= (int32_t)0xFF000000;
            int32_t r = l;
            if (nch == 2) {
                r = bdata[base + 3] | (bdata[base + 4] << 8) | (bdata[base + 5] << 16);
                if (r & 0x800000) r |= (int32_t)0xFF000000;
            }
            fL = (float)l / 8388608.0f;
            fR = (float)r / 8388608.0f;
        } else if (bits == 32) {
            /* 32-bit signed PCM */
            const int32_t *sdata = (const int32_t *)bb->data;
            if (nch == 1) {
                fL = fR = (float)sdata[idx] / 2147483648.0f;
            } else {
                fL = (float)sdata[idx * 2]     / 2147483648.0f;
                fR = (float)sdata[idx * 2 + 1] / 2147483648.0f;
            }
        } else {
            fL = fR = 0.0f;
        }
        
        int32_t L = (int32_t)(fL * 32767.0f);
        int32_t R = (int32_t)(fR * 32767.0f);
        
        if (L > 32767) L = 32767; else if (L < -32768) L = -32768;
        if (R > 32767) R = 32767; else if (R < -32768) R = -32768;
        
        out_lr[i * 2]     = (int16_t)L;
        out_lr[i * 2 + 1] = (int16_t)R;
        
        if (bb->sub_slice_active) {
            uint32_t start = bb->slice_starts[bb->current_slice];
            uint32_t len = bb->slice_lengths[bb->current_slice];
            uint32_t part_len = len / bb->retrigger_divisions;
            
            if (bb->play_pos >= start + part_len) {
                if (bb->sub_slice_counter < bb->retrigger_divisions - 1) {
                    bb->play_pos = start; // Loop back!
                    bb->sub_slice_counter++;
                }
            }
        }
        
        if (bb->perf.reverse) {
            /* Live Reverse: step backward, wrapping within the slice bounds. */
            uint32_t _rs = bb->slice_starts[bb->current_slice];
            uint32_t _rl = bb->slice_lengths[bb->current_slice];
            bb->play_pos -= rate;
            if (bb->play_pos < (float)_rs)
                bb->play_pos = (float)(_rs + (_rl > 0 ? _rl - 1 : 0));
        } else {
            bb->play_pos += rate;
            /* Live 2× (double-time): the slice plays at double rate, so it
             * reaches its end mid-interval. Loop it back to the start to repeat
             * within the same clock interval ("plays in half the time then
             * re-triggers") instead of bleeding into the next slice's audio. */
            if (bb->perf.rate_mult > 1.0f) {
                uint32_t _ds = bb->slice_starts[bb->current_slice];
                uint32_t _dl = bb->slice_lengths[bb->current_slice];
                if (_dl > 0 && bb->play_pos >= (float)(_ds + _dl))
                    bb->play_pos = (float)_ds;
            }
        }
    }
    bb->sample_counter += frames;
}

static plugin_api_v2_t g_plugin_api_v2 = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = bb_create_instance,
    .destroy_instance = bb_destroy_instance,
    .on_midi = bb_on_midi,
    .set_param = bb_set_param,
    .get_param = bb_get_param,
    .get_error = NULL,
    .render_block = bb_render_block
};

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    if (g_host && g_host->log) g_host->log("breakbeat: plugin initialized");
    return &g_plugin_api_v2;
}
