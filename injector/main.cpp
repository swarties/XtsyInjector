// main.cpp — Injector entry point: creates SDL2 window, initializes ImGui, runs the main loop.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <filesystem>
#include <algorithm>
#include <unistd.h>
#include <SDL.h>
#include <GL/gl.h>
#include "stb_image.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "App.h"
#include "Theme.h"
#include "RuntimePaths.h"

extern unsigned char g_embedded_logo_png[];
extern unsigned int g_embedded_logo_png_len;

// Simple append-mode logger for the injector.
static void log(const std::string& msg) {
    FILE* f = fopen(RuntimePaths::injectorLogPath().c_str(), "a");
    if (f) { fprintf(f, "%s\n", msg.c_str()); fclose(f); }
}

static void setWindowIconFromEmbeddedLogo(SDL_Window* window) {
    if (!window || g_embedded_logo_png_len == 0) return;

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* rgba = stbi_load_from_memory(
        g_embedded_logo_png,
        static_cast<int>(g_embedded_logo_png_len),
        &width,
        &height,
        &channels,
        4
    );
    if (!rgba || width <= 0 || height <= 0) {
        if (rgba) stbi_image_free(rgba);
        log("Window icon: failed to decode embedded logo");
        return;
    }

    SDL_Surface* iconSurface = SDL_CreateRGBSurfaceWithFormatFrom(
        rgba, width, height, 32, width * 4, SDL_PIXELFORMAT_RGBA32
    );
    if (!iconSurface) {
        log("Window icon: failed to create SDL surface");
        stbi_image_free(rgba);
        return;
    }

    SDL_SetWindowIcon(window, iconSurface);
    SDL_FreeSurface(iconSurface);
    stbi_image_free(rgba);
    log("Window icon: applied embedded logo");
}

// Window sizing as display percentages for resolution-independent scaling.
static constexpr float WINDOW_W_PCT = 520.0f / 1920.0f;
static constexpr float WINDOW_H_LOCKED_PCT = 560.0f / 1080.0f;

// Manual window drag state (borderless window requires custom dragging).
static bool  g_dragging = false;
static int   g_dragStartX = 0;
static int   g_dragStartY = 0;
static int   g_winStartX  = 0;
static int   g_winStartY  = 0;

static std::string getSelfDir() {
    char self[4096];
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (len <= 0) return ".";
    self[len] = '\0';
    return std::filesystem::path(self).parent_path().string();
}

// Runs the injector GUI event loop and owns the SDL2/OpenGL/ImGui lifetime.
int main(int /*argc*/, char* /*argv*/[]) {
    log("=== XtsyInjector startup ===");

    // ── Wayland-first: set SDL video driver hint before SDL_Init ──
    // Minecraft itself runs under XWayland, but the injector GUI should be Wayland-native.
    bool waylandOk = false;
    const char* waylandDisplay = getenv("WAYLAND_DISPLAY");

    if (waylandDisplay && waylandDisplay[0] != '\0') {
        SDL_SetHint(SDL_HINT_VIDEODRIVER, "wayland");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0) {
            waylandOk = true;
            log("SDL init: Wayland-native path taken");
        } else {
            log("SDL init: Wayland failed (" + std::string(SDL_GetError()) + "), falling back to XWayland");
            SDL_SetHint(SDL_HINT_VIDEODRIVER, "");
        }
    }

    if (!waylandOk) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
            log("SDL init: FATAL — " + std::string(SDL_GetError()));
            fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
        log("SDL init: XWayland/X11 fallback path taken");
    }

    // ── OpenGL 3.3 context attributes ──
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    // ── Center the window on the primary display ──
    SDL_Rect displayBounds;
    SDL_GetDisplayBounds(0, &displayBounds);
    const int windowW = std::max(420, (int)(displayBounds.w * WINDOW_W_PCT));
    const int windowHLocked = std::max(420, (int)(displayBounds.h * WINDOW_H_LOCKED_PCT));
    int winX = displayBounds.x + (displayBounds.w - windowW) / 2;
    int winY = displayBounds.y + (displayBounds.h - windowHLocked) / 2;

    // Create the borderless, non-resizable SDL2+OpenGL window.
    SDL_Window* window = SDL_CreateWindow(
        "Xtsy Injector",
        winX, winY, windowW, windowHLocked,
        SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE
    );
    if (!window && waylandOk) {
        log("SDL_CreateWindow failed on Wayland (" + std::string(SDL_GetError()) + "), retrying XWayland fallback");
        SDL_Quit();
        SDL_SetHint(SDL_HINT_VIDEODRIVER, "");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
            window = SDL_CreateWindow(
                "Xtsy Injector",
                winX, winY, windowW, windowHLocked,
                SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE
            );
        }
    }
    if (!window) {
        log("SDL_CreateWindow failed: " + std::string(SDL_GetError()));
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    setWindowIconFromEmbeddedLogo(window);
    log("SDL window created: " + std::to_string(windowW) + "x" + std::to_string(windowHLocked));

    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    if (!glContext) {
        log("GL context creation failed: " + std::string(SDL_GetError()));
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, glContext);
    SDL_GL_SetSwapInterval(1); // VSync
    glEnable(GL_MULTISAMPLE);
    log("OpenGL context created successfully");

    // ── Initialize Dear ImGui ──
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiContext* mainImGuiCtx = ImGui::GetCurrentContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr; // Disable imgui.ini saving
    io.FontGlobalScale = 1.0f;

    // Prefer a medium-weight TTF for smoother text rendering.
    std::string fontPath = getSelfDir() + "/../assets/Roboto-Medium.ttf";
    if (!std::filesystem::exists(fontPath)) {
        fontPath = getSelfDir() + "/assets/Roboto-Medium.ttf";
    }
    ImFontConfig fontCfg;
    fontCfg.OversampleH = 3;
    fontCfg.OversampleV = 2;
    fontCfg.PixelSnapH = false;
    if (std::filesystem::exists(fontPath)) {
        io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 17.0f, &fontCfg);
        log("Loaded UI font: " + fontPath);
    } else {
        log("UI font not found, using default ImGui font");
    }

    // Apply custom dark theme before any rendering.
    Theme::apply();
    ImGui_ImplSDL2_InitForOpenGL(window, glContext);
    ImGui_ImplOpenGL3_Init("#version 130");
    log("ImGui initialized with SDL2+OpenGL3 backends");

    // ── Create the App instance and load assets ──
    App app;
    app.init();

    // ── Main loop ──
    SDL_SetWindowResizable(window, SDL_TRUE);
    SDL_SetWindowMinimumSize(window, windowW, windowHLocked);
    SDL_SetWindowMaximumSize(window, windowW, windowHLocked);
    SDL_SetWindowSize(window, windowW, windowHLocked);
    while (app.isRunning()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui::SetCurrentContext(mainImGuiCtx);
            ImGui_ImplSDL2_ProcessEvent(&event);

            if (event.type == SDL_QUIT) {
                exit(0);
            }
            Uint32 mainWindowId = SDL_GetWindowID(window);

            // Manual borderless window dragging in the top 40px strip.
            if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT &&
                event.button.windowID == mainWindowId) {
                const bool uiWantsMouse = ImGui::GetIO().WantCaptureMouse;
                if (!uiWantsMouse && event.button.y < 40) {
                    g_dragging = true;
                    g_dragStartX = event.button.x;
                    g_dragStartY = event.button.y;
                    SDL_GetWindowPosition(window, &g_winStartX, &g_winStartY);
                }
            }
            if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT &&
                event.button.windowID == mainWindowId) {
                g_dragging = false;
            }
            if (event.type == SDL_MOUSEMOTION && g_dragging &&
                event.motion.windowID == mainWindowId) {
                int newX = g_winStartX + (event.motion.x - g_dragStartX);
                int newY = g_winStartY + (event.motion.y - g_dragStartY);
                SDL_SetWindowPosition(window, newX, newY);
            }
        }

        // ── Render frame ──
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        int windowW = 0;
        int windowH = 0;
        SDL_GetWindowSize(window, &windowW, &windowH);

        // Set up a fullscreen ImGui window matching the SDL window.
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)windowW, (float)windowH));
        ImGui::Begin("##MainWindow", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);

        app.render();

        ImGui::End();
        ImGui::Render();

        // Clear with dark background #1E1E1E.
        int drawableW = 0;
        int drawableH = 0;
        SDL_GL_GetDrawableSize(window, &drawableW, &drawableH);
        glViewport(0, 0, drawableW, drawableH);
        glClearColor(0.118f, 0.118f, 0.118f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    // ── Cleanup ──
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    log("=== XtsyInjector shutdown ===");
    return 0;
}
