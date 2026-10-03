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
