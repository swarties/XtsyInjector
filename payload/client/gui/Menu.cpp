// Menu.cpp — ImGui overlay menu rendered inside the Minecraft OpenGL context.

#include "Menu.h"
#include "Renderer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <dlfcn.h>
#include <chrono>
#include <filesystem>

#include <imgui.h>
#include <imgui_impl_opengl2.h>

#include <GL/gl.h>

// Static member definitions.
std::atomic<bool> Menu::menuVisible{false};
bool Menu::imguiInitialized = false;
std::mutex Menu::renderMutex;
static std::chrono::steady_clock::time_point g_overlayStart;

// Append a message to the client log file.
static void log(const std::string& msg) {
    FILE* f = fopen("/tmp/mc_client.log", "a");
    if (f) { fprintf(f, "[Menu] %s\n", msg.c_str()); fclose(f); }
}

// Resolves a bundled font path copied next to client.so by CMake.
static std::string resolveFontPath() {
    Dl_info info {};
    if (dladdr(reinterpret_cast<void*>(&Menu::render), &info) != 0 && info.dli_fname) {
        return (std::filesystem::path(info.dli_fname).parent_path() / "Roboto-Medium.ttf").string();
    }
    return "Roboto-Medium.ttf";
}

// Draws a small in-game notification so successful injection is visible before the menu is toggled.
static void renderLoadedToast() {
    ImGui::SetNextWindowPos(ImVec2(18.0f, 18.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.82f);
    ImGui::Begin("##XtsyLoadedToast", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav);
    ImGui::TextColored(ImVec4(0.298f, 0.686f, 0.314f, 1.0f), "XtsyClient injected");
    ImGui::TextUnformatted("Press Arrow Up to toggle menu");
    ImGui::End();
}

// Detect the display server and install the appropriate swap-buffer hook.
void Menu::initialize(JNIEnv* /*env*/, JavaVM* /*jvm*/) {
    log("initialize() — native overlay hooks disabled; using JNI HUD markers");
}

// Render one frame of the ImGui overlay if the menu is visible.
void Menu::render() {
    std::lock_guard<std::mutex> lock(renderMutex);

    // First-time ImGui initialization inside the GL context thread.
    if (!imguiInitialized) {
        log("First render call — creating ImGui context");

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        log("ImGui context created");

        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();
        ImFontConfig fontConfig;
        fontConfig.OversampleH = 3;
        fontConfig.OversampleV = 2;
        fontConfig.PixelSnapH = false;
        std::string fontPath = resolveFontPath();
        if (std::filesystem::exists(fontPath)) {
            io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f, &fontConfig);
            log("Loaded smooth font: " + fontPath);
        } else {
            io.Fonts->AddFontDefault(&fontConfig);
            log("Roboto font missing next to client.so, using smoothed default font");
        }
        io.FontGlobalScale = 1.0f;

        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.118f, 0.118f, 0.118f, 1.0f);
        style.AntiAliasedLines = true;
        style.AntiAliasedLinesUseTex = true;
        style.AntiAliasedFill = true;
        style.WindowRounding = 5.0f;
        style.FrameRounding = 4.0f;
        log("Style applied: dark theme with custom WindowBg");

        if (!ImGui_ImplOpenGL2_Init()) {
            log("ERROR: ImGui_ImplOpenGL2_Init failed");
            return;
        }
        log("ImGui OpenGL2 backend initialized");

        g_overlayStart = std::chrono::steady_clock::now();
        imguiInitialized = true;
    }

    bool showToast = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - g_overlayStart).count() < 10;
    bool showMenu = menuVisible.load();
    if (!showMenu && !showToast) return;

    // Query the current viewport to set ImGui's display size.
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(viewport[2]), static_cast<float>(viewport[3]));
    io.DeltaTime = 1.0f / 60.0f;

    // Begin a new ImGui frame.
    ImGui_ImplOpenGL2_NewFrame();
    ImGui::NewFrame();

    if (showToast) {
        renderLoadedToast();
    }

    if (showMenu) {
        // Render a compact client-style module menu.
        static bool sprint = false;
        static bool fly = false;
        static bool noFall = false;
        static bool velocity = false;
        static float reach = 3.0f;

        ImGui::SetNextWindowSize(ImVec2(360.0f, 240.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Xtsy Client", nullptr, ImGuiWindowFlags_NoCollapse);
        ImGui::TextUnformatted("Forge Session");
        ImGui::Separator();
        ImGui::Checkbox("Sprint", &sprint);
        ImGui::Checkbox("Fly", &fly);
        ImGui::Checkbox("NoFall", &noFall);
        ImGui::Checkbox("Velocity", &velocity);
        ImGui::SliderFloat("Reach", &reach, 3.0f, 6.0f, "%.1f");
        ImGui::Separator();
        ImGui::TextUnformatted("Toggle Key: Arrow Up");
        ImGui::End();
    }

    // Finalize and draw.
    ImGui::Render();
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
}
