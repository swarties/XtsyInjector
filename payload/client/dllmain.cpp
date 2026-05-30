// dllmain.cpp — Shared library entry point injected into the Minecraft process.

#include <thread>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <fstream>
#include <jni.h>
#include <ctime>
#include <unistd.h>
#include <dlfcn.h>
#include <filesystem>
#include "gui/Menu.h"
#include "hooks/InputHook.h"

// Append a timestamped line to the client log file.
static void log(const std::string& msg) {
    FILE* f = fopen("/tmp/mc_client.log", "a");
    if (f) { fprintf(f, "[dllmain] %s\n", msg.c_str()); fclose(f); }
}

// Clears a pending JNI exception and records the location where it happened.
static void clearJniException(JNIEnv* env, const std::string& where) {
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        log("JNI exception cleared at " + where);
    }
}

// Tries multiple JVM class names and returns the first class that can be resolved.
static jclass findAnyClass(JNIEnv* env, const std::vector<const char*>& names) {
    for (const char* name : names) {
        jclass cls = env->FindClass(name);
        if (cls && !env->ExceptionCheck()) {
            log("Resolved class " + std::string(name));
            return cls;
        }
        clearJniException(env, std::string("FindClass(") + name + ")");
    }
    return nullptr;
}

// Updates the LWJGL window title so the injection is visible without our OpenGL hook.
static void setWindowTitleMarker(JNIEnv* env) {
    jclass displayClass = findAnyClass(env, {"org/lwjgl/opengl/Display"});
    if (!displayClass) {
        log("Window title marker: Display class not found");
        return;
    }

    jmethodID setTitle = env->GetStaticMethodID(displayClass, "setTitle", "(Ljava/lang/String;)V");
    if (!setTitle || env->ExceptionCheck()) {
        clearJniException(env, "Display.setTitle lookup");
        log("Window title marker: setTitle not found");
        return;
    }

    jstring title = env->NewStringUTF("Minecraft 1.8.9 - XtsyClient injected");
    env->CallStaticVoidMethod(displayClass, setTitle, title);
    clearJniException(env, "Display.setTitle call");
    env->DeleteLocalRef(title);
    log("Window title marker applied");
}

// Locates Minecraft's client class in common obfuscated and deobfuscated 1.8.9 names.
static jclass findMinecraftClass(JNIEnv* env) {
    return findAnyClass(env, {"ave", "net/minecraft/client/Minecraft"});
}

// Returns the directory containing the injected client.so.
static std::string selfDir() {
    Dl_info info {};
    if (dladdr(reinterpret_cast<void*>(&selfDir), &info) != 0 && info.dli_fname) {
        return std::filesystem::path(info.dli_fname).parent_path().string();
    }
    return ".";
}

static std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
        start++;
    }
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        end--;
    }
    return s.substr(start, end - start);
}

static std::unordered_map<std::string, std::string> readBootstrapConfig(const std::string& dir) {
    std::unordered_map<std::string, std::string> cfg;
    const std::string path = (std::filesystem::path(dir) / "payload.properties").string();
    std::ifstream in(path);
    if (!in.is_open()) {
        log("Bootstrap config not found: " + path + " (using fallback entrypoints)");
        return cfg;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (!key.empty()) cfg[key] = value;
    }
    log("Bootstrap config loaded: " + path);
    return cfg;
}

// Lion-style bootstrap path: load a Java agent class from client.jar and invoke start(jarPath, nativePath).
static bool bootstrapJavaAgent(JNIEnv* env) {
    const std::string dir = selfDir();
    const std::string jarPath = (std::filesystem::path(dir) / "client.jar").string();
    const std::string nativePath = (std::filesystem::path(dir) / "client.so").string();
    const auto cfg = readBootstrapConfig(dir);

    if (!std::filesystem::exists(jarPath)) {
        log("Java bootstrap skipped: client.jar not found next to client.so");
        return false;
    }

    jclass urlClass = env->FindClass("java/net/URL");
    if (!urlClass || env->ExceptionCheck()) {
        clearJniException(env, "bootstrap FindClass(URL)");
        return false;
    }
    jmethodID urlCtor = env->GetMethodID(urlClass, "<init>", "(Ljava/lang/String;)V");
    if (!urlCtor || env->ExceptionCheck()) {
        clearJniException(env, "bootstrap URL.<init>");
        return false;
    }

    const std::string uri = "file://" + jarPath;
    jstring jUri = env->NewStringUTF(uri.c_str());
    jobject urlObj = env->NewObject(urlClass, urlCtor, jUri);
    env->DeleteLocalRef(jUri);
    if (!urlObj || env->ExceptionCheck()) {
        clearJniException(env, "bootstrap new URL");
        return false;
    }

    jobjectArray urls = env->NewObjectArray(1, urlClass, urlObj);
    jclass loaderClass = env->FindClass("java/net/URLClassLoader");
    if (!loaderClass || env->ExceptionCheck()) {
        clearJniException(env, "bootstrap FindClass(URLClassLoader)");
        return false;
    }

    jobject parentLoader = nullptr;
    jclass classClass = env->FindClass("java/lang/Class");
    jmethodID getClassLoader = classClass
        ? env->GetMethodID(classClass, "getClassLoader", "()Ljava/lang/ClassLoader;")
        : nullptr;
    jclass mcClass = env->FindClass("ave");
    if (!mcClass || env->ExceptionCheck()) {
        clearJniException(env, "bootstrap FindClass(ave)");
        mcClass = env->FindClass("net/minecraft/client/Minecraft");
    }
    if (mcClass && !env->ExceptionCheck() && getClassLoader) {
        parentLoader = env->CallObjectMethod(mcClass, getClassLoader);
        clearJniException(env, "bootstrap Minecraft.getClassLoader");
    } else {
        clearJniException(env, "bootstrap FindClass(net/minecraft/client/Minecraft)");
    }

    if (!parentLoader) {
        jclass threadClass = env->FindClass("java/lang/Thread");
        jmethodID currentThread = threadClass
            ? env->GetStaticMethodID(threadClass, "currentThread", "()Ljava/lang/Thread;")
            : nullptr;
        jmethodID getContextClassLoaderMid = threadClass
            ? env->GetMethodID(threadClass, "getContextClassLoader", "()Ljava/lang/ClassLoader;")
            : nullptr;
        if (currentThread && getContextClassLoaderMid) {
            jobject threadObj = env->CallStaticObjectMethod(threadClass, currentThread);
            if (threadObj && !env->ExceptionCheck()) {
                parentLoader = env->CallObjectMethod(threadObj, getContextClassLoaderMid);
                clearJniException(env, "bootstrap Thread.getContextClassLoader");
            } else {
                clearJniException(env, "bootstrap Thread.currentThread");
            }
        }
    }

    jmethodID loaderCtor = env->GetMethodID(loaderClass, "<init>", "([Ljava/net/URL;Ljava/lang/ClassLoader;)V");
    jobject loader = env->NewObject(loaderClass, loaderCtor, urls, parentLoader);
    if (!loader || env->ExceptionCheck()) {
        clearJniException(env, "bootstrap new URLClassLoader");
        return false;
    }

    jmethodID loadClass = env->GetMethodID(loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!loadClass || env->ExceptionCheck()) {
        clearJniException(env, "bootstrap URLClassLoader.loadClass");
        return false;
    }

    jclass agentClass = nullptr;
    std::string loadedAgentName;
    std::vector<std::string> candidates;
    const auto customClassIt = cfg.find("entry_class");
    if (customClassIt != cfg.end() && !customClassIt->second.empty()) {
        candidates.push_back(customClassIt->second);
    }
    if (candidates.empty() || candidates[0] != "xtsy.client.Agent") {
        candidates.push_back("xtsy.client.Agent");
    }
    if (candidates.empty() || candidates[0] != "lion.client.Agent") {
        candidates.push_back("lion.client.Agent");
    }

    for (const std::string& candidate : candidates) {
        jstring agentName = env->NewStringUTF(candidate.c_str());
        agentClass = static_cast<jclass>(env->CallObjectMethod(loader, loadClass, agentName));
        env->DeleteLocalRef(agentName);
        if (agentClass && !env->ExceptionCheck()) {
            loadedAgentName = candidate;
            break;
        }
        clearJniException(env, std::string("bootstrap loadClass(") + candidate + ")");
        agentClass = nullptr;
    }

    if (!agentClass) {
        log("Java bootstrap failed: no supported Agent class found");
        return false;
    }

    std::string entryMethod = "start";
    const auto customMethodIt = cfg.find("entry_method");
    if (customMethodIt != cfg.end() && !customMethodIt->second.empty()) {
        entryMethod = customMethodIt->second;
    }

    jmethodID start = env->GetStaticMethodID(agentClass, entryMethod.c_str(), "(Ljava/lang/String;Ljava/lang/String;)V");
    if (!start || env->ExceptionCheck()) {
        clearJniException(env, std::string("bootstrap Agent.") + entryMethod + " lookup");
        return false;
    }

    jstring jJar = env->NewStringUTF(jarPath.c_str());
    jstring jNative = env->NewStringUTF(nativePath.c_str());
    env->CallStaticVoidMethod(agentClass, start, jJar, jNative);
    env->DeleteLocalRef(jJar);
    env->DeleteLocalRef(jNative);
    if (env->ExceptionCheck()) {
        clearJniException(env, std::string("bootstrap Agent.") + entryMethod + " call");
        return false;
    }

    log(std::string("Java bootstrap succeeded via ") + loadedAgentName + "." + entryMethod);
    return true;
}

// Resolves Minecraft.ingameGUI instance using common 1.8.9 names.
static jobject findIngameGui(JNIEnv* env, jobject minecraft, jclass mcClass) {
    for (const char* fieldName : {"q", "ingameGUI", "field_71456_v"}) {
        jfieldID guiField = env->GetFieldID(mcClass, fieldName, "Lavo;");
        if (guiField && !env->ExceptionCheck()) {
            return env->GetObjectField(minecraft, guiField);
        }
        clearJniException(env, std::string("ingameGUI field lookup ") + fieldName);
    }
    return nullptr;
}

// Patches Minecraft.debug, the static string used by the F3 debug header in 1.8.9.
static bool patchDebugString(JNIEnv* env) {
    jclass mcClass = findMinecraftClass(env);
    if (!mcClass) {
        log("F3 marker: Minecraft class not found");
        return false;
    }

    jfieldID debugField = nullptr;
    for (const char* fieldName : {"C", "debug", "field_71426_K"}) {
        debugField = env->GetStaticFieldID(mcClass, fieldName, "Ljava/lang/String;");
        if (debugField && !env->ExceptionCheck()) {
            log("F3 marker: found debug field " + std::string(fieldName));
            break;
        }
        clearJniException(env, std::string("debug field lookup ") + fieldName);
        debugField = nullptr;
    }
    if (!debugField) {
        static bool loggedMissingField = false;
        if (!loggedMissingField) {
            log("F3 marker: could not resolve Minecraft.debug field; disabling repeated attempts");
            loggedMissingField = true;
        }
        return false;
    }

    jobject currentObj = env->GetStaticObjectField(mcClass, debugField);
    std::string current;
    if (currentObj) {
        const char* chars = env->GetStringUTFChars(static_cast<jstring>(currentObj), nullptr);
        if (chars) {
            current = chars;
            env->ReleaseStringUTFChars(static_cast<jstring>(currentObj), chars);
        }
        env->DeleteLocalRef(currentObj);
    }

    const std::string prefix = "[XtsyClient] blue ships beat wind | ";
    if (current.rfind(prefix, 0) == 0) return true;
    jstring replacement = env->NewStringUTF((prefix + current).c_str());
    env->SetStaticObjectField(mcClass, debugField, replacement);
    clearJniException(env, "Set Minecraft.debug");
    env->DeleteLocalRef(replacement);
    log("F3 marker: patched Minecraft.debug");
    return true;
}

// Sends a one-time chat message through GuiNewChat when the obfuscated 1.8.9 path is available.
static void sendChatMarker(JNIEnv* env) {
    jclass mcClass = findMinecraftClass(env);
    if (!mcClass) return;

    jmethodID getMinecraft = env->GetStaticMethodID(mcClass, "A", "()Lave;");
    if (!getMinecraft || env->ExceptionCheck()) {
        clearJniException(env, "Minecraft.A lookup");
        getMinecraft = env->GetStaticMethodID(mcClass, "getMinecraft", "()Lnet/minecraft/client/Minecraft;");
    }
    if (!getMinecraft || env->ExceptionCheck()) {
        clearJniException(env, "Minecraft.getMinecraft lookup");
        return;
    }

    jobject minecraft = env->CallStaticObjectMethod(mcClass, getMinecraft);
    clearJniException(env, "Minecraft instance lookup");
    if (!minecraft) return;

    jobject gui = findIngameGui(env, minecraft, mcClass);
    if (!gui) return;
    jclass guiClass = env->GetObjectClass(gui);
    jmethodID getChat = env->GetMethodID(guiClass, "d", "()Lavt;");
    if (!getChat || env->ExceptionCheck()) {
        clearJniException(env, "GuiIngame.getChatGUI lookup");
        return;
    }

    jobject chat = env->CallObjectMethod(gui, getChat);
    clearJniException(env, "GuiIngame.getChatGUI call");
    if (!chat) return;

    jclass componentClass = findAnyClass(env, {"fa", "net/minecraft/util/ChatComponentText"});
    if (!componentClass) return;
    jmethodID ctor = env->GetMethodID(componentClass, "<init>", "(Ljava/lang/String;)V");
    if (!ctor || env->ExceptionCheck()) {
        clearJniException(env, "ChatComponentText constructor lookup");
        return;
    }

    jstring text = env->NewStringUTF("[XtsyClient] blue ships beat wind");
    jobject component = env->NewObject(componentClass, ctor, text);
    clearJniException(env, "ChatComponentText constructor call");
    env->DeleteLocalRef(text);
    if (!component) return;

    jclass chatClass = env->GetObjectClass(chat);
    jmethodID print = env->GetMethodID(chatClass, "a", "(Leu;)V");
    if (!print || env->ExceptionCheck()) {
        clearJniException(env, "GuiNewChat.printChatMessage lookup");
        return;
    }
    env->CallVoidMethod(chat, print, component);
    clearJniException(env, "GuiNewChat.printChatMessage call");
    log("Chat marker sent");
}

// Prints a marker through Java System.out and native stdout/stderr so Minecraft logs prove the payload loaded.
static void printMinecraftLogMarker(JNIEnv* env) {
    const char* marker = "blue ships beat wind";
    fprintf(stdout, "[XtsyClient] %s\n", marker);
    fprintf(stderr, "[XtsyClient] %s\n", marker);
    fflush(stdout);
    fflush(stderr);

    jclass systemClass = env->FindClass("java/lang/System");
    if (!systemClass || env->ExceptionCheck()) {
        env->ExceptionClear();
        log("Minecraft marker: failed to find java/lang/System");
        return;
    }

    jfieldID outField = env->GetStaticFieldID(systemClass, "out", "Ljava/io/PrintStream;");
    jobject outObject = outField ? env->GetStaticObjectField(systemClass, outField) : nullptr;
    if (!outObject || env->ExceptionCheck()) {
        env->ExceptionClear();
        log("Minecraft marker: failed to access System.out");
        return;
    }

    jclass printStreamClass = env->GetObjectClass(outObject);
    jmethodID printlnMethod = printStreamClass
        ? env->GetMethodID(printStreamClass, "println", "(Ljava/lang/String;)V")
        : nullptr;
    jstring message = env->NewStringUTF(marker);
    if (printlnMethod && message && !env->ExceptionCheck()) {
        env->CallVoidMethod(outObject, printlnMethod, message);
        log("Minecraft marker printed through System.out: " + std::string(marker));
    } else {
        env->ExceptionClear();
        log("Minecraft marker: failed to call PrintStream.println");
    }
    if (message) env->DeleteLocalRef(message);
}

// Locate the running JVM, attach this thread, and initialize all subsystems.
static void initialize() {
    log("initialize() started");

    using GetCreatedJavaVMsFn = jint (*)(JavaVM**, jsize, jsize*);
    auto getCreatedJavaVMs = reinterpret_cast<GetCreatedJavaVMsFn>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
    if (!getCreatedJavaVMs) {
        log("ERROR: dlsym could not resolve JNI_GetCreatedJavaVMs: " + std::string(dlerror() ? dlerror() : "unknown"));
        return;
    }

    JavaVM* jvm = nullptr;
    jsize count = 0;
    if (getCreatedJavaVMs(&jvm, 1, &count) != JNI_OK || count == 0 || jvm == nullptr) {
        log("ERROR: JNI_GetCreatedJavaVMs failed or no JVM found");
        return;
    }
    log("Found running JVM, attaching thread");

    JNIEnv* env = nullptr;
    if (jvm->AttachCurrentThread((void**)&env, nullptr) != JNI_OK || env == nullptr) {
        log("ERROR: AttachCurrentThread failed");
        return;
    }
    log("Thread attached to JVM successfully");

    if (bootstrapJavaAgent(env)) {
        log("Java agent path active; native overlay path will stay minimal");
    }

    printMinecraftLogMarker(env);
    setWindowTitleMarker(env);
    patchDebugString(env);
    sendChatMarker(env);

    InputHook::initialize();
    log("InputHook initialized");

    Menu::initialize(env, jvm);
    log("Menu initialized — client fully loaded");
}

// GCC constructor attribute: called automatically when the .so is loaded into the process.
extern "C" __attribute__((constructor)) void client_entry() {
    log("client_entry() — shared library loaded in PID " + std::to_string(getpid()) +
        " at " + std::to_string(static_cast<long long>(std::time(nullptr))) + ", spawning init thread");
    std::thread(initialize).detach();
}
