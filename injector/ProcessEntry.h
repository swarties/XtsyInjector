#pragma once
#include <string>
#include <cstdint>
#include <sys/types.h>

// Enum for the type of Minecraft client detected.
enum class ClientType {
    GENERIC,          // Vanilla / unknown Minecraft Java
    FORGE,            // Forge or LaunchWrapper-based
    LUNAR_CLIENT,     // Lunar Client (requires injectpatch.so)
    BADLION_CLIENT    // Badlion Client (requires injectpatch.so)
};

// Represents a single detected Minecraft-related Java process.
struct ProcessEntry {
    pid_t       pid;          // Linux process ID
    std::string displayName;  // Human-readable name shown in the UI
    ClientType  clientType;   // Sub-classification of the client
    bool        supported;    // False for Lunar/Badlion (require injectpatch workaround)
};
