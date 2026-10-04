// Unit tests for NativeAudio (mix-in-PortAudio-callback model, validated by
// the 9-instance proof) — the real-time callback renders the mix-down.
//
// The invariant under test: each libpd instance's libpd_process_float runs on
// exactly ONE thread that called libpd_set_instance(it). The mix-down instance
// renders on the real-time callback thread (driven here by NullPort): bind
// (libpd_set_instance), gather the worker rings, process one block, and copy
// to the device buffer. Every synth instance is rendered on its own pinned
// worker thread and pushes its 64-frame blocks into its own MixInputRing
// (the last test: 8 of them, porting spike/native_audio/mixdown_multi.c).
//
// Real libpd instances throughout: each instance is created and init'd on the
// test thread (new_instance / set_instance / init_audio / dsp on / openfile)
// and rendered on its single owner thread — the exact sequence validated by
// the spike. Teardown happens AFTER the owning thread has been joined
// (NativeAudio::close() joins the mix render thread; the harness joins the
// synth workers).

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <pthread.h>
#include <thread>
#include <vector>

#include "core/mix_input_ring.h"
#include "core/native_audio.h"
#include "core/null_port.h"

// libpd multi-instance API.
extern "C" {
#include "z_libpd.h"
}

using namespace godot_libpd;

static int failures = 0;

#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                 \
		}                                                               \
	} while (0)

static void sleep_ms(int p_ms) {
	std::this_thread::sleep_for(std::chrono::milliseconds(p_ms));
}

/** Writes p_text to /tmp/p_name (truncating any pre-existing file). */
static void write_patch(const char *p_name, const char *p_text) {
	std::ofstream f(std::string("/tmp/") + p_name);
	f << p_text;
}

/** Turns dsp on for the calling thread's instance (the validated spike sequence). */
static void turn_dsp_on() {
	libpd_start_message(1);
	libpd_add_float(1.0f);
	libpd_finish_message("pd", "dsp");
}

// Patches (this pd build uses adc~ — there is no audioin~).
static const char *const kSinePatch =
	"#N canvas 0 0 200 200 12;\n"
	"#X obj 20 20 osc~ 200;\n"
	"#X obj 20 90 *~ 0.3;\n"
	"#X obj 20 150 dac~;\n"
	"#X connect 0 0 1 0;\n"
	"#X connect 1 0 2 0;\n"
	"#X connect 1 0 2 1;\n";
// [adc~ N] *~ 1.0 [dac~] — both outputs carry input channel N (1-indexed).
static const char *const kAdcPatchTemplate =
	"#N canvas 0 0 400 300 12;\n"
	"#X obj 20 20 adc~ %d;\n"
	"#X obj 20 60 *~ 1.0;\n"
	"#X obj 20 120 dac~;\n"
	"#X connect 0 0 1 0;\n"
	"#X connect 1 0 2 0;\n"
	"#X connect 1 0 2 1;\n";
// [adc~ 1] *~ 0.25 [dac~] — the 9-concurrent test's mix patch.
static const char *const kMixQuarterPatch =
	"#N canvas 0 0 400 300 12;\n"
	"#X obj 20 20 adc~ 1;\n"
	"#X obj 20 60 *~ 0.25;\n"
	"#X obj 20 120 dac~;\n"
	"#X connect 0 0 1 0;\n"
	"#X connect 1 0 2 0;\n"
	"#X connect 1 0 2 1;\n";

/** A real libpd instance created + init'd on the calling (test) thread. */
struct PdInstance {
	t_pdinstance *pd = nullptr;
	void *file_handle = nullptr;
};

static PdInstance init_pd_instance(int p_n_in, int p_n_out, const char *p_patch) {
	PdInstance inst;
	inst.pd = libpd_new_instance();
	// The test thread never renders: pd_this is thread-local, so this only
	// affects this thread's (unused-for-rendering) binding.
	libpd_set_instance(inst.pd);
	libpd_init_audio(p_n_in, p_n_out, 44100);
	turn_dsp_on();
	inst.file_handle = libpd_openfile(p_patch, "/tmp");
	return inst;
}

/** Tear down only after NativeAudio::close() joined the audio thread. */
static void pd_instance_teardown(PdInstance &p_inst) {
	// The thread must be bound to the instance BEFORE libpd_closefile:
	// closefile runs pd_free(canvas) under the calling thread's thread-
	// local pd_this, and libpd_free_instance (for any instance, including
	// the worker instances freed earlier) RESETS that thread-local to the
	// main libpd instance. closefile without the right binding frees the
	// canvas in the wrong instance context ("couldn't unbind", dangling
	// canvas list, segfault on the subsequent free_instance). free_instance
	// self-binds, so only closefile needs the explicit set_instance.
	if (p_inst.pd == nullptr) {
		return;
	}
	libpd_set_instance(p_inst.pd);
	if (p_inst.file_handle != nullptr) {
		libpd_closefile(p_inst.file_handle);
		p_inst.file_handle = nullptr;
	}
	libpd_free_instance(p_inst.pd);
	p_inst.pd = nullptr;
}

/**
 * Producer thread: pushes 64-frame blocks (the worker's tick size,
 * Option B) into p_ring at worker pacing (~1.45 ms per block at 44100 Hz)
 * until p_stop flips.
 */
static std::thread start_ring_feeder(MixInputRing &p_ring, std::atomic<bool> &p_stop,
		float p_value = 0.5f) {
	return std::thread([&p_ring, &p_stop, p_value]() {
		std::vector<float> block(64 * 2, p_value);
		while (true) {
			// Kick-driven (M5 T9): block on the ring's CV until the callback
			// kicks this ring (reliable wakeup; no self-pacing).
			const int target = p_ring.wait_for_kick(p_stop);
			if (target < 0) {
				break;
			}
			p_ring.push(block.data(), 64);
			p_ring.signal_done(target);
		}
	});
}

/** 1: mix-down renders — real 16-in/2-out libpd mix instance on the
 *     dedicated mix thread; NullPort only drains the output ring. */
static void test_mixdown_renders() {
	char text[512];
	std::snprintf(text, sizeof(text), kAdcPatchTemplate, 1); // [adc~ 1] *~ 1.0 [dac~]
	write_patch("na_t4_mix.pd", text);
	PdInstance mix = init_pd_instance(16, 2, "na_t4_mix.pd");
	CHECK(mix.file_handle != nullptr);

	NullPort port;
	NativeAudio na;
	CHECK(na.open(&port, 16, 2, 256, 44100));
	CHECK(na.is_open());
	CHECK(na.port() == &port);
	CHECK(na.blocksize() == 256);
	CHECK(na.samplerate() == 44100);
	CHECK(!na.has_mixer());

	MixInputRing ring0(2, 64, 8); // Option B: libpd_blocksize() frames
	na.register_worker_ring(&ring0);

	std::atomic<bool> stop{false};
	std::thread feeder = start_ring_feeder(ring0, stop);

	CHECK(na.set_mixer(mix.pd, 16, 2));
	CHECK(na.has_mixer());
	sleep_ms(300); // mix thread renders one 256-frame block every ~5.9 ms

	CHECK(port.frames_rendered() > 0);
	CHECK(port.last_out_peak() > 0.4f); // ring0 (ch 1) -> [adc~ 1] *~ 1.0
	CHECK(port.output_latency_ms() > 0.0);

	stop.store(true);
	ring0.wakeup();
	feeder.join();
	na.close(); // joins the NullPort audio thread + the mix render thread
	CHECK(!na.is_open());
	CHECK(!na.has_mixer());
	pd_instance_teardown(mix); // safe: the mix render thread has been joined
	std::printf("mixdown_renders done (peak=%.3f, frames=%llu)\n", port.last_out_peak(),
			(unsigned long long)port.frames_rendered());
}

/**
 * Sub-run: open a 16-in/2-out NativeAudio, register 8 rings, feed only
 * p_ring_index with 0.5f, and render a mix instance whose patch reads input
 * channel p_adc_channel. Returns the last rendered block's peak.
 */
static float measure_one_ring(const char *p_tag, int p_adc_channel, int p_ring_index) {
	char name[128];
	char text[512];
	std::snprintf(name, sizeof(name), "na_t4_%s_adcc%d.pd", p_tag, p_adc_channel);
	std::snprintf(text, sizeof(text), kAdcPatchTemplate, p_adc_channel);
	write_patch(name, text);

	PdInstance mix = init_pd_instance(16, 2, name);
	CHECK(mix.file_handle != nullptr);

	NullPort port;
	NativeAudio na;
	CHECK(na.open(&port, 16, 2, 256, 44100));

	std::vector<std::unique_ptr<MixInputRing>> rings;
	// Option B: 8 worker rings at (2ch, libpd_blocksize()=64, 8 blocks)
	for (int i = 0; i < 8; i++) {
		rings.push_back(std::make_unique<MixInputRing>(2, 64, 8));
		na.register_worker_ring(rings[i].get());
	}

	std::atomic<bool> stop{false};
	// Kick transport (M5 T9): every registered ring is kicked + waited on by
	// the callback, so EVERY ring needs a feeder that answers the kick —
	// the target ring carries the 0.5 signal, the rest are silent.
	std::vector<std::thread> feeders;
	for (int i = 0; i < (int)rings.size(); i++) {
		const float v = (i == p_ring_index) ? 0.5f : 0.0f;
		feeders.push_back(start_ring_feeder(*rings[i], stop, v));
	}
	CHECK(na.set_mixer(mix.pd, 16, 2));
	CHECK(na.has_mixer());
	sleep_ms(300);

	const float peak = port.last_out_peak();
	stop.store(true);
	for (auto &r : rings) {
		r->wakeup();
	}
	for (auto &f : feeders) {
		f.join();
	}
	na.close();
	CHECK(!na.has_mixer());
	pd_instance_teardown(mix);
	std::printf("  [%s adc~%d, ring%d fed] peak=%.3f\n", p_tag, p_adc_channel, p_ring_index,
			peak);
	return peak;
}

/** 2: ring i maps to mix input channels 2i, 2i+1 (ring 0 -> ch 1; ring 7 -> ch 16). */
static void test_gather_16ch() {
	// ring 0 fed only: [adc~ 1] sees 0.5, [adc~ 16] sees silence
	const float ch1_ring0 = measure_one_ring("a", 1, 0);
	const float ch16_ring0 = measure_one_ring("a", 16, 0);
	CHECK(ch1_ring0 > 0.45f && ch1_ring0 < 0.55f);
	CHECK(ch16_ring0 < 0.01f);
	// ring 7 fed only: [adc~ 16] sees 0.5, [adc~ 1] sees silence
	const float ch16_ring7 = measure_one_ring("b", 16, 7);
	const float ch1_ring7 = measure_one_ring("b", 1, 7);
	CHECK(ch16_ring7 > 0.45f && ch16_ring7 < 0.55f);
	CHECK(ch1_ring7 < 0.01f);
}

/** 3: set_mixer never called -> the callback is null-safe and renders silence. */
static void test_no_mixer_is_silence() {
	NullPort port;
	NativeAudio na;
	CHECK(na.open(&port, 16, 2, 256, 44100));
	CHECK(na.is_open());
	CHECK(!na.has_mixer());

	MixInputRing ring0(2, 64, 8);
	na.register_worker_ring(&ring0);
	std::atomic<bool> stop{false};
	std::thread feeder = start_ring_feeder(ring0, stop);

	sleep_ms(300); // no set_mixer: the callback must still run, silently

	CHECK(port.frames_rendered() > 0);
	CHECK(port.last_out_peak() == 0.0f);

	stop.store(true);
	ring0.wakeup();
	feeder.join();
	na.close();
	std::printf("no_mixer_is_silence done (frames=%llu, peak=%.3f)\n",
			(unsigned long long)port.frames_rendered(), port.last_out_peak());
}

/**
 * 4: with_mixer_lock serializes the real-time callback's mix render with
 * control ops — while the control thread holds mix_render_lock_, the
 * callback's render blocks on it (it binds + processes the mix under that
 * lock), so the NullPort STALLS (frames don't advance); after release the
 * callback resumes and frames advance again.
 */
static void test_with_mixer_lock_serializes() {
	char text[512];
	std::snprintf(text, sizeof(text), kAdcPatchTemplate, 1);
	write_patch("na_t4_lock.pd", text);
	PdInstance mix = init_pd_instance(2, 2, "na_t4_lock.pd");
	CHECK(mix.file_handle != nullptr);

	NullPort port;
	NativeAudio na;
	CHECK(na.open(&port, 2, 2, 256, 44100));
	CHECK(na.set_mixer(mix.pd, 2, 2));

	MixInputRing ring0(2, 64, 8);
	na.register_worker_ring(&ring0);
	std::atomic<bool> stop{false};
	std::thread feeder([&ring0, &stop]() {
		std::vector<float> block(64 * 2, 0.5f);
		while (true) {
			// Kick-driven (M5 T9): block on the ring's CV until kicked.
			const int target = ring0.wait_for_kick(stop);
			if (target < 0) {
				break;
			}
			ring0.push(block.data(), 64);
			ring0.signal_done(target);
		}
	});

	// Wait until the callback is actually rendering ([adc~ 1] *~ 1.0 -> 0.5).
	int waited = 0;
	while (port.last_out_peak() < 0.4f && waited < 1000) {
		sleep_ms(10);
		waited += 10;
	}
	CHECK(port.last_out_peak() >= 0.4f);

	// While the control thread holds the lock, the callback's render blocks
	// on mix_render_lock_, so the NullPort stalls (frames don't advance);
	// after release it resumes.
	const uint64_t f0 = port.frames_rendered();
	uint64_t f_during = 0;
	{
		na.with_mixer_lock([&]() {
			sleep_ms(150);
			f_during = port.frames_rendered();
		});
	}
	CHECK(f_during - f0 < 5); // stalled while the lock was held
	const uint64_t f1 = port.frames_rendered();
	sleep_ms(120); // resume
	CHECK(port.frames_rendered() - f1 >= 5); // advanced after release

	stop.store(true);
	ring0.wakeup();
	feeder.join();
	na.close();
	pd_instance_teardown(mix);
	std::printf("with_mixer_lock_serializes done (stalled=%llu, resumed=%llu)\n",
			(unsigned long long)(f_during - f0), (unsigned long long)(port.frames_rendered() - f1));
}

/** One a1 synth worker: pinned thread, its own instance, set_instance ONCE. */
struct SynthWorker {
	int id = 0;
	MixInputRing *ring = nullptr;
	std::atomic<bool> *stop = nullptr;
	t_pdinstance *pd = nullptr;
	pthread_t thread;
};

/**
 * a1 model (ported from the spike's synth()): its own pinned thread renders
 * its own instance — libpd_set_instance ONCE, then a process_float loop that
 * pushes 64-frame stereo blocks (Option B) into its own ring at worker pace.
 * Runs until *stop.
 */
static void *synth_worker_main(void *p_arg) {
	SynthWorker *w = static_cast<SynthWorker *>(p_arg);
	// The patch file was pre-written by the test thread before startup.
	w->pd = libpd_new_instance();
	libpd_set_instance(w->pd); // once on this thread; never switched again
	libpd_init_audio(0, 2, 44100);
	turn_dsp_on();
	char patch[64];
	std::snprintf(patch, sizeof(patch), "na_t4_synth%d.pd", w->id);
	libpd_openfile(patch, "/tmp");
	std::vector<float> buf(64 * 2, 0.0f);
	while (true) {
		// Kick-driven (M5 T9): block on the ring's CV until the callback
		// kicks this worker (reliable wakeup; no self-pacing).
		const int target = w->ring->wait_for_kick(*w->stop);
		if (target < 0) {
			break;
		}
		libpd_process_float(1, nullptr, buf.data()); // 1 tick = one 64-frame block
		w->ring->push(buf.data(), 64);
		w->ring->signal_done(target);
	}
	return nullptr;
}

/**
 * 5: 9-concurrent regression (the key multi-instance guard) — ports
 * spike/native_audio/mixdown_multi.c: 8 real synth instances on their own
 * pthreads feed 8 rings; the 16-in/2-out mix-down renders on NativeAudio's
 * dedicated mix thread; the NullPort drain only copies. ~600 ms live, no
 * crash expected.
 */
static void test_nine_concurrent_regression() {
	const int kSynths = 8;
	std::vector<std::unique_ptr<MixInputRing>> rings(kSynths);
	std::vector<SynthWorker> workers(kSynths);
	std::atomic<bool> stop{false};
	for (int i = 0; i < kSynths; i++) {
		char patch[64];
		std::snprintf(patch, sizeof(patch), "na_t4_synth%d.pd", i);
		write_patch(patch, kSinePatch);
		// Option B layout: (2ch, libpd_blocksize()=64, 8 blocks)
		rings[i] = std::make_unique<MixInputRing>(2, 64, 8);
		workers[i].id = i;
		workers[i].ring = rings[i].get();
		workers[i].stop = &stop;
	}
	for (auto &w : workers) {
		CHECK(pthread_create(&w.thread, nullptr, synth_worker_main, &w) == 0);
	}

	// The mix-down instance (16-in/2-out) is init'd on the test thread and
	// rendered on NativeAudio's dedicated mix thread (a DIFFERENT thread).
	write_patch("na_t4_mix9.pd", kMixQuarterPatch);
	PdInstance mix = init_pd_instance(16, 2, "na_t4_mix9.pd");
	CHECK(mix.file_handle != nullptr);

	NullPort port;
	NativeAudio na;
	CHECK(na.open(&port, 16, 2, 256, 44100));
	for (int i = 0; i < kSynths; i++) {
		na.register_worker_ring(rings[i].get()); // ring i -> mix channels 2i, 2i+1
	}
	CHECK(na.set_mixer(mix.pd, 16, 2));
	CHECK(na.has_mixer());
	sleep_ms(600); // 9 concurrent libpd instances, live

	CHECK(port.frames_rendered() > 0);
	CHECK(port.last_out_peak() > 0.02f); // ring0's sine reaches [adc~ 1] *~ 0.25

	stop.store(true);
	for (auto &r : rings) {
		r->wakeup();
	}
	for (auto &w : workers) {
		pthread_join(w.thread, nullptr);
	}
	na.close(); // joins the NullPort audio thread + the mix render thread
	for (auto &w : workers) {
		libpd_free_instance(w.pd); // safe: the worker thread has exited
	}
	pd_instance_teardown(mix);
	std::printf("nine_concurrent done (peak=%.4f, frames=%llu)\n", port.last_out_peak(),
			(unsigned long long)port.frames_rendered());
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	CHECK(libpd_init() == 0);

	test_mixdown_renders();
	test_gather_16ch();
	test_no_mixer_is_silence();
	test_with_mixer_lock_serializes();
	test_nine_concurrent_regression();

	if (failures == 0) {
		std::printf("ALL NATIVE AUDIO MIXDOWN TESTS PASSED\n");
		return 0;
	}
	std::printf("%d FAILURES\n", failures);
	return 1;
}
