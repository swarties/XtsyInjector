// App.cpp — State machine, render dispatch, injection lifecycle management.
#include "App.h"
#include "Injector.h"
#include "ImageLoader.h"
#include "Theme.h"
#include "RuntimePaths.h"
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <system_error>
#include <sys/stat.h>
#include "imgui.h"

extern unsigned char g_embedded_client_so[];
extern unsigned int g_embedded_client_so_len;
extern unsigned char g_embedded_logo_png[];
extern unsigned int g_embedded_logo_png_len;

// Simple append-mode logger for the injector.
static void log(const std::string& msg) {
    FILE* f = fopen(RuntimePaths::injectorLogPath().c_str(), "a");
    if (f) { fprintf(f, "%s\n", msg.c_str()); fclose(f); }
}

// Resolves the directory of the currently running executable.
static std::string getSelfDir() {
    char self[4096];
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (len <= 0) return ".";
    self[len] = '\0';
    return std::filesystem::path(self).parent_path().string();
}

static std::string trimNewline(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && (s[end - 1] == '\n' || s[end - 1] == '\r')) end--;
    return s.substr(0, end);
}

static bool pickFileWithZenity(std::string& outPath) {
    if (!RuntimePaths::commandExists("zenity")) return false;
    FILE* pipe = popen("zenity --file-selection 2>/dev/null", "r");
    if (!pipe) return false;
    char buffer[2048];
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    const int rc = pclose(pipe);
    if (rc != 0) return false;
    result = trimNewline(result);
    if (result.empty()) return false;
    outPath = result;
    return true;
}

static const char* kLionJarRawUrl =
    "https://raw.githubusercontent.com/LionClientINC/LionInjectable/main/dist/client.jar";

static bool downloadLionJarToTemp(std::string& outPath, std::string& errorOut) {
    if (!RuntimePaths::commandExists("curl")) {
        errorOut = "Lion payload download requires curl, but curl is not available on this system.";
        return false;
    }
    const std::string tmpPath = (std::filesystem::path(RuntimePaths::tempDir()) /
        ("xtsy_lion_payload_" + std::to_string(getpid()) + "_" + std::to_string((long long)std::time(nullptr)) + ".jar")).string();
    const std::string cmd = "curl -fsSL \"" + std::string(kLionJarRawUrl) + "\" -o \"" + tmpPath + "\"";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        errorOut = "No payload found for Lion (download failed). Check network and retry.";
        return false;
    }
    if (!std::filesystem::exists(tmpPath)) {
        errorOut = "No payload found for Lion (download produced no file).";
        return false;
    }
    outPath = tmpPath;
    return true;
}

static void renderJarPathEditor(
    const char* label,
    const char* inputId,
    const char* buttonId,
    char* buffer,
    size_t bufferSize,
    bool pickerAvailable
) {
    ImGui::TextUnformatted(label);
    const float pickerButtonW = 32.0f;
    ImGui::SetNextItemWidth(-pickerButtonW - 10.0f);
    ImGui::InputText(inputId, buffer, bufferSize);
    ImGui::SameLine(0.0f, 8.0f);
    if (!pickerAvailable) ImGui::BeginDisabled();
    if (ImGui::Button(buttonId, ImVec2(pickerButtonW, 0.0f)) && pickerAvailable) {
        std::string picked;
        if (pickFileWithZenity(picked)) {
            std::snprintf(buffer, bufferSize, "%s", picked.c_str());
        }
    }
    if (!pickerAvailable) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("zenity is not installed");
        }
    }
}

std::string App::payloadProfileName() const {
    if (m_payloadProfile == PayloadProfile::XTSY) return "Xtsy";
    if (m_payloadProfile == PayloadProfile::LION) return "Lion";
    std::string custom = m_customProfileNameBuf.data();
    if (custom.empty()) custom = "Custom";
    return custom;
}

std::string App::selectedJavaJarPath() const {
    if (m_payloadProfile == PayloadProfile::CUSTOM) {
        if (!m_customPayloadEnabled) return "";
        return std::string(m_customJarPathBuf.data());
    }
    return std::string(m_xtsyJarPathBuf.data());
}

bool App::materializeEmbeddedClientSo(std::string& outClientSoPath, std::string& outTempDir, std::string& errorOut) const {
    if (g_embedded_client_so_len == 0) {
        errorOut = "Embedded native payload is empty.";
        return false;
    }

    std::string tempDirTemplateStr = (std::filesystem::path(RuntimePaths::tempDir()) / "xtsy_injector_XXXXXX").string();
    std::vector<char> tempDirTemplate(tempDirTemplateStr.begin(), tempDirTemplateStr.end());
    tempDirTemplate.push_back('\0');
    char* dir = mkdtemp(tempDirTemplate.data());
    if (!dir) {
        errorOut = "Failed to create temporary directory for payload extraction.";
        return false;
    }

    outTempDir = dir;
    const std::filesystem::path soPath = std::filesystem::path(outTempDir) / "client.so";
    FILE* f = fopen(soPath.c_str(), "wb");
    if (!f) {
        errorOut = "Failed to create temporary native payload file.";
        std::error_code ec;
        std::filesystem::remove(soPath, ec);
        std::filesystem::remove(outTempDir, ec);
        return false;
    }

    const size_t written = fwrite(g_embedded_client_so, 1, static_cast<size_t>(g_embedded_client_so_len), f);
    fclose(f);
    if (written != static_cast<size_t>(g_embedded_client_so_len)) {
        errorOut = "Failed to write complete embedded native payload to temp file.";
        std::error_code ec;
        std::filesystem::remove(soPath, ec);
        std::filesystem::remove(outTempDir, ec);
        return false;
    }

    if (chmod(soPath.c_str(), 0755) != 0) {
        errorOut = "Failed to set execute permissions on temporary native payload.";
        std::error_code ec;
        std::filesystem::remove(soPath, ec);
        std::filesystem::remove(outTempDir, ec);
        return false;
    }

    outClientSoPath = soPath.string();
    log("Extracted embedded client.so to temporary path: " + outClientSoPath);
    return true;
}

bool App::stageSelectedJarNearClient(const std::string& nativeSoPath, std::string& outStagedJarPath, std::string& errorOut) const {
    if (nativeSoPath.empty()) {
        errorOut = "Native client.so path is empty.";
        return false;
    }
    std::filesystem::path soPath(nativeSoPath);
    std::filesystem::path destinationPath = soPath.parent_path() / "client.jar";
    const std::string destination = destinationPath.string();
    std::string selected;
    bool cleanupDownloadedPayload = false;
    if (m_payloadProfile == PayloadProfile::LION) {
        if (!downloadLionJarToTemp(selected, errorOut)) return false;
        cleanupDownloadedPayload = true;
    } else {
        selected = selectedJavaJarPath();
        if (selected.empty()) {
            errorOut = "No payload found for selected profile. Set the correct payload path in Settings.";
            return false;
        }
    }

    std::error_code ec;
    if (!std::filesystem::exists(selected, ec)) {
        errorOut = "No payload found at configured path. Set the correct payload path in Settings.";
        return false;
    }
    std::filesystem::copy_file(
        selected,
        destination,
        std::filesystem::copy_options::overwrite_existing,
        ec
    );
    if (ec) {
        errorOut = "Failed to stage selected jar: " + ec.message();
        if (cleanupDownloadedPayload) std::remove(selected.c_str());
        return false;
    }
    if (cleanupDownloadedPayload) std::remove(selected.c_str());
    outStagedJarPath = destination;
    log("Staged Java payload: " + selected + " -> " + destination);
    return true;
}

bool App::writeBootstrapConfig(const std::string& nativeSoPath, std::string& outConfigPath, std::string& errorOut) const {
    if (nativeSoPath.empty()) {
        errorOut = "Native client.so path is empty.";
        return false;
    }

    std::string entryClass;
    std::string entryMethod = "start";

    if (m_payloadProfile == PayloadProfile::XTSY) {
        entryClass = "xtsy.client.Agent";
    } else if (m_payloadProfile == PayloadProfile::LION) {
        entryClass = "lion.client.Agent";
    } else {
        if (!m_customPayloadEnabled) {
            errorOut = "Custom payload is disabled. Enable it in Settings.";
            return false;
        }
        entryClass = m_customEntryClassBuf.data();
        entryMethod = m_customEntryMethodBuf.data();
        if (entryClass.empty()) {
            errorOut = "No custom entry class configured. Set it in Settings.";
            return false;
        }
        if (entryMethod.empty()) {
            entryMethod = "start";
        }
    }

    const std::filesystem::path soPath(nativeSoPath);
    const std::filesystem::path cfgPath = soPath.parent_path() / "payload.properties";
    std::ofstream out(cfgPath.string(), std::ios::trunc);
    if (!out.is_open()) {
        errorOut = "Failed to write bootstrap config: " + cfgPath.string();
        return false;
    }
    out << "entry_class=" << entryClass << "\n";
    out << "entry_method=" << entryMethod << "\n";
    out << "profile=" << payloadProfileName() << "\n";
    out.close();
    outConfigPath = cfgPath.string();
    log("Wrote bootstrap config: " + cfgPath.string() + " class=" + entryClass + " method=" + entryMethod);
    return true;
}

App::App() {}

App::~App() {
    // Stop the background process scanner thread.
    m_scanner.stopScanning();
}

// Initializes assets and starts the background process scanner.
void App::init() {
    m_showSettings = true;

    m_hasZenity = RuntimePaths::commandExists("zenity");
    m_hasCurl = RuntimePaths::commandExists("curl");
    if (!m_hasZenity) {
        log("Dependency warning: zenity not found, path picker buttons are disabled");
    }
    if (!m_hasCurl) {
        log("Dependency warning: curl not found, Lion auto-download is unavailable");
    }
    const std::string defaultXtsy = "";
    std::snprintf(m_xtsyJarPathBuf.data(), m_xtsyJarPathBuf.size(), "%s", defaultXtsy.c_str());
    std::snprintf(m_customProfileNameBuf.data(), m_customProfileNameBuf.size(), "%s", "Custom");
    std::snprintf(m_customJarPathBuf.data(), m_customJarPathBuf.size(), "%s", "");
    std::snprintf(m_customEntryClassBuf.data(), m_customEntryClassBuf.size(), "%s", "");
    std::snprintf(m_customEntryMethodBuf.data(), m_customEntryMethodBuf.size(), "%s", "start");
    loadSettings();

    // Load embedded logo texture bytes.
    m_logoTexture = ImageLoader::loadTextureFromMemory(
        g_embedded_logo_png,
        static_cast<size_t>(g_embedded_logo_png_len),
        m_logoWidth,
        m_logoHeight
    );
    m_logoLoaded = (m_logoTexture != 0);
    if (m_logoLoaded) {
        log("Logo loaded: " + std::to_string(m_logoWidth) + "x" + std::to_string(m_logoHeight));
    } else {
        log("Logo not found or failed to load — using fallback text");
    }

    // Start scanning /proc for Minecraft processes.
    m_scanner.startScanning();
    log("Process scanner started");
}

// Renders the logo or fallback text centered at the top of the window.
void App::renderLogo() {
    float windowWidth = ImGui::GetWindowWidth();
    if (m_logoLoaded) {
        // Render the logo inside a max box while preserving source aspect ratio.
        const float maxLogoW = 280.0f;
        const float maxLogoH = 100.0f;
        const float headerTop = 14.0f;
        const float headerHeight = 86.0f;
        float logoDisplayW = maxLogoW;
        float logoDisplayH = maxLogoH;
        if (m_logoWidth > 0 && m_logoHeight > 0) {
            const float scaleW = maxLogoW / static_cast<float>(m_logoWidth);
            const float scaleH = maxLogoH / static_cast<float>(m_logoHeight);
            const float scale = (scaleW < scaleH) ? scaleW : scaleH;
            logoDisplayW = static_cast<float>(m_logoWidth) * scale;
            logoDisplayH = static_cast<float>(m_logoHeight) * scale;
        }
        float logoX = (windowWidth - logoDisplayW) * 0.5f;
        float logoY = headerTop + (headerHeight - logoDisplayH) * 0.5f;
        ImGui::SetCursorPos(ImVec2(logoX, logoY));
        ImGui::Image((ImTextureID)(intptr_t)m_logoTexture, ImVec2(logoDisplayW, logoDisplayH));
    } else {
        // Fallback text "CLIENT" centered at scale 1.6.
        ImGui::SetWindowFontScale(1.6f);
        const char* fallback = "CLIENT";
        float textWidth = ImGui::CalcTextSize(fallback).x;
        ImGui::SetCursorPos(ImVec2((windowWidth - textWidth) * 0.5f, 20.0f));
        ImGui::TextUnformatted(fallback);
        ImGui::SetWindowFontScale(1.0f);
    }
}

// Renders the close button (×) in the top-right corner.
void App::renderCloseButton() {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();
    float winWidth = ImGui::GetWindowWidth();

    // Close button area: 20x20, top-right with 8px padding.
    ImVec2 btnMin(winPos.x + winWidth - 28.0f, winPos.y + 8.0f);
    ImVec2 btnMax(btnMin.x + 20.0f, btnMin.y + 20.0f);

    ImVec2 mousePos = ImGui::GetMousePos();
    bool hovered = (mousePos.x >= btnMin.x && mousePos.x <= btnMax.x &&
                    mousePos.y >= btnMin.y && mousePos.y <= btnMax.y);

    // Normal: #888888, Hover: #FFFFFF.
    ImU32 color = hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(136, 136, 136, 255);

    // Draw the × character.
    const char* label = "\xC3\x97"; // UTF-8 for ×
    ImVec2 textSize = ImGui::CalcTextSize(label);
    ImVec2 textPos(btnMin.x + (20.0f - textSize.x) * 0.5f, btnMin.y + (20.0f - textSize.y) * 0.5f);
    drawList->AddText(textPos, color, label);

    // Handle click.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        exit(0);
    }
}

// Dispatches rendering to the appropriate state handler.
void App::render() {
    renderLogo();
    renderCloseButton();

    switch (m_state) {
        case InjectorState::PROCESS_LIST: renderProcessList(); break;
        case InjectorState::INJECTING:    renderInjecting();   break;
        case InjectorState::DONE:         renderDone();        break;
        case InjectorState::ERROR_STATE:  renderError();       break;
    }

    renderSettingsButton();
    renderInlineSettingsPanel();
}

// Renders the scrollable process list with clickable rows.
void App::renderProcessList() {
    auto processes = m_scanner.getProcesses();

    float windowWidth = ImGui::GetWindowWidth();
    float windowHeight = ImGui::GetWindowHeight();
    float padding = 16.0f;
    float contentWidth = windowWidth - padding * 2.0f;
    const float listStartY = 108.0f;
    const float btnH = 30.0f;
    const float panelH = windowHeight * 0.32f;
    const float buttonY = m_showSettings
        ? (windowHeight - panelH - btnH - 18.0f)
        : (windowHeight - btnH - 14.0f);
    float childHeight = buttonY - 12.0f - listStartY;
    if (childHeight < 56.0f) childHeight = 56.0f;

    // Start the scrollable child region below the logo/header area.
    ImGui::SetCursorPos(ImVec2(padding, listStartY));
    ImGui::BeginChild("##ProcessList", ImVec2(contentWidth, childHeight), false);

    const float comboW = 170.0f;
    const char* payloadLabel = "Payload";
    const float rowGap = 8.0f;
    const float rowW = comboW + rowGap + ImGui::CalcTextSize(payloadLabel).x;
    ImGui::SetCursorPosX((contentWidth - rowW) * 0.5f);
    ImGui::PushItemWidth(comboW);
    const char* currentPreview = payloadProfileName().c_str();
    if (ImGui::BeginCombo("##PayloadCombo", currentPreview)) {
        const bool selectedXtsy = (m_payloadProfile == PayloadProfile::XTSY);
        if (ImGui::Selectable("Xtsy", selectedXtsy)) {
            m_payloadProfile = PayloadProfile::XTSY;
            log("Payload profile changed to " + payloadProfileName());
        }
        if (selectedXtsy) ImGui::SetItemDefaultFocus();

        const bool selectedLion = (m_payloadProfile == PayloadProfile::LION);
        if (!m_hasCurl) ImGui::BeginDisabled();
        if (ImGui::Selectable("Lion (jar bootstrap)", selectedLion) && m_hasCurl) {
            m_payloadProfile = PayloadProfile::LION;
            log("Payload profile changed to " + payloadProfileName());
        }
        if (!m_hasCurl) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("curl is required to auto-download Lion payload");
            }
        }

        const bool selectedCustom = (m_payloadProfile == PayloadProfile::CUSTOM);
        if (m_customPayloadEnabled) {
            std::string customLabel = "Custom: ";
            customLabel += m_customProfileNameBuf.data()[0] ? m_customProfileNameBuf.data() : "Custom";
            if (ImGui::Selectable(customLabel.c_str(), selectedCustom)) {
                m_payloadProfile = PayloadProfile::CUSTOM;
                log("Payload profile changed to " + payloadProfileName());
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::Selectable("Custom (disabled)", false);
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0.0f, rowGap);
    ImGui::TextUnformatted(payloadLabel);
    ImGui::PopItemWidth();

    std::string profileText = "Profile: " + payloadProfileName();
    float profileW = ImGui::CalcTextSize(profileText.c_str()).x;
    ImGui::SetCursorPosX((contentWidth - profileW) * 0.5f);
    ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "%s", profileText.c_str());
    ImGui::Dummy(ImVec2(0.0f, 12.0f));

    if (processes.empty()) {
        // Centered "no processes" message in #666666.
        const char* msg = "No Minecraft instances detected.";
        float textW = ImGui::CalcTextSize(msg).x;
        ImGui::SetCursorPosX((contentWidth - textW) * 0.5f);
        ImGui::SetCursorPosY(100.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
        ImGui::TextUnformatted(msg);
        ImGui::PopStyleColor();
    } else {
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        for (const auto& proc : processes) {
            ImVec2 cursorPos = ImGui::GetCursorScreenPos();
            ImVec2 rowMin = cursorPos;
            ImVec2 rowMax(rowMin.x + contentWidth, rowMin.y + 36.0f);

            bool isUnsupported = !proc.supported;

            // Background: check hover state.
            ImVec2 mousePos = ImGui::GetMousePos();
            bool hovered = (mousePos.x >= rowMin.x && mousePos.x <= rowMax.x &&
                            mousePos.y >= rowMin.y && mousePos.y <= rowMax.y);
            bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

            ImU32 bgColor;
            if (clicked && !isUnsupported)
                bgColor = IM_COL32(58, 58, 58, 255);   // Active: #3A3A3A
            else if (hovered)
                bgColor = IM_COL32(51, 51, 51, 255);   // Hover: #333333
            else
                bgColor = IM_COL32(42, 42, 42, 255);   // Normal: #2A2A2A

            // Draw row background with rounded corners.
            constexpr float rowRounding = 8.0f;
            drawList->AddRectFilled(rowMin, rowMax, bgColor, rowRounding);
            // Draw 1px border in #3D3D3D.
            drawList->AddRect(rowMin, rowMax, IM_COL32(61, 61, 61, 255), rowRounding);

            float textY = rowMin.y + 4.0f;

            if (isUnsupported) {
                // Yellow 8x8 warning square at the left.
                ImVec2 sqMin(rowMin.x + 8.0f, rowMin.y + 6.0f);
                ImVec2 sqMax(sqMin.x + 8.0f, sqMin.y + 8.0f);
                drawList->AddRectFilled(sqMin, sqMax, IM_COL32(255, 215, 0, 255)); // #FFD700

                // Display name in white, offset past the warning square.
                drawList->AddText(ImVec2(rowMin.x + 22.0f, textY),
                                  IM_COL32(255, 255, 255, 255), proc.displayName.c_str());

                // Warning text below in #888888.
                const char* warning = "Requires injectpatch.so \xe2\x80\x94 see run-patched.sh";
                drawList->AddText(ImVec2(rowMin.x + 22.0f, textY + 16.0f),
                                  IM_COL32(136, 136, 136, 255), warning);
            } else {
                // Display name in white.
                drawList->AddText(ImVec2(rowMin.x + 8.0f, textY + 6.0f),
                                  IM_COL32(255, 255, 255, 255), proc.displayName.c_str());
            }

            // PID label right-aligned in #888888.
            std::string pidLabel = "PID: " + std::to_string(proc.pid);
            ImVec2 pidSize = ImGui::CalcTextSize(pidLabel.c_str());
            drawList->AddText(ImVec2(rowMax.x - pidSize.x - 8.0f, rowMin.y + (36.0f - pidSize.y) * 0.5f),
                              IM_COL32(136, 136, 136, 255), pidLabel.c_str());

            // Handle click for supported processes.
            if (clicked && !isUnsupported) {
                startInjection(proc);
            }

            // Dummy item to grow the scrollable region boundaries for ImGui
            ImGui::Dummy(ImVec2(contentWidth, 36.0f));
        }
    }

    ImGui::EndChild();
}

// Renders the injection progress bar and stage labels.
void App::renderInjecting() {
    float windowWidth = ImGui::GetWindowWidth();
    float progress = m_progress.load();

    // Title: "Injecting..." centered.
    ImGui::SetWindowFontScale(1.15f);
    const char* title = "Injecting...";
    float titleW = ImGui::CalcTextSize(title).x;
    ImGui::SetCursorPos(ImVec2((windowWidth - titleW) * 0.5f, 90.0f));
    ImGui::TextUnformatted(title);
    ImGui::SetWindowFontScale(1.0f);

    // Full-width progress bar, 8px tall, color #4CAF50.
    ImGui::SetCursorPos(ImVec2(16.0f, 130.0f));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.298f, 0.686f, 0.314f, 1.0f));
    ImGui::ProgressBar(progress, ImVec2(windowWidth - 32.0f, 8.0f), "");
    ImGui::PopStyleColor();

    // Stage label centered in #CCCCCC.
    {
        std::lock_guard<std::mutex> lock(m_stageMutex);
        float labelW = ImGui::CalcTextSize(m_stageLabel.c_str()).x;
        ImGui::SetCursorPos(ImVec2((windowWidth - labelW) * 0.5f, 155.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
        ImGui::TextUnformatted(m_stageLabel.c_str());
        ImGui::PopStyleColor();

        // Detail text centered in #888888 at scale 0.85.
        ImGui::SetWindowFontScale(0.85f);
        float detailW = ImGui::CalcTextSize(m_stageDetail.c_str()).x;
        ImGui::SetCursorPos(ImVec2((windowWidth - detailW) * 0.5f, 180.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.533f, 0.533f, 0.533f, 1.0f));
        ImGui::TextUnformatted(m_stageDetail.c_str());
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
    }
}

// Renders the success view with a close button.
void App::renderDone() {
    float windowWidth = ImGui::GetWindowWidth();

    // Success message in #4CAF50.
    const char* msg = "Injection successful.";
    float msgW = ImGui::CalcTextSize(msg).x;
    ImGui::SetCursorPos(ImVec2((windowWidth - msgW) * 0.5f, 140.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.298f, 0.686f, 0.314f, 1.0f));
    ImGui::TextUnformatted(msg);
    ImGui::PopStyleColor();

    float btnW = 140.0f;
    float gap = 14.0f;
    float totalW = btnW * 2.0f + gap;
    float startX = (windowWidth - totalW) * 0.5f;

    ImGui::SetCursorPos(ImVec2(startX, 180.0f));
    if (ImGui::Button("Main Menu", ImVec2(btnW, 32.0f))) {
        m_state = InjectorState::PROCESS_LIST;
        m_progress.store(0.0f);
        std::lock_guard<std::mutex> lock(m_stageMutex);
        m_stageLabel.clear();
        m_stageDetail.clear();
    }
    ImGui::SameLine(0.0f, gap);
    if (ImGui::Button("Close", ImVec2(btnW, 32.0f))) {
        exit(0);
    }
}

// Renders the error view with retry and close buttons.
void App::renderError() {
    float windowWidth = ImGui::GetWindowWidth();

    // Error message in #FF4444.
    std::string msg = "Injection failed: " + m_errorMessage;
    float msgW = ImGui::CalcTextSize(msg.c_str()).x;
    // If the message is too wide, just left-pad it.
    float msgX = (msgW > windowWidth - 32.0f) ? 16.0f : (windowWidth - msgW) * 0.5f;
    ImGui::SetCursorPos(ImVec2(msgX, 130.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.267f, 0.267f, 1.0f));
    ImGui::TextWrapped("%s", msg.c_str());
    ImGui::PopStyleColor();

    // Retry and Close buttons side by side.
    float btnW = 100.0f;
    float gap = 16.0f;
    float totalW = btnW * 2.0f + gap;
    float startX = (windowWidth - totalW) * 0.5f;

    ImGui::SetCursorPos(ImVec2(startX, 180.0f));
    if (ImGui::Button("Retry", ImVec2(btnW, 32.0f))) {
        m_state = InjectorState::PROCESS_LIST;
        m_errorMessage.clear();
    }
    ImGui::SameLine(0.0f, gap);
    if (ImGui::Button("Close", ImVec2(btnW, 32.0f))) {
        exit(0);
    }
}

void App::renderSettingsButton() {
    float windowWidth = ImGui::GetWindowWidth();
    float windowHeight = ImGui::GetWindowHeight();
    const float btnH = 30.0f;
    const float panelH = windowHeight * 0.32f;
    const float buttonY = m_showSettings
        ? (windowHeight - panelH - btnH - 18.0f)
        : (windowHeight - btnH - 14.0f);

    const char* header = "Settings";
    ImGui::SetWindowFontScale(1.18f);
    ImVec2 textSize = ImGui::CalcTextSize(header);
    ImGui::SetCursorPos(ImVec2((windowWidth - textSize.x) * 0.5f, buttonY + (btnH - textSize.y) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.80f, 0.80f, 1.0f));
    ImGui::TextUnformatted(header);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
}

void App::renderInlineSettingsPanel() {
    if (!m_showSettings) return;

    const float windowW = ImGui::GetWindowWidth();
    const float windowH = ImGui::GetWindowHeight();
    const float panelW = windowW - 24.0f;
    const float panelH = windowH * 0.32f;
    const float btnH = 30.0f;
    const float buttonY = windowH - panelH - btnH - 18.0f;
    const float panelY = buttonY + btnH + 8.0f;

    ImGui::SetCursorPos(ImVec2(12.0f, panelY));
    ImGui::BeginChild("##InlineSettingsPanel", ImVec2(panelW, panelH), true, ImGuiWindowFlags_NoScrollbar);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);

    const float innerW = ImGui::GetContentRegionAvail().x;
    const float innerH = ImGui::GetContentRegionAvail().y;
    const float footerH = innerH * 0.25f;
    const float formH = innerH - footerH - 6.0f;

    ImGui::BeginChild("##InlineSettingsForm", ImVec2(innerW, formH), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    renderJarPathEditor("Xtsy Profile Jar", "##XtsyJarPath", "...##pickxtsy",
                        m_xtsyJarPathBuf.data(), m_xtsyJarPathBuf.size(), m_hasZenity);
    ImGui::TextUnformatted("Lion Payload");
    ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.70f, 1.0f), "Auto-download from GitHub on inject");
    if (!m_hasCurl) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "curl not found: Lion profile unavailable");
    }
    if (!m_hasZenity) {
        ImGui::TextColored(ImVec4(1.0f, 0.76f, 0.35f, 1.0f), "zenity not found: file picker buttons disabled");
    }

    ImGui::Separator();
    ImGui::Checkbox("Enable Custom Payload", &m_customPayloadEnabled);
    ImGui::TextUnformatted("Custom Profile Name");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##CustomProfileName", m_customProfileNameBuf.data(), m_customProfileNameBuf.size());
    if (!m_customPayloadEnabled && m_payloadProfile == PayloadProfile::CUSTOM) {
        m_payloadProfile = PayloadProfile::XTSY;
    }

    if (m_customPayloadEnabled) {
        renderJarPathEditor("Custom Profile Jar", "##CustomJarPath", "...##pickcustom",
                            m_customJarPathBuf.data(), m_customJarPathBuf.size(), m_hasZenity);
        ImGui::TextUnformatted("Custom Entry Class");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##CustomEntryClass", m_customEntryClassBuf.data(), m_customEntryClassBuf.size());
        ImGui::TextUnformatted("Custom Entry Method");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##CustomEntryMethod", m_customEntryMethodBuf.data(), m_customEntryMethodBuf.size());
    } else {
        ImGui::BeginDisabled();
        renderJarPathEditor("Custom Profile Jar", "##CustomJarPath", "...##pickcustom",
                            m_customJarPathBuf.data(), m_customJarPathBuf.size(), m_hasZenity);
        ImGui::TextUnformatted("Custom Entry Class");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##CustomEntryClass", m_customEntryClassBuf.data(), m_customEntryClassBuf.size());
        ImGui::TextUnformatted("Custom Entry Method");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##CustomEntryMethod", m_customEntryMethodBuf.data(), m_customEntryMethodBuf.size());
        ImGui::EndDisabled();
    }
    ImGui::EndChild();

    ImGui::BeginChild("##InlineSettingsFooter", ImVec2(innerW, footerH), false, ImGuiWindowFlags_NoScrollbar);
    if (ImGui::Button("Save", ImVec2(innerW * 0.24f, 0.0f))) {
        saveSettings();
    }
    ImGui::EndChild();

    ImGui::PopStyleVar(2);
    ImGui::EndChild();
}

void App::renderSettingsWindow() {
    // Legacy no-op: settings are now inline in the main window.
}

void App::loadSettings() {
    const std::string settingsPath = getSelfDir() + "/injector_settings.ini";
    std::ifstream in(settingsPath);
    if (!in.is_open()) return;

    std::string line;
    while (std::getline(in, line)) {
        const std::string xtsyPrefix = "xtsy_jar=";
        const std::string customNamePrefix = "custom_name=";
        const std::string customJarPrefix = "custom_jar=";
        const std::string customEntryClassPrefix = "custom_entry_class=";
        const std::string customEntryMethodPrefix = "custom_entry_method=";
        const std::string customEnabledPrefix = "custom_enabled=";
        if (line.rfind(xtsyPrefix, 0) == 0) {
            std::snprintf(m_xtsyJarPathBuf.data(), m_xtsyJarPathBuf.size(), "%s", line.substr(xtsyPrefix.size()).c_str());
        } else if (line.rfind(customNamePrefix, 0) == 0) {
            std::snprintf(m_customProfileNameBuf.data(), m_customProfileNameBuf.size(), "%s", line.substr(customNamePrefix.size()).c_str());
        } else if (line.rfind(customJarPrefix, 0) == 0) {
            std::snprintf(m_customJarPathBuf.data(), m_customJarPathBuf.size(), "%s", line.substr(customJarPrefix.size()).c_str());
        } else if (line.rfind(customEntryClassPrefix, 0) == 0) {
            std::snprintf(m_customEntryClassBuf.data(), m_customEntryClassBuf.size(), "%s", line.substr(customEntryClassPrefix.size()).c_str());
        } else if (line.rfind(customEntryMethodPrefix, 0) == 0) {
            std::snprintf(m_customEntryMethodBuf.data(), m_customEntryMethodBuf.size(), "%s", line.substr(customEntryMethodPrefix.size()).c_str());
        } else if (line.rfind(customEnabledPrefix, 0) == 0) {
            m_customPayloadEnabled = (line.substr(customEnabledPrefix.size()) == "1");
        }
    }
}

void App::saveSettings() const {
    const std::string settingsPath = getSelfDir() + "/injector_settings.ini";
    std::ofstream out(settingsPath, std::ios::trunc);
    if (!out.is_open()) return;
    out << "xtsy_jar=" << m_xtsyJarPathBuf.data() << "\n";
    out << "custom_name=" << m_customProfileNameBuf.data() << "\n";
    out << "custom_jar=" << m_customJarPathBuf.data() << "\n";
    out << "custom_entry_class=" << m_customEntryClassBuf.data() << "\n";
    out << "custom_entry_method=" << m_customEntryMethodBuf.data() << "\n";
    out << "custom_enabled=" << (m_customPayloadEnabled ? "1" : "0") << "\n";
}

// Launches the injection thread targeting the given process entry.
void App::startInjection(const ProcessEntry& entry) {
    m_state = InjectorState::INJECTING;
    m_progress.store(0.0f);
    {
        std::lock_guard<std::mutex> lock(m_stageMutex);
        m_stageLabel = "Preparing...";
        m_stageDetail = "";
    }

    std::string clientPath;
    std::string stagingTempDir;
    std::string stagedJarPath;
    std::string bootstrapConfigPath;
    pid_t targetPid = entry.pid;

    std::string stageError;
    if (!materializeEmbeddedClientSo(clientPath, stagingTempDir, stageError)) {
        log("Injection blocked: " + stageError);
        m_errorMessage = stageError;
        m_state = InjectorState::ERROR_STATE;
        return;
    }
    if (!stageSelectedJarNearClient(clientPath, stagedJarPath, stageError)) {
        log("Injection blocked: " + stageError);
        std::error_code ec;
        if (!clientPath.empty()) {
            std::filesystem::remove(clientPath, ec);
            ec.clear();
        }
        if (!stagingTempDir.empty()) {
            std::filesystem::remove(stagingTempDir, ec);
        }
        m_errorMessage = stageError;
        m_state = InjectorState::ERROR_STATE;
        return;
    }
    if (!writeBootstrapConfig(clientPath, bootstrapConfigPath, stageError)) {
        log("Injection blocked: " + stageError);
        std::error_code ec;
        if (!stagedJarPath.empty()) {
            std::filesystem::remove(stagedJarPath, ec);
            ec.clear();
        }
        if (!clientPath.empty()) {
            std::filesystem::remove(clientPath, ec);
            ec.clear();
        }
        if (!stagingTempDir.empty()) {
            std::filesystem::remove(stagingTempDir, ec);
        }
        m_errorMessage = stageError;
        m_state = InjectorState::ERROR_STATE;
        return;
    }
    log("Injection using payload profile: " + payloadProfileName());

    bool alreadyInjected = Injector::isAlreadyInjected(targetPid, clientPath);
    if (alreadyInjected) {
        log("Warning: target already has client mapped; continuing reinjection by user request");
    }

    log("Injection start: PID=" + std::to_string(targetPid) + " client=" + clientPath);

    // Run injection on a background thread to keep the UI responsive.
    std::thread([this, targetPid, clientPath, alreadyInjected, stagedJarPath, bootstrapConfigPath, stagingTempDir]() {
        auto cleanupStagingFiles = [&]() {
            std::error_code ec;
            if (!stagingTempDir.empty()) {
                std::filesystem::remove_all(stagingTempDir, ec);
                if (ec) {
                    log("Temporary cleanup warning for " + stagingTempDir + ": " + ec.message());
                }
                return;
            }
            if (!bootstrapConfigPath.empty()) {
                std::filesystem::remove(bootstrapConfigPath, ec);
                ec.clear();
            }
            if (!stagedJarPath.empty()) {
                std::filesystem::remove(stagedJarPath, ec);
                ec.clear();
            }
            if (!clientPath.empty()) {
                std::filesystem::remove(clientPath, ec);
            }
        };

        // Stage definitions: progress target, label, detail text.
        struct Stage {
            float target;
            std::string label;
            std::string detail;
        };
        Stage stages[] = {
            {0.15f, "Locating process",   "Reading /proc/" + std::to_string(targetPid) + "/maps..."},
            {0.35f, "Resolving symbols",  "Finding dlopen in libdl.so..."},
            {0.55f, "Attaching debugger",  "ptrace PTRACE_ATTACH..."},
            {0.75f, "Writing shellcode",   "Injecting call stub into JVM memory..."},
            {0.90f, "Loading client",      "dlopen: loading client.so..."},
            {1.00f, "Detaching",           "ptrace PTRACE_DETACH, cleaning up..."}
        };

        if (alreadyInjected) {
            stages[0].detail += " Warning: already injected; forcing reinjection.";
        }

        // Animate through stages with minimum 400ms per stage.
        auto stageStart = std::chrono::steady_clock::now();

        for (int i = 0; i < 6; ++i) {
            {
                std::lock_guard<std::mutex> lock(m_stageMutex);
                m_stageLabel = stages[i].label;
                m_stageDetail = stages[i].detail;
            }
            log("Stage " + std::to_string(i + 1) + ": " + stages[i].label);

            float startProgress = (i == 0) ? 0.0f : stages[i - 1].target;
            float endProgress = stages[i].target;

            // Smooth animation toward the target over 400ms minimum.
            stageStart = std::chrono::steady_clock::now();
            while (true) {
                auto now = std::chrono::steady_clock::now();
                float elapsed = std::chrono::duration<float>(now - stageStart).count();
                float t = std::min(elapsed / 0.4f, 1.0f);
                float current = startProgress + (endProgress - startProgress) * t;
                m_progress.store(current);

                if (elapsed >= 0.4f) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60fps
            }
            m_progress.store(endProgress);

            // Run actual injection at stage 3 (attaching debugger) — this is where the real work happens.
            if (i == 2) {
                std::string err = Injector::inject(targetPid, clientPath);
                if (!err.empty()) {
                    log("Injection failed: " + err);
                    cleanupStagingFiles();
                    log("Temporary payload files cleaned after failed injection");
                    m_errorMessage = err;
                    m_state = InjectorState::ERROR_STATE;
                    return;
                }
                log("Injection succeeded");
            }
        }

        // Successful inject: clean temporary staged artifacts.
        cleanupStagingFiles();
        log("Temporary payload files cleaned after successful injection");

        m_progress.store(1.0f);
        m_state = InjectorState::DONE;
        log("Injection complete, transitioning to DONE state");
    }).detach();
}
