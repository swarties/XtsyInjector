// ImageLoader.cpp — Loads PNG images via stb_image and uploads them to OpenGL textures.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "ImageLoader.h"
#include "RuntimePaths.h"
#include <cstdio>
#include <string>

// Simple append-mode logger for the injector.
static void log(const std::string& msg) {
    FILE* f = fopen(RuntimePaths::injectorLogPath().c_str(), "a");
    if (f) { fprintf(f, "%s\n", msg.c_str()); fclose(f); }
}

// Loads a PNG file from disk and uploads it to an OpenGL texture. Returns 0 on failure.
GLuint ImageLoader::loadTexture(const std::string& path, int& outWidth, int& outHeight) {
    int channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &outWidth, &outHeight, &channels, 4); // Force RGBA
    if (!data) {
        log("ImageLoader: failed to load " + path + " — " + stbi_failure_reason());
        return 0;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    // Set texture parameters for clean scaling.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Upload pixel data to the GPU.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, outWidth, outHeight, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, data);

    stbi_image_free(data);
    log("ImageLoader: loaded " + path + " (" + std::to_string(outWidth) + "x" +
        std::to_string(outHeight) + ") as texture " + std::to_string(texture));
    return texture;
}

GLuint ImageLoader::loadTextureFromMemory(const unsigned char* bytes, size_t size, int& outWidth, int& outHeight) {
    if (!bytes || size == 0) {
        log("ImageLoader: loadTextureFromMemory called with empty bytes");
        return 0;
    }
    int channels = 0;
    unsigned char* data = stbi_load_from_memory(bytes, static_cast<int>(size), &outWidth, &outHeight, &channels, 4);
    if (!data) {
        log("ImageLoader: failed to load image from memory — " + std::string(stbi_failure_reason()));
        return 0;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, outWidth, outHeight, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(data);

    log("ImageLoader: loaded embedded image (" + std::to_string(outWidth) + "x" +
        std::to_string(outHeight) + ") as texture " + std::to_string(texture));
    return texture;
}
