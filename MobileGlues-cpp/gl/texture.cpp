// MobileGlues - gl/texture.cpp
// Copyright (c) 2025 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1
// SPDX-License-Identifier: LGPL-2.1-only

#include "texture.h"
#include "../config/settings.h"
#include "../egl/context.h"
#include <mutex>
#include <memory>
#include <unordered_map>
#include "GLES3/gl32.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif
#ifndef __APPLE__
#include <malloc.h>
#endif

#include "../gles/gles.h"
#include "../gles/loader.h"
#include "framebuffer.h"
#include "log.h"
#include "transfer.h"
#include "pixel.h"
#include "buffer.h"
#include "mg.h"
#include <GL/gl.h>

#define DEBUG 0

#define TX_WARN_ONCE(...)                                                                                              \
    do {                                                                                                               \
        static bool mg_tx_warned = false;                                                                              \
        if (!mg_tx_warned) {                                                                                           \
            mg_tx_warned = true;                                                                                       \
            LOG_W_FORCE(__VA_ARGS__)                                                                                   \
        }                                                                                                              \
    } while (0)

int nlevel(int size, int level) {
    if (size) {
        size >>= level;
        if (!size) size = 1;
    }
    return size;
}

// ============================================================================
// GL_EXT_texture_buffer - RESOLUCAO
// ============================================================================
typedef void(GLAPIENTRY* mg_pfn_tex_buffer_ext)(GLenum, GLenum, GLuint);
typedef void(GLAPIENTRY* mg_pfn_tex_buffer_range_ext)(GLenum, GLenum, GLuint, GLintptr, GLsizeiptr);

static mg_pfn_tex_buffer_ext g_tex_buffer_ext = nullptr;
static mg_pfn_tex_buffer_range_ext g_tex_buffer_range_ext = nullptr;
static bool g_texture_buffer_resolved = false;
static bool g_texture_buffer_available = false;

extern "C" void* gles;

static bool mg_gles_has_extension(const char* name) {
    if (!GLES.glGetStringi || !GLES.glGetIntegerv) return false;
    GLint count = 0;
    GLES.glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const GLubyte* s = GLES.glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
        if (s && strcmp(reinterpret_cast<const char*>(s), name) == 0) return true;
    }
    return false;
}

static bool mg_texture_buffer_ext_available() {
    if (g_texture_buffer_resolved) return g_texture_buffer_available;
    g_texture_buffer_resolved = true;
    if (mg_gles_has_extension("GL_EXT_texture_buffer")) {
        if (gles && gles != reinterpret_cast<void*>(~(uintptr_t)0)) {
            g_tex_buffer_ext = reinterpret_cast<mg_pfn_tex_buffer_ext>(dlsym(gles, "glTexBufferEXT"));
            g_tex_buffer_range_ext = reinterpret_cast<mg_pfn_tex_buffer_range_ext>(dlsym(gles, "glTexBufferRangeEXT"));
            g_texture_buffer_available = (g_tex_buffer_ext != nullptr);
        }
    }
    return g_texture_buffer_available;
}

// ============================================================================
// TEXTURE OBJECT MANAGEMENT (ORIGINAL)
// ============================================================================

GLenum ConvertTextureTargetToGLEnum(TextureTarget target) {
    switch (target) {
    case TextureTarget::TEXTURE_1D: return GL_TEXTURE_1D;
    case TextureTarget::PROXY_TEXTURE_1D: return GL_PROXY_TEXTURE_1D;
    case TextureTarget::TEXTURE_1D_ARRAY: return GL_TEXTURE_1D_ARRAY;
    case TextureTarget::PROXY_TEXTURE_1D_ARRAY: return GL_PROXY_TEXTURE_1D_ARRAY;
    case TextureTarget::TEXTURE_2D: return GL_TEXTURE_2D;
    case TextureTarget::PROXY_TEXTURE_2D: return GL_PROXY_TEXTURE_2D;
    case TextureTarget::TEXTURE_2D_ARRAY: return GL_TEXTURE_2D_ARRAY;
    case TextureTarget::PROXY_TEXTURE_2D_ARRAY: return GL_PROXY_TEXTURE_2D_ARRAY;
    case TextureTarget::TEXTURE_2D_MULTISAMPLE: return GL_TEXTURE_2D_MULTISAMPLE;
    case TextureTarget::PROXY_TEXTURE_2D_MULTISAMPLE: return GL_PROXY_TEXTURE_2D_MULTISAMPLE;
    case TextureTarget::TEXTURE_2D_MULTISAMPLE_ARRAY: return GL_TEXTURE_2D_MULTISAMPLE_ARRAY;
    case TextureTarget::PROXY_TEXTURE_2D_MULTISAMPLE_ARRAY: return GL_PROXY_TEXTURE_2D_MULTISAMPLE_ARRAY;
    case TextureTarget::TEXTURE_3D: return GL_TEXTURE_3D;
    case TextureTarget::PROXY_TEXTURE_3D: return GL_PROXY_TEXTURE_3D;
    case TextureTarget::TEXTURE_RECTANGLE: return GL_TEXTURE_RECTANGLE;
    case TextureTarget::PROXY_TEXTURE_RECTANGLE: return GL_PROXY_TEXTURE_RECTANGLE;
    case TextureTarget::TEXTURE_CUBE_MAP: return GL_TEXTURE_CUBE_MAP;
    case TextureTarget::PROXY_TEXTURE_CUBE_MAP: return GL_PROXY_TEXTURE_CUBE_MAP;
    case TextureTarget::TEXTURE_CUBE_MAP_ARRAY: return GL_TEXTURE_CUBE_MAP_ARRAY;
    case TextureTarget::PROXY_TEXTURE_CUBE_MAP_ARRAY: return GL_PROXY_TEXTURE_CUBE_MAP_ARRAY;
    case TextureTarget::TEXTURE_BUFFER: return GL_TEXTURE_BUFFER;
    default: return GL_TEXTURE_2D;
    }
}

TextureTarget ConvertGLEnumToTextureTarget(GLenum target) {
    switch (target) {
    case GL_TEXTURE_1D: return TextureTarget::TEXTURE_1D;
    case GL_PROXY_TEXTURE_1D: return TextureTarget::PROXY_TEXTURE_1D;
    case GL_TEXTURE_1D_ARRAY: return TextureTarget::TEXTURE_1D_ARRAY;
    case GL_PROXY_TEXTURE_1D_ARRAY: return TextureTarget::PROXY_TEXTURE_1D_ARRAY;
    case GL_TEXTURE_2D: return TextureTarget::TEXTURE_2D;
    case GL_PROXY_TEXTURE_2D: return TextureTarget::PROXY_TEXTURE_2D;
    case GL_TEXTURE_2D_ARRAY: return TextureTarget::TEXTURE_2D_ARRAY;
    case GL_PROXY_TEXTURE_2D_ARRAY: return TextureTarget::PROXY_TEXTURE_2D_ARRAY;
    case GL_TEXTURE_2D_MULTISAMPLE: return TextureTarget::TEXTURE_2D_MULTISAMPLE;
    case GL_PROXY_TEXTURE_2D_MULTISAMPLE: return TextureTarget::PROXY_TEXTURE_2D_MULTISAMPLE;
    case GL_TEXTURE_2D_MULTISAMPLE_ARRAY: return TextureTarget::TEXTURE_2D_MULTISAMPLE_ARRAY;
    case GL_PROXY_TEXTURE_2D_MULTISAMPLE_ARRAY: return TextureTarget::PROXY_TEXTURE_2D_MULTISAMPLE_ARRAY;
    case GL_TEXTURE_3D: return TextureTarget::TEXTURE_3D;
    case GL_PROXY_TEXTURE_3D: return TextureTarget::PROXY_TEXTURE_3D;
    case GL_TEXTURE_RECTANGLE: return TextureTarget::TEXTURE_RECTANGLE;
    case GL_PROXY_TEXTURE_RECTANGLE: return TextureTarget::PROXY_TEXTURE_RECTANGLE;
    case GL_PROXY_TEXTURE_CUBE_MAP: return TextureTarget::PROXY_TEXTURE_CUBE_MAP;
    case GL_TEXTURE_CUBE_MAP_POSITIVE_X: case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Y: case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Z: case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
    case GL_TEXTURE_CUBE_MAP: return TextureTarget::TEXTURE_CUBE_MAP;
    case GL_TEXTURE_CUBE_MAP_ARRAY: return TextureTarget::TEXTURE_CUBE_MAP_ARRAY;
    case GL_PROXY_TEXTURE_CUBE_MAP_ARRAY: return TextureTarget::PROXY_TEXTURE_CUBE_MAP_ARRAY;
    case GL_TEXTURE_BUFFER: return TextureTarget::TEXTURE_BUFFER;
    default: return TextureTarget::UNKNWON;
    }
}

const int MAX_TEXTURE_IMAGE_UNITS = 128;

class TextureBindingSlot {
public:
    using TargetEnum = TextureTarget;
    TextureBindingSlot() : m_target((TargetEnum)0), m_boundObject(nullptr) {}
    explicit TextureBindingSlot(TargetEnum target) : m_target(target), m_boundObject(nullptr) {}
    void Bind(TextureObject* object) { m_boundObject = object; }
    TextureObject* GetBoundObject() const { return m_boundObject; }
    TargetEnum GetTarget() const { return m_target; }
private:
    TargetEnum m_target;
    TextureObject* m_boundObject;
};

class TextureUnit {
public:
    TextureBindingSlot& GetBindingSlot(TextureBindingSlot::TargetEnum target) { return m_slots[(int)target]; }
private:
    std::array<TextureBindingSlot, (int)TextureTarget::TEXTURES_COUNT> m_slots;
};

namespace {
struct texture_group_state_t {
    std::vector<TextureObject*> objects;
};
struct texture_ctx_state_t {
    std::array<TextureUnit, MAX_TEXTURE_IMAGE_UNITS> units;
    int current_unit = 0;
    std::array<std::array<GLuint, (int)TextureTarget::TEXTURES_COUNT>, MAX_TEXTURE_IMAGE_UNITS> driver_bindings{};
    int driver_active_unit = 0;
    unsigned long long group = 0;
};

std::mutex g_tex_mutex;
std::unordered_map<unsigned long long, std::unique_ptr<texture_group_state_t>> g_tex_groups;
std::unordered_map<unsigned long long, std::unique_ptr<texture_ctx_state_t>> g_tex_ctxs;
texture_group_state_t g_tex_group_default;
texture_ctx_state_t g_tex_ctx_default;
thread_local texture_group_state_t* g_tg = &g_tex_group_default;
thread_local texture_ctx_state_t* g_tc = &g_tex_ctx_default;
} // namespace

void mg_texture_bind_context(unsigned long long ctx_id, unsigned long long group_id) {
    if (ctx_id == 0) {
        g_tg = &g_tex_group_default;
        g_tc = &g_tex_ctx_default;
        return;
    }
    std::lock_guard<std::mutex> lock(g_tex_mutex);
    std::unique_ptr<texture_group_state_t>& group = g_tex_groups[group_id];
    if (!group) group = std::make_unique<texture_group_state_t>();
    std::unique_ptr<texture_ctx_state_t>& ctx = g_tex_ctxs[ctx_id];
    if (!ctx) ctx = std::make_unique<texture_ctx_state_t>();
    g_tg = group.get();
    g_tc = ctx.get();
    g_tc->group = group_id;
}

void mg_texture_forget_context(unsigned long long ctx_id) {
    if (ctx_id == 0) return;
    std::lock_guard<std::mutex> lock(g_tex_mutex);
    const auto it = g_tex_ctxs.find(ctx_id);
    if (it == g_tex_ctxs.end()) return;
    if (g_tc == it->second.get()) g_tc = &g_tex_ctx_default;
    g_tex_ctxs.erase(it);
}

#define BufferObjectsVec (g_tg->objects)
#define TextureUnits (g_tc->units)
#define CurrentTextureUnitIndex (g_tc->current_unit)
#define DriverTextureBindings (g_tc->driver_bindings)
#define DriverActiveTextureUnit (g_tc->driver_active_unit)

static const int MG_TEXTURE_BUFFER_EMULATION_UNIT = 15;

static inline bool driver_binding_key_valid(int unit, TextureTarget target) {
    return unit >= 0 && unit < MAX_TEXTURE_IMAGE_UNITS && (int)target >= 0 &&
           (int)target < (int)TextureTarget::TEXTURES_COUNT;
}

static inline void set_driver_texture_binding(int unit, TextureTarget target, GLuint texture) {
    if (driver_binding_key_valid(unit, target)) DriverTextureBindings[unit][(int)target] = texture;
}

static inline GLuint get_driver_texture_binding(int unit, TextureTarget target) {
    return driver_binding_key_valid(unit, target) ? DriverTextureBindings[unit][(int)target] : 0;
}

void InitTextureMap(size_t expectedSize) {
    BufferObjectsVec.reserve(expectedSize);
}

TextureObject* GetOrCreateTextureObject(GLuint index) {
    if (index >= BufferObjectsVec.size()) BufferObjectsVec.resize(index + 100, nullptr);
    auto& obj = BufferObjectsVec[index];
    if (!obj) {
        obj = new TextureObject();
        obj->texture = index;
    }
    return obj;
}

void ActivateTextureUnit(int unit) {
    if (unit < 0 || unit >= MAX_TEXTURE_IMAGE_UNITS) { LOG_E("Invalid texture unit: %d", unit); return; }
    CurrentTextureUnitIndex = unit;
}

int GetCurrentTextureUnitIndex() { return CurrentTextureUnitIndex; }

TextureUnit& GetTextureUnit(int unit) {
    if (unit < 0 || unit >= MAX_TEXTURE_IMAGE_UNITS) { LOG_E("Invalid texture unit: %d", unit); return TextureUnits[0]; }
    return TextureUnits[unit];
}

void MarkTextureObjectForDeletion(unsigned texture) {
    if (texture == 0) return;
    if (texture >= BufferObjectsVec.size() || !BufferObjectsVec[texture]) {
        LOG_D("Texture %u not found in BufferObjectsVec!", texture);
        return;
    }
    auto textureObject = BufferObjectsVec[texture];
    auto sweep = [&](texture_ctx_state_t& ctx) {
        for (auto& unit : ctx.units) {
            for (int t = 0; t < (int)TextureTarget::TEXTURES_COUNT; ++t) {
                auto& slot = unit.GetBindingSlot((TextureBindingSlot::TargetEnum)t);
                if (slot.GetBoundObject() == textureObject) slot.Bind(nullptr);
            }
        }
    };
    sweep(*g_tc);
    {
        std::lock_guard<std::mutex> lock(g_tex_mutex);
        const unsigned long long group = g_tc->group;
        for (const auto& entry : g_tex_ctxs) {
            if (entry.second.get() != g_tc && entry.second->group == group) sweep(*entry.second);
        }
    }
    if (g_tc != &g_tex_ctx_default) sweep(g_tex_ctx_default);
    BufferObjectsVec[texture] = nullptr;
    delete textureObject;
}

int mg_max_texture_units(void) { return MAX_TEXTURE_IMAGE_UNITS; }

static inline bool driver_shadow_tracks_this_context() { return g_tc != &g_tex_ctx_default; }
static inline bool driver_active_unit_shadow_trustworthy() { return driver_shadow_tracks_this_context(); }
static inline bool driver_texture_shadow_trustworthy() {
    return driver_shadow_tracks_this_context() && global_settings.fsr1_setting == FSR1_Quality_Preset::Disabled;
}

int mg_driver_active_texture_unit(void) {
    if (driver_active_unit_shadow_trustworthy()) return DriverActiveTextureUnit;
    GLint queried = GL_TEXTURE0;
    GLES.glGetIntegerv(GL_ACTIVE_TEXTURE, &queried);
    const int unit = (int)(queried - GL_TEXTURE0);
    return (unit >= 0 && unit < MAX_TEXTURE_IMAGE_UNITS) ? unit : 0;
}

bool mg_driver_texture_binding_at_unit(int unit, GLenum target, GLuint* out) {
    const TextureTarget targetR = ConvertGLEnumToTextureTarget(target);
    if (!out || !driver_texture_shadow_trustworthy() || !driver_binding_key_valid(unit, targetR)) return false;
    *out = get_driver_texture_binding(unit, targetR);
    return true;
}

bool mg_driver_texture_binding(GLenum target, GLuint* out) {
    return mg_driver_texture_binding_at_unit(DriverActiveTextureUnit, target, out);
}

TextureObject* mgGetTexObjectByTarget(GLenum target) {
    return GetTextureUnit(GetCurrentTextureUnitIndex()).GetBindingSlot(ConvertGLEnumToTextureTarget(target)).GetBoundObject();
}

TextureObject* mgGetTexObjectByID(unsigned texture) {
    if (texture >= BufferObjectsVec.size() || !BufferObjectsVec[texture]) {
        LOG_E("Texture %u not found in BufferObjectsVec!", texture);
        return nullptr;
    }
    return BufferObjectsVec[texture];
}

// ============================================================================
// glTexBuffer OTIMIZADO COM GL_EXT_texture_buffer
// ============================================================================

void glTexBuffer(GLenum target, GLenum internalformat, GLuint buffer) {
    LOG()
    LOG_D("glTexBuffer, target = %s, internalformat = %s, buffer = %d",
          glEnumToString(target), glEnumToString(internalformat), buffer)
    if (target != GL_TEXTURE_BUFFER) return;

    if (!has_buffer(buffer) || buffer == 0) {
        GLES.glTexBuffer(target, internalformat, buffer);
        CHECK_GL_ERROR
        return;
    }
    GLuint real_buffer = find_real_buffer(buffer);
    if (!real_buffer) {
        GLES.glGenBuffers(1, &real_buffer);
        modify_buffer(buffer, real_buffer);
        CHECK_GL_ERROR
    }

    // PRIORIDADE 1: Usar GL_EXT_texture_buffer nativo
    if (mg_texture_buffer_ext_available() && g_tex_buffer_ext) {
        g_tex_buffer_ext(target, internalformat, real_buffer);
        CHECK_GL_ERROR
        return;
    }

    // FALLBACK: emulacao via 2D texture (codigo original)
    if (hardware->emulate_texture_buffer) {
        LOG_D("Emulating glTexBuffer");
        GLuint pixelSize = get_internal_format_size(internalformat);
        if (pixelSize == 0) {
            TX_WARN_ONCE("glTexBuffer: no texel size known for internalformat %s",
                         glEnumToString(internalformat));
            mg_set_gl_error(GL_INVALID_ENUM);
            return;
        }

        GLenum tb_format = GL_RED_INTEGER, tb_type = GL_BYTE;
        if (!get_internal_format_transfer(internalformat, &tb_format, &tb_type)) {
            TX_WARN_ONCE("glTexBuffer: no GLES transfer pair for internalformat %s",
                         glEnumToString(internalformat));
            mg_set_gl_error(GL_INVALID_ENUM);
            return;
        }

        GLint boundTexture = 0;
        GLint prev_pixel_buffer_binding = 0;

        GLES.glActiveTexture(GL_TEXTURE0 + 15);
        GLES.glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundTexture);
        GLES.glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prev_pixel_buffer_binding);

        if (!boundTexture) {
            GLES.glActiveTexture(GL_TEXTURE0 + gl_state->current_tex_unit);
            return;
        }

        GLES.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, real_buffer);
        GLint bufferSize;
        GLES.glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &bufferSize);
        GLES.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        GLES.glBindTexture(GL_TEXTURE_2D, boundTexture);

        const GLuint MAX_WIDTH = 8192;
        GLuint numElements = bufferSize / pixelSize;
        if (numElements == 0) {
            TX_WARN_ONCE("glTexBuffer: buffer of %d bytes holds no %u-byte texel", bufferSize, pixelSize);
            mg_set_gl_error(GL_INVALID_VALUE);
            GLES.glActiveTexture(GL_TEXTURE0 + gl_state->current_tex_unit);
            return;
        }

        GLuint width = numElements;
        GLuint height = 1;
        if (width > MAX_WIDTH) {
            width = MAX_WIDTH;
            height = (numElements + MAX_WIDTH - 1) / MAX_WIDTH;
        }

        GLint prev_alignment, prev_row_length, prev_skip_pixels, prev_skip_rows;
        GLES.glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_alignment);
        GLES.glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_row_length);
        GLES.glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &prev_skip_pixels);
        GLES.glGetIntegerv(GL_UNPACK_SKIP_ROWS, &prev_skip_rows);
        GLES.glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        GLES.glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

        GLES.glTexImage2D(GL_TEXTURE_2D, 0, internalformat, width, height, 0, tb_format, tb_type, nullptr);
        GLES.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, real_buffer);

        for (GLuint row = 0; row < height; ++row) {
            const GLuint row_texels = (row + 1 == height) ? (numElements - row * width) : width;
            if (row_texels == 0) break;
            void* offset = (void*)(static_cast<size_t>(row) * width * pixelSize);
            GLES.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, row_texels, 1, tb_format, tb_type, offset);
        }

        GLES.glPixelStorei(GL_UNPACK_ALIGNMENT, prev_alignment);
        GLES.glPixelStorei(GL_UNPACK_ROW_LENGTH, prev_row_length);
        GLES.glPixelStorei(GL_UNPACK_SKIP_PIXELS, prev_skip_pixels);
        GLES.glPixelStorei(GL_UNPACK_SKIP_ROWS, prev_skip_rows);

        auto tex = mgGetTexObjectByTarget(target);
        tex->target = ConvertGLEnumToTextureTarget(target);
        tex->internal_format = internalformat;
        tex->width = width;
        tex->height = height;
        tex->depth = 1;
        tex->swizzle_param[0] = GL_RED;
        tex->swizzle_param[1] = GL_GREEN;
        tex->swizzle_param[2] = GL_BLUE;
        tex->swizzle_param[3] = GL_ALPHA;

        GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

        GLES.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, prev_pixel_buffer_binding);
        GLES.glActiveTexture(GL_TEXTURE0 + gl_state->current_tex_unit);
        CHECK_GL_ERROR;
        return;
    }

    GLES.glTexBuffer(target, internalformat, real_buffer);
    CHECK_GL_ERROR
}

void glTexBufferRange(GLenum target, GLenum internalformat, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    LOG()
    LOG_D("glTexBufferRange, target = %s, internalformat = %s, buffer = %d, offset = %p, size = %zi",
          glEnumToString(target), glEnumToString(internalformat), buffer, (void*)offset, size)

    // PRIORIDADE: Usar extensao nativa se disponivel
    if (mg_texture_buffer_ext_available() && g_tex_buffer_range_ext) {
        if (!has_buffer(buffer) || buffer == 0) {
            g_tex_buffer_range_ext(target, internalformat, buffer, offset, size);
        } else {
            GLuint real_buffer = find_real_buffer(buffer);
            if (!real_buffer) {
                GLES.glGenBuffers(1, &real_buffer);
                modify_buffer(buffer, real_buffer);
            }
            g_tex_buffer_range_ext(target, internalformat, real_buffer, offset, size);
        }
        CHECK_GL_ERROR
        return;
    }

    if (!has_buffer(buffer) || buffer == 0) {
        GLES.glTexBufferRange(target, internalformat, buffer, offset, size);
        CHECK_GL_ERROR
        return;
    }
    GLuint real_buffer = find_real_buffer(buffer);
    if (!real_buffer) {
        GLES.glGenBuffers(1, &real_buffer);
        modify_buffer(buffer, real_buffer);
        CHECK_GL_ERROR
    }
    GLES.glTexBufferRange(target, internalformat, real_buffer, offset, size);
    CHECK_GL_ERROR
}

// ============================================================================
// RESTO DAS FUNCOES (mantidas do original)
// ============================================================================

void internal_convert(GLenum* internal_format, GLenum* type, GLenum* format, bool has_data) {
    switch (*internal_format) {
    case GL_DEPTH_COMPONENT16: if (type) *type = GL_UNSIGNED_SHORT; break;
    case GL_DEPTH_COMPONENT24: if (type) *type = GL_UNSIGNED_INT; break;
    case GL_DEPTH_COMPONENT32: *internal_format = GL_DEPTH_COMPONENT24; if (type) *type = GL_UNSIGNED_INT; break;
    case GL_DEPTH_COMPONENT32F: if (type) *type = GL_FLOAT; break;
    case GL_DEPTH_COMPONENT:
        if (type && has_data) {
            if (*type == GL_FLOAT) *internal_format = GL_DEPTH_COMPONENT32F;
            else { *internal_format = GL_DEPTH_COMPONENT; *type = GL_UNSIGNED_INT; }
        } else if (type) {
            *internal_format = GL_DEPTH_COMPONENT; *type = GL_UNSIGNED_INT;
        } else {
            *internal_format = GL_DEPTH_COMPONENT24;
        }
        break;
    case GL_DEPTH_STENCIL:
        if (type && *type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV) *internal_format = GL_DEPTH32F_STENCIL8;
        else { *internal_format = GL_DEPTH24_STENCIL8; if (type) *type = GL_UNSIGNED_INT_24_8; }
        break;
    case GL_RGB10_A2: if (type) *type = GL_UNSIGNED_INT_2_10_10_10_REV; break;
    case GL_RGB5_A1: if (type) *type = GL_UNSIGNED_SHORT_5_5_5_1; break;
    case GL_COMPRESSED_RED_RGTC1: case GL_COMPRESSED_RG_RGTC2:
        LOG_E("GL_COMPRESSED_RED_RGTC1 or GL_COMPRESSED_RG_RGTC2 is not supported!"); break;
    case GL_SRGB8: if (type) *type = GL_UNSIGNED_BYTE; break;
    case GL_RGBA32F: case GL_RGB32F: if (type) *type = GL_FLOAT; break;
    case GL_RGB9_E5: if (type) *type = GL_UNSIGNED_INT_5_9_9_9_REV; break;
    case GL_R11F_G11F_B10F:
        if (type) *type = GL_UNSIGNED_INT_10F_11F_11F_REV;
        if (format) *format = GL_RGB;
        break;
    case GL_RGBA32UI: case GL_RGB32UI: if (type) *type = GL_UNSIGNED_INT; break;
    case GL_RGBA32I: case GL_RGB32I: if (type) *type = GL_INT; break;
    case GL_RGBA16:
        if (g_gles_caps.GL_EXT_texture_norm16) { if (type) *type = GL_UNSIGNED_SHORT; }
        else { *internal_format = GL_RGBA16F; if (type) *type = GL_FLOAT; }
        break;
    case GL_RGBA8: case GL_RGBA: if (type) *type = GL_UNSIGNED_BYTE; if (format) *format = GL_RGBA; break;
    case GL_RGBA16F: if (type) *type = GL_HALF_FLOAT; break;
    case GL_R16:
        if (g_gles_caps.GL_EXT_texture_norm16) { if (type) *type = GL_UNSIGNED_SHORT; }
        else { *internal_format = GL_R16F; if (type) *type = GL_FLOAT; }
        if (format) *format = GL_RED;
        break;
    case GL_RGB16:
        if (g_gles_caps.GL_EXT_texture_norm16) { if (type) *type = GL_UNSIGNED_SHORT; }
        else { *internal_format = GL_RGB16F; if (type) *type = GL_HALF_FLOAT; }
        if (format) *format = GL_RGB;
        break;
    case GL_RGB16F: if (type) *type = GL_HALF_FLOAT; if (format) *format = GL_RGB; break;
    case GL_RG16:
        if (g_gles_caps.GL_EXT_texture_norm16) { if (type) *type = GL_UNSIGNED_SHORT; }
        else { *internal_format = GL_RG16F; if (type) *type = GL_HALF_FLOAT; }
        if (format) *format = GL_RG;
        break;
    case GL_R8: if (format) *format = GL_RED; if (type) *type = GL_UNSIGNED_BYTE; break;
    case GL_R8_SNORM: if (format) *format = GL_RED; if (type) *type = GL_BYTE; break;
    case GL_R16F: if (format) *format = GL_RED; if (type) *type = GL_HALF_FLOAT; break;
    case GL_RED:
        if (type) {
            switch (*type) {
            case GL_UNSIGNED_BYTE: *internal_format = GL_R8; if (format) *format = GL_RED; break;
            case GL_BYTE: *internal_format = GL_R8_SNORM; if (format) *format = GL_RED; break;
            case GL_HALF_FLOAT: *internal_format = GL_R16F; if (format) *format = GL_RED; break;
            case GL_FLOAT: *internal_format = GL_R32F; if (format) *format = GL_RED; break;
            default:
                LOG_E("Unsupported type for GL_RED: %s", glEnumToString(*type));
                if (type) *type = GL_UNSIGNED_BYTE;
                *internal_format = GL_R8;
                if (format) *format = GL_RED;
                break;
            }
        }
        break;
    case GL_R8UI: if (format) *format = GL_RED_INTEGER; if (type) *type = GL_UNSIGNED_BYTE; break;
    case GL_R8I: if (format) *format = GL_RED_INTEGER; if (type) *type = GL_BYTE; break;
    case GL_R16UI: if (format) *format = GL_RED_INTEGER; if (type) *type = GL_UNSIGNED_SHORT; break;
    case GL_R16I: if (format) *format = GL_RED_INTEGER; if (type) *type = GL_SHORT; break;
    case GL_R32UI: if (format) *format = GL_RED_INTEGER; if (type) *type = GL_UNSIGNED_INT; break;
    case GL_R32I: if (format) *format = GL_RED_INTEGER; if (type) *type = GL_INT; break;
    case GL_RG8: if (format) *format = GL_RG; if (type) *type = GL_UNSIGNED_BYTE; break;
    case GL_RG8_SNORM: if (format) *format = GL_RG; if (type) *type = GL_BYTE; break;
    case GL_RG16F: if (format) *format = GL_RG; if (type) *type = GL_HALF_FLOAT; break;
    case GL_RG32F: if (format) *format = GL_RG; if (type) *type = GL_FLOAT; break;
    case GL_RG8UI: if (format) *format = GL_RG_INTEGER; if (type) *type = GL_UNSIGNED_BYTE; break;
    case GL_RG8I: if (format) *format = GL_RG_INTEGER; if (type) *type = GL_BYTE; break;
    case GL_RG16UI: if (format) *format = GL_RG_INTEGER; if (type) *type = GL_UNSIGNED_SHORT; break;
    case GL_RG16I: if (format) *format = GL_RG_INTEGER; if (type) *type = GL_SHORT; break;
    case GL_RG32UI: if (format) *format = GL_RG_INTEGER; if (type) *type = GL_UNSIGNED_INT; break;
    case GL_RG32I: if (format) *format = GL_RG_INTEGER; if (type) *type = GL_INT; break;
    case GL_R32F: if (format) *format = GL_RED; if (type) *type = GL_FLOAT; break;
    case GL_RGBA8_SNORM: if (format) *format = GL_RGBA; if (type) *type = GL_BYTE; break;
    case GL_RGB: if (format) *format = GL_RGB; if (type && *type != GL_UNSIGNED_BYTE && *type != GL_UNSIGNED_SHORT_5_6_5) *type = GL_UNSIGNED_BYTE; break;
    default:
        if (*internal_format == GL_RGB8) {
            if (type && *type != GL_UNSIGNED_BYTE) *type = GL_UNSIGNED_BYTE;
            if (format) *format = GL_RGB;
        } else if (*internal_format == GL_RGBA16_SNORM) {
            if (type && *type != GL_SHORT) *type = GL_SHORT;
        }
        break;
    }
}
