/*
 * AAudio spike variant 2 (M6 recon): diagnose "callback runs but no sound"
 * on the RG DS. The baseline spike (LOW_LATENCY + PCM_FLOAT) reaches STARTED
 * and the mixer runs, but produces no audible output, and AAudioFlinger
 * downgraded perfMode 12 -> 10 + dropped the requested flags.
 *
 * This variant takes two knobs via argv so we can A/B without rebuilding:
 *   arg1: "latency"  -> setPerformanceMode(LOW_LATENCY)  (default: none)
 *   arg2: "float"    -> AAUDIO_FORMAT_PCM_FLOAT          (default: PCM_I16)
 *
 * Plays a 440 Hz tone for 5 s. Prints the negotiated format + perfMode so we
 * can see what the system actually accepted.
 */
#include <AAudio/AAudio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PI 3.14159265358979323846
static double kSampleRate = 44100.0;
static double kFreq = 440.0;
static double phase = 0.0;
static aaudio_format_t g_format = AAUDIO_FORMAT_PCM_I16;

static aaudio_data_callback_result_t data_cb(AAudioStream *stream, void *userData,
                                             void *audioData, int32_t numFrames) {
    (void)stream;
    (void)userData;
    /* Full-scale (1.0) amplitude — the RG DS speaker needs headroom. */
    if (g_format == AAUDIO_FORMAT_PCM_FLOAT) {
        float *out = (float *)audioData;
        for (int32_t i = 0; i < numFrames; i++) {
            double a = 2.0 * PI * phase / kSampleRate;
            float s = (float)(sin(a) * 1.0);
            out[2 * i] = s;
            out[2 * i + 1] = s;
            phase += 1.0;
        }
    } else {
        int16_t *out = (int16_t *)audioData;
        for (int32_t i = 0; i < numFrames; i++) {
            double a = 2.0 * PI * phase / kSampleRate;
            int16_t s = (int16_t)(sin(a) * 32767.0);
            out[2 * i] = s;
            out[2 * i + 1] = s;
            phase += 1.0;
        }
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void dump_result(const char *what, aaudio_result_t r) {
    if (r != AAUDIO_OK) {
        printf("[spike2] %s FAILED: %s (%d)\n", what, AAudio_convertResultToText(r), (int)r);
        exit(1);
    }
}

int main(int argc, char **argv) {
    int want_latency = 0;
    int want_float = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "latency") == 0) want_latency = 1;
        else if (strcmp(argv[i], "float") == 0) want_float = 1;
    }
    g_format = want_float ? AAUDIO_FORMAT_PCM_FLOAT : AAUDIO_FORMAT_PCM_I16;
    printf("[spike2] mode: latency=%d format=%s\n", want_latency,
           want_float ? "PCM_FLOAT" : "PCM_I16");
    fflush(stdout);

    AAudioStreamBuilder *builder = NULL;
    dump_result("createStreamBuilder", AAudio_createStreamBuilder(&builder));
    AAudioStreamBuilder_setSampleRate(builder, (int32_t)kSampleRate);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setFormat(builder, g_format);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    if (want_latency)
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFramesPerDataCallback(builder, 256);
    AAudioStreamBuilder_setDataCallback(builder, data_cb, NULL);

    AAudioStream *stream = NULL;
    dump_result("openStream", AAudioStreamBuilder_openStream(builder, &stream));
    printf("[spike2] negotiated: rate=%d ch=%d fmt=%d bufsize=%d perf=%d state=%d\n",
           AAudioStream_getSampleRate(stream), AAudioStream_getChannelCount(stream),
           (int)AAudioStream_getFormat(stream), AAudioStream_getBufferSizeInFrames(stream),
           (int)AAudioStream_getPerformanceMode(stream), (int)AAudioStream_getState(stream));
    fflush(stdout);

    dump_result("requestStart", AAudioStream_requestStart(stream));
    printf("[spike2] playing 440 Hz for 5 s...\n");
    fflush(stdout);
    for (int i = 0; i < 5; i++) sleep(1);

    aaudio_result_t r = AAudioStream_requestStop(stream);
    printf("[spike2] stop -> %s\n", AAudio_convertResultToText(r));
    dump_result("close", AAudioStream_close(stream));
    dump_result("delete", AAudioStreamBuilder_delete(builder));
    printf("[spike2] done\n");
    fflush(stdout);
    return 0;
}
