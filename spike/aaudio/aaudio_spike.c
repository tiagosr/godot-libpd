/*
 * AAudio spike (M6 recon): play a 440 Hz stereo sine on the default output
 * device for 6 seconds, then report the negotiated stream parameters +
 * measured callback behavior. Confirms on the RG DS that:
 *   - libaaudio.so links + loads,
 *   - an AAudioStream opens + starts on RK3568 / Android 14,
 *   - the data callback delivers a fixed frame count (via
 *     AAudioStreamBuilder_setFramesPerDataCallback),
 *   - the stream closes cleanly.
 *
 * Uses the NDK r25 (API 26-era) AAudio C API: AAUDIO_FORMAT_PCM_FLOAT,
 * AAudioStreamBuilder_setCallback / _openStream, AAudioStream_close.
 *
 * Built for arm64-v8a against the NDK sysroot (AAudio C API, API 29+).
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
static long cb_calls = 0;
static long cb_frames = 0;
static int cb_min_frames = 1 << 30;
static int cb_max_frames = 0;
static int last_frames = -1;

static aaudio_data_callback_result_t data_cb(AAudioStream *stream, void *userData,
                                             void *audioData, int32_t numFrames) {
    (void)stream;
    (void)userData;
    float *out = (float *)audioData; /* stereo: 2 channels */
    for (int32_t i = 0; i < numFrames; i++) {
        double angle = 2.0 * PI * phase / kSampleRate;
        float s = (float)(sin(angle) * 0.25);
        out[2 * i] = s;     /* L */
        out[2 * i + 1] = s; /* R */
        phase += 1.0;
    }
    cb_calls++;
    cb_frames += numFrames;
    if (numFrames < cb_min_frames) cb_min_frames = numFrames;
    if (numFrames > cb_max_frames) cb_max_frames = numFrames;
    last_frames = numFrames;
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void dump_result(const char *what, aaudio_result_t r) {
    if (r != AAUDIO_OK) {
        printf("[aaudio_spike] %s FAILED: %s (code %d)\n", what, AAudio_convertResultToText(r), (int)r);
        exit(1);
    }
}

int main(void) {
    printf("[aaudio_spike] start\n");
    fflush(stdout);

    AAudioStreamBuilder *builder = NULL;
    dump_result("AAudio_createStreamBuilder", AAudio_createStreamBuilder(&builder));

    AAudioStreamBuilder_setSampleRate(builder, (int32_t)kSampleRate);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFramesPerDataCallback(builder, 256); /* fixed 256-frame callbacks */
    AAudioStreamBuilder_setDataCallback(builder, data_cb, NULL);

    AAudioStream *stream = NULL;
    dump_result("AAudioStreamBuilder_openStream", AAudioStreamBuilder_openStream(builder, &stream));

    int32_t sr = AAudioStream_getSampleRate(stream);
    int32_t ch = AAudioStream_getChannelCount(stream);
    int32_t buf = AAudioStream_getBufferSizeInFrames(stream);
    int32_t cap = AAudioStream_getBufferCapacityInFrames(stream);
    int32_t fmt = (int32_t)AAudioStream_getFormat(stream);
    printf("[aaudio_spike] negotiated: rate=%d ch=%d format=%d bufsize=%d capacity=%d state=%d\n",
           sr, ch, fmt, buf, cap, (int)AAudioStream_getState(stream));
    fflush(stdout);

    dump_result("AAudioStream_requestStart", AAudioStream_requestStart(stream));
    printf("[aaudio_spike] started; playing 440 Hz stereo sine for 6 s\n");
    fflush(stdout);

    for (int i = 0; i < 6; i++) {
        sleep(1);
        printf("[aaudio_spike] t=%ds cb_calls=%ld cb_frames=%ld last_frames=%d\n",
               i + 1, cb_calls, cb_frames, last_frames);
        fflush(stdout);
    }

    aaudio_result_t r = AAudioStream_requestStop(stream);
    printf("[aaudio_spike] stop -> %s\n", AAudio_convertResultToText(r));
    dump_result("AAudioStream_close", AAudioStream_close(stream));
    dump_result("AAudioStreamBuilder_delete", AAudioStreamBuilder_delete(builder));

    printf("[aaudio_spike] callback: calls=%ld total_frames=%ld min=%d max=%d\n",
           cb_calls, cb_frames, cb_min_frames, cb_max_frames);
    printf("[aaudio_spike] done\n");
    fflush(stdout);
    return 0;
}
