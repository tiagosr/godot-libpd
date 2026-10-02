// MidiBackend contract tests through a fake backend (v2 M2 Task 1).
//
// No platform MIDI, no device: FakeBackend implements MidiBackend in
// memory, and the real MidiRouter drives it exactly as it drives
// PortMidiBackend. Pins the interface contract the Android RtMidi
// backend (Task 2) must satisfy:
//   1. input: words pushed to poll_input() produce the dual command
//      stream (MIDI_BYTE per wire byte + the high-level command) and
//      signal events on the main thread;
//   2. input sysex: an F0..F7 word run produces exactly one
//      MIDI_SYSEX command carrying the full F0..F7 message;
//   3. output: a NOTE from an instance's output queue is written as a
//      full-form {status, d1, d2} triple;
//   4. output raw bytes: [midiout] RAW_BYTE streams are framed per
//      message with running-status re-expansion (full form on every
//      write, as the PM path did);
//   5. output sysex: a SYSEX message goes out via write_sysex with the
//      full F0..F7 sequence (no per-byte writes);
//   6. create_virtual_loopback: a loopback write reaches the loopback
//      input as words — the in-process round trip that is the on-device
//      test path on Android (no system MIDI devices).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "midi_backend.h"
#include "midi_router.h"
#include "core/midi_output_queue.h"
#include "core/pd_command_queue.h"

using godot_libpd::MidiBackend;
using godot_libpd::MidiBackendPort;
using godot_libpd::MidiError;
using godot_libpd::MidiOutputQueue;
using godot_libpd::MidiOutMsg;
using godot_libpd::MidiRouter;
using godot_libpd::MidiSignalEvent;
using godot_libpd::PdCommand;

static int failures = 0;

#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                 \
		}                                                                \
	} while (0)

namespace {

constexpr int kFakeDevice = 0;
constexpr int kLoopbackIn = 200;
constexpr int kLoopbackOut = 201;

// Packs <= 4 bytes low-byte-first into a backend word.
uint32_t word(uint8_t b0, uint8_t b1 = 0, uint8_t b2 = 0, uint8_t b3 = 0) {
	return (uint32_t)b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16) |
			((uint32_t)b3 << 24);
}

class FakeBackend : public MidiBackend {
public:
	bool available() const override { return true; }
	MidiError initialize() override { return MidiError::OK; }

	std::vector<MidiBackendPort> list_ports() const override {
		std::lock_guard<std::mutex> lock(mutex_);
		std::vector<MidiBackendPort> out;
		MidiBackendPort dev;
		dev.index = kFakeDevice;
		dev.name = "Fake Device";
		dev.is_input = true;
		dev.is_output = true;
		out.push_back(dev);
		for (int i = 0; i < (int)loopback_ports_.size(); ++i) {
			out.push_back(loopback_ports_[i]);
		}
		return out;
	}

	MidiError open_input(int p_index, int p_buffer, PortHandle &r_handle) override {
		std::lock_guard<std::mutex> lock(mutex_);
		if (p_index == kLoopbackIn) {
			r_handle = loopback_in_handle_;
			inputs_.insert(r_handle);
			input_handles_in_order_.push_back(r_handle);
			input_queues_[r_handle];
			return MidiError::OK;
		}
		if (p_index == kFakeDevice) {
			r_handle = next_handle_++;
			inputs_.insert(r_handle);
			input_handles_in_order_.push_back(r_handle);
			input_queues_[r_handle];
			return MidiError::OK;
		}
		last_error_ = "no such input port";
		return MidiError::Failed;
	}

	MidiError open_output(int p_index, int p_buffer, PortHandle &r_handle) override {
		std::lock_guard<std::mutex> lock(mutex_);
		if (p_index == kLoopbackOut) {
			r_handle = loopback_out_handle_;
			outputs_.insert(r_handle);
			output_handles_in_order_.push_back(r_handle);
			return MidiError::OK;
		}
		if (p_index == kFakeDevice) {
			r_handle = next_handle_++;
			outputs_.insert(r_handle);
			output_handles_in_order_.push_back(r_handle);
			return MidiError::OK;
		}
		last_error_ = "no such output port";
		return MidiError::Failed;
	}

	PollResult poll_input(PortHandle p_handle, const WordPush &p_push) override {
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = input_queues_.find(p_handle);
		if (it == input_queues_.end()) {
			last_error_ = "no open input";
			return PollResult::FATAL;
		}
		while (!it->second.empty()) {
			p_push(it->second.front());
			it->second.pop_front();
		}
		return PollResult::OK;
	}

	bool has_host_error(PortHandle, std::string &) override { return false; }

	MidiError write(PortHandle p_handle, const uint8_t *p_bytes, int p_len) override {
		std::lock_guard<std::mutex> lock(mutex_);
		if (outputs_.count(p_handle) == 0) {
			last_error_ = "no open output";
			return MidiError::Failed;
		}
		std::vector<uint8_t> v(p_bytes, p_bytes + p_len);
		writes_.push_back(v);
		// Loopback: chunk the written bytes into <= 4-byte words and
		// deliver them to the loopback input's poll queue (in-process
		// round trip).
		if (p_handle == loopback_out_handle_) {
			std::deque<uint32_t> &q = input_queues_[loopback_in_handle_];
			for (int i = 0; i < p_len; i += 4) {
				uint8_t b[4] = {0, 0, 0, 0};
				for (int k = 0; k < 4 && i + k < p_len; ++k) {
					b[k] = p_bytes[i + k];
				}
				q.push_back(word(b[0], b[1], b[2], b[3]));
			}
		}
		return MidiError::OK;
	}

	MidiError write_sysex(PortHandle p_handle, const uint8_t *p_bytes, int p_len) override {
		std::lock_guard<std::mutex> lock(mutex_);
		if (outputs_.count(p_handle) == 0) {
			last_error_ = "no open output";
			return MidiError::Failed;
		}
		if (p_len < 2 || p_bytes[0] != 0xF0 || p_bytes[p_len - 1] != 0xF7) {
			last_error_ = "malformed sysex";
			return MidiError::InvalidParameter;
		}
		sysex_writes_.push_back(std::vector<uint8_t>(p_bytes, p_bytes + p_len));
		return MidiError::OK;
	}

	MidiError close(PortHandle p_handle) override {
		std::lock_guard<std::mutex> lock(mutex_);
		inputs_.erase(p_handle);
		outputs_.erase(p_handle);
		input_queues_.erase(p_handle);
		return MidiError::OK;
	}

	MidiError create_virtual_input(const std::string &, int &r_index) override {
		r_index = kFakeDevice; // the fake has no separate virtual devices
		return MidiError::OK;
	}

	MidiError create_virtual_output(const std::string &, int &r_index) override {
		r_index = kFakeDevice;
		return MidiError::OK;
	}

	MidiError create_virtual_loopback(const std::string &p_name) override {
		std::lock_guard<std::mutex> lock(mutex_);
		MidiBackendPort in;
		in.index = kLoopbackIn;
		in.name = p_name + " in";
		in.is_input = true;
		MidiBackendPort out;
		out.index = kLoopbackOut;
		out.name = p_name + " out";
		out.is_output = true;
		loopback_ports_.push_back(in);
		loopback_ports_.push_back(out);
		loopback_in_handle_ = next_handle_++;
		loopback_out_handle_ = next_handle_++;
		input_queues_[loopback_in_handle_];
		return MidiError::OK;
	}

	void shutdown() override {}

	const std::string &last_error() const override { return last_error_; }

	// Test helpers.
	void push_input_words(PortHandle p_handle, const std::vector<uint32_t> &p_words) {
		std::lock_guard<std::mutex> lock(mutex_);
		for (uint32_t w : p_words) {
			input_queues_[p_handle].push_back(w);
		}
	}
	std::vector<std::vector<uint8_t>> snapshot_writes() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return writes_;
	}
	std::vector<std::vector<uint8_t>> snapshot_sysex_writes() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return sysex_writes_;
	}
	PortHandle loopback_in_handle() const { return loopback_in_handle_; }
	PortHandle loopback_out_handle() const { return loopback_out_handle_; }
	const std::vector<PortHandle> &open_inputs() const { return input_handles_in_order_; }
	const std::vector<PortHandle> &open_outputs() const { return output_handles_in_order_; }

private:
	mutable std::mutex mutex_;
	bool initialized_ = true;
	int next_handle_ = 1000;
	std::set<PortHandle> inputs_;
	std::set<PortHandle> outputs_;
	std::vector<PortHandle> input_handles_in_order_;
	std::vector<PortHandle> output_handles_in_order_;
	std::unordered_map<PortHandle, std::deque<uint32_t>> input_queues_;
	std::vector<std::vector<uint8_t>> writes_;
	std::vector<std::vector<uint8_t>> sysex_writes_;
	std::vector<MidiBackendPort> loopback_ports_;
	// Loopback handles are fixed (assigned in create_virtual_loopback),
	// so the test can address them directly.
	PortHandle loopback_in_handle_ = -1;
	PortHandle loopback_out_handle_ = -1;
	std::string last_error_ = "ok";
};

// Polls p_pred up to p_timeout_ms; returns true when it holds.
bool wait_for(std::function<bool()> p_pred, int p_timeout_ms) {
	const auto deadline =
			std::chrono::steady_clock::now() + std::chrono::milliseconds(p_timeout_ms);
	while (std::chrono::steady_clock::now() < deadline) {
		if (p_pred()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return p_pred();
}

} // namespace

int main() {
	auto owned = std::make_unique<FakeBackend>();
	FakeBackend *fake = owned.get();
	MidiRouter router(std::move(owned));
	if (!router.available()) {
		std::printf("FAIL router.available() with the fake backend\n");
		return 1;
	}

	std::mutex cmds_mutex;
	std::vector<PdCommand> cmds;
	router.on_midi_command = [&](int64_t p_instance, const PdCommand &p_command) {
		std::lock_guard<std::mutex> lock(cmds_mutex);
		cmds.push_back(p_command);
	};

	// ------------------------------------------------------------------
	// 1. Input: a note-on word -> MIDI_BYTE x3 + MIDI_NOTE.
	// ------------------------------------------------------------------
	const int in_port = router.open_input(kFakeDevice);
	CHECK(in_port >= 0);
	if (in_port >= 0) {
		router.route_input(in_port, 1, true);
		const MidiBackend::PortHandle in_handle =
				fake->open_inputs().empty() ? -1 : fake->open_inputs().front();
		fake->push_input_words(in_handle, {word(0x90, 60, 100)});

		bool got_note = wait_for([&] {
			std::lock_guard<std::mutex> lock(cmds_mutex);
			for (const PdCommand &c : cmds) {
				if (c.opcode == PdCommand::MIDI_NOTE && c.i32 == 0 &&
						c.i64 == (int64_t)60 * 256 + 100) {
					return true;
				}
			}
			return false;
		}, 2000);
		CHECK(got_note);
		if (!got_note) {
			std::printf("FAIL: no MIDI_NOTE (60,100 ch0) delivered\n");
		}
		// Dual delivery: the three wire bytes precede it as MIDI_BYTE.
		{
			std::lock_guard<std::mutex> lock(cmds_mutex);
			int byte_idx = -1;
			int note_idx = -1;
			int n_bytes = 0;
			for (size_t i = 0; i < cmds.size(); ++i) {
				if (cmds[i].opcode == PdCommand::MIDI_BYTE && cmds[i].i64 == 0x90 &&
						byte_idx == -1) {
					byte_idx = (int)i;
				}
				if (cmds[i].opcode == PdCommand::MIDI_NOTE) {
					note_idx = (int)i;
				}
				if (cmds[i].opcode == PdCommand::MIDI_BYTE && i < (size_t)note_idx) {
					n_bytes++;
				}
			}
			CHECK(byte_idx != -1);
			CHECK(note_idx > byte_idx);
			CHECK(n_bytes == 3); // 0x90, 60, 100
		}
	}

	// ------------------------------------------------------------------
	// 2. Input sysex: F0..F7 across two words -> one MIDI_SYSEX.
	// ------------------------------------------------------------------
	{
		const MidiBackend::PortHandle in_handle = fake->open_inputs().front();
		fake->push_input_words(in_handle, {word(0xF0, 0x7E, 0x7F, 0x09),
											word(0x01, 0x02, 0x03, 0xF7)});
		bool got_sysex = wait_for([&] {
			std::lock_guard<std::mutex> lock(cmds_mutex);
			for (const PdCommand &c : cmds) {
				if (c.opcode == PdCommand::MIDI_SYSEX) {
					return true;
				}
			}
			return false;
		}, 2000);
		CHECK(got_sysex);
		{
			std::lock_guard<std::mutex> lock(cmds_mutex);
			int n = 0;
			for (const PdCommand &c : cmds) {
				if (c.opcode == PdCommand::MIDI_SYSEX) {
					n++;
					if (n == 1) {
						CHECK(c.midi_len == 8); // F0 + 6 data + F7
						const uint8_t expect[8] = {0xF0, 0x7E, 0x7F, 0x09, 0x01,
												   0x02, 0x03, 0xF7};
						// (0x7E,0x7F,0x09,0x01,0x02,0x03 = 6 data bytes)
						for (int i = 0; i < 8; ++i) {
							CHECK(c.midi[i] == expect[i]);
						}
						CHECK(c.midi[7] == 0xF7);
					}
				}
			}
			CHECK(n == 1); // exactly one sysex command for the message
		}
	}

	// ------------------------------------------------------------------
	// 3+4+5. Output: note triple, raw-byte framing, sysex path.
	// ------------------------------------------------------------------
	const int out_port = router.open_output(kFakeDevice);
	CHECK(out_port >= 0);
	if (out_port >= 0) {
		MidiOutputQueue queue;
		router.register_instance_output(2, &queue);
		router.route_output(2, out_port, true);

		// 3. NOTE -> full-form triple.
		{
			MidiOutMsg m;
			m.kind = MidiOutMsg::NOTE;
			m.channel = 2;
			m.d1 = 64;
			m.d2 = 90;
			queue.push(m);
			bool got = wait_for([&] {
				auto w = fake->snapshot_writes();
				for (const auto &v : w) {
					if (v.size() == 3 && v[0] == 0x92 && v[1] == 64 && v[2] == 90) {
						return true;
					}
				}
				return false;
			}, 2000);
			CHECK(got);
			if (!got) {
				std::printf("FAIL: no {0x92,64,90} write\n");
			}
		}

		// 4. RAW_BYTE stream (full-form, as pd's [midiout] emits):
		// each message is written as a full-form triple.
		{
			const int before = (int)fake->snapshot_writes().size();
			uint8_t stream[6] = {0xB0, 0x64, 0x7F, 0xB0, 0x10, 0x30}; // CC1 127, CC16 48
			for (uint8_t b : stream) {
				MidiOutMsg m;
				m.kind = MidiOutMsg::RAW_BYTE;
				m.byte = b;
				queue.push(m);
			}
			bool got = wait_for([&] { return fake->snapshot_writes().size() >= (size_t)(before + 2); },
					2000);
			CHECK(got);
			if (got) {
				auto w = fake->snapshot_writes();
				CHECK(w.size() >= (size_t)(before + 2));
				CHECK(w[before].size() == 3 && w[before][0] == 0xB0 &&
						w[before][1] == 0x64 && w[before][2] == 0x7F);
				CHECK(w[before + 1].size() == 3 && w[before + 1][0] == 0xB0 &&
						w[before + 1][1] == 0x10 && w[before + 1][2] == 0x30);
			}
		}
		// 4b. PINNED M1 limitation: a data byte arriving after a
		// complete message is dropped (no running-status re-framing in
		// the raw path). B0 64 7F 10 30 -> exactly one write. This is
		// byte-identical to the pre-refactor PM path (raw_byte_to_stream
		// policy, M1 Task 4 review); pd emits full-form so real patches
		// are unaffected.
		{
			const int before = (int)fake->snapshot_writes().size();
			uint8_t stream[5] = {0xB0, 0x64, 0x7F, 0x10, 0x30};
			for (uint8_t b : stream) {
				MidiOutMsg m;
				m.kind = MidiOutMsg::RAW_BYTE;
				m.byte = b;
				queue.push(m);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
			auto w = fake->snapshot_writes();
			CHECK(w.size() == (size_t)before + 1);
		}

		// 5. SYSEX -> write_sysex with the full F0..F7, no per-byte
		// writes for it.
		{
			const int before = (int)fake->snapshot_writes().size();
			MidiOutMsg m;
			m.kind = MidiOutMsg::SYSEX;
			m.sysex = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
			queue.push(m);
			bool got = wait_for([&] { return fake->snapshot_sysex_writes().size() >= 1; },
					2000);
			CHECK(got);
			if (got) {
				auto s = fake->snapshot_sysex_writes();
				CHECK(s[0] == m.sysex);
			}
			// The router's raw path drops F0..F7 (no [sysexout] in this
			// libpd build); nothing extra reaches write().
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			auto w = fake->snapshot_writes();
			CHECK(w.size() == (size_t)before);
		}
		router.forget_instance(2);
	}

	// ------------------------------------------------------------------
	// 6. Virtual loopback: in-process round trip.
	// ------------------------------------------------------------------
	{
		const int lc = router.create_virtual_loopback("libpd loopback");
		CHECK(lc == 0);
		// Reported by list_*():
		bool listed_in = false, listed_out = false;
		for (auto &p : router.list_inputs()) {
			if (p.second.find("libpd loopback") != std::string::npos) {
				listed_in = true;
			}
		}
		for (auto &p : router.list_outputs()) {
			if (p.second.find("libpd loopback") != std::string::npos) {
				listed_out = true;
			}
		}
		CHECK(listed_in);
		CHECK(listed_out);

		const int lb_in = router.open_input(kLoopbackIn);
		const int lb_out = router.open_output(kLoopbackOut);
		CHECK(lb_in >= 0);
		CHECK(lb_out >= 0);
		if (lb_in >= 0 && lb_out >= 0) {
			router.route_input(lb_in, 4, true);
			router.route_output(3, lb_out, true);
			MidiOutputQueue queue;
			router.register_instance_output(3, &queue);
			MidiOutMsg m;
			m.kind = MidiOutMsg::NOTE;
			m.channel = 0;
			m.d1 = 72;
			m.d2 = 127;
			queue.push(m);
			bool got = wait_for([&] {
				std::lock_guard<std::mutex> lock(cmds_mutex);
				for (const PdCommand &c : cmds) {
					if (c.opcode == PdCommand::MIDI_NOTE && c.i64 == (int64_t)72 * 256 + 127) {
						return true;
					}
				}
				return false;
			}, 2000);
			CHECK(got);
			if (!got) {
				std::printf("FAIL: loopback round trip did not deliver (72,127)\n");
			}
			router.forget_instance(3);
			router.forget_instance(4);
		}
	}

	router.shutdown();
	if (failures != 0) {
		std::printf("%d FAILURE(S)\n", failures);
		return 1;
	}
	std::printf("midi_backend_fake_tests OK\n");
	return 0;
}
