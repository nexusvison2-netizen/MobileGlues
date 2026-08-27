// MobileGlues - gl/shader_binary_cache.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// END OF SOURCE FILE HEADER

#ifndef MOBILEGLUES_SHADER_BINARY_CACHE_H
#define MOBILEGLUES_SHADER_BINARY_CACHE_H

#include <unordered_map>
#include <vector>
#include <string>
#include <cstring>
#include <GL/gl.h>
#include <atomic>
#include <mutex>
#include <fstream>

#pragma pack(push, 1)
struct ProgramBinaryHeader {
    static constexpr uint32_t MAGIC = 0xB1AAAA00;
    static constexpr uint32_t VERSION = 1;
    
    uint32_t magic;
    uint32_t version;
    uint32_t binaryFormat;      // GL_IMG_PROGRAM_BINARY or GL_OES_PROGRAM_BINARY
    uint32_t binarySize;
    uint32_t glVersionRequired; // e.g., 32 for ES 3.2
    uint64_t timestamp;
    uint32_t reserved[4];       // Future use
    
    bool IsValid() const {
        return magic == MAGIC && version == VERSION;
    }
};
#pragma pack(pop)

class ShaderBinaryCache {
private:
    std::unordered_map<std::string, std::vector<uint8_t>> g_binary_cache;
    std::unordered_map<std::string, GLenum> g_binary_formats;
    std::unordered_map<GLuint, std::string> g_program_keys;
    std::mutex g_cache_mutex;
    
    std::string g_cache_dir;
    bool g_supports_img_binary = false;
    bool g_supports_oes_binary = false;
    
public:
    ShaderBinaryCache() {
        InitializeCacheDir();
        DetectBinarySupport();
    }
    
    void InitializeCacheDir() {
        // ~/.mobileglues/shader_cache/
        const char* home = getenv("HOME");
        if (!home) home = "/data/local/tmp";
        
        g_cache_dir = std::string(home) + "/.mobileglues/shader_cache";
        
        // Create directory if not exists
        mkdir(g_cache_dir.c_str(), 0755);
    }
    
    void DetectBinarySupport() {
        const char* exts = (const char*)glGetString(GL_EXTENSIONS);
        if (exts) {
            std::string extensions(exts);
            g_supports_img_binary = (extensions.find("GL_IMG_program_binary") != std::string::npos);
            g_supports_oes_binary = (extensions.find("GL_OES_get_program_binary") != std::string::npos);
        }
        
        if (!g_supports_img_binary && !g_supports_oes_binary) {
            LOG_W("No program binary support detected! Shaders will recompile each run.")
        } else if (g_supports_img_binary) {
            LOG_D("GL_IMG_program_binary supported (PowerVR optimized)")
        } else {
            LOG_D("GL_OES_get_program_binary supported (OpenGL ES standard)")
        }
    }
    
    // Generate cache key from shader source
    std::string GenerateCacheKey(const std::string& vertex_src, const std::string& fragment_src) {
        // Simple hash based on source code
        // In production, use SHA256, but for speed use CRC32
        uint32_t hash = 5381;
        
        for (char c : vertex_src) {
            hash = ((hash << 5) + hash) ^ c;
        }
        for (char c : fragment_src) {
            hash = ((hash << 5) + hash) ^ c;
        }
        
        char key[64];
        snprintf(key, sizeof(key), "shader_%08x", hash);
        return std::string(key);
    }
    
    // Save program binary to disk + memory
    bool CacheProgram(GLuint program, const std::string& cache_key) {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        
        if (!g_supports_img_binary && !g_supports_oes_binary) {
            return false;
        }
        
        GLint binaryLength = 0;
        glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &binaryLength);
        
        if (binaryLength <= 0) {
            LOG_E("Program %u has no binary data", program)
            return false;
        }
        
        // Allocate space for header + binary
        std::vector<uint8_t> cached_data(sizeof(ProgramBinaryHeader) + binaryLength);
        
        // Write header
        ProgramBinaryHeader* header = (ProgramBinaryHeader*)cached_data.data();
        header->magic = ProgramBinaryHeader::MAGIC;
        header->version = ProgramBinaryHeader::VERSION;
        header->binarySize = binaryLength;
        
        // Determine binary format (prefer IMG for PowerVR)
        GLenum format = GL_PROGRAM_BINARY_FORMAT_QCOM; // Fallback
        if (g_supports_img_binary) {
            format = GL_PROGRAM_BINARY_IMG; // 0x8C40 PowerVR format
        } else if (g_supports_oes_binary) {
            format = GL_PROGRAM_BINARY_FORMAT_QCOM;
        }
        
        header->binaryFormat = format;
        header->glVersionRequired = 32; // ES 3.2
        header->timestamp = std::time(nullptr);
        
        // Extract binary
        uint8_t* binary_ptr = cached_data.data() + sizeof(ProgramBinaryHeader);
        glGetProgramBinary(program, binaryLength, nullptr, &format, binary_ptr);
        
        // Store in memory cache
        g_binary_cache[cache_key] = cached_data;
        g_binary_formats[cache_key] = format;
        g_program_keys[program] = cache_key;
        
        // Write to disk
        std::string filepath = g_cache_dir + "/" + cache_key + ".bin";
        std::ofstream file(filepath, std::ios::binary);
        if (file.is_open()) {
            file.write((const char*)cached_data.data(), cached_data.size());
            file.close();
            LOG_D("Cached program binary to: %s (%d bytes)", filepath.c_str(), binaryLength)
            return true;
        } else {
            LOG_W("Failed to write cache file: %s", filepath.c_str())
            return false;
        }
    }
    
    // Load program binary from disk/memory (ZERO compilation!)
    bool LoadCachedProgram(GLuint program, const std::string& cache_key) {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        
        if (!g_supports_img_binary && !g_supports_oes_binary) {
            return false;
        }
        
        std::vector<uint8_t>* cached_data = nullptr;
        
        // Try memory cache first
        auto it = g_binary_cache.find(cache_key);
        if (it != g_binary_cache.end()) {
            cached_data = &it->second;
            LOG_D("Loaded program binary from memory cache")
        } else {
            // Try disk cache
            std::string filepath = g_cache_dir + "/" + cache_key + ".bin";
            std::ifstream file(filepath, std::ios::binary);
            
            if (!file.is_open()) {
                LOG_D("No cached binary for key: %s", cache_key.c_str())
                return false;
            }
            
            // Read entire file
            file.seekg(0, std::ios::end);
            size_t file_size = file.tellg();
            file.seekg(0, std::ios::beg);
            
            std::vector<uint8_t> file_data(file_size);
            file.read((char*)file_data.data(), file_size);
            file.close();
            
            // Validate header
            if (file_size < sizeof(ProgramBinaryHeader)) {
                LOG_E("Cache file corrupted (too small): %s", filepath.c_str())
                return false;
            }
            
            ProgramBinaryHeader* header = (ProgramBinaryHeader*)file_data.data();
            if (!header->IsValid()) {
                LOG_E("Invalid cache file header: %s", filepath.c_str())
                return false;
            }
            
            // Store in memory cache
            g_binary_cache[cache_key] = file_data;
            g_binary_formats[cache_key] = header->binaryFormat;
            cached_data = &g_binary_cache[cache_key];
            
            LOG_D("Loaded program binary from disk: %s (%u bytes)", filepath.c_str(), header->binarySize)
        }
        
        // Load binary into program
        if (!cached_data || cached_data->size() < sizeof(ProgramBinaryHeader)) {
            LOG_E("Invalid cached data")
            return false;
        }
        
        ProgramBinaryHeader* header = (ProgramBinaryHeader*)cached_data->data();
        uint8_t* binary_ptr = cached_data->data() + sizeof(ProgramBinaryHeader);
        GLenum format = header->binaryFormat;
        
        glProgramBinary(program, format, binary_ptr, header->binarySize);
        
        // Verify program linkage
        GLint link_status = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &link_status);
        
        if (link_status == GL_TRUE) {
            g_program_keys[program] = cache_key;
            LOG_D("Program %u loaded from binary cache successfully", program)
            return true;
        } else {
            GLchar infoLog[512];
            glGetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
            LOG_E("Program %u binary link failed: %s", program, infoLog)
            return false;
        }
    }
    
    // Check if cached binary exists
    bool HasCachedBinary(const std::string& cache_key) {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        
        if (g_binary_cache.find(cache_key) != g_binary_cache.end()) {
            return true;
        }
        
        std::string filepath = g_cache_dir + "/" + cache_key + ".bin";
        std::ifstream file(filepath);
        return file.good();
    }
    
    // Clear all caches (for development/testing)
    void ClearCache() {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_binary_cache.clear();
        g_binary_formats.clear();
        g_program_keys.clear();
    }
};

extern ShaderBinaryCache g_shader_binary_cache;

#endif // MOBILEGLUES_SHADER_BINARY_CACHE_H
