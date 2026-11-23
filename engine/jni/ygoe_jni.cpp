// JNI bridge for com.ygoengine.YgoNative (android/ygoe). Every call is a thin pass-through
// to ygoe::Engine; requests and results are JSON strings (docs/api.md).
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <string>

#include "api.h"
#include "live.h"

namespace {

std::string to_string(JNIEnv* env, jstring s) {
	if(!s)
		return {};
	const char* chars = env->GetStringUTFChars(s, nullptr);
	std::string out(chars ? chars : "");
	if(chars)
		env->ReleaseStringUTFChars(s, chars);
	return out;
}

// NewStringUTF expects "modified UTF-8"; card names can contain 4-byte characters,
// so build the string from UTF-16 instead.
jstring to_jstring(JNIEnv* env, const std::string& s) {
	std::u16string u;
	u.reserve(s.size());
	for(size_t i = 0; i < s.size();) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		uint32_t cp;
		size_t n;
		if(c < 0x80) {
			cp = c;
			n = 1;
		} else if((c >> 5) == 0x6 && i + 1 < s.size()) {
			cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
			n = 2;
		} else if((c >> 4) == 0xE && i + 2 < s.size()) {
			cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
			n = 3;
		} else if((c >> 3) == 0x1E && i + 3 < s.size()) {
			cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
			n = 4;
		} else {
			cp = 0xFFFD;
			n = 1;
		}
		i += n;
		if(cp >= 0x10000) {
			cp -= 0x10000;
			u.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
			u.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
		} else {
			u.push_back(static_cast<char16_t>(cp));
		}
	}
	return env->NewString(reinterpret_cast<const jchar*>(u.data()), static_cast<jsize>(u.size()));
}

ygoe::Engine* engine(jlong handle) { return reinterpret_cast<ygoe::Engine*>(handle); }
ygoe::LiveDuel* live(jlong handle) { return reinterpret_cast<ygoe::LiveDuel*>(handle); }
// The last error of a call, per thread. A fixed buffer, not a std::string: on Windows (MinGW)
// thread_locals with a destructor corrupted the heap when threads ended.
thread_local char last_error_text[1024] = "";

void set_last_error(const std::string& e) {
	size_t n = std::min(e.size(), sizeof(last_error_text) - 1);
	std::memcpy(last_error_text, e.data(), n);
	last_error_text[n] = 0;
}

// ---- crash notes ----
// An error the engine doesn't catch ends the whole app (std::terminate), before the app can write
// anything. With a crash file set, the engine first appends one line there: when, which engine
// call was running, and the error's message, so the app can explain the crash when it restarts.
std::string crash_file;
std::atomic<const char*> last_call{"none"};

[[noreturn]] void on_terminate() {
	std::string what = "no error message";
	if(auto e = std::current_exception()) {
		try {
			std::rethrow_exception(e);
		} catch(const std::exception& x) {
			what = x.what();
		} catch(...) {
			what = "an error that isn't a std::exception";
		}
	}
	if(!crash_file.empty()) {
		if(FILE* f = std::fopen(crash_file.c_str(), "ab")) {
			std::fprintf(f, "%lld\tengine call: %s\t%s\n", static_cast<long long>(std::time(nullptr)), last_call.load(), what.c_str());
			std::fclose(f);
		}
	}
	std::abort();
}

}  // namespace

extern "C" {

// Where to note an engine crash (see on_terminate); also installs the handler.
JNIEXPORT void JNICALL Java_com_ygoengine_YgoNative_crashFile(JNIEnv* env, jclass, jstring path) {
	crash_file = to_string(env, path);
	std::set_terminate(on_terminate);
}

JNIEXPORT jlong JNICALL Java_com_ygoengine_YgoNative_create(JNIEnv* env, jclass, jstring data_dir, jstring scripts_dir) {
	last_call.store("create");
	auto* e = new ygoe::Engine(to_string(env, data_dir), to_string(env, scripts_dir));
	return reinterpret_cast<jlong>(e);
}

#ifndef YGOE_GIT_SHA
#define YGOE_GIT_SHA "unknown"
#endif
#ifndef YGOE_API_COMPAT
#define YGOE_API_COMPAT 1
#endif
// "<git commit>\t<api compat>": which engine this library is.
JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_version(JNIEnv* env, jclass) {
	return to_jstring(env, std::string(YGOE_GIT_SHA) + "\t" + std::to_string(YGOE_API_COMPAT));
}

JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_error(JNIEnv* env, jclass, jlong h) {
	return to_jstring(env, engine(h)->ok() ? std::string() : engine(h)->error());
}

JNIEXPORT void JNICALL Java_com_ygoengine_YgoNative_destroy(JNIEnv*, jclass, jlong h) {
	last_call.store("destroy");
	delete engine(h);
}

JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_analyze(JNIEnv* env, jclass, jlong h, jstring request) {
	last_call.store("analyze");
	return to_jstring(env, engine(h)->analyze(to_string(env, request)));
}

JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_profile(JNIEnv* env, jclass, jlong h, jstring request) {
	last_call.store("profile");
	return to_jstring(env, engine(h)->profile(to_string(env, request)));
}

JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_side(JNIEnv* env, jclass, jlong h, jstring request) {
	last_call.store("side");
	return to_jstring(env, engine(h)->side(to_string(env, request)));
}

JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_reference(JNIEnv* env, jclass, jlong h, jstring request) {
	last_call.store("reference");
	return to_jstring(env, engine(h)->reference(to_string(env, request)));
}

JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_videoLines(JNIEnv* env, jclass, jlong h, jstring request) {
	last_call.store("videoLines");
	return to_jstring(env, engine(h)->video_lines(to_string(env, request)));
}

JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_replayPositions(JNIEnv* env, jclass, jlong h, jstring request) {
	last_call.store("replayPositions");
	return to_jstring(env, engine(h)->replay_positions(to_string(env, request)));
}

JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_followReplay(JNIEnv* env, jclass, jlong h, jstring request) {
	last_call.store("followReplay");
	return to_jstring(env, engine(h)->follow_replay(to_string(env, request)));
}

JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_comboTree(JNIEnv* env, jclass, jlong h, jstring request) {
	last_call.store("comboTree");
	return to_jstring(env, engine(h)->combo_tree(to_string(env, request)));
}

JNIEXPORT void JNICALL Java_com_ygoengine_YgoNative_cancel(JNIEnv*, jclass, jlong h) {
	engine(h)->cancel();
}

// "phase\tdone\ttotal"
JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_progress(JNIEnv* env, jclass, jlong h) {
	auto p = engine(h)->progress();
	return to_jstring(env, p.phase + "\t" + std::to_string(p.done) + "\t" + std::to_string(p.total));
}

// ---- live duels ----

JNIEXPORT jlong JNICALL Java_com_ygoengine_YgoNative_liveCreate(JNIEnv* env, jclass, jlong h, jstring request) {
	last_call.store("liveCreate");
	std::string error;
	auto* d = engine(h)->create_live(to_string(env, request), error);
	set_last_error(error);
	return reinterpret_cast<jlong>(d);
}

JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_lastError(JNIEnv* env, jclass) {
	return to_jstring(env, std::string(last_error_text));
}

JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_liveState(JNIEnv* env, jclass, jlong d) {
	last_call.store("liveState");
	return to_jstring(env, live(d)->state_json());
}

JNIEXPORT jboolean JNICALL Java_com_ygoengine_YgoNative_liveRespond(JNIEnv*, jclass, jlong d, jint index) {
	last_call.store("liveRespond");
	return index >= 0 && live(d)->respond(static_cast<size_t>(index)) ? JNI_TRUE : JNI_FALSE;
}

// level: 0 easy, 1 normal, 2 strong. Returns the chosen move's label ("" if none).
JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_liveBot(JNIEnv* env, jclass, jlong d, jint level) {
	last_call.store("liveBot");
	auto lv = level <= 0 ? ygoe::BotLevel::Easy : level == 1 ? ygoe::BotLevel::Normal : ygoe::BotLevel::Strong;
	return to_jstring(env, live(d)->bot_move(lv));
}

JNIEXPORT void JNICALL Java_com_ygoengine_YgoNative_liveCancel(JNIEnv*, jclass, jlong d) {
	live(d)->cancel();
}

JNIEXPORT void JNICALL Java_com_ygoengine_YgoNative_liveDestroy(JNIEnv*, jclass, jlong d) {
	last_call.store("liveDestroy");
	delete live(d);
}

}  // extern "C"
