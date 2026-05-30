// ProcessScanner.cpp — Scans /proc for Minecraft-related Java processes.
#include "ProcessScanner.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <dirent.h>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <string>
#include "RuntimePaths.h"

// Simple append-mode logger for the injector.
static void log(const std::string& msg) {
    FILE* f = fopen(RuntimePaths::injectorLogPath().c_str(), "a");
    if (f) { fprintf(f, "%s\n", msg.c_str()); fclose(f); }
}

ProcessScanner::ProcessScanner() {}

ProcessScanner::~ProcessScanner() {
    // Ensure the background thread stops cleanly.
    stopScanning();
}

// Returns a thread-safe snapshot of the current process list.
std::vector<ProcessEntry> ProcessScanner::getProcesses() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_processes;
}

// Starts the background scanning thread.
void ProcessScanner::startScanning() {
    m_running.store(true);
    m_thread = std::thread(&ProcessScanner::scanLoop, this);
}

// Stops the background scanning thread and joins it.
void ProcessScanner::stopScanning() {
    m_running.store(false);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

// Background loop: scans every 2 seconds while m_running is true.
void ProcessScanner::scanLoop() {
    while (m_running.load()) {
        scanOnce();
        // Sleep for 2 seconds, checking for shutdown every 100ms.
        for (int i = 0; i < 20 && m_running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

// Reads /proc/<pid>/cmdline and returns the args as a space-joined string.
static std::string readCmdline(pid_t pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/cmdline";
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return "";

    std::string content;
    std::getline(file, content, '\0');  // Read first arg
    // Read remaining null-separated args.
    std::string arg;
    while (std::getline(file, arg, '\0')) {
        if (!arg.empty()) {
            content += " " + arg;
        }
    }
    return content;
}

// Converts a string to lowercase in-place and returns it.
static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Checks whether a process exposes enough procfs state to be a plausible ptrace target.
static bool isProcessInspectable(pid_t pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("Dumpable:", 0) == 0) {
            return line.find('1') != std::string::npos;
        }
    }
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    return maps.good();
}

// Classifies a process based on its lowercased cmdline string.
ProcessEntry ProcessScanner::classifyProcess(pid_t pid, const std::string& cmdline) const {
    ProcessEntry entry;
    entry.pid = pid;
    entry.supported = true;

    std::string lower = toLower(cmdline);

    // Sub-classify by client type.
    if (lower.find("lunarclient") != std::string::npos ||
        lower.find("lunar") != std::string::npos) {
        entry.displayName = "Lunar Client";
        entry.clientType = ClientType::LUNAR_CLIENT;
        // Lunar appears disabled until launched through run-patched.sh; dumpable=1 means it can be injected.
        entry.supported = isProcessInspectable(pid);
    } else if (lower.find("badlion") != std::string::npos) {
        entry.displayName = "Badlion Client";
        entry.clientType = ClientType::BADLION_CLIENT;
        // Badlion appears disabled until launched through run-patched.sh; dumpable=1 means it can be injected.
        entry.supported = isProcessInspectable(pid);
    } else if (lower.find("forge") != std::string::npos ||
               lower.find("launchwrapper") != std::string::npos) {
        entry.displayName = "Minecraft 1.8.9 \xe2\x80\x94 Forge";
        entry.clientType = ClientType::FORGE;
    } else {
        entry.displayName = "Minecraft Java";
        entry.clientType = ClientType::GENERIC;
    }

    return entry;
}

// Scans /proc for all Minecraft-related Java processes and updates the list.
void ProcessScanner::scanOnce() {
    std::vector<ProcessEntry> found;

    DIR* procDir = opendir("/proc");
    if (!procDir) {
        log("ProcessScanner: failed to open /proc");
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(procDir)) != nullptr) {
        // Only look at numeric directories (PIDs).
        if (entry->d_type != DT_DIR) continue;
        bool isNumeric = true;
        for (const char* c = entry->d_name; *c; ++c) {
            if (!std::isdigit(*c)) { isNumeric = false; break; }
        }
        if (!isNumeric) continue;

        pid_t pid = static_cast<pid_t>(std::stoi(entry->d_name));
        std::string cmdline = readCmdline(pid);
        if (cmdline.empty()) continue;

        std::string lower = toLower(cmdline);

        // Must contain "java" AND at least one Minecraft indicator.
        if (lower.find("java") == std::string::npos) continue;

        bool isMinecraft = (lower.find("minecraft") != std::string::npos ||
                            lower.find("forge") != std::string::npos ||
                            lower.find("net.minecraft") != std::string::npos ||
                            lower.find("launchwrapper") != std::string::npos ||
                            lower.find("1.8.9") != std::string::npos);

        if (!isMinecraft) continue;

        ProcessEntry pe = classifyProcess(pid, cmdline);
        found.push_back(pe);

        // Log Lunar/Badlion detection events.
        if (pe.clientType == ClientType::LUNAR_CLIENT) {
            log("Detected Lunar Client process: PID=" + std::to_string(pid));
        } else if (pe.clientType == ClientType::BADLION_CLIENT) {
            log("Detected Badlion Client process: PID=" + std::to_string(pid));
        }
    }

    closedir(procDir);

    // Update the shared process list.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_processes = std::move(found);
    }

    log("Process scan complete: " + std::to_string(m_processes.size()) + " Minecraft process(es) found");
}
