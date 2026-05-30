#pragma once

#include <filesystem>
#include <string>
#include <cstdlib>
#include <unistd.h>

namespace RuntimePaths {

inline std::string tempDir() {
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
    if (!ec && !tmp.empty()) return tmp.string();
    return "/tmp";
}

inline std::string injectorLogPath() {
    return (std::filesystem::path(tempDir()) / "mc_injector.log").string();
}

inline bool commandExists(const std::string& command) {
    if (command.empty()) return false;
    if (command.find('/') != std::string::npos) {
        return access(command.c_str(), X_OK) == 0;
    }

    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return false;

    std::string pathVar(pathEnv);
    size_t start = 0;
    while (start <= pathVar.size()) {
        const size_t sep = pathVar.find(':', start);
        const std::string dir = (sep == std::string::npos)
            ? pathVar.substr(start)
            : pathVar.substr(start, sep - start);
        const std::filesystem::path candidate =
            std::filesystem::path(dir.empty() ? "." : dir) / command;
        if (access(candidate.c_str(), X_OK) == 0) return true;
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return false;
}

} // namespace RuntimePaths
