#include <jni.h>

// Compatibility JNI stubs for Lion profile bootstrap.
// These avoid UnsatisfiedLinkError when lion.client.Agent probes NativeBridge
// methods that are not implemented by the Xtsy native payload.
extern "C" {

JNIEXPORT jboolean JNICALL Java_lion_client_NativeBridge_isKeyDown(JNIEnv*, jclass, jint) {
    return JNI_FALSE;
}

JNIEXPORT void JNICALL Java_lion_client_NativeBridge_sendClick(JNIEnv*, jclass, jint) {}

JNIEXPORT jboolean JNICALL Java_lion_client_NativeBridge_isMinecraftFocused(JNIEnv*, jclass) {
    return JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_lion_client_NativeBridge_getFocusedWindowTitle(JNIEnv* env, jclass) {
    return env->NewStringUTF("");
}

JNIEXPORT jint JNICALL Java_lion_client_NativeBridge_getForegroundPid(JNIEnv*, jclass) {
    return 0;
}

JNIEXPORT jlong JNICALL Java_lion_client_NativeBridge_findMinecraftHwnd(JNIEnv*, jclass) {
    return 0;
}

JNIEXPORT jintArray JNICALL Java_lion_client_NativeBridge_getWindowRect(JNIEnv* env, jclass, jlong) {
    return env->NewIntArray(0);
}

JNIEXPORT void JNICALL Java_lion_client_NativeBridge_setClickThrough(JNIEnv*, jclass, jlong) {}

JNIEXPORT jlong JNICALL Java_lion_client_NativeBridge_findOwnWindowByTitle(JNIEnv*, jclass, jstring) {
    return 0;
}

JNIEXPORT jboolean JNICALL Java_lion_client_NativeBridge_attachAgent(JNIEnv*, jclass, jstring) {
    return JNI_FALSE;
}

}
