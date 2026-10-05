// M6' T2 — OpenSL ES device gate spike (RG DS)
//
// Mirrors the Godot Android audio driver setup (verified working on this
// device): engine -> output mix -> audio player with
// SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE source, SL_IID_BUFFERQUEUE
// interface, callback-driven 256-frame ring.
//
// Plays a 440 Hz sine for ~7 s. 16-bit by default (the Godot path); pass
// "float" as argv[2] to test SL_PCMSAMPLEFORMAT_FIXED_32 (float32).
//
// Build (NDK r25):
//   aarch64-linux-android29-clang -O2 -o build/arm64/opensles_spike \
//       opensles_spike.c -lopenSLES -lm
// Run:  opensles_spike [rate] [float]
//
// Gate: user HEARS the tone AND the buffer-queue callback fires steadily
// (the callback = our render clock; frame count per callback = 256 by
// construction since each enqueued buffer is 256 frames).

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define BUFFER_COUNT 8
#define CHANNELS 2
#define DURATION_S 7

static double g_rate = 48000.0;
static int g_float = 0;
static int g_frames = 256; // frames per buffer (256 default; 1024 = Godot shape)
static int g_nbuf = 8; // buffer count (Godot uses 2)

// Audio-thread written; main-thread read at exit only (single consumer).
static long g_cb_calls = 0;
static long g_enq_fail = 0;
static long g_dmin_ns = 0x7fffffffffffffL;
static long g_dmax_ns = 0;
static long g_late = 0; // deltas > 2x expected
static long g_prev_ns = 0;

static SLObjectItf g_engine;
static SLEngineItf g_engine_itf;
static SLObjectItf g_outmix;
static SLObjectItf g_player;
static SLBufferQueueItf g_queue;
static SLPlayItf g_play;
static SLAndroidConfigurationItf g_aconf;

static void **g_buffers = 0; // g_nbuf x (g_frames*2 samples)
static size_t g_sample_bytes = 2; // 2 (int16) or 4 (float)
static int g_ring = 0; // next buffer to fill in the callback

// WAV dump of everything actually enqueued (debug: generation vs playback).
static unsigned char *g_pcm_dump = 0;
static long g_pcm_dump_len = 0;
#define PCM_DUMP_CAP (2L * 1024 * 1024)

static double next_phase(long total_samples) {
	double angle = 2.0 * M_PI * 440.0 * (double)total_samples / g_rate;
	return angle;
}

static void fill_buffer(void *buf, long total_frames) {
	if (g_float) {
		float *p = (float *)buf;
		for (long f = 0; f < g_frames; f++) {
			float v = (float)(0.5 * sin(next_phase(total_frames + f)));
			p[f * 2 + 0] = v;
			p[f * 2 + 1] = v;
		}
	} else {
		short *p = (short *)buf;
		for (long f = 0; f < g_frames; f++) {
			float v = (float)(0.5 * sin(next_phase(total_frames + f)));
			p[f * 2 + 0] = (short)(v * 32767.0f);
			p[f * 2 + 1] = (short)(v * 32767.0f);
		}
	}
}

static void SLAPIENTRY data_cb(SLBufferQueueItf caller, void *p_ctx) {
	(void)p_ctx;
	(void)caller;
	g_cb_calls++;
	// Timing stats (audio-thread only).
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	long now = (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
	if (g_prev_ns != 0) {
		long d = now - g_prev_ns;
		if (d < g_dmin_ns) {
			g_dmin_ns = d;
		}
		if (d > g_dmax_ns) {
			g_dmax_ns = d;
		}
		long expect = (long)(g_frames * 1000000000.0 / g_rate);
		if (d > 2 * expect) {
			g_late++;
		}
	}
	g_prev_ns = now;
	// Slot (g_ring) was consumed (g_cb_calls-1) callbacks ago; it is recycled
	// for the position BUFFER_COUNT buffers ahead: frame
	// (g_cb_calls + BUFFER_COUNT - 1) * g_frames.
	long total_frames = (g_cb_calls + g_nbuf - 1) * (long)g_frames;
	fill_buffer(g_buffers[g_ring], total_frames);
	{
		size_t sz = (size_t)g_frames * CHANNELS * g_sample_bytes;
		if (g_pcm_dump_len + (long)sz <= PCM_DUMP_CAP) {
			memcpy(g_pcm_dump + g_pcm_dump_len, g_buffers[g_ring], sz);
			g_pcm_dump_len += (long)sz;
		}
	}
	SLresult r = (*g_queue)->Enqueue(g_queue, g_buffers[g_ring],
			(long)g_frames * CHANNELS * g_sample_bytes);
	if (r != SL_RESULT_SUCCESS) {
		g_enq_fail++;
	}
	(void)r;
	g_ring = (g_ring + 1) % g_nbuf;
}

static int fail(const char *what, SLresult r) {
	printf("[opensles_spike] %s failed: %u\n", what, (unsigned)r);
	return 1;
}

int main(int argc, char **argv) {
	if (argc > 1) {
		g_rate = atof(argv[1]);
	}
	if (argc > 2 && strcmp(argv[2], "float") == 0) {
		g_float = 1;
	}
	if (argc > 3) {
		g_frames = atoi(argv[3]);
	}
	if (argc > 4) {
		g_nbuf = atoi(argv[4]);
	}
	if (g_frames < 64) {
		g_frames = 256;
	}
	if (g_nbuf < 1) {
		g_nbuf = 2;
	}
	if (g_float) {
		printf("[opensles_spike] ERROR: float32 unsupported on Android OpenSL ES (no float PCM "
				"sample format in the NDK header; SL_PCMSAMPLEFORMAT_FLOAT_32 does not exist) "
				"— use int16\n");
		return 2;
	}
	printf("[opensles_spike] start (rate=%.0f, int16, %d frames/buffer, %d buffers)\n", g_rate,
			g_frames, g_nbuf);
	fflush(stdout);

	SLEngineOption opts[] = { { SL_ENGINEOPTION_THREADSAFE, SL_BOOLEAN_TRUE } };
	SLresult r = slCreateEngine(&g_engine, 1, opts, 0, 0, 0);
	if (r != SL_RESULT_SUCCESS) {
		return fail("slCreateEngine", r);
	}
	r = (*g_engine)->Realize(g_engine, SL_BOOLEAN_FALSE);
	if (r != SL_RESULT_SUCCESS) {
		return fail("engine Realize", r);
	}
	r = (*g_engine)->GetInterface(g_engine, SL_IID_ENGINE, (void *)&g_engine_itf);
	if (r != SL_RESULT_SUCCESS) {
		return fail("GetInterface(ENGINE)", r);
	}

	// Output mix.
	r = (*g_engine_itf)->CreateOutputMix(g_engine_itf, &g_outmix, 0, 0, 0);
	if (r != SL_RESULT_SUCCESS) {
		return fail("CreateOutputMix", r);
	}
	r = (*g_outmix)->Realize(g_outmix, SL_BOOLEAN_FALSE);
	if (r != SL_RESULT_SUCCESS) {
		return fail("outmix Realize", r);
	}

	// Player: AndroidSimpleBufferQueue source -> output mix sink.
	SLDataLocator_AndroidSimpleBufferQueue loc_src = {
			SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, (SLuint32)g_nbuf };
	SLDataLocator_OutputMix loc_out = { SL_DATALOCATOR_OUTPUTMIX, g_outmix };
	SLDataFormat_PCM pcm = {
			SL_DATAFORMAT_PCM,
			CHANNELS,
			g_rate < 46000.0 ? SL_SAMPLINGRATE_44_1 : SL_SAMPLINGRATE_48,
			SL_PCMSAMPLEFORMAT_FIXED_16,
			SL_PCMSAMPLEFORMAT_FIXED_16,
			SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
			SL_BYTEORDER_LITTLEENDIAN,
	};
	SLDataSource src = { &loc_src, &pcm };
	SLDataSink snk = { &loc_out, 0 };

	// CONFIG-VARIANT: plain BUFFERQUEUE only (no ANDROIDCONFIGURATION),
	// matching Godot's production driver shape.
	const SLInterfaceID ids[1] = { SL_IID_BUFFERQUEUE };
	const SLboolean req[1] = { SL_BOOLEAN_TRUE };
	r = (*g_engine_itf)->CreateAudioPlayer(g_engine_itf, &g_player, &src, &snk, 1, ids, req);
	if (r != SL_RESULT_SUCCESS) {
		return fail("CreateAudioPlayer", r);
	}
	r = (*g_player)->Realize(g_player, SL_BOOLEAN_FALSE);
	if (r != SL_RESULT_SUCCESS) {
		return fail("player Realize", r);
	}
	r = (*g_player)->GetInterface(g_player, SL_IID_PLAY, (void *)&g_play);
	if (r != SL_RESULT_SUCCESS) {
		return fail("GetInterface(PLAY)", r);
	}
	r = (*g_player)->GetInterface(g_player, SL_IID_BUFFERQUEUE, (void *)&g_queue);
	if (r != SL_RESULT_SUCCESS) {
		return fail("GetInterface(BUFFERQUEUE)", r);
	}

	r = (*g_queue)->RegisterCallback(g_queue, data_cb, 0);
	if (r != SL_RESULT_SUCCESS) {
		return fail("RegisterCallback", r);
	}

	g_buffers = (void **)calloc(g_nbuf, sizeof(void *));
	g_pcm_dump = (unsigned char *)malloc(PCM_DUMP_CAP);
	long buf_bytes = (long)g_frames * CHANNELS * g_sample_bytes;
	for (int i = 0; i < g_nbuf; i++) {
		g_buffers[i] = calloc(1, buf_bytes);
		long total_frames = (long)i * g_frames;
		fill_buffer(g_buffers[i], total_frames);
		r = (*g_queue)->Enqueue(g_queue, g_buffers[i], buf_bytes);
		if (r != SL_RESULT_SUCCESS) {
			printf("[opensles_spike] prime enqueue %d failed: %u\n", i, (unsigned)r);
			return 1;
		}
	}

	r = (*g_play)->SetPlayState(g_play, SL_PLAYSTATE_PLAYING);
	if (r != SL_RESULT_SUCCESS) {
		return fail("SetPlayState(PLAYING)", r);
	}
	{
		SLBufferQueueState qst;
		(*g_queue)->GetState(g_queue, &qst);
		printf("[opensles_spike] queue state after start: count=%u playIndex=%u\n",
				(unsigned)qst.count, (unsigned)qst.playIndex);
	}
	printf("[opensles_spike] playing 440 Hz %s for %d s — LISTEN\n",
			g_float ? "float32" : "int16", DURATION_S);
	fflush(stdout);

	for (int s = 1; s <= DURATION_S; s++) {
		usleep(1000000); // DO NOT busy-spin: starves the OpenSL audio thread
		if (s % 2 == 1) {
			printf("[opensles_spike] t=%ds cb_calls=%ld enq_fail=%ld\n", s, g_cb_calls, g_enq_fail);
			fflush(stdout);
		}
	}

	// Teardown.
	(*g_play)->SetPlayState(g_play, SL_PLAYSTATE_STOPPED);
	(*g_player)->Destroy(g_player);
	(*g_outmix)->Destroy(g_outmix);
	(*g_engine)->Destroy(g_engine);
	// Write the enqueued PCM as a 16-bit WAV (float → int16 packed).
	if (g_pcm_dump != 0) {
		if (g_float) {
			unsigned char *i16 = (unsigned char *)malloc((size_t)(g_pcm_dump_len / 2));
			float *f = (float *)g_pcm_dump;
			for (long i = 0; i < g_pcm_dump_len / 4; i++) {
				int v = (int)(f[i] * 32767.0f);
				if (v > 32767) {
					v = 32767;
				} else if (v < -32768) {
					v = -32768;
				}
				((short *)i16)[i] = (short)v;
			}
			free(g_pcm_dump);
			g_pcm_dump = i16;
			g_pcm_dump_len /= 2;
		}
		long data_bytes = g_pcm_dump_len;
		FILE *wf = fopen("/data/local/tmp/tone.wav", "wb");
		if (wf != 0) {
			int rate = (int)g_rate;
			unsigned char hdr[44] = { 0 };
			memcpy(hdr, "RIFF", 4);
			hdr[4] = (unsigned char)(data_bytes + 36);
			hdr[5] = (unsigned char)((data_bytes + 36) >> 8);
			hdr[6] = (unsigned char)((data_bytes + 36) >> 16);
			hdr[7] = (unsigned char)((data_bytes + 36) >> 24);
			memcpy(hdr + 8, "WAVEfmt ", 8);
			hdr[16] = 16; // fmt chunk size
			hdr[18] = 1; // PCM
			hdr[20] = 2; // channels
			hdr[22] = (unsigned char)(rate);
			hdr[23] = (unsigned char)(rate >> 8);
			hdr[24] = (unsigned char)(rate >> 16);
			hdr[25] = (unsigned char)(rate >> 24);
			hdr[26] = (unsigned char)(rate * 4); // byte rate (2ch x 16bit)
			hdr[27] = (unsigned char)((rate * 4) >> 8);
			hdr[28] = 4; // block align
			hdr[30] = 16; // bits per sample
			memcpy(hdr + 36, "data", 4);
			hdr[40] = (unsigned char)(data_bytes);
			hdr[41] = (unsigned char)(data_bytes >> 8);
			hdr[42] = (unsigned char)(data_bytes >> 16);
			hdr[43] = (unsigned char)(data_bytes >> 24);
			fwrite(hdr, 1, 44, wf);
			fwrite(g_pcm_dump, 1, (size_t)data_bytes, wf);
			fclose(wf);
			printf("[opensles_spike] wrote /data/local/tmp/tone.wav (%ld bytes, %.2f s)\n",
					data_bytes, (double)data_bytes / (2 * 2 * (int)g_rate));
		}
	}
	printf(
			"[opensles_spike] done: cb_calls=%ld (expect ~%ld) enq_fail=%ld dmin=%ldus dmax=%ldus late=%ld\n",
			g_cb_calls,
			(long)(DURATION_S * g_rate / g_frames), g_enq_fail, g_dmin_ns / 1000,
			g_dmax_ns / 1000, g_late);
	for (int i = 0; i < g_nbuf; i++) {
		free(g_buffers[i]);
	}
	free(g_buffers);
	free(g_pcm_dump);
	return 0;
}
