#define _GNU_SOURCE
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
#include "bb_timing.h"
#include <time.h>
#include <math.h>
#include <stdatomic.h>
#include <sched.h>
#include <pthread.h>

/* WAV audio format codes */
#define WAV_FORMAT_PCM   1
#define WAV_FORMAT_FLOAT 3

#define BB_PATH_MAX 512

typedef struct {
    int fd;
    void *map;
    size_t map_size;
    void *data;
    uint32_t total_frames;
    int num_channels;
    int audio_format;
    int bits_per_sample;
    uint32_t slice_starts[8];
    uint32_t slice_lengths[8];
    float musical_length;
} bb_sample_slot_t;

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
    int suppress_next_preset_preview; /* first host preset assignment is restore */

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
    bb_timing_t timing;            /* authoritative Start/Stop + 24 PPQN phase */

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

    uint32_t rng_state;  /* per-instance xorshift PRNG; never takes libc rand() locks */

    /* The inactive A/B sample stays mapped so phrase transitions only swap
     * in-memory metadata. No file operation is permitted in render_block. */
    bb_sample_slot_t standby_sample;
    char standby_loop;
    int pending_sample_switch;

    /* Non-blocking reader gate. Parameter changes may replace the active map
     * from a control thread; render never waits and emits silence while the
     * control thread waits for any in-flight block to finish. */
    atomic_int sample_update;
    atomic_int sample_readers;

    /* Filepath parameters arrive on Schwung's realtime SPI callback.  Publish
     * fixed-size requests to a SCHED_OTHER worker; it performs open/mmap and
     * replaces the requested A/B slot behind sample_update. */
    pthread_t sample_loader_thread;
    atomic_int sample_loader_stop;
    atomic_int sample_loader_started;
    atomic_uint sample_request_seq[2];
    char sample_request_path[2][BB_PATH_MAX];
    float sample_request_length[2];

    /* Diagnostics — log key events once rather than every block. */
    int dbg_first_render;     /* 1 until first render_block call is logged */
    int dbg_silence_reason;   /* last silence reason code logged (0 = none yet) */
    int dbg_last_clock_status;/* last value returned by get_clock_status */
    uint64_t dbg_last_heartbeat; /* sample_counter at last periodic status log */

} breakbeat_t;

static const host_api_v1_t *g_host = NULL;
static char g_preset_filenames[32][64];
static int g_total_presets = 0;

static void bb_start_preview(breakbeat_t *bb);

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

static uint32_t bb_random_u32(breakbeat_t *bb) {
    uint32_t x = bb->rng_state;
    if (x == 0) x = 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    bb->rng_state = x;
    return x;
}

static float bb_rand(void *ctx) {
    breakbeat_t *bb = (breakbeat_t *)ctx;
    return (float)(bb_random_u32(bb) & 0x00ffffffu) / 16777216.0f;
}

static void bb_update_status(breakbeat_t *bb) {
    int div = bb->sub_slice_active ? bb->retrigger_divisions : 1;
    bb->status_str[0] = bb->current_loop;
    bb->status_str[1] = '_';
    bb->status_str[2] = (char)('0' + (bb->current_slice & 7));
    bb->status_str[3] = '_';
    bb->status_str[4] = (char)('0' + div);
    bb->status_str[5] = 'x';
    bb->status_str[6] = '\0';
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

static void close_sample_slot(bb_sample_slot_t *slot) {
    if (!slot) return;
    if (slot->map && slot->map != MAP_FAILED) munmap(slot->map, slot->map_size);
    if (slot->fd >= 0) close(slot->fd);
    memset(slot, 0, sizeof(*slot));
    slot->fd = -1;
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
    (void)module_dir;
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
        "{\"key\":\"save_preset\",\"label\":\"Save Preset\",\"type\":\"int\",\"min\":0,\"max\":1,\"access\":\"write\"},"
        "{\"key\":\"status\",\"label\":\"Status\",\"type\":\"enum\",\"options\":[\"-\"],\"access\":\"read\"}"
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
        "{\"key\":\"save_preset\",\"name\":\"Save Preset\",\"type\":\"int\",\"min\":0,\"max\":1,\"access\":\"write\"},"
        "{\"key\":\"status\",\"name\":\"Status\",\"type\":\"enum\",\"options\":[\"-\"],\"access\":\"read\"}"
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
    /* Pre-fault all pages on the loader thread. Without this, render_block would trigger OS page
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

static void move_active_sample_to_slot(breakbeat_t *src, bb_sample_slot_t *dst,
                                       float musical_length) {
    memset(dst, 0, sizeof(*dst));
    dst->fd = src->fd;
    dst->map = src->map;
    dst->map_size = src->map_size;
    dst->data = src->data;
    dst->total_frames = src->total_frames;
    dst->num_channels = src->num_channels;
    dst->audio_format = src->audio_format;
    dst->bits_per_sample = src->bits_per_sample;
    memcpy(dst->slice_starts, src->slice_starts, sizeof(dst->slice_starts));
    memcpy(dst->slice_lengths, src->slice_lengths, sizeof(dst->slice_lengths));
    dst->musical_length = musical_length;
    src->fd = -1;
    src->map = NULL;
    src->data = NULL;
}

static void install_loaded_sample(breakbeat_t *bb, char loop,
                                  bb_sample_slot_t *loaded) {
    atomic_store_explicit(&bb->sample_update, 1, memory_order_release);
    while (atomic_load_explicit(&bb->sample_readers, memory_order_acquire) > 0)
        sched_yield();

    /* A length knob may have moved while the worker was opening this file. */
    loaded->musical_length = (loop == 'A') ? bb->main_length : bb->alt_length;

    if (bb->current_loop == loop) {
        close_file(bb);
        bb->fd = loaded->fd;
        bb->map = loaded->map;
        bb->map_size = loaded->map_size;
        bb->data = loaded->data;
        bb->total_frames = loaded->total_frames;
        bb->num_channels = loaded->num_channels;
        bb->audio_format = loaded->audio_format;
        bb->bits_per_sample = loaded->bits_per_sample;
        memcpy(bb->slice_starts, loaded->slice_starts, sizeof(bb->slice_starts));
        memcpy(bb->slice_lengths, loaded->slice_lengths, sizeof(bb->slice_lengths));
        bb->active_length = loaded->musical_length;
        bb->ticks_per_trigger = (int)(12.0f * bb->active_length + 0.5f);
        if (bb->ticks_per_trigger < 1) bb->ticks_per_trigger = 1;
        /* Keep the transport's slice identity. A new file should replace the
         * sound under the playhead, not restart the sequencer at slice zero. */
        bb->current_slice &= 7;
        bb->play_pos = (float)bb->slice_starts[bb->current_slice];
        if (!bb->timing.running) bb_start_preview(bb);
        loaded->fd = -1;
        loaded->map = NULL;
        loaded->data = NULL;
    } else if (bb->standby_loop == loop) {
        close_sample_slot(&bb->standby_sample);
        bb->standby_sample = *loaded;
        loaded->fd = -1;
        loaded->map = NULL;
        loaded->data = NULL;
    }

    atomic_store_explicit(&bb->sample_update, 0, memory_order_release);
}

static void *sample_loader_main(void *arg) {
    breakbeat_t *bb = (breakbeat_t *)arg;

    /* pthread_create inherits Schwung's FIFO-90 callback priority. Demote and
     * leave core 3 to the SPI/audio callback before doing any work. */
#ifdef __linux__
    struct sched_param sp = { .sched_priority = 0 };
    (void)sched_setscheduler(0, SCHED_OTHER, &sp);
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(0, &cpus); CPU_SET(1, &cpus); CPU_SET(2, &cpus);
    (void)sched_setaffinity(0, sizeof(cpus), &cpus);
#endif

    unsigned consumed[2] = {0, 0};
    const struct timespec idle = { .tv_sec = 0, .tv_nsec = 5000000 };
    while (!atomic_load_explicit(&bb->sample_loader_stop, memory_order_acquire)) {
        int did_work = 0;
        for (int which = 0; which < 2; which++) {
            unsigned seq1 = atomic_load_explicit(&bb->sample_request_seq[which],
                                                 memory_order_acquire);
            if ((seq1 & 1u) || seq1 == consumed[which]) continue;

            char path[BB_PATH_MAX];
            float length;
            memcpy(path, bb->sample_request_path[which], sizeof(path));
            length = bb->sample_request_length[which];
            atomic_thread_fence(memory_order_acquire);
            unsigned seq2 = atomic_load_explicit(&bb->sample_request_seq[which],
                                                 memory_order_acquire);
            if (seq1 != seq2 || (seq2 & 1u)) continue;
            consumed[which] = seq2;
            did_work = 1;

            breakbeat_t tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.fd = -1;
            tmp.ticks_per_trigger = 12;
            if (open_wav(&tmp, path, length) == 0) {
                bb_sample_slot_t loaded;
                move_active_sample_to_slot(&tmp, &loaded, length);
                /* If the user chose another file while this one was loading,
                 * discard the stale result instead of briefly auditioning it. */
                if (atomic_load_explicit(&bb->sample_request_seq[which],
                                         memory_order_acquire) == seq2)
                    install_loaded_sample(bb, which == 0 ? 'A' : 'B', &loaded);
                close_sample_slot(&loaded);
            }
        }
        if (!did_work) nanosleep(&idle, NULL);
    }
    return NULL;
}

static void request_sample_load(breakbeat_t *bb, int which,
                                const char *path, float length) {
    if (!bb || which < 0 || which > 1 || !path || !path[0]) return;
    atomic_fetch_add_explicit(&bb->sample_request_seq[which], 1,
                              memory_order_acq_rel); /* odd: writer active */
    snprintf(bb->sample_request_path[which], BB_PATH_MAX, "%s", path);
    bb->sample_request_length[which] = length;
    atomic_fetch_add_explicit(&bb->sample_request_seq[which], 1,
                              memory_order_release); /* even: published */
}

static void set_loop_musical_length(breakbeat_t *bb, char loop, float length) {
    if (bb->current_loop == loop) {
        /* Playback rate is calculated from active_length every render block,
         * so this is audible immediately. The MIDI-clock trigger cadence uses
         * the new divisor beginning with the next tick. */
        bb->active_length = length;
        bb->ticks_per_trigger = (int)(12.0f * length + 0.5f);
        if (bb->ticks_per_trigger < 1) bb->ticks_per_trigger = 1;
        /* Length changes begin a fresh, deterministic slice cycle without
         * disturbing tick_in_bar, which owns the four-bar phrase grid. */
        bb_timing_reset_trigger_phase(&bb->timing);
        bb->pending_trigger = 0;
        bb->current_slice = 0;
        bb->play_pos = (float)bb->slice_starts[0];
        bb->sub_slice_active = 0;
        bb->sub_slice_counter = 0;
        bb_update_status(bb);
    } else if (bb->standby_loop == loop) {
        bb->standby_sample.musical_length = length;
    }
}

/* Load the non-playing A/B sample from a control-thread call. open_wav is
 * reused for validation, then ownership of the map moves into standby_sample. */
static int load_standby_sample(breakbeat_t *bb, const char *path, float expected_length) {
    if (!bb || !path || !path[0] || !has_wav_ext(path)) return -1;

    char resolved[BB_PATH_MAX * 2];
    resolve_sample_path(bb->module_dir, path, resolved, sizeof(resolved));

    breakbeat_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.fd = -1;
    tmp.ticks_per_trigger = 12;
    if (open_wav(&tmp, resolved, expected_length) != 0) return -1;

    close_sample_slot(&bb->standby_sample);
    bb->standby_sample.fd = tmp.fd;
    bb->standby_sample.map = tmp.map;
    bb->standby_sample.map_size = tmp.map_size;
    bb->standby_sample.data = tmp.data;
    bb->standby_sample.total_frames = tmp.total_frames;
    bb->standby_sample.num_channels = tmp.num_channels;
    bb->standby_sample.audio_format = tmp.audio_format;
    bb->standby_sample.bits_per_sample = tmp.bits_per_sample;
    memcpy(bb->standby_sample.slice_starts, tmp.slice_starts, sizeof(tmp.slice_starts));
    memcpy(bb->standby_sample.slice_lengths, tmp.slice_lengths, sizeof(tmp.slice_lengths));
    bb->standby_sample.musical_length = expected_length;

    /* Ownership moved; prevent tmp cleanup from touching the map. */
    tmp.fd = -1;
    tmp.map = NULL;
    return 0;
}

/* Swap active and standby sample ownership. This function performs no I/O,
 * allocation, locking, logging, or formatting and is safe at a bar boundary. */
static int activate_standby_sample(breakbeat_t *bb) {
    bb_sample_slot_t *s = &bb->standby_sample;
    if (!s->data || s->total_frames == 0) return -1;

    int old_fd = bb->fd;
    void *old_map = bb->map;
    size_t old_map_size = bb->map_size;
    void *old_data = bb->data;
    uint32_t old_total_frames = bb->total_frames;
    int old_channels = bb->num_channels;
    int old_format = bb->audio_format;
    int old_bits = bb->bits_per_sample;
    uint32_t old_starts[8], old_lengths[8];
    memcpy(old_starts, bb->slice_starts, sizeof(old_starts));
    memcpy(old_lengths, bb->slice_lengths, sizeof(old_lengths));
    float old_musical_length = bb->active_length;

    bb->fd = s->fd;
    bb->map = s->map;
    bb->map_size = s->map_size;
    bb->data = s->data;
    bb->total_frames = s->total_frames;
    bb->num_channels = s->num_channels;
    bb->audio_format = s->audio_format;
    bb->bits_per_sample = s->bits_per_sample;
    memcpy(bb->slice_starts, s->slice_starts, sizeof(bb->slice_starts));
    memcpy(bb->slice_lengths, s->slice_lengths, sizeof(bb->slice_lengths));
    bb->active_length = s->musical_length;

    s->fd = old_fd;
    s->map = old_map;
    s->map_size = old_map_size;
    s->data = old_data;
    s->total_frames = old_total_frames;
    s->num_channels = old_channels;
    s->audio_format = old_format;
    s->bits_per_sample = old_bits;
    memcpy(s->slice_starts, old_starts, sizeof(s->slice_starts));
    memcpy(s->slice_lengths, old_lengths, sizeof(s->slice_lengths));
    s->musical_length = old_musical_length;

    bb->ticks_per_trigger = (int)(12.0f * bb->active_length + 0.5f);
    if (bb->ticks_per_trigger < 1) bb->ticks_per_trigger = 1;
    bb->play_pos = (float)bb->slice_starts[0];
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

    atomic_store_explicit(&bb->sample_update, 1, memory_order_release);
    while (atomic_load_explicit(&bb->sample_readers, memory_order_acquire) > 0)
        sched_yield();
    int result = open_wav(bb, resolved, expected_length);
    atomic_store_explicit(&bb->sample_update, 0, memory_order_release);
    return result;
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
    if (load_standby_sample(bb, bb->alt_sample_path, bb->alt_length) == 0)
        bb->standby_loop = 'B';
    bb->pending_sample_path[0] = '\0';
    bb->pending_sample_switch = 0;
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
    bb->suppress_next_preset_preview = 1;
    bb->length = 1.0f;
    bb->main_length = 1.0f;
    bb->alt_length = 1.0f;
    bb->current_loop = 'A';
    bb->pending_loop = 'A';
    bb->standby_loop = 'B';
    bb->pending_sample_switch = 0;
    strcpy(bb->status_str, "A_0_1x");
    bb->sample_counter = 0;
    bb->trigger_phase = 0.0f;
    bb->bar_phase = 0.0f;
    bb->trigger_count = 0;
    bb->active_length = 1.0f;
    bb->pending_main_length = 1.0f;
    bb->was_running = 0;
    bb->ticks_per_trigger  = 12;   /* 1-bar default; updated when active_length changes */
    bb_timing_init(&bb->timing);
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
    bb->fd = -1;
    bb->standby_sample.fd = -1;
    atomic_init(&bb->sample_update, 0);
    atomic_init(&bb->sample_readers, 0);
    atomic_init(&bb->sample_loader_stop, 0);
    atomic_init(&bb->sample_loader_started, 0);
    atomic_init(&bb->sample_request_seq[0], 0);
    atomic_init(&bb->sample_request_seq[1], 0);
    bb->dbg_first_render = 1;
    bb->dbg_silence_reason = 0;
    bb->dbg_last_clock_status = -1;
    bb->dbg_last_heartbeat = 0;
    bb->rng_state = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)bb;

    /* Seed the stored tempo. get_bpm() already resolves to the Set tempo when
     * no clock is running -- its documented fallback chain is MIDI clock ->
     * set tempo -> settings -> 120 -- and nothing is running at instantiation,
     * so this is the Set's own tempo rather than a live estimate. */
    if (bb->stable_bpm < 20.0f && g_host && g_host->get_bpm) {
        float bpm = g_host->get_bpm();
        if (bpm >= 20.0f && bpm <= 400.0f) bb->stable_bpm = bpm;
    }

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

    if (pthread_create(&bb->sample_loader_thread, NULL,
                       sample_loader_main, bb) == 0)
        atomic_store_explicit(&bb->sample_loader_started, 1,
                              memory_order_release);

    return bb;
}

static void bb_destroy_instance(void *instance) {
    breakbeat_t *bb = (breakbeat_t *)instance;
    if (!bb) return;
    if (atomic_load_explicit(&bb->sample_loader_started, memory_order_acquire)) {
        atomic_store_explicit(&bb->sample_loader_stop, 1, memory_order_release);
        pthread_join(bb->sample_loader_thread, NULL);
    }
    close_file(bb);
    close_sample_slot(&bb->standby_sample);
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
    /* Every new song run starts with A, even if Stop arrived during the B
     * fill. Both samples are resident, so this metadata swap is RT-safe. */
    if (bb->current_loop != 'A' && bb->standby_loop == 'A') {
        char old_loop = bb->current_loop;
        if (activate_standby_sample(bb) == 0) {
            bb->current_loop = 'A';
            bb->standby_loop = old_loop;
        }
    }
    bb->trigger_count     = 0;
    bb->bar_counter       = 0;
    bb->current_slice     = 0;
    bb->sub_slice_active  = 0;
    bb->sub_slice_counter = 0;
    bb->just_reset        = 1;
    bb->bpm_low_count     = 0;
    bb->pending_trigger   = 0;
    bb->pending_bar       = 0;
    bb->last_trigger_sample = bb->sample_counter;
    bb->trigger_phase = 0.0f;
    bb->bar_phase     = 0.0f;
    bb->was_running = 1;
    bb->pending_sample_path[0] = '\0';
    bb->pending_sample_switch = 0;
    bb->preview_frames = 0;
    bb_update_status(bb);
}

static void bb_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    breakbeat_t *bb = (breakbeat_t *)instance;
    (void)source;
    if (!bb || len < 1) return;

    if (msg[0] >= 0xF8) {
        int beat_position = 0;
        int events = bb_timing_on_realtime(&bb->timing, msg[0],
                                            bb->ticks_per_trigger,
                                            &beat_position);
        if (events & BB_TIMING_START) bb_reset_transport(bb);
        if (events & BB_TIMING_STOP) {
            bb->was_running = 0;
            bb->pending_trigger = 0;
            bb->pending_bar = 0;
            bb->preview_frames = 0;
            bb->sub_slice_active = 0;
        }
        if (events & BB_TIMING_TRIGGER) {
            bb->pending_beat_pos = beat_position;
            bb->pending_trigger = 1;
        }
        if (events & BB_TIMING_BAR) bb->pending_bar = 1;
        return;
    }

    if (len < 3) return;

    uint8_t status = msg[0] & 0xF0;
    uint8_t note   = msg[1];
    uint8_t vel    = msg[2];

    if (status == 0x90 && vel > 0) {
        /* Pad presses: notes 36-43 → slices 0-7 */
        if (note >= 36 && note <= 43) {
            int slice = note - 36;
            bb->play_pos      = bb->slice_starts[slice];
            bb->current_slice = slice;
            bb->playing = 1;
        }
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
        int preview_allowed = !bb->suppress_next_preset_preview;
        bb->suppress_next_preset_preview = 0;
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
            if (load_standby_sample(bb, bb->alt_sample_path, bb->alt_length) == 0)
                bb->standby_loop = 'B';
            bb->pending_sample_path[0] = '\0';
            bb->pending_sample_switch = 0;
            if (preview_allowed) bb_start_preview(bb);
            else bb->preview_frames = 0;
        } else {
            wp_log("breakbeat: stop transport before changing presets");
        }
    }
    else if (strcmp(key, "A_sample_path") == 0) {
        resolve_sample_path(bb->module_dir, val, bb->main_sample_path, sizeof(bb->main_sample_path));
        if (atomic_load_explicit(&bb->sample_loader_started, memory_order_acquire))
            request_sample_load(bb, 0, bb->main_sample_path, bb->main_length);
        else if (!bb->timing.running) {
            apply_sample_path(bb, bb->main_sample_path, bb->main_length);
            bb_start_preview(bb);
        }
    }
    else if (strcmp(key, "B_sample_path") == 0) {
        resolve_sample_path(bb->module_dir, val, bb->alt_sample_path, sizeof(bb->alt_sample_path));
        if (atomic_load_explicit(&bb->sample_loader_started, memory_order_acquire))
            request_sample_load(bb, 1, bb->alt_sample_path, bb->alt_length);
        else if (!bb->timing.running &&
                 load_standby_sample(bb, bb->alt_sample_path, bb->alt_length) == 0)
            bb->standby_loop = 'B';
    }
    else if (strcmp(key, "A_sample_length") == 0 || strcmp(key, "length") == 0) {
        int idx = atoi(val);
        float lengths[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
        if (idx >= 0 && idx < 6) {
            bb->length = lengths[idx];
            bb->main_length = lengths[idx];
            set_loop_musical_length(bb, 'A', bb->main_length);
        }
    }
    else if (strcmp(key, "B_sample_length") == 0 || strcmp(key, "alt_length") == 0) {
        int idx = atoi(val);
        float lengths[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
        if (idx >= 0 && idx < 6) {
            bb->alt_length = lengths[idx];
            set_loop_musical_length(bb, 'B', bb->alt_length);
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
        /* State restore is not a user audition. The chain host applies the
         * preset immediately before state, so cancel any preview it started. */
        bb->preview_frames = 0;
        if (!json_get_int(val, "preset_index", &bb->preset_idx))
            json_get_int(val, "preset", &bb->preset_idx); /* v0.4 compatibility */
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
            if (i >= 0 && i < 6) {
                bb->length = lengths[i];
                bb->main_length = lengths[i];
            }
        }
        if (json_get_int(val, "alt_length", &i)) {
            static const float lengths[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
            if (i >= 0 && i < 6) bb->alt_length = lengths[i];
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
                if (load_standby_sample(bb, bb->alt_sample_path, bb->alt_length) == 0)
                    bb->standby_loop = 'B';
                bb->pending_sample_path[0] = '\0';
                bb->pending_sample_switch = 0;
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
        return snprintf(buf, buf_len, "%s", bb->status_str);
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
    else if (strcmp(key, "tempo_bpm") == 0) {
        return snprintf(buf, buf_len, "%.3f", bb->stable_bpm);
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

        int alt_len_idx = 2;
        if (bb->alt_length == 0.25f) alt_len_idx = 0;
        else if (bb->alt_length == 0.5f) alt_len_idx = 1;
        else if (bb->alt_length == 1.0f) alt_len_idx = 2;
        else if (bb->alt_length == 2.0f) alt_len_idx = 3;
        else if (bb->alt_length == 4.0f) alt_len_idx = 4;
        else if (bb->alt_length == 8.0f) alt_len_idx = 5;

        return snprintf(buf, buf_len,
            "{\"preset_index\":%d,\"sample_path\":\"%s\",\"alt_sample_path\":\"%s\","
            "\"length\":%d,\"alt_length\":%d,\"complexity\":%d,\"anchor\":%d,\"roll\":%d,\"phrase\":%d,\"fill\":%d,"
            "\"retrig_2x\":%d,\"retrig_3x\":%d,\"retrig_4x\":%d,\"retrig_8x\":%d,\"swap_prob\":%d}",
            bb->preset_idx,
            bb->main_sample_path, bb->alt_sample_path,
            len_idx, alt_len_idx,
            (int)(bb->complexity * 100.0f), (int)(bb->anchor * 100.0f),
            (int)(bb->roll * 100.0f), phrase_idx, (int)(bb->fill * 100.0f),
            (int)(bb->retrig_p[0] * 100.0f), (int)(bb->retrig_p[1] * 100.0f),
            (int)(bb->retrig_p[2] * 100.0f), (int)(bb->retrig_p[3] * 100.0f),
            (int)(bb->swap_prob * 100.0f));
    }
    
    return -1;
}

static void bb_fire_trigger(breakbeat_t *bb, int beat_pos) {
    slice_inputs_t in = {
        .current_slice = bb->current_slice,
        .beat_position = beat_pos,
        .complexity = bb->complexity,
        .anchor = bb->anchor,
        .roll = bb->roll,
        .phrase_bars = bb->phrase_bars,
        .fill = bb->fill,
        .bar_in_phrase = bb->phrase_bars > 0
                       ? bb->bar_counter % bb->phrase_bars : 0,
    };
    bb->current_slice = (bb->complexity == 0.0f)
                      ? beat_pos
                      : slice_select_next(&in, bb_rand, bb);
    bb->sub_slice_counter = 0;

    static const int retrigger_divs[4] = {2, 3, 4, 8};
    float triggers_per_bar = (bb->active_length > 0.0f)
                           ? (8.0f / bb->active_length) : 8.0f;
    float inv_triggers_per_bar = 1.0f / triggers_per_bar;
    int fired[4];
    int fired_count = 0;
    for (int i = 0; i < 4; i++) {
        float p_bar = bb->retrig_p[i];
        if (p_bar <= 0.0f) continue;
        float p_trigger = (p_bar >= 1.0f) ? 1.0f
                        : 1.0f - powf(1.0f - p_bar, inv_triggers_per_bar);
        if (bb_rand(bb) < p_trigger) fired[fired_count++] = i;
    }
    if (fired_count > 0) {
        bb->sub_slice_active = 1;
        int selected = fired[bb_random_u32(bb) % (uint32_t)fired_count];
        bb->retrigger_divisions = retrigger_divs[selected];
    } else {
        bb->sub_slice_active = 0;
    }
    bb_update_status(bb);
    bb->play_pos = (float)bb->slice_starts[bb->current_slice];
}

static void bb_render_block(void *instance, int16_t *out_lr, int frames) {
    breakbeat_t *bb = (breakbeat_t *)instance;

    if (!bb) {
        memset(out_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    atomic_fetch_add_explicit(&bb->sample_readers, 1, memory_order_acquire);
    if (atomic_load_explicit(&bb->sample_update, memory_order_acquire) ||
        !bb->data || bb->total_frames == 0) {
        atomic_fetch_sub_explicit(&bb->sample_readers, 1, memory_order_release);
        memset(out_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    int running = bb->timing.running;
    if (running && !bb->was_running) bb_reset_transport(bb);
    if (!running) bb->was_running = 0;

    /* The stored Set tempo gives an immediate rate before the live clock has
     * completed its first clean measurement window. While running, prefer the
     * host's measured clock BPM so tempo-knob changes alter sample rate without
     * requiring Stop/Start (Move may defer writing Song.abl until Stop). */
    if (g_host && g_host->get_bpm && (!running || bb->stable_bpm < 20.0f)) {
        /* Never follow the live clock estimator continuously while audio is
         * running: take a tempo while stopped, or once if we have none yet,
         * and retain the last valid value after that. */
        float bpm = g_host->get_bpm();
        if (bpm >= 20.0f && bpm <= 400.0f) bb->stable_bpm = bpm;
    }
    if (running && g_host && g_host->get_bpm) {
        /* Once MIDI clock is measured, get_bpm() returns that live value. Until
         * then Schwung returns the stored Set BPM, so this is safe from the
         * old first-bar low/zero estimate. */
        float bpm = g_host->get_bpm();
        if (bpm >= 20.0f && bpm <= 400.0f) bb->stable_bpm = bpm;
    }
    if (bb->stable_bpm < 20.0f || bb->stable_bpm > 400.0f)
        bb->stable_bpm = 120.0f;

    int previewing = (bb->preview_frames > 0);
    if (!running && !previewing) {
        atomic_fetch_sub_explicit(&bb->sample_readers, 1, memory_order_release);
        memset(out_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }
    if (previewing) {
        bb->preview_frames -= frames;
        if (bb->preview_frames < 0) bb->preview_frames = 0;
    }
    float spt = bb_timing_samples_per_trigger(bb->stable_bpm,
                                               bb->active_length,
                                               MOVE_SAMPLE_RATE);

    /* Bar boundary ---------------------------------------------------- */
    if (running && bb->pending_bar) {
            bb->pending_bar = 0;
            bb->bar_counter++;
            if (bb->phrase_bars > 0) {
                int bar_in_phrase = bb->bar_counter % bb->phrase_bars;
                char wanted = 'A';
                if (bar_in_phrase == bb->phrase_bars - 1 &&
                    bb_rand(bb) < bb->swap_prob)
                    wanted = 'B';

                /* Both loops are resident: enforce the phrase directly at
                 * the downbeat instead of carrying a one-bar-ahead pending
                 * switch that can become stale after parameter edits. */
                if (bb->current_loop != wanted && bb->standby_loop == wanted) {
                    char old_loop = bb->current_loop;
                    if (activate_standby_sample(bb) == 0) {
                        bb->current_loop = wanted;
                        bb->standby_loop = old_loop;
                    }
                    bb_timing_reset_trigger_phase(&bb->timing);
                    bb->pending_trigger = 0;
                    bb->current_slice = 0;
                    bb->play_pos = (float)bb->slice_starts[0];
                    bb->sub_slice_active = 0;
                    bb->sub_slice_counter = 0;
                    bb_update_status(bb);
                }
            }
    }

    /* Slice triggers -------------------------------------------------- */
    if (bb->just_reset) {
        bb->just_reset = 0;
        bb->play_pos = (float)bb->slice_starts[0];
    }

    if (running && bb->pending_trigger) {
        bb->pending_trigger = 0;
        bb_fire_trigger(bb, bb->pending_beat_pos);
    }

    /* Rate is exact from the current Set BPM on the first block. MIDI clock
     * corrects phase at each trigger but is never used as a warm-up estimator. */
    float slice_len = (bb->current_slice < 8)
                    ? (float)bb->slice_lengths[bb->current_slice] : 0.0f;
    float rate = (slice_len > 0.0f && spt > 0.0f) ? slice_len / spt : 1.0f;
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
        
        bb->play_pos += rate;
    }
    bb->sample_counter += frames;
    atomic_fetch_sub_explicit(&bb->sample_readers, 1, memory_order_release);
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
