// Renderer.cpp — Inline-hooks glXSwapBuffers or eglSwapBuffers to inject ImGui rendering.

#include "Renderer.h"
#include "Menu.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <link.h>
#include <vector>

#include <GL/glx.h>
#include <X11/Xlib.h>

// Append a message to the client log file.
static void log(const std::string& msg) {
    FILE* f = fopen("/tmp/mc_client.log", "a");
    if (f) { fprintf(f, "[Renderer] %s\n", msg.c_str()); fclose(f); }
}

// Size of the absolute-jump trampoline we overwrite at the function start.
static constexpr size_t kPatchSize = 12;

// Saved original bytes from the hooked function's prologue.
static uint8_t savedBytesGLX[kPatchSize];
static uint8_t savedBytesEGL[kPatchSize];

// Executable trampoline that runs the original prologue then jumps back.
static void* trampolineGLX = nullptr;
static void* trampolineEGL = nullptr;

// Original function addresses (past the patched region).
static void* realGLXSwapBuffers = nullptr;
static void* realEGLSwapBuffers = nullptr;
static std::atomic<unsigned long long> g_glxCalls{0};
static std::atomic<unsigned long long> g_eglCalls{0};

// Typedef for the original glXSwapBuffers.
using glXSwapBuffers_t = void (*)(Display*, GLXDrawable);
// Typedef for the original eglSwapBuffers.
using eglSwapBuffers_t = int (*)(void*, void*);

// Align an address down to its page boundary.
static uintptr_t pageAlign(uintptr_t addr) {
    return addr & ~(static_cast<uintptr_t>(sysconf(_SC_PAGESIZE)) - 1);
}

// Build a 12-byte absolute jump: mov rax, <addr>; jmp rax.
static void writeAbsoluteJump(uint8_t* dest, void* target) {
    // 48 B8 <8-byte immediate>  =  mov rax, imm64
    dest[0] = 0x48;
    dest[1] = 0xB8;
    std::memcpy(dest + 2, &target, sizeof(void*));
    // FF E0  =  jmp rax
    dest[10] = 0xFF;
    dest[11] = 0xE0;
}

// Allocate an executable trampoline: saved original bytes + jump back to real+12.
static void* createTrampoline(const uint8_t* originalBytes, void* realFunc) {
    // mmap an executable page for our trampoline code.
    void* mem = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        log("ERROR: mmap for trampoline failed");
        return nullptr;
    }

    auto* code = static_cast<uint8_t*>(mem);

    // Copy the original function prologue bytes.
    std::memcpy(code, originalBytes, kPatchSize);

    // Write a jump back to real_func + kPatchSize (past our patch).
    void* returnAddr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(realFunc) + kPatchSize);
    writeAbsoluteJump(code + kPatchSize, returnAddr);

    log("Trampoline created at " + std::to_string(reinterpret_cast<uintptr_t>(mem)));
    return mem;
}

// Make the first kPatchSize bytes of a function writable, save originals, and write a jump.
static bool patchFunction(void* funcAddr, void* hookAddr, uint8_t* savedBytes, void** outTrampoline) {
    auto* target = static_cast<uint8_t*>(funcAddr);

    // Save original bytes before overwriting.
    std::memcpy(savedBytes, target, kPatchSize);

    // Make the target page writable.
    uintptr_t page = pageAlign(reinterpret_cast<uintptr_t>(target));
    long pageSize = sysconf(_SC_PAGESIZE);
    // Ensure we cover both pages if the patch straddles a page boundary.
    size_t len = static_cast<size_t>((reinterpret_cast<uintptr_t>(target) + kPatchSize) - page);
    if (len < static_cast<size_t>(pageSize)) len = static_cast<size_t>(pageSize);

    if (mprotect(reinterpret_cast<void*>(page), len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        log("ERROR: mprotect failed on target function");
        return false;
    }

    // Build the callable trampoline from saved bytes.
    *outTrampoline = createTrampoline(savedBytes, funcAddr);
    if (*outTrampoline == nullptr) {
        return false;
    }

    // Overwrite the function's start with a jump to our hook.
    writeAbsoluteJump(target, hookAddr);

    return true;
}

// Hooked glXSwapBuffers: render ImGui overlay then call original.
static void hooked_glXSwapBuffers(Display* dpy, GLXDrawable drawable) {
    unsigned long long call = ++g_glxCalls;
    if (call <= 5 || (call % 600) == 0) {
        log("hooked_glXSwapBuffers call #" + std::to_string(call));
    }
    Menu::render();
    // Call through the trampoline to execute the original function.
    reinterpret_cast<glXSwapBuffers_t>(trampolineGLX)(dpy, drawable);
}

// Hooked eglSwapBuffers: render ImGui overlay then call original.
static int hooked_eglSwapBuffers(void* display, void* surface) {
    unsigned long long call = ++g_eglCalls;
    if (call <= 5 || (call % 600) == 0) {
        log("hooked_eglSwapBuffers call #" + std::to_string(call));
    }
    Menu::render();
    // Call through the trampoline to execute the original function.
    return reinterpret_cast<eglSwapBuffers_t>(trampolineEGL)(display, surface);
}

// Resolve the target swap function and install an inline hook via trampoline.
bool Renderer::installHook(HookMode mode) {
    if (mode == HookMode::GLX) {
        log("Installing GLX hook on glXSwapBuffers");

        void* sym = dlsym(RTLD_DEFAULT, "glXSwapBuffers");
        if (!sym) {
            log("ERROR: dlsym failed for glXSwapBuffers — " + std::string(dlerror() ? dlerror() : "unknown"));
            return false;
        }
        realGLXSwapBuffers = sym;

        if (!patchFunction(sym, reinterpret_cast<void*>(hooked_glXSwapBuffers), savedBytesGLX, &trampolineGLX)) {
            log("ERROR: Failed to patch glXSwapBuffers");
            return false;
        }
        log("glXSwapBuffers hooked successfully");
        return true;

    } else if (mode == HookMode::EGL) {
        log("Installing EGL hook on eglSwapBuffers");

        void* sym = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
        if (!sym) {
            log("ERROR: dlsym failed for eglSwapBuffers — " + std::string(dlerror() ? dlerror() : "unknown"));
            return false;
        }
        realEGLSwapBuffers = sym;

        if (!patchFunction(sym, reinterpret_cast<void*>(hooked_eglSwapBuffers), savedBytesEGL, &trampolineEGL)) {
            log("ERROR: Failed to patch eglSwapBuffers");
            return false;
        }
        log("eglSwapBuffers hooked successfully");
        return true;
    } else if (mode == HookMode::LWJGL_SWAP) {
        log("LWJGL native swap hook is disabled (stability guard)");
        return false;
    }
    return false;
}
