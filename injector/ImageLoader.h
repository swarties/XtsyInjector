#pragma once
#include <GL/gl.h>
#include <string>
#include <cstddef>

// Loads a PNG image from disk and uploads it to an OpenGL texture.
namespace ImageLoader {
    // Loads a PNG file and returns an OpenGL texture ID. Returns 0 on failure.
    GLuint loadTexture(const std::string& path, int& outWidth, int& outHeight);
    GLuint loadTextureFromMemory(const unsigned char* bytes, size_t size, int& outWidth, int& outHeight);
}
