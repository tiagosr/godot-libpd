// Unit tests for MixInputRing (Task 3).
// Plain CHECK-style harness: any failure prints and exits non-zero.

#include <cstdio>
#include <thread>
#include <vector>

#include "core/mix_input_ring.h"

using namespace godot_libpd;

static int failures = 0;

#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                 \
		}                                                               \
	} while (0)

/** True when every sample in p_block (p_samples floats) equals p_expected. */
static bool block_is_uniform(const float *p_block, int p_samples, float p_expected) {
	for (int i = 0; i < p_samples; i++) {
		if (p_block[i] != p_expected) {
			return false;
		}
	}
	return true;
}

/** Push block A (all 0.25f) then block B (all 0.5f); gather must return B. */
static void test_push_then_gather_returns_latest() {
	MixInputRing ring(2 /* ch */, 256 /* blocksize */, 8 /* num_blocks */);
	CHECK(ring.channels() == 2);
	CHECK(ring.blocksize() == 256);
	CHECK(ring.num_blocks() == 8);

	std::vector<float> a(256 * 2, 0.25f);
	std::vector<float> b(256 * 2, 0.5f);
	ring.push(a.data(), 256);
	ring.push(b.data(), 256);

	std::vector<float> dst(256 * 2, -123.0f); // sentinel: must be fully overwritten
	CHECK(ring.gather_latest(dst.data(), 256) == 256);
	CHECK(block_is_uniform(dst.data(), 256 * 2, 0.5f)); // latest wins
	std::printf("push_then_gather_returns_latest done\n");
}

/** Push 20 distinct blocks; gather must return the 20th block's value. */
static void test_latest_wins_after_many_pushes() {
	MixInputRing ring(2 /* ch */, 256 /* blocksize */, 8 /* num_blocks */);
	for (int i = 1; i <= 20; i++) {
		const float marker = 0.25f * static_cast<float>(i); // exact in float32
		std::vector<float> block(256 * 2, marker);
		ring.push(block.data(), 256);
	}
	std::vector<float> dst(256 * 2, 0.0f);
	CHECK(ring.gather_latest(dst.data(), 256) == 256);
	CHECK(block_is_uniform(dst.data(), 256 * 2, 0.25f * 20.0f)); // the 20th block
	std::printf("latest_wins_after_many_pushes done\n");
}

/** Fresh ring: gather returns 0 and fills dst with 0.0. */
static void test_empty_ring_gathers_silence() {
	MixInputRing ring(2 /* ch */, 256 /* blocksize */, 8 /* num_blocks */);
	std::vector<float> dst(256 * 2, 9.0f); // sentinel: must be fully overwritten
	CHECK(ring.gather_latest(dst.data(), 256) == 0);
	CHECK(block_is_uniform(dst.data(), 256 * 2, 0.0f));
	std::printf("empty_ring_gathers_silence done\n");
}

/**
 * Producer thread pushes 500 blocks (per-block integer marker); consumer
 * thread gathers 500 times concurrently. Every gathered block must be
 * either all-silence or one consistent marker value — no torn/mixed block.
 */
static void test_producer_consumer_no_races() {
	static constexpr int kChannels = 2;
	static constexpr int kBlocksize = 256;
	static constexpr int kBlocks = 500;

	MixInputRing ring(kChannels, kBlocksize, 8 /* num_blocks */);
	const int samples = kChannels * kBlocksize;

	std::thread producer([&ring]() {
		for (int i = 0; i < kBlocks; i++) {
			const float marker = static_cast<float>(i + 1); // 1.0 .. 500.0
			std::vector<float> block(samples, marker);
			ring.push(block.data(), kBlocksize);
		}
	});

	std::thread consumer([&ring]() {
		std::vector<float> dst(samples, 0.0f);
		int silences = 0;
		for (int i = 0; i < kBlocks; i++) {
			const int frames = ring.gather_latest(dst.data(), kBlocksize);
			if (frames == 0) {
				CHECK(block_is_uniform(dst.data(), samples, 0.0f));
				silences++;
				continue;
			}
			CHECK(frames == kBlocksize);
			// No torn/mixed block: every sample carries one consistent value,
			// and that value is one of the pushed integer markers.
			const float v = dst[0];
			CHECK(block_is_uniform(dst.data(), samples, v));
			CHECK(v == 0.0f || (v >= 1.0f && v <= static_cast<float>(kBlocks) &&
											v == static_cast<float>(static_cast<int>(v))));
		}
		std::printf("producer_consumer_no_races done (silences=%d)\n", silences);
	});

	producer.join();
	consumer.join();
}

/** Push num_blocks * 10 blocks; gather returns the most recent. Fixed size. */
static void test_overflow_wraps_fixed_size() {
	MixInputRing ring(2 /* ch */, 256 /* blocksize */, 8 /* num_blocks */);
	const int total = ring.num_blocks() * 10; // 80 blocks >> capacity of 8
	for (int i = 0; i < total; i++) {
		const float marker = static_cast<float>(i + 1);
		std::vector<float> block(256 * 2, marker);
		ring.push(block.data(), 256);
	}
	std::vector<float> dst(256 * 2, 0.0f);
	CHECK(ring.gather_latest(dst.data(), 256) == 256);
	CHECK(block_is_uniform(dst.data(), 256 * 2, static_cast<float>(total))); // most recent
	// Fixed size: accessors unchanged; the ring cannot grow.
	CHECK(ring.num_blocks() == 8);
	CHECK(ring.blocksize() == 256);
	CHECK(ring.channels() == 2);
	std::printf("overflow_wraps_fixed_size done\n");
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	test_push_then_gather_returns_latest();
	test_latest_wins_after_many_pushes();
	test_empty_ring_gathers_silence();
	test_producer_consumer_no_races();
	test_overflow_wraps_fixed_size();

	if (failures == 0) {
		std::printf("ALL MIX INPUT RING TESTS PASSED\n");
		return 0;
	}
	std::printf("%d FAILURES\n", failures);
	return 1;
}
