#include "libpd_worker.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <algorithm>

#include "core/pd_debug.h"

namespace godot_libpd {

namespace {

// Callback-driven synth kick (M5 T9): the owning SYNTH worker idles in
// SYNTH_IDLE_SLEEP_US steps while waiting for the PortAudio callback to kick
// it. This bounds the wakeup latency the callback waits on. Confirmed 200 µs
// for the first cut; the documented tunable for portable (low-power) targets
// (shorten it, or replace the bounded sleep with a futex/eventfd shim) is
// this single constant.
constexpr int SYNTH_IDLE_SLEEP_US = 200;

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

namespace {
std::once_flag g_pd_globals_once;
}

void LibpdWorker::init_pd_globals_once() {
	std::call_once(g_pd_globals_once, [] {
		const int err = libpd_init();
		if (err != 0) {
			std::fprintf(stderr, "godot-libpd: libpd_init() failed (%d)\n", err);
		}
	});
}

void LibpdWorker::adopt_precreated_instance(t_pdinstance *p_pd, int p_samplerate, int p_n_out) {
	// Records the main-thread-created instance so the worker adopts it (INIT) and
	// tears it down (run) on its own thread. The worker never creates it.
	pd_instance = p_pd;
	samplerate = p_samplerate;
	n_out = p_n_out;
	blocksize = libpd_blocksize();
	out_buffer.resize((size_t)blocksize * (size_t)n_out);
	has_precreated_instance_ = true;
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
	// Kick-loop workers (SYNTH) block on the ring's condition variable, not
	// the command queue — nudge it so wait_for_kick() re-checks stop_requested
	// and exits (a kick will not arrive once the mixer is unbound).
	if (config.worker_ring != nullptr) {
		config.worker_ring->wakeup();
	}
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

void LibpdWorker::close_patch() {
	if (patch_handle == nullptr) {
		return;
	}
	if (config.role == WorkerRole::MIXER && pd_instance != nullptr) {
		// Re-bind before closefile (see the declaration, Task 4 finding).
		libpd_set_instance(pd_instance);
	}
	libpd_closefile(patch_handle);
	patch_handle = nullptr;
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

		// 2) dsp block. MIXER never renders (the PortAudio callback renders the
		//    mix instance). SYNTH renders WHEN KICKED by the callback (M5 T9,
		//    no self-pacing); ANDROID/a1 render self-paced.
		if (dsp_on.load() && pd_instance != nullptr && patch_handle != nullptr &&
				config.role != WorkerRole::MIXER) {
			if (!last_dsp_branch) {
				wlog(config.instance_id, "dsp branch: ON");
				last_dsp_branch = true;
			}
			const int bs = libpd_blocksize();
			const int frames = bs * n_out;
			if (config.role == WorkerRole::SYNTH && config.worker_ring != nullptr) {
				// ── SYNTH (M5 T9): render WHEN KICKED by the callback. The
				//    PortAudio callback is the single clock — no software clock,
				//    no sleep_until. Block on the ring's condition variable until
				//    the callback advances kick_seq_ (reliable wakeup, no poll).
				MixInputRing *ring = config.worker_ring;
				const int target = ring->wait_for_kick(stop_requested);
				if (target < 0) {
					// stop_requested was set: exit the loop.
					break;
				}
				// Kicked: render the FULL stream window the callback will gather
				// (mix_blocksize frames == K ring-blocks), pushing each block to the
				// ring, so the gather sees a CONTIGUOUS window — not one fresh block
				// plus stale blocks from earlier ticks (an atonal hum + discontinuities).
				// K = mix_blocksize / ring_blocksize (0 or unregistered -> 1 block).
				const int k = ring->mix_blocksize() > ring->blocksize()
						? ring->mix_blocksize() / ring->blocksize()
						: 1;
				const auto t_render_start = steady_clock::now();
				if ((int)out_buffer.size() >= frames) {
					for (int i = 0; i < k; ++i) {
						libpd_process_float(1, nullptr, out_buffer.data());
						ring->push(out_buffer.data(), bs);
					}
				}
				const auto t_render_done = steady_clock::now();
				if (std::getenv("RDIAG") != nullptr && (dsp_blocks % 50) == 1) {
					const double render_ms =
							duration_cast<nanoseconds>(t_render_done - t_render_start).count() / 1e6;
						fprintf(stderr, "[rdiag] synth id=%lld render+push=%.4f ms\n",
								(long long)config.instance_id, render_ms);
					}
				ring->signal_done(target);
				dsp_blocks++;
			} else {
				// ── ANDROID/a1: self-paced (existing behavior, unchanged).
				const auto t_render_start = steady_clock::now();
				if ((int)out_buffer.size() >= frames) {
					libpd_process_float(1, nullptr, out_buffer.data());
					if (config.sink != nullptr) {
						config.sink->push_block(out_buffer.data(), bs, n_out);
					}
				}
				const auto t_render_done = steady_clock::now();
				if (std::getenv("RDIAG") != nullptr && (dsp_blocks % 50) == 1) {
					const double render_ms =
							duration_cast<nanoseconds>(t_render_done - t_render_start).count() / 1e6;
						fprintf(stderr, "[rdiag] synth id=%lld render+push=%.4f ms\n",
								(long long)config.instance_id, render_ms);
					}
				// Pacing: sleep to the next tick (self-clock, a1/Android only).
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
				dsp_blocks++;
			}
			if ((dsp_blocks % 1000) == 1) {
				wlog(config.instance_id, "dsp blocks=%llu", (unsigned long long)dsp_blocks);
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

	// Teardown on the worker thread (spec §5). MIXER runs the whole
	// teardown under with_mixer_lock: the PortAudio callback may still be
	// rendering the mix instance (Task 4 finding).
	if (patch_handle != nullptr || pd_instance != nullptr) {
		with_lock([&] {
			if (patch_handle != nullptr) {
				wlog(config.instance_id, "teardown: libpd_closefile start");
				close_patch();
				wlog(config.instance_id, "teardown: libpd_closefile done");
			}
			if (pd_instance != nullptr) {
				wlog(config.instance_id, "teardown: libpd_free_instance start");
				libpd_free_instance(pd_instance);
				pd_instance = nullptr;
				wlog(config.instance_id, "teardown: libpd_free_instance done; thread exit");
			}
		});
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
			// MIXER: the PortAudio callback may be rendering the mix instance
			// concurrently, so the whole INIT body runs under with_mixer_lock
			// (Task 4 finding). SYNTH/ANDROID: runs as-is (existing path).
			with_lock([&] {
				if (has_precreated_instance_) {
					// MIXER, T8 root-cause fix: the instance was created on the MAIN
					// thread (option A). Adopt it on THIS thread (thread-local pd_this)
					// and install the worker's hooks; libpd_new_instance / init_audio /
					// dsp-on already happened on the main thread. The worker renders
					// nothing (MIXER) but owns the teardown (closefile/free_instance).
					wlog(config.instance_id, "INIT: adopting pre-created instance");
					libpd_set_instance(pd_instance);
					blocksize = libpd_blocksize();
					out_buffer.resize((size_t)blocksize * n_out);
					libpd_set_printhook(c_printhook);
					install_midi_output_hooks(this);
					wlog(config.instance_id, "INIT: done (adopted, blocksize=%d)", blocksize);
					fulfill(0);
					return;
				}
				wlog(config.instance_id, "INIT: libpd_new_instance start");
				init_pd_globals_once();
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
			});
			return;
		}
		case PdCommand::LOAD: {
			// MIXER: openfile is RT-sensitive — under with_mixer_lock (Task 4).
			with_lock([&] {
				close_patch();
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
			});
			return;
		}
		case PdCommand::UNLOAD: {
			// MIXER: closefile is RT-sensitive — under with_mixer_lock (Task 4).
			with_lock([&] {
				close_patch();
				fulfill(0);
			});
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
			// Send with the correct pd message type so the [r ...] receiver
			// forwards the RIGHT thing to its outlets. libpd_message(recv, "")
			// goes through receive_anything() -> outlet_anything(), which
			// forwards an empty-selector typedmess that audio objects (osc~,
			// dac~, ...) reject with "error: <obj>: no method for ''". Use the
			// typed entry points: bang (no args), float/symbol (one arg), or
			// list (multiple args).
			const char *recv = p_command.path.c_str();
			if (argc == 0) {
				libpd_bang(recv);
			} else if (argc == 1) {
				if (atoms[0].a_type == A_FLOAT) {
					libpd_float(recv, atoms[0].a_w.w_float);
				} else {
					libpd_symbol(recv, atoms[0].a_w.w_symbol->s_name);
				}
			} else {
				libpd_list(recv, argc, atoms);
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
