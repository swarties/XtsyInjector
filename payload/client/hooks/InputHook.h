// InputHook.h — Keyboard listener interface for toggling the client menu.

#pragma once

#include <atomic>

class InputHook {
public:
    // Launch the keyboard listener thread (LWJGL in-process path first).
    static void initialize();

    // True if at least one input backend was successfully started.
    static std::atomic<bool> inputAvailable;
};
