#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
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

	// ── Kick/done sequencer (callback-driven transport, M5 T9) ────────────
	// The PortAudio callback is the single clock: it kicks the owning synth
	// worker to render one block, then waits for that worker to finish before
	// gathering. The kick side uses a condition variable so the worker wakes
	// RELIABLY (a bounded-sleep poll loses a race under load and can leave the
	// callback blocked in wait_done forever). done is a plain atomic the
	// callback polls (the worker is fast; the callback is the sole waiter).
	//
	//   kick_seq_  (callback -> worker): the tick to render; advanced under kick_mu_
	//   done_seq_  (worker   -> callback): written after the worker pushes
	//
	// kick() is called by the callback thread; wait_for_kick() by the owning
	// worker thread; wait_done() by the callback thread. The sequencer is
	// independent of the audio storage above (push/gather are unchanged).

	/** Callback thread: request a render of the owning worker, tagged with the
	 * current tick #p_target. Wakes the worker's condition variable. */
	void kick(int p_target) {
		{
			std::lock_guard<std::mutex> lock(kick_mu_);
			kick_seq_.store(p_target, std::memory_order_release);
		}
		kick_cv_.notify_one();
	}

	/// Wake any thread blocked in wait_for_kick() (e.g. on stop). Re-checks the
	/// predicate, which includes the stop flag.
	void wakeup() {
		kick_cv_.notify_all();
	}

	/**
	 * Worker thread: block until the callback kicks (kick_seq_ advances past
	 * done_seq_) or p_stop is set. Returns the kick target to render, or -1 if
	 * p_stop is set. This is the ONLY place the worker waits for a kick — no
	 * self-pacing, no software clock.
	 */
	int wait_for_kick(const std::atomic<bool> &p_stop) {
		std::unique_lock<std::mutex> lock(kick_mu_);
		kick_cv_.wait(lock, [&] {
			return p_stop.load(std::memory_order_relaxed) ||
				   kick_seq_.load(std::memory_order_relaxed) >
				   done_seq_.load(std::memory_order_relaxed);
		});
		if (p_stop.load(std::memory_order_relaxed)) {
			return -1;
		}
		return kick_seq_.load(std::memory_order_relaxed);
	}

	/** Worker thread: signal that kick #p_target is done (block pushed). */
	void signal_done(int p_target) {
		done_seq_.store(p_target, std::memory_order_release);
	}

	/**
	 * Callback thread: block until the owning worker has completed kick
	 * #p_target (i.e. done_seq_ >= p_target) OR p_closing is set. Plain bounded
	 * sleep — the worker is fast and the callback is the sole waiter; a
	 * per-worker deadline guard is a documented later step. The p_closing flag
	 * makes the wait abortable: when the audio is being closed the workers are
	 * stopped and will not signal done, so an unbounded wait would hang the
	 * audio thread (and hence the close() join) forever.
	 */
	void wait_done(int p_target, const std::atomic<bool> &p_closing) const {
		while (!p_closing.load(std::memory_order_acquire) &&
				done_seq_.load(std::memory_order_acquire) < p_target) {
			std::this_thread::sleep_for(std::chrono::microseconds(50));
		}
	}

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
		last_push_ = std::chrono::steady_clock::now();
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
		// Write at most p_frames * channels_ samples: the caller's buffer
		// (e.g. a real-time device callback) may hold FEWER frames than this
		// ring's blocksize, and a full block copy would overflow it.
		const int frames = std::min(p_frames, blocksize_);
		if (count_ == 0) {
			std::fill_n(p_dst, static_cast<size_t>(frames) * static_cast<size_t>(channels_), 0.0f);
			return 0;
		}
		const size_t slot = (count_ - 1) % static_cast<size_t>(num_blocks_);
		std::copy_n(storage_.data() + slot * slot_samples(),
				static_cast<size_t>(frames) * static_cast<size_t>(channels_), p_dst);
		return frames;
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

	/**
	 * Control-thread only (safe at ring registration: before the first kick
	 * the worker is blocked in wait_for_kick() and pushes nothing): grow the
	 * ring to at least p_min_blocks so the consumer's gather window
	 * (stream_blocksize / ring_blocksize) is never capped by ring depth. A
	 * capped window leaves the older part of each mix block silence/stale
	 * (device symptom: warble + crackle at the block-boundary rate).
	 * Growing resets the fill state: gathers return silence until the window
	 * refills (one stream block of startup silence).
	 */
	void ensure_capacity(int p_min_blocks) {
		if (p_min_blocks <= 0) {
			return;
		}
		std::lock_guard<std::mutex> lock(mutex_);
		if (p_min_blocks <= num_blocks_) {
			return;
		}
		num_blocks_ = p_min_blocks;
		storage_.assign((size_t)num_blocks_ * slot_samples(), 0.0f);
		count_ = 0;
	}

	int num_blocks() const {
		return num_blocks_;
	}

	/**
	 * The stream (mix) blocksize the consuming callback renders each tick —
	 * set by NativeAudio::register_worker_ring. A kick-driven SYNTH worker
	 * renders mix_blocksize()/blocksize() blocks per kick so the gather sees a
	 * CONTIGUOUS window of `mix_blocksize` frames (not one fresh block plus
	 * stale ones from earlier ticks, which sounds like an atonal hum with
	 * discontinuities). 0 until registered; the worker treats 0 as "render one
	 * block" (the unregistered / default case).
	 */
	int mix_blocksize() const {
		return mix_blocksize_;
	}
	void set_mix_blocksize(int p_mix_blocksize) {
		mix_blocksize_ = p_mix_blocksize;
	}

	/**
	 * DIAGNOSTIC: milliseconds since the most recent push (how stale the newest
	 * block is). -1 if nothing has been pushed yet. A healthy ring pushed at the
	 * libpd blocksize pace reads well under one block period (~1.45 ms at 44.1k/
	 * 64); a large value means the producer is falling behind the consumer.
	 */
	double lag_ms() const {
		std::lock_guard<std::mutex> lock(mutex_);
		if (count_ == 0) {
			return -1.0;
		}
		return std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - last_push_).count();
	}

private:
	std::mutex kick_mu_;            // guards kick_seq_ (and the kick cv)
	std::condition_variable kick_cv_; // signaled when kick_seq_ advances
	std::atomic<int> kick_seq_{0}; // callback -> worker: "render block #kick_seq_"
	std::atomic<int> done_seq_{0}; // worker   -> callback: "rendered up to #done_seq_"
	int mix_blocksize_ = 0; // stream blocksize the gather needs (0 until registered)

	size_t slot_samples() const {
		return static_cast<size_t>(blocksize_) * static_cast<size_t>(channels_);
	}

	int channels_;
	int blocksize_;
	int num_blocks_;
	std::vector<float> storage_; // num_blocks * blocksize * channels floats
	mutable std::mutex mutex_;   // guards count_ (and thus the newest-slot index)
	uint64_t count_ = 0;         // total pushes so far; newest slot = (count_ - 1) % num_blocks_
	std::chrono::steady_clock::time_point last_push_{}; // DIAGNOSTIC: newest push time
};

} // namespace godot_libpd
