#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace godot_libpd {

/**
 * Per-synth-worker handoff ring: a fixed-size circular buffer of complete
 * audio blocks, "latest wins" — gather_latest() always returns the newest
 * block pushed.
 *
 * SPSC: push() from one synth worker thread, gather_latest() from the
 * PortAudio callback thread. A single mutex guards the whole push/gather
 * operation (critical section is one counter read/bump plus a block
 * copy of at most `num_blocks` slots' worth — called once per audio
 * block, ~every 5.8 ms), so a concurrent producer/consumer can never
 * observe a torn (half-written) block.
 *
 * Storage is allocated once in the constructor; nothing is allocated
 * afterwards (the ring is used from a real-time thread).
 */
class MixInputRing {
public:
	/** All constructor arguments must be positive. */
	explicit MixInputRing(int p_channels = 2, int p_blocksize = 256, int p_num_blocks = 8)
		: channels_(p_channels), blocksize_(p_blocksize), num_blocks_(p_num_blocks),
			 storage_(static_cast<size_t>(p_num_blocks) * static_cast<size_t>(p_blocksize) *
					 static_cast<size_t>(p_channels), 0.0f) {}

	/**
	 * Producer (synth worker thread): copy one p_frames-frame block
	 * (p_frames * channels() floats) into the next slot. p_frames must
	 * equal blocksize(). The newest push wins; when the ring is full the
	 * oldest block is silently overwritten.
	 */
	void push(const float *p_block, int p_frames) {
		std::lock_guard<std::mutex> lock(mutex_);
		const size_t samples = static_cast<size_t>(p_frames) * static_cast<size_t>(channels_);
		const size_t slot = count_ % static_cast<size_t>(num_blocks_);
		std::copy_n(p_block, samples, storage_.data() + slot * slot_samples());
		++count_;
	}

	/**
	 * Consumer (PortAudio callback thread): copy the newest complete block
	 * (blocksize() * channels() floats) into p_dst and return blocksize().
	 * If no block has been pushed yet, writes 0.0 to every p_dst sample
	 * and returns 0. p_frames must equal blocksize().
	 */
	int gather_latest(float *p_dst, int p_frames) const {
		std::lock_guard<std::mutex> lock(mutex_);
		if (count_ == 0) {
			std::fill_n(p_dst, static_cast<size_t>(p_frames) * static_cast<size_t>(channels_), 0.0f);
			return 0;
		}
		const size_t slot = (count_ - 1) % static_cast<size_t>(num_blocks_);
		std::copy_n(storage_.data() + slot * slot_samples(), slot_samples(), p_dst);
		return blocksize_;
	}

	/**
	 * Consumer (M5 Task 6, Option B): copy the latest p_n_blocks blocks,
	 * OLDEST-FIRST, into p_dst — each block is blocksize() * channels()
	 * floats, so p_dst must hold p_n_blocks * blocksize() * channels()
	 * floats and is ALWAYS fully written: positions without a pushed block
	 * (the ring has not filled that far) stay silence (0.0).
	 *
	 * Returns min(p_n_blocks, blocks available, num_blocks()): 0 if nothing
	 * has been pushed. Same mutex as gather_latest(); the critical section
	 * is one counter read plus up to p_n_blocks block copies.
	 *
	 * Used by the PortAudio callback to gather the stream blocksize worth
	 * of frames (K = stream_blocksize / ring_blocksize) per render from a
	 * ring that is pushed at the libpd blocksize.
	 */
	int gather_latest_n(float *p_dst, int p_n_blocks) const {
		if (p_dst == nullptr || p_n_blocks <= 0) {
			return 0;
		}
		const int n = std::min(p_n_blocks, num_blocks_); // window capped at ring depth
		// Zero the whole window first: the render path scatters whatever is
		// here unconditionally, so missing blocks must be silence, never stale.
		std::fill_n(p_dst, static_cast<size_t>(n) * slot_samples(), 0.0f);
		std::lock_guard<std::mutex> lock(mutex_);
		const int available =
				count_ >= static_cast<uint64_t>(num_blocks_) ? num_blocks_ : static_cast<int>(count_);
		if (available == 0) {
			return 0;
		}
		const int copy = std::min(n, available);
		// Newest block sits in slot (count_ - 1) % num_blocks_; the oldest
		// block of the requested window sits `copy - 1` slots before it.
		const int oldest_slot = static_cast<int>(((count_ - static_cast<uint64_t>(copy)) % static_cast<uint64_t>(num_blocks_)));
		const int base = n - copy; // missing OLDER slots (0..base-1) stay silence
		for (int i = 0; i < copy; ++i) {
			const int slot = (oldest_slot + i) % num_blocks_;
			std::copy_n(storage_.data() + static_cast<size_t>(slot) * slot_samples(),
					slot_samples(), p_dst + static_cast<size_t>(base + i) * slot_samples());
		}
		return copy;
	}

	int channels() const {
		return channels_;
	}

	int blocksize() const {
		return blocksize_;
	}

	int num_blocks() const {
		return num_blocks_;
	}

private:
	size_t slot_samples() const {
		return static_cast<size_t>(blocksize_) * static_cast<size_t>(channels_);
	}

	int channels_;
	int blocksize_;
	int num_blocks_;
	std::vector<float> storage_; // num_blocks * blocksize * channels floats
	mutable std::mutex mutex_;   // guards count_ (and thus the newest-slot index)
	uint64_t count_ = 0;         // total pushes so far; newest slot = (count_ - 1) % num_blocks_
};

} // namespace godot_libpd
