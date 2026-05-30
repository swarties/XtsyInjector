// Renderer.h — OpenGL swap-buffer hook interface for GLX and EGL.

#pragma once

// Selects which swap function to hook.
enum class HookMode {
    GLX,
    EGL,
    LWJGL_SWAP
};

class Renderer {
public:
    // Install an inline hook on the selected swap function.
    static bool installHook(HookMode mode);
};
