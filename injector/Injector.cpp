// Injector.cpp — ptrace-based .so injection into a target Linux process.
#include "Injector.h"
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <dlfcn.h>
#include <unistd.h>
#include <vector>
#include <sys/syscall.h>
#include <signal.h>
#include <sys/capability.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <algorithm>
#include <filesystem>
#include "RuntimePaths.h"

// Simple append-mode logger for the injector.
static void log(const std::string& msg) {
    FILE* f = fopen(RuntimePaths::injectorLogPath().c_str(), "a");
    if (f) { fprintf(f, "%s\n", msg.c_str()); fclose(f); }
}

// Reads the current Yama ptrace_scope value, or -1 when the sysctl is unavailable.
static int readPtraceScope() {
    std::ifstream file("/proc/sys/kernel/yama/ptrace_scope");
    int value = -1;
    file >> value;
    return value;
}

// Returns true when this process currently has CAP_SYS_PTRACE in its effective set.
static bool hasSysPtraceCapability() {
    cap_t caps = cap_get_proc();
    if (!caps) return false;
    cap_flag_value_t value = CAP_CLEAR;
    cap_get_flag(caps, CAP_SYS_PTRACE, CAP_EFFECTIVE, &value);
    cap_free(caps);
    return value == CAP_SET;
}

// Builds a useful ptrace permission error that points at the non-sudo fixes.
static std::string ptracePermissionHint(const std::string& syscallError) {
    int scope = readPtraceScope();
    std::string message = "ptrace ATTACH failed: " + syscallError;
    if (scope > 0 && !hasSysPtraceCapability()) {
        message += ". Linux Yama ptrace_scope=" + std::to_string(scope) +
                   " blocks attaching to unrelated processes. Run once: "
                   "sudo setcap cap_sys_ptrace+ep ./build/mc_injector, or temporarily: "
                   "echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope.";
    }
    return message;
}

// Formats a pointer-sized value as a hexadecimal string for log readability.
static std::string hexValue(uintptr_t value) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "0x%lx", static_cast<unsigned long>(value));
    return buffer;
}

// Logs the injector privilege context and selected payload metadata before ptrace work begins.
static void logInjectionContext(pid_t pid, const std::string& clientSoPath) {
    struct stat st {};
    std::string payloadInfo = stat(clientSoPath.c_str(), &st) == 0
        ? "size=" + std::to_string(static_cast<long long>(st.st_size)) + " mode=" + std::to_string(st.st_mode & 0777)
        : "stat failed: " + std::string(strerror(errno));
    log("context: target_pid=" + std::to_string(pid) +
        " uid=" + std::to_string(getuid()) +
        " euid=" + std::to_string(geteuid()) +
        " ptrace_scope=" + std::to_string(readPtraceScope()) +
        " cap_sys_ptrace=" + std::string(hasSysPtraceCapability() ? "yes" : "no"));
    log("context: payload=" + clientSoPath + " " + payloadInfo);
}

// Parses /proc/<pid>/maps to find the base address of a library matching libName.
uintptr_t Injector::findRemoteLibBase(pid_t pid, const std::string& libName) {
    std::string mapsPath = "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream maps(mapsPath);
    if (!maps.is_open()) {
        log("findRemoteLibBase: cannot open " + mapsPath);
        return 0;
    }

    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(libName) != std::string::npos) {
            // Extract the base address from the start of the line (before the '-').
            size_t dashPos = line.find('-');
            if (dashPos != std::string::npos) {
                std::string addrStr = line.substr(0, dashPos);
                uintptr_t addr = std::stoull(addrStr, nullptr, 16);
                log("findRemoteLibBase: found " + libName + " at 0x" + addrStr + " in PID " + std::to_string(pid));
                return addr;
            }
        }
    }

    log("findRemoteLibBase: " + libName + " not found in PID " + std::to_string(pid));
    return 0;
}

// Resolves a symbol's address in the target process by computing local offset + remote base.
uintptr_t Injector::resolveRemoteSymbol(pid_t pid, const std::string& libName,
                                          const std::string& symbolName) {
    // Find the library's base address in the target process.
    uintptr_t remoteBase = findRemoteLibBase(pid, libName);
    if (remoteBase == 0) return 0;

    // Find the same library in our own process to compute the symbol offset.
    uintptr_t localBase = findRemoteLibBase(getpid(), libName);
    if (localBase == 0) {
        // Try to load it locally to get the base.
        void* handle = dlopen(libName.c_str(), RTLD_NOW | RTLD_NOLOAD);
        if (!handle) {
            handle = dlopen(libName.c_str(), RTLD_NOW);
        }
        if (!handle) {
            log("resolveRemoteSymbol: cannot dlopen " + libName + " locally");
            return 0;
        }
        // Re-check our maps after loading.
        localBase = findRemoteLibBase(getpid(), libName);
        if (localBase == 0) {
            dlclose(handle);
            return 0;
        }
        dlclose(handle);
    }

    // Resolve the symbol address locally.
    void* localSym = dlsym(RTLD_DEFAULT, symbolName.c_str());
    if (!localSym) {
        // Try opening the library explicitly.
        void* handle = dlopen(libName.c_str(), RTLD_NOW);
        if (handle) {
            localSym = dlsym(handle, symbolName.c_str());
            dlclose(handle);
        }
    }
    if (!localSym) {
        log("resolveRemoteSymbol: cannot find symbol " + symbolName);
        return 0;
    }

    // Calculate the offset and apply it to the remote base.
    uintptr_t offset = reinterpret_cast<uintptr_t>(localSym) - localBase;
    uintptr_t remoteAddr = remoteBase + offset;
    log("resolveRemoteSymbol: " + symbolName + " at remote 0x" +
        std::to_string(remoteAddr) + " (offset 0x" + std::to_string(offset) + ")");
    return remoteAddr;
}

// Writes data into target process memory using PTRACE_POKETEXT (8 bytes at a time).
bool Injector::writeMemory(pid_t pid, uintptr_t addr, const void* data, size_t len) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    size_t i = 0;

    // Write 8-byte words via PTRACE_POKETEXT.
    for (; i + sizeof(long) <= len; i += sizeof(long)) {
        long word = 0;
        memcpy(&word, bytes + i, sizeof(long));
        if (ptrace(PTRACE_POKETEXT, pid, addr + i, word) == -1) {
            log("writeMemory: PTRACE_POKETEXT failed at offset " + std::to_string(i) +
                ": " + strerror(errno));
            return false;
        }
    }

    // Handle remaining bytes (partial word at the end).
    if (i < len) {
        long word = ptrace(PTRACE_PEEKTEXT, pid, addr + i, nullptr);
        memcpy(&word, bytes + i, len - i);
        if (ptrace(PTRACE_POKETEXT, pid, addr + i, word) == -1) {
            log("writeMemory: PTRACE_POKETEXT (partial) failed: " + std::string(strerror(errno)));
            return false;
        }
    }

    return true;
}

// Finds the first executable memory region (r-xp) in the target process.
uintptr_t Injector::findExecutableRegion(pid_t pid) {
    std::string mapsPath = "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream maps(mapsPath);
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(" r-xp ") != std::string::npos) {
            size_t dashPos = line.find('-');
            if (dashPos != std::string::npos) {
                std::string addrStr = line.substr(0, dashPos);
                return std::stoull(addrStr, nullptr, 16);
            }
        }
    }
    return 0;
}

/// Finds a 0xCC (int3) byte in the given memory region.
uintptr_t Injector::findInt3(pid_t pid, uintptr_t startAddr) {
    for (size_t i = 0; i < 0x100000; i += sizeof(long)) {
        long word = ptrace(PTRACE_PEEKTEXT, pid, startAddr + i, nullptr);
        uint8_t* bytes = (uint8_t*)&word;
        for (size_t j = 0; j < sizeof(long); j++) {
            if (bytes[j] == 0xCC) {
                uintptr_t addr = startAddr + i + j;
                if ((addr & (sizeof(long) - 1)) <= sizeof(long) - 3) {
                    return addr;
                }
            }
        }
    }
    return 0;
}

// Allocates writable executable memory in the target by running mmap from a temporary code cave.
uintptr_t Injector::allocateRemoteMemory(pid_t pid, size_t size, const user_regs_struct& savedRegs) {
    uintptr_t execRegion = findExecutableRegion(pid);
    if (execRegion == 0) {
        log("allocateRemoteMemory: no executable region found for syscall stub");
        return 0;
    }

    uintptr_t stubAddr = findInt3(pid, execRegion);
    if (stubAddr == 0) {
        log("allocateRemoteMemory: no int3 code cave found in executable region");
        return 0;
    }
    log("allocateRemoteMemory: using code cave " + hexValue(stubAddr) +
        " from executable region " + hexValue(execRegion));

    errno = 0;
    uintptr_t alignedStub = stubAddr & ~(sizeof(long) - 1);
    size_t byteOffset = stubAddr - alignedStub;
    long originalWord = ptrace(PTRACE_PEEKTEXT, pid, alignedStub, nullptr);
    if (originalWord == -1 && errno != 0) {
        log("allocateRemoteMemory: PTRACE_PEEKTEXT failed: " + std::string(strerror(errno)));
        return 0;
    }

    long patchedWord = originalWord;
    unsigned char code[sizeof(long)] {};
    std::memset(code, 0x90, sizeof(code));
    code[0] = 0x0f; // syscall
    code[1] = 0x05;
    code[2] = 0xcc; // int3
    std::memcpy(reinterpret_cast<unsigned char*>(&patchedWord) + byteOffset, code,
                std::min(sizeof(code), sizeof(long) - byteOffset));
    if (ptrace(PTRACE_POKETEXT, pid, alignedStub, patchedWord) == -1) {
        log("allocateRemoteMemory: failed to write syscall stub: " + std::string(strerror(errno)));
        return 0;
    }

    user_regs_struct regs = savedRegs;
    regs.rax = SYS_mmap;
    regs.rdi = 0;
    regs.rsi = size;
    regs.rdx = PROT_READ | PROT_WRITE | PROT_EXEC;
    regs.r10 = MAP_PRIVATE | MAP_ANONYMOUS;
    regs.r8 = static_cast<unsigned long>(-1);
    regs.r9 = 0;
    regs.rip = stubAddr;
    regs.eflags &= ~(1 << 10);

    if (ptrace(PTRACE_SETREGS, pid, nullptr, &regs) == -1) {
        log("allocateRemoteMemory: PTRACE_SETREGS failed: " + std::string(strerror(errno)));
        ptrace(PTRACE_POKETEXT, pid, alignedStub, originalWord);
        return 0;
    }
    if (ptrace(PTRACE_CONT, pid, nullptr, nullptr) == -1) {
        log("allocateRemoteMemory: PTRACE_CONT failed: " + std::string(strerror(errno)));
        ptrace(PTRACE_POKETEXT, pid, alignedStub, originalWord);
        ptrace(PTRACE_SETREGS, pid, nullptr, &savedRegs);
        return 0;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        log("allocateRemoteMemory: waitpid failed: " + std::string(strerror(errno)));
        ptrace(PTRACE_POKETEXT, pid, alignedStub, originalWord);
        ptrace(PTRACE_SETREGS, pid, nullptr, &savedRegs);
        return 0;
    }

    if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP) {
        if (WIFSTOPPED(status)) {
            log("allocateRemoteMemory: unexpected signal " + std::to_string(WSTOPSIG(status)) +
                " while running mmap stub");
        } else {
            log("allocateRemoteMemory: target exited while running mmap stub, status=" + std::to_string(status));
        }
        ptrace(PTRACE_POKETEXT, pid, alignedStub, originalWord);
        ptrace(PTRACE_SETREGS, pid, nullptr, &savedRegs);
        return 0;
    }

    user_regs_struct afterRegs {};
    if (ptrace(PTRACE_GETREGS, pid, nullptr, &afterRegs) == -1) {
        log("allocateRemoteMemory: PTRACE_GETREGS after mmap failed: " + std::string(strerror(errno)));
        ptrace(PTRACE_POKETEXT, pid, alignedStub, originalWord);
        ptrace(PTRACE_SETREGS, pid, nullptr, &savedRegs);
        return 0;
    }

    if (ptrace(PTRACE_POKETEXT, pid, alignedStub, originalWord) == -1) {
        log("allocateRemoteMemory: warning, failed to restore original code: " + std::string(strerror(errno)));
    }
    if (ptrace(PTRACE_SETREGS, pid, nullptr, &savedRegs) == -1) {
        log("allocateRemoteMemory: warning, failed to restore registers: " + std::string(strerror(errno)));
    }

    if (afterRegs.rax >= static_cast<unsigned long>(-4095)) {
        log("allocateRemoteMemory: mmap returned error " + std::to_string(static_cast<long>(afterRegs.rax)));
        return 0;
    }

    log("allocateRemoteMemory: mmap returned 0x" + std::to_string(afterRegs.rax));
    return static_cast<uintptr_t>(afterRegs.rax);
}

// Main injection entry point: attaches to pid, injects shellcode to dlopen clientSoPath.
std::string Injector::inject(pid_t pid, const std::string& clientSoPath) {
    log("inject: starting injection into PID " + std::to_string(pid));
    log("inject: client.so path: " + clientSoPath);
    logInjectionContext(pid, clientSoPath);

    // Verify the client.so file exists.
    if (access(clientSoPath.c_str(), F_OK) != 0) {
        return "client.so not found at " + clientSoPath;
    }

    // Step 1: Attach to the target process.
    log("inject: PTRACE_ATTACH");
    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) == -1) {
        std::string err = errno == EPERM
            ? ptracePermissionHint(strerror(errno))
            : "ptrace ATTACH failed: " + std::string(strerror(errno));
        log("inject: " + err);
        return err;
    }

    // Step 2: Wait for the process to stop.
    int status;
    if (waitpid(pid, &status, 0) == -1) {
        std::string err = "waitpid after ATTACH failed: " + std::string(strerror(errno));
        log("inject: " + err);
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return err;
    }
    log("inject: target stopped, status=" + std::to_string(status));

    // Step 3: Save original registers.
    struct user_regs_struct origRegs;
    if (ptrace(PTRACE_GETREGS, pid, nullptr, &origRegs) == -1) {
        std::string err = "PTRACE_GETREGS failed: " + std::string(strerror(errno));
        log("inject: " + err);
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return err;
    }
    log("inject: original RIP=" + hexValue(origRegs.rip) +
        " RSP=" + hexValue(origRegs.rsp) +
        " RAX=" + hexValue(origRegs.rax));

    // Step 4: Find dlopen in the target process.
    // Try multiple possible library names for libdl.
    uintptr_t remoteDlopen = 0;
    bool useInternalDlopen = false;
    const char* libNames[] = {"libdl.so", "libdl-", "libc.so", "libc-"};
    for (const auto& name : libNames) {
        remoteDlopen = resolveRemoteSymbol(pid, name, "dlopen");
        if (remoteDlopen != 0) break;
        remoteDlopen = resolveRemoteSymbol(pid, name, "__libc_dlopen_mode");
        if (remoteDlopen != 0) {
            useInternalDlopen = true;
            break;
        }
    }

    if (remoteDlopen == 0) {
        // On modern glibc, dlopen is in ld-linux.
        remoteDlopen = resolveRemoteSymbol(pid, "ld-linux", "dlopen");
        if (remoteDlopen == 0) {
            remoteDlopen = resolveRemoteSymbol(pid, "ld-linux", "__libc_dlopen_mode");
            useInternalDlopen = remoteDlopen != 0;
        }
    }

    if (remoteDlopen == 0) {
        std::string err = "Could not resolve dlopen in target process";
        log("inject: " + err);
        ptrace(PTRACE_SETREGS, pid, nullptr, &origRegs);
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return err;
    }
    log("inject: remote dlopen at " + hexValue(remoteDlopen) +
        std::string(useInternalDlopen ? " (__libc_dlopen_mode)" : " (dlopen)"));

    // Step 5: Allocate executable memory in the target for shellcode, path string, and scratch stack.
    size_t pathLen = clientSoPath.size() + 1; // Include null terminator.
    size_t allocSize = 16384; // Four pages for payload text and a scratch stack, Phantom-style.
    uintptr_t remoteMem = allocateRemoteMemory(pid, allocSize, origRegs);
    if (remoteMem == 0) {
        std::string err = "Failed to allocate memory in target process";
        log("inject: " + err);
        ptrace(PTRACE_SETREGS, pid, nullptr, &origRegs);
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return err;
    }

    // Step 6: Write the path string into the allocated memory.
    uintptr_t pathAddr = remoteMem;
    if (!writeMemory(pid, pathAddr, clientSoPath.c_str(), pathLen)) {
        std::string err = "Failed to write path string to target memory";
        log("inject: " + err);
        ptrace(PTRACE_SETREGS, pid, nullptr, &origRegs);
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return err;
    }
    log("inject: wrote path string at " + hexValue(pathAddr) +
        " len=" + std::to_string(pathLen));

    // Build a Phantom-style remote call stub: call *%rax; int3; nops.
    uintptr_t codeAddr = remoteMem + 1024; // Offset past the path string.
    uintptr_t stackTop = (remoteMem + allocSize - 32) & ~static_cast<uintptr_t>(0xFUL);
    uint32_t dlopenFlags = 0x1 | 0x100; // RTLD_LAZY | RTLD_GLOBAL
    if (useInternalDlopen) {
        dlopenFlags |= 0x80000000U; // __RTLD_DLOPEN, matching Phantom-Injector/kubo injector.
    }

    std::vector<uint8_t> shellcode = {
        0xFF, 0xD0,       // call *%rax
        0xCC,             // int3
        0x90, 0x90, 0x90, 0x90, 0x90
    };

    // Step 7: Write the shellcode to the allocated memory.
    if (!writeMemory(pid, codeAddr, shellcode.data(), shellcode.size())) {
        std::string err = "Failed to write shellcode to target memory";
        log("inject: " + err);
        ptrace(PTRACE_SETREGS, pid, nullptr, &origRegs);
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return err;
    }
    log("inject: wrote " + std::to_string(shellcode.size()) + " bytes of shellcode at " +
        hexValue(codeAddr));

    // Step 8: Redirect execution to the shellcode stub.
    struct user_regs_struct newRegs = origRegs;
    newRegs.rip = codeAddr;
    newRegs.rsp = stackTop;
    newRegs.rbp = stackTop;
    newRegs.rax = remoteDlopen;
    newRegs.rdi = pathAddr;
    newRegs.rsi = dlopenFlags;
    newRegs.rdx = 0;
    newRegs.rcx = 0;
    newRegs.r8 = 0;
    newRegs.r9 = 0;
    // Clear DF flag to be safe.
    newRegs.eflags &= ~(1 << 10);

    if (ptrace(PTRACE_SETREGS, pid, nullptr, &newRegs) == -1) {
        std::string err = "PTRACE_SETREGS (redirect) failed: " + std::string(strerror(errno));
        log("inject: " + err);
        ptrace(PTRACE_SETREGS, pid, nullptr, &origRegs);
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return err;
    }
    log("inject: RIP redirected to shellcode, scratch RSP=" + hexValue(newRegs.rsp) +
        " flags=" + hexValue(dlopenFlags));

    // Step 9: Let the target run the shellcode.
    if (ptrace(PTRACE_CONT, pid, nullptr, nullptr) == -1) {
        std::string err = "PTRACE_CONT failed: " + std::string(strerror(errno));
        log("inject: " + err);
        ptrace(PTRACE_SETREGS, pid, nullptr, &origRegs);
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return err;
    }
    log("inject: target running shellcode, waiting for SIGTRAP...");

    // Step 10: Wait for SIGTRAP from int3.
    if (waitpid(pid, &status, 0) == -1) {
        std::string err = "waitpid after shellcode failed: " + std::string(strerror(errno));
        log("inject: " + err);
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return err;
    }

    if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP) {
        log("inject: SIGTRAP received — shellcode executed");

        // Check dlopen return value (in RAX).
        struct user_regs_struct postRegs {};
        if (ptrace(PTRACE_GETREGS, pid, nullptr, &postRegs) == -1) {
            log("inject: WARNING — failed to read post-shellcode registers: " + std::string(strerror(errno)));
        }
        if (postRegs.rax == 0) {
            std::string err = "dlopen returned NULL (client.so did not load)";
            log("inject: ERROR — " + err);
            ptrace(PTRACE_SETREGS, pid, nullptr, &origRegs);
            ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
            return err;
        } else {
            log("inject: dlopen returned handle " + hexValue(postRegs.rax));
        }
    } else {
        struct user_regs_struct faultRegs {};
        if (ptrace(PTRACE_GETREGS, pid, nullptr, &faultRegs) == 0) {
            log("inject: fault registers RIP=" + hexValue(faultRegs.rip) +
                " RSP=" + hexValue(faultRegs.rsp) +
                " RAX=" + hexValue(faultRegs.rax));
        }
        siginfo_t info {};
        if (ptrace(PTRACE_GETSIGINFO, pid, nullptr, &info) == 0) {
            log("inject: signal info signo=" + std::to_string(info.si_signo) +
                " code=" + std::to_string(info.si_code) +
                " addr=" + hexValue(reinterpret_cast<uintptr_t>(info.si_addr)));
        }
        std::string err = "unexpected stop after shellcode, status=" + std::to_string(status) +
                          " signal=" + (WIFSTOPPED(status) ? std::to_string(WSTOPSIG(status)) : "none");
        log("inject: " + err);
        ptrace(PTRACE_SETREGS, pid, nullptr, &origRegs);
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return err;
    }

    // Step 11: Restore original registers.
    if (ptrace(PTRACE_SETREGS, pid, nullptr, &origRegs) == -1) {
        log("inject: WARNING — failed to restore registers: " + std::string(strerror(errno)));
    }
    log("inject: original registers restored");

    // Step 12: Detach from the target.
    if (ptrace(PTRACE_DETACH, pid, nullptr, nullptr) == -1) {
        std::string err = "PTRACE_DETACH failed: " + std::string(strerror(errno));
        log("inject: " + err);
        return err;
    }
    log("inject: detached from PID " + std::to_string(pid) + " — injection complete");

    return ""; // Empty string = success.
}

// Checks whether the target process already has this client mapping loaded.
bool Injector::isAlreadyInjected(pid_t pid, const std::string& clientSoPath) {
    std::string mapsPath = "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream maps(mapsPath);
    if (!maps.is_open()) return false;

    const std::string absTarget = std::filesystem::weakly_canonical(clientSoPath).string();
    const std::string baseTarget = std::filesystem::path(clientSoPath).filename().string();

    std::string line;
    while (std::getline(maps, line)) {
        if (!absTarget.empty() && line.find(absTarget) != std::string::npos) {
            log("isAlreadyInjected: exact path match found in target maps");
            return true;
        }
        if (!baseTarget.empty() && line.find(baseTarget) != std::string::npos &&
            line.find(".so") != std::string::npos) {
            log("isAlreadyInjected: basename match found in target maps: " + baseTarget);
            return true;
        }
    }
    return false;
}
