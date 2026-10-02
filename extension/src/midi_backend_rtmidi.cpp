// RtMidi Android backend (v2 M2, Task 2) — see midi_backend_rtmidi.h.
//
// Wraps the vendored RtMidi ANDROID_AMIDI backend (pinned 759d4e6,
// which carries the local multi-chunk-sysex fix on top of upstream
// 23b8cd5) behind the MidiBackend interface. __ANDROID__ only: on
// other hosts this file compiles to nothing (the factory in
// midi_backend_factory.cpp picks the backend per platform).
//
// Threading: the router I/O thread calls every method here;
// RtMidi's own pollMidi pthread delivers input through the
// input_callback below into the per-port WordRing (the callback
// never touches a router lock). The asynchronous device open
// (MidiManager.openDevice on the main looper) is settled by a
// bounded wait on this thread — see the header note.

#if defined(__ANDROID__)

#include "midi_backend_rtmidi.h"

#include <android/log.h>
#include <jni.h>

#include <cstddef>
#include <cstdio>
#include <dlfcn.h>
#include <cstring>
#include <utility>
#include <vector>

namespace godot_libpd {

namespace {

constexpr const char *kLogTag = "libpd.midi.rtmidi";

#define RLOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

// ---------------------------------------------------------------------------
// JavaVM capture. The app's linker namespace ("clns-N") cannot dlopen
// libart at all (verified on-device: /apex/com.android.art is "not
// accessible for the namespace clns-6") and the VM is not in the
// RTLD_DEFAULT scope for app processes either — but the libart.so
// TEXT is mapped into our address space (it is the JVM itself) and
// /proc/self/mem is readable by the app. So the JNI_GetCreatedJavaVMs
// entry point is located by parsing libart.so's own ELF dynamic
// symbol table through /proc/self/mem and the function pointer is
// taken at its mapped address. (The JavaVM* itself is the same
// process-wide singleton either way.)
// ---------------------------------------------------------------------------

typedef jint (*JniGetCreatedVMsFn)(JavaVM **, jsize, jsize *);

JavaVM *g_jvm = nullptr;

namespace {

struct Mapping {
	uintptr_t start = 0;
	uintptr_t end = 0;
};

// The address range of the named library's first file mapping
// (its ELF load base). Returns false if not mapped.
bool find_mapping_base(const char *p_lib, Mapping *r_map) {
	FILE *f = fopen("/proc/self/maps", "r");
	if (f == nullptr) {
		return false;
	}
	// Match only the exact /lib64/<p_lib> path suffix — a plain
	// strstr("libart") would land on libart-compiler.so first, whose
	// dynamic symbols do not contain the JNI bootstrap entry.
	const size_t liblen = strlen(p_lib);
	bool found = false;
	char line[512];
	while (fgets(line, sizeof(line), f) != nullptr) {
		const char *path = strrchr(line, '/');
		if (path == nullptr) {
			continue;
		}
		char name[128];
		if (snprintf(name, sizeof(name), "%s", path) >=
				(int)sizeof(name)) {
			continue;
		}
		name[strcspn(name, "\r\n")] = '\0';
		if (strlen(name) < liblen ||
			strcmp(name + strlen(name) - liblen, p_lib) != 0) {
			continue;
		}
		uintptr_t lo = 0;
		uintptr_t hi = 0;
		if (sscanf(line, "%lx-%lx", &lo, &hi) == 2) {
			r_map->start = lo;
			r_map->end = hi;
			found = true;
			break; // first mapping = ELF header at the load base
		}
	}
	fclose(f);
	return found;
}

// Reads p_len bytes from the current process's memory (our own
// address space, which is always readable via /proc/self/mem).
bool read_self_mem(uintptr_t p_addr, void *p_out, size_t p_len) {
	FILE *f = fopen("/proc/self/mem", "r");
	if (f == nullptr) {
		return false;
	}
	if (fseeko(f, (off_t)p_addr, SEEK_SET) != 0) {
		fclose(f);
		return false;
	}
	const size_t n = fread(p_out, 1, p_len, f);
	fclose(f);
	return n == p_len;
}

// True if p_addr falls inside an r-x mapping of the named library.
bool address_in_exec_mapping(uintptr_t p_addr, const char *p_lib) {
	FILE *f = fopen("/proc/self/maps", "r");
	if (f == nullptr) {
		return false;
	}
	const size_t liblen = strlen(p_lib);
	bool ok = false;
	char line[512];
	while (fgets(line, sizeof(line), f) != nullptr) {
		if (strstr(line, "r-xp") == nullptr) {
			continue;
		}
		const char *path = strrchr(line, '/');
		if (path == nullptr) {
			continue;
		}
		char name[128];
		if (snprintf(name, sizeof(name), "%s", path) >= (int)sizeof(name)) {
			continue;
		}
		name[strcspn(name, "\r\n")] = '\0';
		if (strlen(name) < liblen ||
			strcmp(name + strlen(name) - liblen, p_lib) != 0) {
			continue;
		}
		uintptr_t lo = 0;
		uintptr_t hi = 0;
		if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && lo <= p_addr &&
				p_addr < hi) {
			ok = true;
			break;
		}
	}
	fclose(f);
	return ok;
}

} // namespace

bool ensure_jvm() {
	if (g_jvm != nullptr) {
		return true;
	}
	// 1) Find libart's load base (the first /apex/com.android.art
	//    mapping in /proc/self/maps).
	Mapping map;
	if (!find_mapping_base("libart.so", &map)) {
		RLOGE("libart mapping not found in /proc/self/maps");
		return false;
	}
	// 2) Walk the ELF: PT_DYNAMIC -> DT_SYMTAB/DT_STRTAB/DT_STRSZ,
	//    then scan the dynamic symbols for JNI_GetCreatedJavaVMs.
	char hdr[64];
	if (!read_self_mem(map.start, hdr, sizeof(hdr))) {
		RLOGE("cannot read the ELF header");
		return false;
	}
	const char *eh = hdr;
	if (eh[0] != 0x7F || strncmp(eh + 1, "ELF", 3) != 0) {
		RLOGE("not an ELF image at the libart base");
		return false;
	}
	const bool big = (eh[5] == 2);
	(void)big; // aarch64 Android images are little-endian
	const uint16_t ei_class = eh[4];
	if (ei_class != 2) { // ELFCLASS64
		RLOGE("unexpected ELF class");
		return false;
	}
	// Program header table.
	struct Elf64Phdr {
		uint32_t p_type;
		uint32_t p_flags;
		uint64_t p_offset;
		uint64_t p_vaddr;
		uint64_t p_paddr;
		uint64_t p_filesz;
		uint64_t p_memsz;
		uint64_t p_align;
	};
	struct Elf64Dyn {
		uint64_t d_tag;
		uint64_t d_val;
	};
	const uintptr_t e_phoff = *reinterpret_cast<const uint64_t *>(eh + 0x20);
	const int e_phentsize = *reinterpret_cast<const uint16_t *>(eh + 0x36);
	const int e_phnum = *reinterpret_cast<const uint16_t *>(eh + 0x38);
	uintptr_t symtab = 0;
	uintptr_t strtab = 0;
	size_t strsz = 0;
	{
		Elf64Phdr ph;
		for (int i = 0; i < e_phnum; ++i) {
			if (!read_self_mem(map.start + e_phoff + (uintptr_t)i * (uintptr_t)e_phentsize,
						&ph, sizeof(ph))) {
				RLOGE("cannot read program headers");
				return false;
			}
			if (ph.p_type == 2 /* PT_DYNAMIC */) {
				// Read all PT_DYNAMIC entries.
				const int n = (int)(ph.p_filesz / sizeof(Elf64Dyn));
				std::vector<Elf64Dyn> dyn;
				dyn.resize(n);
				if (!read_self_mem(map.start + ph.p_offset, dyn.data(),
							ph.p_filesz)) {
					RLOGE("cannot read PT_DYNAMIC");
					return false;
				}
				for (const Elf64Dyn &d : dyn) {
					if (d.d_tag == 6) { // DT_SYMTAB
						symtab = d.d_val;
					} else if (d.d_tag == 5) { // DT_STRTAB
						strtab = d.d_val;
					} else if (d.d_tag == 10) { // DT_STRSZ
						strsz = d.d_val;
					} else if (d.d_tag == 0) { // DT_END
						break;
					}
				}
			}
		}
	}
	if (symtab == 0 || strtab == 0) {
		RLOGE("no dynamic symbol table found (phoff=%lx phnum=%d phentsize=%d)",
				e_phoff, e_phnum, e_phentsize);
		return false;
	}
	RLOGE("dyn: symtab=%lx strtab=%lx strsz=%zu phoff=%lx phnum=%d",
			symtab, strtab, strsz, e_phoff, e_phnum);
	// The dynamic vaddrs are relative to the load base.
	const size_t sym64 = 24; // sizeof(Elf64_Sym)
	// Bound: DT_SYMTAB gives the address; symbol count comes from
	// DT_HASH/DT_GNU_HASH — instead, scan until a NUL name AND
	// value past strsz; cap at a generous count.
	const size_t kMaxSyms = 4096;
	std::vector<char> syms(kMaxSyms * sym64);
	std::vector<char> strs(strsz);
	if (!read_self_mem(map.start + symtab, syms.data(), syms.size())) {
		RLOGE("cannot read symtab");
		return false;
	}
	if (!read_self_mem(map.start + strtab, strs.data(), strsz)) {
		RLOGE("cannot read strtab");
		return false;
	}
	constexpr char kSymName[] = "JNI_GetCreatedJavaVMs";
	uintptr_t fn_addr = 0;
	for (size_t i = 0; i < kMaxSyms; ++i) {
		struct Elf64Sym {
			uint32_t st_name;
			uint8_t st_info;
			uint8_t st_other;
			uint16_t st_shndx;
			uint64_t st_value;
			uint64_t st_size;
		}; // sizeof == 24, exactly Elf64_Sym
		static_assert(sizeof(Elf64Sym) == 24, "Elf64_Sym is 24 bytes");
		Elf64Sym sym;
		std::memcpy(&sym, syms.data() + i * sym64, sym64);
		if (sym.st_name == 0 || sym.st_name >= strsz) {
			if (i > 64) {
				break; // end of the table
			}
			continue;
		}
		if (std::strncmp(strs.data() + sym.st_name, kSymName,
					sizeof(kSymName)) == 0) {
			fn_addr = map.start + sym.st_value;
			break;
		}
	}
	if (fn_addr == 0) {
		RLOGE("JNI_GetCreatedJavaVMs not in libart's dynamic symbols");
		return false;
	}
	RLOGE("found JNI_GetCreatedJavaVMs at %lx (base=%lx)",
			fn_addr, map.start);
	if (!address_in_exec_mapping(fn_addr, "libart.so")) {
		RLOGE("refusing to call %lx: not in an executable libart mapping",
				fn_addr);
		return false;
	}
	auto fn = reinterpret_cast<JniGetCreatedVMsFn>(fn_addr);
	JavaVM *vms[1] = { nullptr };
	jsize found = 0;
	if (fn(vms, 1, &found) != JNI_OK || found != 1) {
		RLOGE("JNI_GetCreatedJavaVMs call failed");
		return false;
	}
	g_jvm = vms[0];
	RLOGE("jvm resolved via /proc/self/mem ELF scan (libart base=%lx)",
			map.start);
	return true;
}

// A JNIEnv for the current (router I/O) thread; attaches a daemon
// thread if needed. Retries briefly: the libart mapping is present
// from process start, but the router I/O thread may race the very
// first /proc read during early engine init.
bool ensure_env(JNIEnv **r_env) {
	for (int attempt = 0; attempt < 10 && !ensure_jvm(); ++attempt) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	if (!ensure_jvm()) {
		return false;
	}
	JNIEnv *env = nullptr;
	int rc = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
	if (rc == JNI_EDETACHED) {
		rc = g_jvm->AttachCurrentThreadAsDaemon(&env, nullptr);
	}
	if (rc != JNI_OK || env == nullptr) {
		RLOGE("unable to attach to the JVM");
		return false;
	}
	*r_env = env;
	return true;
}

// Host JVM hook for the vendored RtMidi. RtMidi's own
// androidGetThreadEnv() resolves JNI_GetCreatedJavaVMs with
// dlsym(RTLD_DEFAULT) — which fails in an Android app's linker
// namespace (verified on-device: libart is namespace-private, and
// the VM is not in the global scope either). RtMidi (this same
// .so) therefore looks up this extern "C" symbol first and takes
// the JavaVM* from our /proc/self/mem ELF scan — one source of
// truth, no duplicated memory parsing. The lookup goes through
// dlsym(RTLD_DEFAULT) against OUR OWN symbol, which is in the
// default namespace because the engine dlopened the whole .so.
extern "C" JavaVM *gdpd_rtmidi_host_java_vm() {
	ensure_jvm();
	return g_jvm;
}

// Build.VERSION.SDK_INT (the AMidi C API requires API 29).
int jni_sdk_int(JNIEnv *p_env) {
	const jclass cls = p_env->FindClass("android/os/Build$VERSION");
	if (cls == nullptr) {
		return 0;
	}
	const jfieldID fid = p_env->GetStaticFieldID(cls, "SDK_INT", "I");
	if (fid == nullptr) {
		p_env->DeleteLocalRef(cls);
		return 0;
	}
	const int version = p_env->GetStaticIntField(cls, fid);
	p_env->DeleteLocalRef(cls);
	return version;
}

// ---------------------------------------------------------------------------
// Device enumeration: the UNFILTERED MidiManager.getDevices() list
// (raw order; name + per-side port counts). This is the backend's
// unified device index space. RtMidi's own openPort wants the
// SIDE-FILTERED position inside its shared androidMidiDevices list —
// rtmidi_seam::filtered_position() translates (both lists preserve
// the raw MidiManager order).
// ---------------------------------------------------------------------------

struct RawDevice {
	std::string name;
	bool is_input = false;
	bool is_output = false;
};

bool jni_enumerate_devices(JNIEnv *p_env, std::vector<RawDevice> *r_out) {
	// Every JNI call below can throw (hidden-API NoSuchMethodError on
	// OEM ROMs, a missing midi service, ...). A pending exception MUST
	// be cleared before returning or the NEXT JNI call on this thread
	// aborts the whole process — so each failure path funnels through
	// the cleanup lambda. Enumeration failure degrades to "no system
	// devices"; the in-process loopback stays available.
	std::vector<jobject> locals; // cleaned at scope exit
	auto cleanup = [&]() {
		if (p_env->ExceptionCheck()) {
			p_env->ExceptionClear();
		}
		for (auto ref : locals) {
			p_env->DeleteLocalRef(ref);
		}
		locals.clear();
	};
	const jclass at_class = p_env->FindClass("android/app/ActivityThread");
	if (at_class == nullptr) {
		cleanup();
		return false;
	}
	locals.push_back(at_class);
	const jmethodID cur_at = p_env->GetStaticMethodID(at_class,
			"currentActivityThread", "()Landroid/app/ActivityThread;");
	auto at = p_env->CallStaticObjectMethod(at_class, cur_at);
	if (at == nullptr) {
		cleanup();
		return false;
	}
	locals.push_back(at);
	// On API 29+ ActivityThread.getApplication() returns Application
	// (a Context). Older ROMs expose it with the Context return type;
	// try both and take whichever resolves.
	jmethodID get_app = p_env->GetMethodID(at_class, "getApplication",
			"()Landroid/app/Application;");
	if (get_app == nullptr && p_env->ExceptionCheck()) {
		p_env->ExceptionClear();
		get_app = p_env->GetMethodID(at_class, "getApplication",
				"()Landroid/content/Context;");
	}
	if (get_app == nullptr) {
		RLOGE("ActivityThread.getApplication not resolvable (hidden API "
				"blocked?)");
		cleanup();
		return false;
	}
	auto context = p_env->CallObjectMethod(at, get_app);
	if (context == nullptr) {
		cleanup();
		return false;
	}
	locals.push_back(context);
	const jclass context_class = p_env->FindClass("android/content/Context");
	if (context_class == nullptr) {
		cleanup();
		return false;
	}
	locals.push_back(context_class);
	const jmethodID get_service = p_env->GetMethodID(context_class,
			"getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
	auto midi_mgr = p_env->CallObjectMethod(context, get_service,
			p_env->NewStringUTF("midi"));
	if (midi_mgr == nullptr) {
		RLOGE("MIDI system service unavailable (feature missing?)");
		cleanup();
		return false;
	}
	locals.push_back(midi_mgr);
	const jclass mgr_class = p_env->FindClass("android/media/midi/MidiManager");
	const jmethodID get_devices = p_env->GetMethodID(mgr_class, "getDevices",
			"()[Landroid/media/midi/MidiDeviceInfo;");
	auto j_devices = (jobjectArray) p_env->CallObjectMethod(midi_mgr, get_devices);
	if (j_devices == nullptr) {
		cleanup();
		return false;
	}
	locals.push_back(j_devices);
	const jclass info_class = p_env->FindClass("android/media/midi/MidiDeviceInfo");
	const jmethodID in_count = p_env->GetMethodID(info_class, "getInputPortCount", "()I");
	const jmethodID out_count = p_env->GetMethodID(info_class, "getOutputPortCount", "()I");
	const jmethodID get_props = p_env->GetMethodID(info_class, "getProperties", "()Landroid/os/Bundle;");
	const jclass bundle_class = p_env->FindClass("android/os/Bundle");
	const jmethodID get_string = p_env->GetMethodID(bundle_class, "getString",
			"(Ljava/lang/String;)Ljava/lang/String;");

	r_out->clear();
	const jsize len = p_env->GetArrayLength(j_devices);
	for (jsize i = 0; i < len; ++i) {
		auto dev = p_env->GetObjectArrayElement(j_devices, i);
		RawDevice d;
		d.is_input = p_env->CallIntMethod(dev, in_count) > 0;
		d.is_output = p_env->CallIntMethod(dev, out_count) > 0;
		auto props = p_env->CallObjectMethod(dev, get_props);
		if (props != nullptr) {
			auto j_name = (jstring) p_env->CallObjectMethod(props, get_string,
						p_env->NewStringUTF("name"));
			if (j_name == nullptr) {
				j_name = (jstring) p_env->CallObjectMethod(props, get_string,
							p_env->NewStringUTF("product"));
			}
			if (j_name != nullptr) {
				const char *chars = p_env->GetStringUTFChars(j_name, nullptr);
				if (chars != nullptr) {
					d.name = chars;
				}
				p_env->ReleaseStringUTFChars(j_name, chars);
				p_env->DeleteLocalRef(j_name);
			}
			p_env->DeleteLocalRef(props);
		}
		if (d.is_input || d.is_output) {
			r_out->push_back(std::move(d));
		}
		p_env->DeleteLocalRef(dev);
	}
	cleanup();
	return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------


RtMidiAndroidBackend::~RtMidiAndroidBackend() {
	// The router shuts down before destroying the backend; this is a
	// belt-and-braces close for anything left open.
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &kv : in_ports_) {
		if (kv.second != nullptr) {
			try {
				kv.second->rt->closePort();
			} catch (const RtMidiError &) {
				// best effort at teardown
			}
		}
	}
	for (auto &kv : out_ports_) {
		if (kv.second != nullptr) {
			try {
				kv.second->rt->closePort();
			} catch (const RtMidiError &) {
				// best effort at teardown
			}
		}
	}
}

bool RtMidiAndroidBackend::available() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return initialized_;
}

MidiError RtMidiAndroidBackend::initialize() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (initialized_) {
		return MidiError::OK; // defensive: the router initializes once
	}
	// sdk_guard: the NDK AMidi C API exists from API 29. Fail clean
	// (backend unavailable) instead of a crash on a device below the
	// floor (the gradle preset pins minSdk 29; this covers lowered
	// minSdk / side-loaded builds).
	JNIEnv *env = nullptr;
	if (!ensure_env(&env)) {
		last_error_ = "RtMidi: no JavaVM available";
		return MidiError::Unavailable;
	}
	const int sdk = jni_sdk_int(env);
	if (sdk < 29) {
		last_error_ = "RtMidi: the Android MIDI (AMidi) API requires "
				"API 29 or newer (this device reports API " +
				std::to_string(sdk) + ")";
		return MidiError::Unavailable;
	}
	initialized_ = true;
	return MidiError::OK;
}

std::vector<MidiBackendPort> RtMidiAndroidBackend::list_ports() const {
	std::vector<MidiBackendPort> out;
	std::lock_guard<std::mutex> lock(mutex_);
	if (!initialized_) {
		return out;
	}
	JNIEnv *env = nullptr;
	if (!ensure_env(&env)) {
		return out; // no enumeration without a JVM
	}
	std::vector<RawDevice> raw;
	if (jni_enumerate_devices(env, &raw)) {
		devices_.clear();
		devices_.reserve(raw.size());
		for (const RawDevice &r : raw) {
			Device d;
			d.name = r.name;
			d.is_input = r.is_input;
			d.is_output = r.is_output;
			devices_.push_back(std::move(d));
		}
		for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
			MidiBackendPort port;
			port.index = i;
			port.name = devices_[i].name;
			port.is_input = devices_[i].is_input;
			port.is_output = devices_[i].is_output;
			out.push_back(port);
		}
	}
	if (loopback_active_) {
		MidiBackendPort in;
		in.index = kLoopbackInputIndex;
		in.name = loopback_name_ + " in";
		in.is_input = true;
		out.push_back(in);
		MidiBackendPort outport;
		outport.index = kLoopbackOutputIndex;
		outport.name = loopback_name_ + " out";
		outport.is_output = true;
		out.push_back(outport);
	}
	return out;
}

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------

void RtMidiAndroidBackend::input_callback(double p_time,
		std::vector<unsigned char> *p_bytes, void *p_userdata) {
	// RtMidi's pollMidi pthread. With the vendored sysex fix the
	// bytes are a COMPLETE message (short: status + data; sysex: the
	// full F0..F7). Chopped into <= 4-byte words for the router's
	// read stage (same layout PortMidi delivers via PmEvent words).
	(void)p_time;
	auto *ring = static_cast<rtmidi_seam::WordRing *>(p_userdata);
	if (ring == nullptr || p_bytes == nullptr) {
		return;
	}
	rtmidi_seam::chop_to_words(p_bytes->data(), static_cast<int>(p_bytes->size()),
			[p_ring = ring](uint32_t w) { p_ring->push(w); });
}

MidiError RtMidiAndroidBackend::open_impl(int p_index, int p_buffer_events,
		bool p_is_input, PortHandle &r_handle) {
	r_handle = NO_HANDLE;
	std::lock_guard<std::mutex> lock(mutex_);
	if (!initialized_) {
		last_error_ = "RtMidi: backend not initialized";
		return MidiError::Failed;
	}
	// In-process loopback pair (synchronous; no device open needed).
	if (p_index == kLoopbackInputIndex) {
		if (!loopback_active_ || !p_is_input) {
			last_error_ = "loopback input not available";
			return MidiError::InvalidParameter;
		}
		if (loopback_in_open_) {
			last_error_ = "loopback input already open";
			return MidiError::Failed;
		}
		loopback_in_handle_ = next_handle_++;
		loopback_in_open_ = true;
		r_handle = loopback_in_handle_;
		return MidiError::OK;
	}
	if (p_index == kLoopbackOutputIndex) {
		if (!loopback_active_ || p_is_input) {
			last_error_ = "loopback output not available";
			return MidiError::InvalidParameter;
		}
		if (loopback_out_open_) {
			last_error_ = "loopback output already open";
			return MidiError::Failed;
		}
		loopback_out_handle_ = next_handle_++;
		loopback_out_open_ = true;
		r_handle = loopback_out_handle_;
		return MidiError::OK;
	}
	// Real device.
	if (p_index < 0 || p_index >= static_cast<int>(devices_.size())) {
		last_error_ = "unknown MIDI device index " + std::to_string(p_index) +
				" (re-list the ports; the device list may have changed)";
		return MidiError::InvalidParameter;
	}
	if (p_is_input ? !devices_[p_index].is_input : !devices_[p_index].is_output) {
		last_error_ = "MIDI device " + std::to_string(p_index) +
				" (" + devices_[p_index].name + ") has no " +
				(p_is_input ? "input" : "output") + " port";
		return MidiError::Failed;
	}
	const int pos = side_position(devices_, p_index, p_is_input);
	if (pos < 0) {
		last_error_ = "internal: filtered position for device " +
				std::to_string(p_index);
		return MidiError::Failed;
	}
	// One RtMidi object per open port (the RtMidi Android backend
	// supports exactly one open port per object). The constructor
	// refreshes RtMidi's side-filtered device list, so `pos` (same
	// raw order) is valid for the openPort call below.
	std::unique_ptr<RtMidiIn> rt_in;
	std::unique_ptr<RtMidiOut> rt_out;
	try {
		if (p_is_input) {
			rt_in = std::make_unique<RtMidiIn>(RtMidi::ANDROID_AMIDI,
					"godot-libpd", static_cast<unsigned int>(p_buffer_events));
		} else {
			rt_out = std::make_unique<RtMidiOut>(RtMidi::ANDROID_AMIDI,
					"godot-libpd");
		}
	} catch (const RtMidiError &e) {
		last_error_ = std::string("RtMidi: ") + e.what();
		return MidiError::Failed;
	}

	PortHandle handle = next_handle_++;
	if (p_is_input) {
		// Receive EVERYTHING (the default ignoreTypes(7) would drop
		// sysex, realtime and sensing — the read stage needs all of
		// them, e.g. F4..F7 single-byte system commons).
		rt_in->ignoreTypes(false, false, false);
		// WordRing (mutex) is non-copyable: allocate the InPort. The
		// map stores a unique_ptr, so the ring pointer given to the
		// callback is stable for the port's whole life.
		auto in = std::unique_ptr<InPort>(new InPort{std::move(rt_in), p_index,
				devices_[p_index].name, rtmidi_seam::WordRing(kInputRingCapacity)});
		in->rt->setCallback(&RtMidiAndroidBackend::input_callback,
				&in->ring);
		in_ports_.emplace(handle, std::move(in));
		r_handle = handle;
	} else {
		auto outp = std::unique_ptr<OutPort>(new OutPort{std::move(rt_out), p_index,
				devices_[p_index].name});
		out_ports_.emplace(handle, std::move(outp));
		r_handle = handle;
	}
	// Fire the (asynchronous) open: RtMidi schedules
	// MidiManager.openDevice on the main looper; the native listener
	// then opens the AMidi port (input: also starts the pollMidi
	// pthread). There is no failure callback, so settle-wait on this
	// thread (NOT the main looper — the open completes while we wait).
	// openPort only throws for a stale index (device list changed
	// under us) — report that as a normal open failure.
	try {
		if (p_is_input) {
			in_ports_[handle]->rt->openPort(static_cast<unsigned int>(pos),
					devices_[p_index].name);
		} else {
			out_ports_[handle]->rt->openPort(static_cast<unsigned int>(pos),
					devices_[p_index].name);
		}
	} catch (const RtMidiError &e) {
		last_error_ = std::string("RtMidi: ") + e.what();
		if (p_is_input) {
			in_ports_.erase(handle);
		} else {
			out_ports_.erase(handle);
		}
		r_handle = NO_HANDLE;
		return MidiError::Failed;
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(kOpenSettleMs));
	return MidiError::OK;
}

int RtMidiAndroidBackend::side_position(const std::vector<Device> &p_devices,
		int p_index, bool p_is_input) {
	std::vector<rtmidi_seam::DeviceSide> sides;
	sides.reserve(p_devices.size());
	for (const Device &d : p_devices) {
		rtmidi_seam::DeviceSide s;
		s.input = d.is_input;
		s.output = d.is_output;
		sides.push_back(s);
	}
	return rtmidi_seam::filtered_position(sides, p_index, p_is_input);
}

MidiError RtMidiAndroidBackend::open_input(int p_index, int p_buffer_events,
		PortHandle &r_handle) {
	return open_impl(p_index, p_buffer_events, true, r_handle);
}

MidiError RtMidiAndroidBackend::open_output(int p_index, int p_buffer_events,
		PortHandle &r_handle) {
	return open_impl(p_index, p_buffer_events, false, r_handle);
}

// ---------------------------------------------------------------------------
// Input polling / stream health
// ---------------------------------------------------------------------------

MidiBackend::PollResult RtMidiAndroidBackend::poll_input(PortHandle p_handle,
		const WordPush &p_push) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (p_handle == loopback_in_handle_ && loopback_in_open_) {
		return loopback_ring_.drain(p_push) ? PollResult::BUFFER_OVERFLOW
				: PollResult::OK;
	}
	auto it = in_ports_.find(p_handle);
	if (it == in_ports_.end()) {
		return PollResult::OK; // defensive: the router stops polling on close
	}
	// No PollResult::FATAL on this backend: RtMidi Android has no
	// device-removal signal (polling a dead port just stops
	// delivering words — see header note).
	return it->second->ring.drain(p_push) ? PollResult::BUFFER_OVERFLOW
			: PollResult::OK;
}

bool RtMidiAndroidBackend::has_host_error(PortHandle p_handle,
		std::string &r_text) {
	// RtMidi has no queryable per-stream error state: non-warning
	// errors surface as RtMidiError exceptions AT CALL TIME (openPort
	// / sendMessage), which open_impl()/write_locked() already
	// convert to Failed returns the router handles. Nothing to poll.
	(void)p_handle;
	(void)r_text;
	return false;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

MidiError RtMidiAndroidBackend::write(PortHandle p_handle, const uint8_t *p_bytes,
		int p_len) {
	std::lock_guard<std::mutex> lock(mutex_);
	return write_locked(p_handle, p_bytes, p_len);
}

MidiError RtMidiAndroidBackend::write_locked(PortHandle p_handle,
		const uint8_t *p_bytes, int p_len) {
	if (p_handle == loopback_out_handle_) {
		if (!loopback_out_open_) {
			last_error_ = "no open port";
			return MidiError::Failed;
		}
		// In-process loopback: the wire bytes are exactly the router's
		// full-form framing, chopped to words into the loopback
		// input's ring (identical layout to a real device delivery).
		if (p_len <= 0) {
			return MidiError::OK;
		}
		rtmidi_seam::chop_to_words(p_bytes, p_len,
				[&](uint32_t w) { loopback_ring_.push(w); });
		return MidiError::OK;
	}
	auto it = out_ports_.find(p_handle);
	if (it == out_ports_.end()) {
		last_error_ = "no open port";
		return MidiError::Failed;
	}
	// The open settle-wait in open_impl already elapsed before the
	// router could send (same I/O thread); the AMidi input port is
	// open in the foreground case. A throw here (device removed /
	// port died) is the fatal path: the router's write-failure
	// handling closes the port and notifies (spec §7).
	try {
		it->second->rt->sendMessage(p_bytes, static_cast<size_t>(p_len));
	} catch (const RtMidiError &e) {
		last_error_ = std::string("RtMidi: ") + e.what();
		return MidiError::Failed;
	}
	return MidiError::OK;
}

MidiError RtMidiAndroidBackend::write_sysex(PortHandle p_handle,
		const uint8_t *p_bytes, int p_len) {
	// Guard the payload shape first (mirrors the PortMidi backend);
	// then the same path as short messages (AMidi accepts the full
	// F0..F7 stream).
	if (p_len < 2 || p_bytes[0] != 0xF0 || p_bytes[p_len - 1] != 0xF7) {
		std::lock_guard<std::mutex> lock(mutex_);
		last_error_ = "malformed sysex (must start with 0xF0 and end with 0xF7)";
		return MidiError::InvalidParameter;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	return write_locked(p_handle, p_bytes, p_len);
}

// ---------------------------------------------------------------------------
// Close / virtual devices / shutdown
// ---------------------------------------------------------------------------

MidiError RtMidiAndroidBackend::close(PortHandle p_handle) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (p_handle == loopback_in_handle_) {
		if (loopback_in_open_) {
			loopback_in_open_ = false;
			loopback_ring_.reset();
		}
		return MidiError::OK; // best effort (nothing to clean)
	}
	if (p_handle == loopback_out_handle_) {
		loopback_out_open_ = false;
		return MidiError::OK;
	}
	auto in = in_ports_.find(p_handle);
	if (in != in_ports_.end()) {
		// closePort joins the pollMidi thread (which pushes into this
		// InPort's ring — alive until the erase below).
		if (in->second != nullptr) {
			try {
				in->second->rt->closePort();
			} catch (const RtMidiError &) {
				// best effort (spec: close never fails loudly)
			}
		}
		in_ports_.erase(in);
		return MidiError::OK;
	}
	auto out = out_ports_.find(p_handle);
	if (out != out_ports_.end()) {
		if (out->second != nullptr) {
			try {
				out->second->rt->closePort();
			} catch (const RtMidiError &) {
				// best effort
			}
		}
		out_ports_.erase(out);
		return MidiError::OK;
	}
	return MidiError::OK; // best effort: unknown handle is a no-op
}

MidiError RtMidiAndroidBackend::create_virtual_input(const std::string &p_name,
		int &r_index) {
	std::lock_guard<std::mutex> lock(mutex_);
	(void)p_name;
	(void)r_index;
	last_error_ = "no virtual MIDI device API on Android (use the "
			"in-process loopback: create_virtual_loopback)";
	return MidiError::Unavailable;
}

MidiError RtMidiAndroidBackend::create_virtual_output(const std::string &p_name,
		int &r_index) {
	std::lock_guard<std::mutex> lock(mutex_);
	(void)p_name;
	(void)r_index;
	last_error_ = "no virtual MIDI device API on Android (use the "
			"in-process loopback: create_virtual_loopback)";
	return MidiError::Unavailable;
}

MidiError RtMidiAndroidBackend::create_virtual_loopback(
		const std::string &p_name) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!initialized_) {
		last_error_ = "RtMidi: backend not initialized";
		return MidiError::Failed;
	}
	if (loopback_active_) {
		last_error_ = "an in-process loopback is already active";
		return MidiError::Failed;
	}
	loopback_active_ = true;
	loopback_name_ = p_name;
	loopback_ring_.reset();
	return MidiError::OK;
}

void RtMidiAndroidBackend::shutdown() {
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &kv : in_ports_) {
		if (kv.second != nullptr) {
			try {
				kv.second->rt->closePort();
			} catch (const RtMidiError &) {
				// best effort at shutdown
			}
		}
	}
	in_ports_.clear();
	for (auto &kv : out_ports_) {
		if (kv.second != nullptr) {
			try {
				kv.second->rt->closePort();
			} catch (const RtMidiError &) {
				// best effort at shutdown
			}
		}
	}
	out_ports_.clear();
	loopback_active_ = false;
	loopback_in_open_ = false;
	loopback_out_open_ = false;
	loopback_ring_.reset();
	initialized_ = false;
}

const std::string &RtMidiAndroidBackend::last_error() const {
	return last_error_;
}

} // namespace godot_libpd

#endif // __ANDROID__

