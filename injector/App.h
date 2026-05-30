#pragma once
#include <string>
#include <atomic>
#include <mutex>
#include <array>
#include <GL/gl.h>
#include "ProcessScanner.h"
#include "ProcessEntry.h"

// State machine states for the injector GUI.
enum class InjectorState {
    PROCESS_LIST,
    INJECTING,
    DONE,
    ERROR_STATE
};

enum class PayloadProfile {
    XTSY,
    LION,
    CUSTOM
};

// Main application class: manages state, rendering, and injection lifecycle.
class App {
public:
    App();
    ~App();

    // Initializes the scanner and loads assets. Call once after ImGui is ready.
    void init();

    // Renders the current frame based on the active state.
    void render();

    // Returns true if the app should keep running.
    bool isRunning() const { return m_running; }
    bool isSettingsVisible() const { return m_showSettings; }
    void setSettingsVisible(bool visible) { m_showSettings = visible; }
    uint64_t settingsToggleSeq() const { return m_settingsToggleSeq; }
    void renderSettingsWindow();

private:
    // Renders the process list view.
    void renderProcessList();

    // Renders the injection progress view.
    void renderInjecting();

    // Renders the success view.
    void renderDone();

    // Renders the error view.
    void renderError();

    // Renders the logo or fallback text at the top of the window.
    void renderLogo();

    // Renders the close button in the top-right corner.
    void renderCloseButton();
    void renderSettingsButton();
    void renderInlineSettingsPanel();
    void loadSettings();
    void saveSettings() const;
    bool writeBootstrapConfig(const std::string& nativeSoPath, std::string& outConfigPath, std::string& errorOut) const;
    bool materializeEmbeddedClientSo(std::string& outClientSoPath, std::string& outTempDir, std::string& errorOut) const;

    // Starts the injection thread targeting the given process.
    void startInjection(const ProcessEntry& entry);
    std::string payloadProfileName() const;
    std::string selectedJavaJarPath() const;
    bool stageSelectedJarNearClient(const std::string& nativeSoPath, std::string& outStagedJarPath, std::string& errorOut) const;

    InjectorState           m_state = InjectorState::PROCESS_LIST;
    ProcessScanner          m_scanner;
    std::atomic<float>      m_progress{0.0f};
    std::string             m_stageLabel;
    std::string             m_stageDetail;
    std::mutex              m_stageMutex;
    std::string             m_errorMessage;
    bool                    m_running = true;

    // Logo texture
    GLuint                  m_logoTexture = 0;
    int                     m_logoWidth = 0;
    int                     m_logoHeight = 0;
    bool                    m_logoLoaded = false;
    PayloadProfile          m_payloadProfile = PayloadProfile::XTSY;
    bool                    m_showSettings = false;
    int                     m_settingsTab = 0;
    bool                    m_customPayloadEnabled = false;
    bool                    m_hasZenity = false;
    bool                    m_hasCurl = false;
    std::array<char, 1024>  m_xtsyJarPathBuf{};
    std::array<char, 128>   m_customProfileNameBuf{};
    std::array<char, 1024>  m_customJarPathBuf{};
    std::array<char, 256>   m_customEntryClassBuf{};
    std::array<char, 128>   m_customEntryMethodBuf{};
    uint64_t                m_settingsToggleSeq = 0;
};
