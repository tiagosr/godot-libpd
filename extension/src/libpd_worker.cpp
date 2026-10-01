#include "libpd_worker.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <algorithm>

#include "core/pd_debug.h"

namespace godot_libpd {

namespace {

// One timeline line for instance p_id (global ms base, see pd_debug.h).
void wlog(uint32_t p_id, const char *p_fmt, ...) {
	char msg[224];
	va_list ap;
	va_start(ap, p_fmt);
	std::vsnprintf(msg, sizeof(msg), p_fmt, ap);
	va_end(ap);
	char line[320];
	std::snprintf(line, sizeof(line), "[%10.1f ms] inst %u | %s", pd_dbg_elapsed_ms(), p_id, msg);
	pd_dbg_log(p_id, line);
}

// C hook trampolines: libpd calls these on the worker thread.
void c_printhook(const char *p_s) {
	auto *ctx = static_cast<LibpdWorker *>(libpd_get_instancedata());
	if (ctx != nullptr && p_s != nullptr) {
		ctx->emit_print(p_s);
	}
}

void c_noteonhook(int p_channel, int p_pitch, int p_velocity) {
	auto *ctx = static_cast<LibpdWorker *>(libpd_get_instancedata());
	if (ctx != nullptr) {
		ctx->emit_note_on(p_channel, p_pitch, p_velocity);
		// Task 2: also feed the bounded MIDI output queue (spec §4).
		MidiOutMsg msg;
		msg.kind = MidiOutMsg::NOTE;
		msg.channel = (uint8_t)(p_channel & 15);
		msg.d1 = (uint8_t)(p_pitch & 0x7F);
		msg.d2 = (uint8_t)(p_velocity & 0x7F);
		ctx->midi_out.push(msg);
	}
}

void c_controlchangehook(int p_channel, int p_control, int p_value) {
	auto *ctx = static_cast<LibpdWorker *>(libpd_get_instancedata());
	if (ctx != nullptr) {
		MidiOutMsg msg;
		msg.kind = MidiOutMsg::CC;
		msg.channel = (uint8_t)(p_channel & 15);
		msg.d1 = (uint8_t)(p_control & 0x7F);
		msg.d2 = (uint8_t)(p_value & 0x7F);
		ctx->midi_out.push(msg);
	}
}

void c_programchangehook(int p_channel, int p_program) {
	auto *ctx = static_cast<LibpdWorker *>(libpd_get_instancedata());
	if (ctx != nullptr) {
		MidiOutMsg msg;
		msg.kind = MidiOutMsg::PROGRAM_CHANGE;
		msg.channel = (uint8_t)(p_channel & 15);
		msg.d1 = (uint8_t)(p_program & 0x7F);
		ctx->midi_out.push(msg);
	}
}

void c_pitchbendhook(int p_channel, int p_value) {
	auto *ctx = static_cast<LibpdWorker *>(libpd_get_instancedata());
	if (ctx != nullptr) {
		MidiOutMsg msg;
		msg.kind = MidiOutMsg::PITCH_BEND;
		msg.channel = (uint8_t)(p_channel & 15);
		// p_value is CENTERED -8192..8191: s_libpdmidi.c outmidi_pitchbend()
		// re-centers pd's raw value for the hook (CLAMP14BIT(value) - 8192).
		// The queue contract is raw 0..16383 (d1 = low 7 bits, d2 = high 7
		// bits), so normalize back to raw before splitting.
		const int raw = p_value + 8192;
		msg.d1 = (uint8_t)(raw & 0x7F);
		msg.d2 = (uint8_t)((raw >> 7) & 0x7F);
		ctx->midi_out.push(msg);
	}
}

void c_aftertouchhook(int p_channel, int p_pressure) {
	auto *ctx = static_cast<LibpdWorker *>(libpd_get_instancedata());
	if (ctx != nullptr) {
		MidiOutMsg msg;
		msg.kind = MidiOutMsg::AFTERTOUCH;
		msg.channel = (uint8_t)(p_channel & 15);
		msg.d1 = (uint8_t)(p_pressure & 0x7F);
		ctx->midi_out.push(msg);
	}
}

void c_polyaftertouchhook(int p_channel, int p_pitch, int p_pressure) {
	auto *ctx = static_cast<LibpdWorker *>(libpd_get_instancedata());
	if (ctx != nullptr) {
		MidiOutMsg msg;
		msg.kind = MidiOutMsg::POLY_AFTERTOUCH;
		msg.channel = (uint8_t)(p_channel & 15);
		msg.d1 = (uint8_t)(p_pitch & 0x7F);
		msg.d2 = (uint8_t)(p_pressure & 0x7F);
		ctx->midi_out.push(msg);
	}
}

void c_midibytehook(int p_port, int p_byte) {
	(void)p_port;
	auto *ctx = static_cast<LibpdWorker *>(libpd_get_instancedata());
	if (ctx != nullptr) {
		MidiOutMsg msg;
		msg.kind = MidiOutMsg::RAW_BYTE;
		msg.byte = (uint8_t)(p_byte & 0xFF);
		ctx->midi_out.push(msg);
	}
}

} // namespace

LibpdWorker::LibpdWorker(Config p_config) :
		config(p_config) {}

LibpdWorker::~LibpdWorker() {
	request_stop();
	join();
}

void LibpdWorker::start() {
	if (thread_started) {
		return;
	}
	thread_started = true;
	thread = std::thread([this] {
		run();
	});
}

bool LibpdWorker::is_running() const {
	return thread_started;
}

void LibpdWorker::request_stop() {
	stop_requested = true;
	// Nudge the queue so a waiting worker wakes up promptly.
	PdCommand stop;
	stop.opcode = PdCommand::STOP_THREAD;
	queue.push(stop);
}

void LibpdWorker::join() {
	if (thread.joinable()) {
		thread.join();
	}
	thread_started = false;
}

void LibpdWorker::push_command(const PdCommand &p_command) {
	queue.push(p_command);
}

void LibpdWorker::emit_print(const char *p_text) {
	if (!config.on_event) {
		return;
	}
	PdEvent e;
	e.instance_id = config.instance_id;
	e.type = PdEvent::PRINT;
	// Truncate at 63 bytes on a UTF-8 boundary.
	size_t len = std::strlen(p_text);
	if (len > 63) {
		len = 63;
		while (len > 0 && ((static_cast<unsigned char>(p_text[len]) & 0xC0) == 0x80)) {
			len--;
		}
	}
	std::memcpy(e.data, p_text, len);
	e.data[len] = '\0';
	config.on_event(e);
}

void LibpdWorker::emit_note_on(int p_channel, int p_pitch, int p_velocity) {
	if (!config.on_event) {
		return;
	}
	PdEvent e;
	e.instance_id = config.instance_id;
	e.type = PdEvent::NOTE_ON;
	e.data[0] = static_cast<char>(p_channel & 0x7F);
	e.data[1] = static_cast<char>(p_pitch & 0x7F);
	e.data[2] = static_cast<char>(p_velocity & 0x7F);
	config.on_event(e);
}

void LibpdWorker::install_midi_output_hooks(void *p_worker_ptr) {
	libpd_set_noteonhook(c_noteonhook);
	libpd_set_controlchangehook(c_controlchangehook);
	libpd_set_programchangehook(c_programchangehook);
	libpd_set_pitchbendhook(c_pitchbendhook);
	libpd_set_aftertouchhook(c_aftertouchhook);
	libpd_set_polyaftertouchhook(c_polyaftertouchhook);
	libpd_set_midibytehook(c_midibytehook);
	libpd_set_instancedata(p_worker_ptr, nullptr);
}

void LibpdWorker::run() {
	// The whole pd lifetime of this instance lives on this thread.
	using namespace std::chrono;
	wlog(config.instance_id, "thread start (samplerate=%d n_out=%d)", samplerate, n_out);

	uint64_t dsp_blocks = 0;
	bool last_dsp_branch = false;

	for (;;) {
		if (stop_requested) {
			wlog(config.instance_id, "loop: stop_requested -> exit");
			break;
		}

		// 1) Drain all currently pending commands (pd calls only happen here).
		PdCommand command;
		while (queue.pop(&command, 0) == 1) {
			wlog(config.instance_id, "cmd %u", command.opcode);
			execute_command(command);
			if (command.opcode == PdCommand::STOP_THREAD) {
				stop_requested = true;
			}
		}
		if (stop_requested) {
			wlog(config.instance_id, "post-cmd: stop_requested -> exit");
			break;
		}

		// 2) dsp block + pacing.
		if (dsp_on.load() && pd_instance != nullptr && patch_handle != nullptr) {
			if (!last_dsp_branch) {
				wlog(config.instance_id, "dsp branch: ON");
				last_dsp_branch = true;
			}
			const int bs = libpd_blocksize();
			const int frames = bs * n_out;
			if ((int)out_buffer.size() >= frames) {
				libpd_process_float(1, nullptr, out_buffer.data());
				if (config.sink != nullptr) {
					config.sink->push_block(out_buffer.data(), bs, n_out);
				}
			}
			dsp_blocks++;
			if ((dsp_blocks % 1000) == 1) {
				wlog(config.instance_id, "dsp blocks=%llu", (unsigned long long)dsp_blocks);
			}
			// 3) Pacing: sleep to the next tick.
			const auto period = duration_cast<nanoseconds>(
					duration<double>((double)bs / (double)samplerate));
			next_tick += period;
			const auto now = steady_clock::now();
			if (next_tick <= now) {
				// Fell behind: resync, don't spiral.
				next_tick = now + period / 2;
			} else {
				std::this_thread::sleep_until(next_tick);
			}
		} else {
			// 3) Idle: block for a command or the stop signal.
			if (last_dsp_branch) {
				wlog(config.instance_id, "dsp branch: OFF");
				last_dsp_branch = false;
			}
			if (queue.pop(&command, 1000) == 1) {
				wlog(config.instance_id, "idle: cmd %u", command.opcode);
				execute_command(command);
				if (command.opcode == PdCommand::STOP_THREAD) {
					stop_requested = true;
				}
			}
		}
	}

	// Teardown on the worker thread (spec §5).
	if (patch_handle != nullptr) {
		wlog(config.instance_id, "teardown: libpd_closefile start");
		libpd_closefile(patch_handle);
		patch_handle = nullptr;
		wlog(config.instance_id, "teardown: libpd_closefile done");
	}
	if (pd_instance != nullptr) {
		wlog(config.instance_id, "teardown: libpd_free_instance start");
		libpd_free_instance(pd_instance);
		pd_instance = nullptr;
		wlog(config.instance_id, "teardown: libpd_free_instance done; thread exit");
	}
}

void LibpdWorker::execute_command(const PdCommand &p_command) {
	auto fulfill = [&](int p_result) {
		if (p_command.result != nullptr) {
			auto *slot = static_cast<std::shared_ptr<std::promise<int>> *>(p_command.result);
			(*slot)->set_value(p_result);
		}
	};

	switch (p_command.opcode) {
		case PdCommand::INIT: {
			wlog(config.instance_id, "INIT: libpd_new_instance start");
			{
				static std::once_flag pd_globals_once;
				std::call_once(pd_globals_once, [] {
					const int err = libpd_init();
					if (err != 0) {
						std::fprintf(stderr, "godot-libpd: libpd_init() failed (%d)\n", err);
					}
				});
			}
			pd_instance = libpd_new_instance();
			wlog(config.instance_id, "INIT: libpd_new_instance done (null=%d)", pd_instance == nullptr);
			if (pd_instance == nullptr) {
				fulfill(-1);
				return;
			}
			libpd_set_instance(pd_instance);
			samplerate = p_command.i32;
			const int n_ins = (int)((p_command.i64 / 1000) & 0xFF);
			n_out = (int)(p_command.i64 & 0xFF);
			wlog(config.instance_id, "INIT: libpd_init_audio start (%d/%d/%d)", n_ins, n_out, samplerate);
			const int err = libpd_init_audio(n_ins, n_out, samplerate);
			wlog(config.instance_id, "INIT: libpd_init_audio done (err=%d)", err);
			if (err != 0) {
				fulfill(-1);
				return;
			}
			// Turn pd's dsp engine on once; the dsp_on flag in run() gates whether
			// libpd_process_float is actually called (libpd has no realtime callback).
			libpd_start_message(1);
			libpd_add_float(1.0f);
			libpd_finish_message("pd", "dsp");
			blocksize = libpd_blocksize();
			out_buffer.resize((size_t)blocksize * n_out);
			libpd_set_printhook(c_printhook);
			install_midi_output_hooks(this);
			wlog(config.instance_id, "INIT: done (blocksize=%d)", blocksize);
			fulfill(0);
			return;
		}
		case PdCommand::LOAD: {
			if (patch_handle != nullptr) {
				libpd_closefile(patch_handle);
				patch_handle = nullptr;
			}
			const std::string name = p_command.path;
			// libpd_openfile takes (file, dir).
			const size_t slash = name.find_last_of("/\\");
			const std::string file = (slash == std::string::npos) ? name : name.substr(slash + 1);
			const std::string dir = (slash == std::string::npos) ? "." : name.substr(0, slash);
			if (!p_command.search.empty()) {
				libpd_add_to_search_path(p_command.search.c_str());
			}
			wlog(config.instance_id, "LOAD: libpd_openfile start (%s, %s)", file.c_str(), dir.c_str());
			patch_handle = libpd_openfile(file.c_str(), dir.c_str());
			wlog(config.instance_id, "LOAD: libpd_openfile done (ok=%d)", patch_handle != nullptr);
			fulfill(patch_handle != nullptr ? 0 : -1);
			return;
		}
		case PdCommand::UNLOAD: {
			if (patch_handle != nullptr) {
				libpd_closefile(patch_handle);
				patch_handle = nullptr;
			}
			fulfill(0);
			return;
		}
		case PdCommand::MESSAGE: {
			// Parse the space-joined args into atoms (float if numeric, else symbol).
			static thread_local t_atom atoms[16];
			int argc = 0;
			std::string rest = p_command.args;
			size_t pos = 0;
			while (argc < 16 && pos < rest.size()) {
				size_t sp = rest.find(' ', pos);
				std::string tok = (sp == std::string::npos)
						? rest.substr(pos)
						: rest.substr(pos, sp - pos);
				if (!tok.empty()) {
					char *end = nullptr;
					const float f = std::strtof(tok.c_str(), &end);
					if (end != tok.c_str() && *end == '\0') {
						libpd_set_float(&atoms[argc], f);
					} else {
						libpd_set_symbol(&atoms[argc], tok.c_str());
					}
					argc++;
				}
				pos = (sp == std::string::npos) ? rest.size() : sp + 1;
			}
			if (argc == 0 && p_command.args.empty()) {
				libpd_message(p_command.path.c_str(), "", 0, nullptr);
			} else {
				libpd_message(p_command.path.c_str(), "", argc, atoms);
			}
			return;
		}
		case PdCommand::MIDI: {
			const int channel = p_command.i32;
			const int pitch = (int)((p_command.i64 >> 32) & 0x7F);
			const int velocity = (int)(p_command.i64 & 0x7F);
			// High-level API: reaches [notein] (raw libpd_midibyte only feeds [midiin]).
			// velocity == 0 acts as note-off.
			libpd_noteon(channel & 0x0F, pitch & 0x7F, velocity & 0x7F);
			return;
		}
		case PdCommand::MIDI_NOTE: {
			const int channel = p_command.i32 & 0x0F;
			const int d1 = (int)((p_command.i64 >> 8) & 0xFF);
			const int d2 = (int)(p_command.i64 & 0xFF);
			libpd_noteon(channel, d1, d2);
			return;
		}
		case PdCommand::MIDI_CC: {
			const int channel = p_command.i32 & 0x0F;
			const int d1 = (int)((p_command.i64 >> 8) & 0xFF);
			const int d2 = (int)(p_command.i64 & 0xFF);
			libpd_controlchange(channel, d1, d2);
			return;
		}
		case PdCommand::MIDI_PROGRAM_CHANGE: {
			libpd_programchange(p_command.i32 & 0x0F, (int)(p_command.i64 & 0x7F));
			return;
		}
		case PdCommand::MIDI_PITCH_BEND: {
			const int channel = p_command.i32 & 0x0F;
			const int d1 = (int)((p_command.i64 >> 8) & 0xFF);
			const int d2 = (int)(p_command.i64 & 0xFF);
			// Queue contract is raw 0..16383 (value = d1 + d2*128); the libpd
			// API takes centered -8192..8191 (z_libpd.c libpd_pitchbend rejects
			// out-of-range values and re-raws them for pd via inmidi_pitchbend).
			libpd_pitchbend(channel, (d1 + d2 * 128) - 8192);
			return;
		}
		case PdCommand::MIDI_AFTERTOUCH: {
			// i64 = pressure*256 (pressure in d1, matching MidiShortMsg and the
			// POLY_AFTERTOUCH first-data-byte convention).
			libpd_aftertouch(p_command.i32 & 0x0F, (int)((p_command.i64 >> 8) & 0xFF));
			return;
		}
		case PdCommand::MIDI_POLY_AFTERTOUCH: {
			const int channel = p_command.i32 & 0x0F;
			const int d1 = (int)((p_command.i64 >> 8) & 0xFF);
			const int d2 = (int)(p_command.i64 & 0xFF);
			libpd_polyaftertouch(channel, d1, d2);
			return;
		}
		case PdCommand::MIDI_BYTE: {
			// Raw byte: feeds [midiin] only (no cross-feed to [notein] etc.).
			libpd_midibyte(0, (int)(p_command.i64 & 0xFF));
			return;
		}
		case PdCommand::MIDI_SYSEX: {
			// Per-byte libpd_sysex; clamp to the payload array for safety.
			const uint32_t len = p_command.midi_len < (uint32_t)sizeof(p_command.midi)
					? p_command.midi_len
					: (uint32_t)sizeof(p_command.midi);
			for (uint32_t i = 0; i < len; i++) {
				libpd_sysex(0, p_command.midi[i]);
			}
			return;
		}
		case PdCommand::STOP_THREAD: {
			stop_requested = true;
			return;
		}
	}
}

} // namespace godot_libpd
