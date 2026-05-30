#pragma once
#include <string>
#include <cstdint>
#include <sys/types.h>
#include <sys/user.h>

// Handles ptrace-based .so injection into a target process.
class Injector {
public:
    // Returns true if a mapping matching clientSoPath is already present in /proc/<pid>/maps.
    static bool isAlreadyInjected(pid_t pid, const std::string& clientSoPath);

    // Injects clientSoPath into the target process identified by pid.
    // Returns an empty string on success, or a descriptive error string on failure.
    static std::string inject(pid_t pid, const std::string& clientSoPath);

private:
    // Parses /proc/<pid>/maps to find the base address of a library matching libName.
    static uintptr_t findRemoteLibBase(pid_t pid, const std::string& libName);

    // Resolves the address of a symbol in the target process by computing offsets.
    static uintptr_t resolveRemoteSymbol(pid_t pid, const std::string& libName,
                                          const std::string& symbolName);

    // Writes arbitrary data into the target process memory using PTRACE_POKETEXT.
    static bool writeMemory(pid_t pid, uintptr_t addr, const void* data, size_t len);

    // Allocates writable executable memory in the target by executing a remote mmap syscall.
    static uintptr_t allocateRemoteMemory(pid_t pid, size_t size, const user_regs_struct& savedRegs);

    // Finds the first executable memory region (r-xp) in the target process.
    static uintptr_t findExecutableRegion(pid_t pid);

    // Finds a 0xCC (int3) byte in the given memory region.
    static uintptr_t findInt3(pid_t pid, uintptr_t startAddr);
};
