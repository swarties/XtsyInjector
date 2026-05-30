// InputHook.cpp — Keyboard listener to toggle the client menu overlay.

#include "InputHook.h"
#include "../gui/Menu.h"

#include <cstdio>
#include <string>
#include <thread>
#include <chrono>
#include <dlfcn.h>
#include <jni.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>

#ifdef HAS_LIBINPUT
#include <libinput.h>
#include <libudev.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <linux/input-event-codes.h>
#endif

// Static member definitions.
std::atomic<bool> InputHook::inputAvailable{false};

// Append a message to the client log file.
static void log(const std::string& msg) {
    FILE* f = fopen("/tmp/mc_client.log", "a");
    if (f) { fprintf(f, "[InputHook] %s\n", msg.c_str()); fclose(f); }
}

// Toggles the menu and logs the new visibility state.
static void toggleMenu(const char* source) {
    bool newState = !Menu::menuVisible.load();
    Menu::menuVisible.store(newState);
    log(std::string("Menu toggled (") + source + "): " + (newState ? "VISIBLE" : "HIDDEN"));
}

// LWJGL keyboard listener: polls Arrow Up directly inside the injected JVM process.
static bool lwjglKeyListener() {
    using GetCreatedJavaVMsFn = jint (*)(JavaVM**, jsize, jsize*);
    auto getCreatedJavaVMs = reinterpret_cast<GetCreatedJavaVMsFn>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
    if (!getCreatedJavaVMs) {
        log("LWJGL input: JNI_GetCreatedJavaVMs symbol not found");
        return false;
    }

    JavaVM* jvm = nullptr;
    jsize count = 0;
    if (getCreatedJavaVMs(&jvm, 1, &count) != JNI_OK || !jvm || count == 0) {
        log("LWJGL input: no JVM found");
        return false;
    }

    JNIEnv* env = nullptr;
    if (jvm->AttachCurrentThread((void**)&env, nullptr) != JNI_OK || !env) {
        log("LWJGL input: AttachCurrentThread failed");
        return false;
    }

    jclass keyboardClass = env->FindClass("org/lwjgl/input/Keyboard");
    if (!keyboardClass || env->ExceptionCheck()) {
        env->ExceptionClear();
        log("LWJGL input: Keyboard class not found");
        return false;
    }
    jmethodID isKeyDown = env->GetStaticMethodID(keyboardClass, "isKeyDown", "(I)Z");
    if (!isKeyDown || env->ExceptionCheck()) {
        env->ExceptionClear();
        log("LWJGL input: Keyboard.isKeyDown not found");
        return false;
    }

    constexpr jint LWJGL_KEY_UP = 200;
    bool wasDown = false;
    InputHook::inputAvailable.store(true);
    log("LWJGL input: listening on KEY_UP (200)");

    while (true) {
        jboolean down = env->CallStaticBooleanMethod(keyboardClass, isKeyDown, LWJGL_KEY_UP);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            continue;
        }

        bool nowDown = down == JNI_TRUE;
        if (nowDown && !wasDown) {
            toggleMenu("LWJGL KEY_UP");
        }
        wasDown = nowDown;
        std::this_thread::sleep_for(std::chrono::milliseconds(12));
    }
}

// X11 keyboard listener: polls the keyboard map for the physical quote key with shift held.
static void x11KeyListener() {
    log("X11 key listener thread started (fallback)");

    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        log("XOpenDisplay failed — X11 path unavailable");
        return;
    }

    Window root = DefaultRootWindow(display);
    KeyCode quoteKey = XKeysymToKeycode(display, XK_apostrophe);
    KeyCode quotedblKey = XKeysymToKeycode(display, XStringToKeysym("quotedbl"));
    KeyCode twoKey = XKeysymToKeycode(display, XK_2);
    KeyCode leftShift = XKeysymToKeycode(display, XK_Shift_L);
    KeyCode rightShift = XKeysymToKeycode(display, XK_Shift_R);
    if (quoteKey == 0 && quotedblKey == 0 && twoKey == 0) {
        log("ERROR: could not resolve quote-related X11 keycodes");
        XCloseDisplay(display);
        return;
    }

    InputHook::inputAvailable.store(true);
    log("X11 polling keycodes: apostrophe=" + std::to_string(quoteKey) +
        " quotedbl=" + std::to_string(quotedblKey) + " key2=" + std::to_string(twoKey));

    auto isDown = [](const char keys[32], KeyCode key) {
        return key != 0 && (keys[key / 8] & (1 << (key % 8))) != 0;
    };

    bool wasPressed = false;
    while (true) {
        char keys[32] {};
        XQueryKeymap(display, keys);
        bool shiftHeld = isDown(keys, leftShift) || isDown(keys, rightShift);
        bool quotePressed = isDown(keys, quotedblKey) || (shiftHeld && isDown(keys, quoteKey)) ||
                            (shiftHeld && isDown(keys, twoKey));
        if (quotePressed && !wasPressed) {
            toggleMenu("X11");
        }
        wasPressed = quotePressed;
        std::this_thread::sleep_for(std::chrono::milliseconds(12));
    }

    // Unreachable, but clean up if the loop ever breaks.
    (void)root;
    XCloseDisplay(display);
}

#ifdef HAS_LIBINPUT
// libinput interface callbacks required by libinput_udev_create_context.
static int openRestricted(const char* path, int flags, void* /*userData*/) {
    int fd = open(path, flags);
    return fd < 0 ? -1 : fd;
}

// Close a device file descriptor opened by openRestricted.
static void closeRestricted(int fd, void* /*userData*/) {
    close(fd);
}

static const struct libinput_interface kLibinputInterface = {
    .open_restricted = openRestricted,
    .close_restricted = closeRestricted,
};

// Libinput keyboard listener: polls for KEY_APOSTROPHE with shift state.
static void libinputKeyListener() {
    log("libinput key listener thread started");

    struct udev* udevCtx = udev_new();
    if (!udevCtx) {
        log("ERROR: udev_new() failed");
        return;
    }

    struct libinput* li = libinput_udev_create_context(&kLibinputInterface, nullptr, udevCtx);
    if (!li) {
        log("ERROR: libinput_udev_create_context failed");
        udev_unref(udevCtx);
        return;
    }

    if (libinput_udev_assign_seat(li, "seat0") != 0) {
        log("ERROR: libinput_udev_assign_seat(seat0) failed");
        libinput_unref(li);
        udev_unref(udevCtx);
        return;
    }

    InputHook::inputAvailable.store(true);
    log("libinput initialized on seat0, polling for KEY_APOSTROPHE + Shift and KEY_2 + Shift");

    int fd = libinput_get_fd(li);
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    bool shiftHeld = false;

    while (true) {
        // Wait for input events indefinitely.
        if (poll(&pfd, 1, -1) <= 0) continue;

        libinput_dispatch(li);

        struct libinput_event* ev;
        while ((ev = libinput_get_event(li)) != nullptr) {
            enum libinput_event_type type = libinput_event_get_type(ev);

            if (type == LIBINPUT_EVENT_KEYBOARD_KEY) {
                struct libinput_event_keyboard* kev = libinput_event_get_keyboard_event(ev);
                uint32_t key = libinput_event_keyboard_get_key(kev);
                enum libinput_key_state state = libinput_event_keyboard_get_key_state(kev);

                // Track shift state for quotedbl detection (Shift + apostrophe).
                if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT) {
                    shiftHeld = (state == LIBINPUT_KEY_STATE_PRESSED);
                }

                // Toggle menu on Shift+Apostrophe for US layouts, or Shift+2 for common non-US layouts.
                bool quotedblPressed = (key == KEY_APOSTROPHE || key == KEY_2) &&
                                       state == LIBINPUT_KEY_STATE_PRESSED && shiftHeld;
                if (quotedblPressed) {
                    toggleMenu("libinput");
                }
            }

            libinput_event_destroy(ev);
        }
    }

    libinput_unref(li);
    udev_unref(udevCtx);
}
#endif // HAS_LIBINPUT

// Starts keyboard listeners, prioritizing in-process LWJGL key polling for Forge/LWJGL2.
void InputHook::initialize() {
    log("initialize() — attempting LWJGL KEY_UP path first");

    std::thread([]() {
        if (lwjglKeyListener()) return;
        log("LWJGL input unavailable — falling back to X11/libinput path");

        // Try X11 first: probe whether we can open a display.
        Display* probe = XOpenDisplay(nullptr);
        if (probe) {
            XCloseDisplay(probe);
            log("X11 display available — launching X11 key listener");
            std::thread(x11KeyListener).detach();
            return;
        }

        log("X11 display unavailable — trying libinput fallback");

#ifdef HAS_LIBINPUT
        log("HAS_LIBINPUT defined — launching libinput key listener");
        std::thread(libinputKeyListener).detach();
        return;
#endif

        log("InputHook: failed to initialize LWJGL, X11, and libinput. Toggle key will not work.");
        inputAvailable.store(false);
    }).detach();
}
