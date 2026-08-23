#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/dsp/plugin_api_v1.h"

static int pass_count;
static int fail_count;

#define CHECK(cond, msg) do { \
    if (cond) pass_count++; \
    else { fail_count++; printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

static void quiet_log(const char *msg) { (void)msg; }
static int stopped_status(void) { return MOVE_CLOCK_STATUS_STOPPED; }
static float g_host_bpm = 137.0f;
static float test_bpm(void) { return g_host_bpm; }
static float g_project_bpm = 137.0f;
static float test_project_bpm(void) { return g_project_bpm; }
static double g_beat_position = -1.0;
static double test_beat_position(void) { return g_beat_position; }

static int buffer_is_silent(const int16_t *buf, int count) {
    for (int i = 0; i < count; i++) if (buf[i] != 0) return 0;
    return 1;
}

static int buffers_equal(const int16_t *a, const int16_t *b, int count) {
    for (int i = 0; i < count; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void send_clock_ticks(plugin_api_v2_t *api, void *instance,
                             int ticks, int16_t *audio) {
    const uint8_t clock = 0xF8;
    for (int i = 0; i < ticks; i++) {
        api->on_midi(instance, &clock, 1, MOVE_MIDI_SOURCE_HOST);
        api->render_block(instance, audio, MOVE_FRAMES_PER_BLOCK);
    }
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <native-dsp.so> <sample.wav> <alternate.wav>\n", argv[0]);
        return 2;
    }

    char temp_dir[] = "/tmp/breakbeat-runtime.XXXXXX";
    CHECK(mkdtemp(temp_dir) != NULL, "create temporary module directory");
    char presets_dir[512];
    snprintf(presets_dir, sizeof(presets_dir), "%s/presets", temp_dir);
    CHECK(mkdir(presets_dir, 0700) == 0, "create preset directory");

    char preset_path[640];
    snprintf(preset_path, sizeof(preset_path), "%s/1_test.json", presets_dir);
    FILE *preset = fopen(preset_path, "w");
    CHECK(preset != NULL, "create test preset");
    if (!preset) return 2;
    fprintf(preset,
            "{\"A_sample_path\":\"%s\",\"A_sample_length\":\"4 bars\","
            "\"B_sample_path\":\"%s\",\"B_sample_length\":\"2 bars\","
            "\"phrase\":\"Off\"}\n",
            argv[2], argv[2]);
    fclose(preset);

    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    CHECK(handle != NULL, "load native plugin");
    if (!handle) {
        fprintf(stderr, "%s\n", dlerror());
        return 2;
    }

    move_plugin_init_v2_fn init = (move_plugin_init_v2_fn)dlsym(
        handle, MOVE_PLUGIN_INIT_V2_SYMBOL);
    CHECK(init != NULL, "find v2 plugin entry point");

    host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.api_version = MOVE_PLUGIN_API_VERSION;
    host.sample_rate = MOVE_SAMPLE_RATE;
    host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    host.log = quiet_log;
    host.get_clock_status = stopped_status;
    host.get_bpm = test_bpm;
    host.get_beat_position = test_beat_position;
    host.get_project_bpm = test_project_bpm;

    plugin_api_v2_t *api = init(&host);
    CHECK(api && api->create_instance && api->render_block,
          "plugin exposes required v2 callbacks");
    void *instance = api->create_instance(temp_dir, "{}");
    CHECK(instance != NULL, "create plugin instance");

    char tempo[32];
    api->get_param(instance, "tempo_bpm", tempo, sizeof(tempo));
    CHECK(strncmp(tempo, "137.000", 7) == 0,
          "instance stores the Set tempo instead of measured clock BPM");

    int16_t audio[MOVE_FRAMES_PER_BLOCK * 2];
    memset(audio, 1, sizeof(audio));

    /* This is the chain host's Set-load sequence. The first preset assignment
     * and subsequent state restore must not audition audio. */
    api->set_param(instance, "preset", "0");
    api->render_block(instance, audio, MOVE_FRAMES_PER_BLOCK);
    CHECK(buffer_is_silent(audio, MOVE_FRAMES_PER_BLOCK * 2),
          "preset restore stays silent while stopped");

    char state[2048];
    snprintf(state, sizeof(state),
             "{\"preset_index\":0,\"sample_path\":\"%s\","
             "\"alt_sample_path\":\"%s\",\"length\":4,\"alt_length\":3}",
             argv[2], argv[2]);
    api->set_param(instance, "state", state);
    memset(audio, 1, sizeof(audio));
    api->render_block(instance, audio, MOVE_FRAMES_PER_BLOCK);
    CHECK(buffer_is_silent(audio, MOVE_FRAMES_PER_BLOCK * 2),
          "state restore cancels preview while stopped");

    char saved[2048];
    int saved_len = api->get_param(instance, "state", saved, sizeof(saved));
    CHECK(saved_len > 0 && strstr(saved, "\"preset_index\":0") != NULL,
          "state round-trips preset_index");
    CHECK(saved_len > 0 && strstr(saved, "\"alt_length\":3") != NULL,
          "state round-trips B length");

    snprintf(state, sizeof(state),
             "{\"preset_index\":0,\"sample_path\":\"%s\"," 
             "\"alt_sample_path\":\"%s\",\"length\":2,\"alt_length\":3,"
             "\"complexity\":0,\"anchor\":0,\"roll\":0,"
             "\"retrig_2x\":0,\"retrig_3x\":0,\"retrig_4x\":0,\"retrig_8x\":0,"
             "\"phrase\":2,\"swap_prob\":100}", argv[2], argv[2]);
    api->set_param(instance, "state", state);

    const uint8_t start = 0xFA;
    api->on_midi(instance, &start, 1, MOVE_MIDI_SOURCE_HOST);
    memset(audio, 0, sizeof(audio));
    api->render_block(instance, audio, MOVE_FRAMES_PER_BLOCK);
    CHECK(!buffer_is_silent(audio, MOVE_FRAMES_PER_BLOCK * 2),
          "Start produces audio immediately");

    char status[32];
    api->get_param(instance, "status", status, sizeof(status));
    CHECK(strncmp(status, "A_0_", 4) == 0,
          "MIDI Start begins A on slice zero");

    api->set_param(instance, "A_sample_length", "1"); /* 1/2 bar */
    send_clock_ticks(api, instance, 6, audio);
    api->get_param(instance, "status", status, sizeof(status));
    CHECK(strncmp(status, "A_1_", 4) == 0,
          "live A length change takes effect at the next new trigger interval");
    api->set_param(instance, "A_sample_length", "2"); /* restore 1 bar */
    api->on_midi(instance, &start, 1, MOVE_MIDI_SOURCE_HOST);
    api->render_block(instance, audio, MOVE_FRAMES_PER_BLOCK);

    g_project_bpm = 142.0f;
    g_host_bpm = 142.0f;
    api->render_block(instance, audio, MOVE_FRAMES_PER_BLOCK);
    api->get_param(instance, "tempo_bpm", tempo, sizeof(tempo));
    CHECK(strncmp(tempo, "142.000", 7) == 0,
          "stored tempo updates when the user changes the Set BPM");

    /* Move can defer persisting Song.abl while transport is running. Its MIDI
     * clock changes immediately, so live BPM must win for playback rate. */
    g_host_bpm = 74.0f;
    api->render_block(instance, audio, MOVE_FRAMES_PER_BLOCK);
    api->get_param(instance, "tempo_bpm", tempo, sizeof(tempo));
    CHECK(strncmp(tempo, "74.000", 6) == 0,
          "running playback rate follows a live tempo change without restart");
    g_host_bpm = 142.0f;

    int16_t previous_audio[MOVE_FRAMES_PER_BLOCK * 2];
    memcpy(previous_audio, audio, sizeof(audio));
    api->render_block(instance, audio, MOVE_FRAMES_PER_BLOCK);
    CHECK(!buffers_equal(previous_audio, audio, MOVE_FRAMES_PER_BLOCK * 2),
          "audio cursor remains continuous if the host beat is briefly unchanged");

    send_clock_ticks(api, instance, 12, audio);
    api->get_param(instance, "status", status, sizeof(status));
    CHECK(strncmp(status, "A_1_", 4) == 0,
          "twelfth MIDI clock advances to A slice one");

    send_clock_ticks(api, instance, 84, audio);
    api->get_param(instance, "status", status, sizeof(status));
    CHECK(strncmp(status, "A_0_", 4) == 0,
          "first beat of bar two returns to A slice zero");

    send_clock_ticks(api, instance, 96, audio);
    api->get_param(instance, "status", status, sizeof(status));
    CHECK(strncmp(status, "A_0_", 4) == 0,
          "first beat of bar three remains A slice zero");

    send_clock_ticks(api, instance, 95, audio);
    api->get_param(instance, "status", status, sizeof(status));
    CHECK(status[0] == 'A', "A owns all of the first three bars");

    send_clock_ticks(api, instance, 1, audio);
    api->get_param(instance, "status", status, sizeof(status));
    CHECK(strncmp(status, "B_0_", 4) == 0,
          "B begins on bar four regardless of its two-bar source length");

    api->set_param(instance, "B_sample_length", "1"); /* 1/2 bar */
    send_clock_ticks(api, instance, 6, audio);
    api->get_param(instance, "status", status, sizeof(status));
    CHECK(strncmp(status, "B_1_", 4) == 0,
          "live B length change takes effect while B is active");
    api->set_param(instance, "B_sample_length", "3"); /* restore 2 bars */

    send_clock_ticks(api, instance, 89, audio);
    api->get_param(instance, "status", status, sizeof(status));
    CHECK(status[0] == 'B', "B remains active through the final phrase bar");

    send_clock_ticks(api, instance, 1, audio);
    api->get_param(instance, "status", status, sizeof(status));
    CHECK(strncmp(status, "A_0_", 4) == 0,
          "A returns on the next four-bar phrase boundary");

    /* A Stop during B must not make the next song run begin on B. */
    send_clock_ticks(api, instance, 288, audio);
    api->get_param(instance, "status", status, sizeof(status));
    CHECK(status[0] == 'B', "second phrase reaches its one-bar B fill");

    const uint8_t stop = 0xFC;
    api->on_midi(instance, &stop, 1, MOVE_MIDI_SOURCE_HOST);
    memset(audio, 1, sizeof(audio));
    api->render_block(instance, audio, MOVE_FRAMES_PER_BLOCK);
    CHECK(buffer_is_silent(audio, MOVE_FRAMES_PER_BLOCK * 2),
          "Stop silences the next block");

    api->on_midi(instance, &start, 1, MOVE_MIDI_SOURCE_HOST);
    api->render_block(instance, audio, MOVE_FRAMES_PER_BLOCK);
    api->get_param(instance, "status", status, sizeof(status));
    CHECK(strncmp(status, "A_0_", 4) == 0,
          "restarting after a B fill always begins with A slice zero");

    api->on_midi(instance, &stop, 1, MOVE_MIDI_SOURCE_HOST);

    const uint8_t clock = 0xF8;
    for (int i = 0; i < 192; i++)
        api->on_midi(instance, &clock, 1, MOVE_MIDI_SOURCE_HOST);
    memset(audio, 1, sizeof(audio));
    api->render_block(instance, audio, MOVE_FRAMES_PER_BLOCK);
    CHECK(buffer_is_silent(audio, MOVE_FRAMES_PER_BLOCK * 2),
          "clock while stopped cannot restart playback");

    /* Regression: the Movy filepath browser can commit A while transport is
     * running. It must load off-thread and survive the next Stop/Start. */
    api->on_midi(instance, &start, 1, MOVE_MIDI_SOURCE_HOST);
    api->set_param(instance, "A_sample_path", argv[3]);
    usleep(250000);
    api->on_midi(instance, &stop, 1, MOVE_MIDI_SOURCE_HOST);
    api->on_midi(instance, &start, 1, MOVE_MIDI_SOURCE_HOST);
    int16_t switched_audio[MOVE_FRAMES_PER_BLOCK * 2];
    api->render_block(instance, switched_audio, MOVE_FRAMES_PER_BLOCK);

    preset = fopen(preset_path, "w");
    CHECK(preset != NULL, "rewrite reference preset for alternate sample");
    if (preset) {
        fprintf(preset,
                "{\"A_sample_path\":\"%s\",\"A_sample_length\":\"1 bar\","
                "\"B_sample_path\":\"%s\",\"B_sample_length\":\"2 bars\","
                "\"phrase\":\"Off\"}\n", argv[3], argv[2]);
        fclose(preset);
    }
    void *reference = api->create_instance(temp_dir, "{}");
    CHECK(reference != NULL, "create alternate-sample reference instance");
    int16_t expected_audio[MOVE_FRAMES_PER_BLOCK * 2];
    api->on_midi(reference, &start, 1, MOVE_MIDI_SOURCE_HOST);
    api->render_block(reference, expected_audio, MOVE_FRAMES_PER_BLOCK);
    CHECK(buffers_equal(switched_audio, expected_audio,
                        MOVE_FRAMES_PER_BLOCK * 2),
          "A filepath selection while running changes the mapped audio");
    api->destroy_instance(reference);

    api->destroy_instance(instance);
    dlclose(handle);
    unlink(preset_path);
    rmdir(presets_dir);
    rmdir(temp_dir);

    printf("\n%d passed, %d failed\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
