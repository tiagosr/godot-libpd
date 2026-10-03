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
	// Seeds the system device set with the default fake device (index 0,
	// in+out) so the pre-existing tests that open kFakeDevice keep working.
	// Hotplug tests override this with set_devices() before constructing
	// their router.
	FakeBackend() {
		MidiBackendPort dev;
		dev.index = kFakeDevice;
		dev.name = "Fake Device";
		dev.is_input = true;
		dev.is_output = true;
		devices_.push_back(dev);
	}

	bool available() const override { return true; }
	MidiError initialize() override { return MidiError::OK; }

	std::vector<MidiBackendPort> list_ports() const override {
		std::lock_guard<std::mutex> lock(mutex_);
		// The system device set is mutable so the hotplug tests can simulate
		// plug/unplug. empty_for_ simulates a transient empty enumeration
		// (a bad probe tick) for a bounded number of consecutive calls.
		if (empty_for_ > 0) {
			--empty_for_;
			return {};
		}
		std::vector<MidiBackendPort> out;
		out.reserve(devices_.size() + loopback_ports_.size());
		for (const auto &d : devices_) {
			out.push_back(d);
		}
		for (const auto &d : loopback_ports_) {
			out.push_back(d);
		}
		return out;
	}

	MidiError open_input(int p_index, int p_buffer, PortHandle &r_handle) override {
		std::lock_guard<std::mutex> lock(mutex_);
		if (p_index == kLoopbackIn) {
			r_handle = loopback_in_handle_;
		} else {
			bool ok = false;
			for (const auto &d : devices_) {
				if (d.index == p_index && d.is_input) {
					ok = true;
					break;
				}
			}
			if (!ok) {
				last_error_ = "no such input port";
				return MidiError::Failed;
			}
			r_handle = next_handle_++;
		}
		inputs_.insert(r_handle);
		input_handles_in_order_.push_back(r_handle);
		input_queues_[r_handle];
		return MidiError::OK;
	}

	MidiError open_output(int p_index, int p_buffer, PortHandle &r_handle) override {
		std::lock_guard<std::mutex> lock(mutex_);
		if (p_index == kLoopbackOut) {
			r_handle = loopback_out_handle_;
		} else {
			bool ok = false;
			for (const auto &d : devices_) {
				if (d.index == p_index && d.is_output) {
					ok = true;
					break;
				}
			}
			if (!ok) {
				last_error_ = "no such output port";
				return MidiError::Failed;
			}
			r_handle = next_handle_++;
		}
		outputs_.insert(r_handle);
		output_handles_in_order_.push_back(r_handle);
		return MidiError::OK;
	}

	PollResult poll_input(PortHandle p_handle, const WordPush &p_push) override {
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = input_queues_.find(p_handle);
		if (it == input_queues_.end()) {
			last_error_ = "no open input";
			return PollResult::FATAL;
		}
		while (!it->second.empty()) {
			const uint32_t w = it->second.front();
			// Fake words are status-first short events or 4-byte sysex words
			// (PortMIDI layout); infer the valid-byte count the way the read
			// stage needs it. (No running-status-pair delivery via this double
			// today; add per-word counts if that is ever exercised.)
			const uint8_t b0 = static_cast<uint8_t>(w & 0xFF);
			const int count = (b0 < 0x80 || b0 == 0xF0) ? 4 : midi_short_bytes(b0);
			p_push(w, count);
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
		++close_calls_;
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

	// Hotplug test helpers (M4). Replace / grow the system device set the
	// way a USB plug/unplug would; set_empty_list_for simulates a
	// transient empty enumeration (a bad probe tick) for the next n
	// consecutive list_ports() calls.
	void set_devices(std::vector<MidiBackendPort> p_devs) {
		std::lock_guard<std::mutex> lock(mutex_);
		devices_ = std::move(p_devs);
	}
	void add_device(int p_index, const std::string &p_name, bool p_in, bool p_out) {
		std::lock_guard<std::mutex> lock(mutex_);
		MidiBackendPort d;
		std::vector<MidiBackendPort> nd = devices_;
		d.index = p_index;
		d.name = p_name;
		d.is_input = p_in;
		d.is_output = p_out;
		nd.push_back(d);
		devices_ = std::move(nd);
	}
	void set_empty_list_for(int p_n) {
		std::lock_guard<std::mutex> lock(mutex_);
		empty_for_ = p_n;
	}
	// Count of close() calls — proves a router auto-close reached the backend.
	int close_calls() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return close_calls_;
	}

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
	// The mutable system device set (hotplug tests). Seeded in the ctor.
	std::vector<MidiBackendPort> devices_;
	int close_calls_ = 0;
	mutable int empty_for_ = 0; // mutable: decremented in const list_ports()
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

// Builds a MidiBackendPort for set_devices()/add_device().
MidiBackendPort mport(int p_index, const char *p_name, bool p_in, bool p_out) {
	MidiBackendPort p;
	p.index = p_index;
	p.name = p_name;
	p.is_input = p_in;
	p.is_output = p_out;
	return p;
}

// Captures a router's on_port_changed + on_port_error for hotplug assertions.
struct HotplugProbe {
	mutable std::mutex m;
	struct Ev {
		bool added;
		std::string kind;
		int index;
		std::string name;
	};
	struct Err {
		int port_id;
		std::string what;
	};
	std::vector<Ev> events;
	std::vector<Err> errors;
	void bind(MidiRouter &r) {
		r.on_port_changed = [this](bool added, const char *kind, int index, const char *name) {
			std::lock_guard<std::mutex> l(m);
			events.push_back(Ev{added, kind, index, name});
		};
		r.on_port_error = [this](int port_id, const char *what) {
			std::lock_guard<std::mutex> l(m);
			errors.push_back(Err{port_id, what});
		};
	}
	int count_added(const std::string &p_name) const {
		std::lock_guard<std::mutex> l(m);
		int n = 0;
		for (const Ev &e : events) {
			if (e.added && e.name == p_name) {
				++n;
			}
		}
		return n;
	}
	int count_removed(const std::string &p_name) const {
		std::lock_guard<std::mutex> l(m);
		int n = 0;
		for (const Ev &e : events) {
			if (!e.added && e.name == p_name) {
				++n;
			}
		}
		return n;
	}
	bool has_error(int p_port_id, const std::string &p_what) const {
		std::lock_guard<std::mutex> l(m);
		for (const Err &e : errors) {
			if (e.port_id == p_port_id && e.what == p_what) {
				return true;
			}
		}
		return false;
	}
};

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

	// ------------------------------------------------------------------
	// 7. Hotplug: I/O-thread re-enumeration + diff (M4 Task 1).
	//    Each test uses its own fresh FakeBackend + MidiRouter so state
	//    does not leak between them.
	// ------------------------------------------------------------------

	// 7.1 Startup annotation: everything present at init fires port_added.
	{
		HotplugProbe probe;
		auto owned = std::make_unique<FakeBackend>();
		owned->set_devices({mport(0, "A", true, false), mport(1, "B", false, true)});
		MidiRouter r(std::move(owned));
		probe.bind(r);
		r.set_poll_interval(0.02);
		bool ok = wait_for([&] { return probe.count_added("A") == 1 && probe.count_added("B") == 1; }, 500);
		CHECK(ok);
		CHECK(probe.count_added("A") == 1);
		CHECK(probe.count_added("B") == 1);
		r.shutdown();
	}

	// 7.2 Add diff: a port appearing mid-run fires exactly one port_added.
	{
		HotplugProbe probe;
		auto owned = std::make_unique<FakeBackend>();
		FakeBackend *hp = owned.get();
		hp->set_devices({mport(0, "A", true, false)});
		MidiRouter r(std::move(owned));
		probe.bind(r);
		r.set_poll_interval(0.02);
		wait_for([&] { return probe.count_added("A") == 1; }, 500);
		hp->add_device(2, "C", true, false);
		bool ok = wait_for([&] { return probe.count_added("C") == 1; }, 500);
		CHECK(ok);
		CHECK(probe.count_added("C") == 1);
		CHECK(probe.count_removed("A") == 0); // A still present
		r.shutdown();
	}

	// 7.3 Remove diff + auto-close (non-empty list: some removed).
	{
		HotplugProbe probe;
		auto owned = std::make_unique<FakeBackend>();
		FakeBackend *hp = owned.get();
		hp->set_devices({mport(0, "A", true, false), mport(1, "B", true, false)});
		MidiRouter r(std::move(owned));
		probe.bind(r);
		r.set_poll_interval(0.02);
		wait_for([&] { return probe.count_added("A") == 1 && probe.count_added("B") == 1; }, 500);
		const int a_port = r.open_input(0); // open A (real device)
		CHECK(a_port >= 0);
		const int cc_before = hp->close_calls();
		hp->set_devices({mport(1, "B", true, false)}); // remove A, keep B
		bool ok = wait_for([&] { return probe.count_removed("A") == 1 && probe.has_error(a_port, "device removed"); }, 500);
		CHECK(ok);
		CHECK(probe.count_removed("A") == 1);
		CHECK(probe.count_removed("B") == 0); // B survives
		CHECK(probe.has_error(a_port, "device removed"));
		CHECK(hp->close_calls() > cc_before); // backend stream closed
		r.shutdown();
	}

	// 7.4 Index shift: a removal re-indexes a survivor; name-keyed diff must
	//     NOT spuriously remove the survivor or close its port.
	{
		HotplugProbe probe;
		auto owned = std::make_unique<FakeBackend>();
		FakeBackend *hp = owned.get();
		hp->set_devices({mport(0, "A", true, false), mport(1, "C", true, false)});
		MidiRouter r(std::move(owned));
		probe.bind(r);
		r.set_poll_interval(0.02);
		wait_for([&] { return probe.count_added("A") == 1 && probe.count_added("C") == 1; }, 500);
		const int c_port = r.open_input(1); // open C (index 1)
		CHECK(c_port >= 0);
		hp->set_devices({mport(0, "C", true, false)}); // A gone; C shifts to 0
		bool a_removed = wait_for([&] { return probe.count_removed("A") == 1; }, 500);
		CHECK(a_removed);
		CHECK(probe.count_removed("C") == 0); // C NOT removed (name-keyed)
		CHECK(!probe.has_error(c_port, "device removed")); // C port not closed
		r.shutdown();
	}

	// 7.5 Transient-empty blip: one empty enumeration then recovery is ignored
	//     (no removal, no auto-close).
	{
		HotplugProbe probe;
		auto owned = std::make_unique<FakeBackend>();
		FakeBackend *hp = owned.get();
		hp->set_devices({mport(0, "A", true, true)});
		MidiRouter r(std::move(owned));
		probe.bind(r);
		r.set_poll_interval(0.02);
		wait_for([&] { return probe.count_added("A") == 1; }, 500);
		const int in_port = r.open_input(0);
		CHECK(in_port >= 0);
		const int cc_before = hp->close_calls();
		hp->set_empty_list_for(1); // exactly one bad (empty) tick
		std::this_thread::sleep_for(std::chrono::milliseconds(120)); // let it resolve
		CHECK(probe.count_removed("A") == 0); // blip ignored
		CHECK(!probe.has_error(in_port, "device removed"));
		CHECK(hp->close_calls() == cc_before); // port not closed
		r.shutdown();
	}

	// 7.6 Confirm empty (only device unplugged, stays gone): the debounce
	//     confirms on the 2nd consecutive empty tick -> removed + auto-close.
	{
		HotplugProbe probe;
		auto owned = std::make_unique<FakeBackend>();
		FakeBackend *hp = owned.get();
		hp->set_devices({mport(0, "A", true, false)});
		MidiRouter r(std::move(owned));
		probe.bind(r);
		r.set_poll_interval(0.02);
		wait_for([&] { return probe.count_added("A") == 1; }, 500);
		const int in_port = r.open_input(0);
		const int cc_before = hp->close_calls();
		hp->set_devices({}); // A unplugged and stays gone
		bool ok = wait_for([&] { return probe.count_removed("A") == 1 && probe.has_error(in_port, "device removed"); }, 500);
		CHECK(ok);
		CHECK(probe.count_removed("A") == 1);
		CHECK(probe.has_error(in_port, "device removed"));
		CHECK(hp->close_calls() > cc_before);
		r.shutdown();
	}

	// 7.7 Virtual/loopback not auto-closed: a real device is removed while the
	//     in-process loopback (non-real) stays listed; only the real port closes.
	{
		HotplugProbe probe;
		auto owned = std::make_unique<FakeBackend>();
		FakeBackend *hp = owned.get();
		hp->set_devices({mport(0, "A", true, false)});
		MidiRouter r(std::move(owned));
		probe.bind(r);
		r.set_poll_interval(0.02);
		wait_for([&] { return probe.count_added("A") == 1; }, 500);
		r.create_virtual_loopback("LB");
		const int lb_in = r.open_input(kLoopbackIn);
		CHECK(lb_in >= 0);
		const int a_port = r.open_input(0); // open the real device A
		CHECK(a_port >= 0);
		hp->set_devices({}); // A gone; the loopback stays in list_ports
		bool ok = wait_for([&] { return probe.count_removed("A") == 1; }, 500);
		CHECK(ok);
		CHECK(probe.has_error(a_port, "device removed")); // real device auto-closed
		CHECK(!probe.has_error(lb_in, "device removed")); // loopback NOT auto-closed
		r.shutdown();
	}

	// 7.8 refresh_ports(): forces an immediate diff, returns the change count.
	{
		HotplugProbe probe;
		auto owned = std::make_unique<FakeBackend>();
		FakeBackend *hp = owned.get();
		hp->set_devices({mport(0, "A", true, false)});
		MidiRouter r(std::move(owned));
		probe.bind(r);
		r.set_poll_interval(10.0); // long: periodic ticks won't fire in the test
		// Force the initial enumeration with a refresh -> annotates A.
		const int n0 = r.refresh_ports();
		CHECK(n0 == 1); // A added
		CHECK(probe.count_added("A") == 1);
		// Add a device, then force it with a refresh (no periodic tick relied on).
		hp->add_device(1, "C", true, false);
		const int n = r.refresh_ports();
		CHECK(n == 1); // C added
		CHECK(probe.count_added("C") == 1);
		// Nothing changed -> 0.
		const int n2 = r.refresh_ports();
		CHECK(n2 == 0);
		r.shutdown();
	}

	router.shutdown();
	if (failures != 0) {
		std::printf("%d FAILURE(S)\n", failures);
		return 1;
	}
	std::printf("midi_backend_fake_tests OK\n");
	return 0;
}
