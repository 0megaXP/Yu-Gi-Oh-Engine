// JNI bridge for com.ygoengine.YgoNative (android/ygoe). Every call is a thin pass-through
// to ygoe::Engine; requests and results are JSON strings (docs/api.md).
#include <jni.h>

#include <string>

#include "api.h"

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

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_com_ygoengine_YgoNative_create(JNIEnv* env, jclass, jstring data_dir, jstring scripts_dir) {
	auto* e = new ygoe::Engine(to_string(env, data_dir), to_string(env, scripts_dir));
	return reinterpret_cast<jlong>(e);
}

JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_error(JNIEnv* env, jclass, jlong h) {
	return to_jstring(env, engine(h)->ok() ? std::string() : engine(h)->error());
}

JNIEXPORT void JNICALL Java_com_ygoengine_YgoNative_destroy(JNIEnv*, jclass, jlong h) {
	delete engine(h);
}

JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_analyze(JNIEnv* env, jclass, jlong h, jstring request) {
	return to_jstring(env, engine(h)->analyze(to_string(env, request)));
}

JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_profile(JNIEnv* env, jclass, jlong h, jstring request) {
	return to_jstring(env, engine(h)->profile(to_string(env, request)));
}

JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_side(JNIEnv* env, jclass, jlong h, jstring request) {
	return to_jstring(env, engine(h)->side(to_string(env, request)));
}

JNIEXPORT void JNICALL Java_com_ygoengine_YgoNative_cancel(JNIEnv*, jclass, jlong h) {
	engine(h)->cancel();
}

// "phase\tdone\ttotal"
JNIEXPORT jstring JNICALL Java_com_ygoengine_YgoNative_progress(JNIEnv* env, jclass, jlong h) {
	auto p = engine(h)->progress();
	return to_jstring(env, p.phase + "\t" + std::to_string(p.done) + "\t" + std::to_string(p.total));
}

}  // extern "C"
