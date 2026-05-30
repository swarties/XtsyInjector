// Menu.h — ImGui overlay menu interface for the injected client.

#pragma once

#include <jni.h>
#include <mutex>
#include <atomic>

class Menu {
public:
    // Set up rendering hooks and prepare ImGui (called once from dllmain).
    static void initialize(JNIEnv* env, JavaVM* jvm);

    // Draw the ImGui overlay frame (called every swap from Renderer hooks).
    static void render();

    // True when the menu overlay should be drawn; toggled by InputHook.
    static std::atomic<bool> menuVisible;

private:
    // True after the first ImGui context + backend has been created.
    static bool imguiInitialized;

    // Serializes ImGui init and render calls across threads.
    static std::mutex renderMutex;
};
