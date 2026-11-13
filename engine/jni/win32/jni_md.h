/* Windows definitions for jni.h, used when cross-compiling ygoe_jni.dll from Linux
 * (the JDK's jni.h is the same on every platform; only this file differs). */
#ifndef _JAVASOFT_JNI_MD_H_
#define _JAVASOFT_JNI_MD_H_

#define JNIEXPORT __declspec(dllexport)
#define JNIIMPORT __declspec(dllimport)
#define JNICALL __stdcall

typedef long jint;
typedef long long jlong;
typedef signed char jbyte;

#endif
