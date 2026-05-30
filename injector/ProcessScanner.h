#pragma once
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <string>
#include "ProcessEntry.h"

// Scans /proc for Minecraft-related Java processes on a background thread.
class ProcessScanner {
public:
    ProcessScanner();
    ~ProcessScanner();

    // Returns a snapshot of the current process list (thread-safe).
    std::vector<ProcessEntry> getProcesses() const;

    // Starts the background scanning thread (called once at startup).
    void startScanning();

    // Stops the background scanning thread.
    void stopScanning();

private:
    // Performs a single scan of /proc and updates the internal list.
    void scanOnce();

    // Background thread function that calls scanOnce() every 2 seconds.
    void scanLoop();

    // Classifies a process based on its cmdline and returns a ProcessEntry.
    ProcessEntry classifyProcess(pid_t pid, const std::string& cmdline) const;

    mutable std::mutex        m_mutex;
    std::vector<ProcessEntry> m_processes;
    std::thread               m_thread;
    std::atomic<bool>         m_running{false};
};
