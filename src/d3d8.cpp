// Direct3D 8 fixed-function pipeline implemented on legacy (2.1 compatibility) OpenGL.
//
// Resources keep their data in guest memory so Lock() can return direct pointers.
// Textures are uploaded lazily to GL when bound after being modified. Vertex data is
// converted on every draw into a fixed host layout (D3D colours are BGRA, pre-transformed
// vertices need un-projecting), which is cheap for a game of this era.
#include "platform.h"
#if HB_GLES
#include <GL/gl.h>  // gl4es
#include <GL/glext.h>
#ifdef __EMSCRIPTEN__
// gl4es only exports the core framebuffer entry points in its Emscripten build
#undef glBindFramebufferEXT
#define glBindFramebufferEXT gl4es_glBindFramebuffer
#undef glBindRenderbufferEXT
#define glBindRenderbufferEXT gl4es_glBindRenderbuffer
#undef glBlitFramebufferEXT
#define glBlitFramebufferEXT gl4es_glBlitFramebuffer
#undef glCheckFramebufferStatusEXT
#define glCheckFramebufferStatusEXT gl4es_glCheckFramebufferStatus
#undef glDeleteFramebuffersEXT
#define glDeleteFramebuffersEXT gl4es_glDeleteFramebuffers
#undef glDeleteRenderbuffersEXT
#define glDeleteRenderbuffersEXT gl4es_glDeleteRenderbuffers
#undef glFramebufferRenderbufferEXT
#define glFramebufferRenderbufferEXT gl4es_glFramebufferRenderbuffer
#undef glFramebufferTexture2DEXT
#define glFramebufferTexture2DEXT gl4es_glFramebufferTexture2D
#undef glGenFramebuffersEXT
#define glGenFramebuffersEXT gl4es_glGenFramebuffers
#undef glGenRenderbuffersEXT
#define glGenRenderbuffersEXT gl4es_glGenRenderbuffers
#undef glRenderbufferStorageEXT
#define glRenderbufferStorageEXT gl4es_glRenderbufferStorage
#endif
#else
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#endif
#include <cmath>
#include <cstddef>
#include <algorithm>

#define D3D_OK 0
#define D3DERR_INVALIDCALL 0x8876086C
#define D3DERR_NOTAVAILABLE 0x8876086A
#define E_NOINTERFACE 0x80004002

enum Fmt : u32 {
    F_R8G8B8 = 20, F_A8R8G8B8 = 21, F_X8R8G8B8 = 22, F_R5G6B5 = 23, F_X1R5G5B5 = 24, F_A1R5G5B5 = 25,
    F_A4R4G4B4 = 26, F_R3G3B2 = 27, F_A8 = 28, F_X4R4G4B4 = 30, F_L8 = 50, F_A8L8 = 51, F_A4L4 = 52,
    F_D16 = 80, F_D24S8 = 75, F_D24X8 = 77, F_D32 = 71, F_D15S1 = 73, F_D16_LOCKABLE = 70, F_INDEX16 = 101, F_INDEX32 = 102,
    F_DXT1 = 0x31545844, F_DXT2 = 0x32545844, F_DXT3 = 0x33545844, F_DXT4 = 0x34545844, F_DXT5 = 0x35545844,
};
static bool is_dxt(u32 f) { return f == F_DXT1 || f == F_DXT2 || f == F_DXT3 || f == F_DXT4 || f == F_DXT5; }
static u32 fmt_bpp(u32 f) {
    switch (f) {
        case F_A8R8G8B8: case F_X8R8G8B8: case F_D24S8: case F_D24X8: case F_D32: case F_INDEX32: return 4;
        case F_R8G8B8: return 3;
        case F_R5G6B5: case F_X1R5G5B5: case F_A1R5G5B5: case F_A4R4G4B4: case F_X4R4G4B4: case F_A8L8:
        case F_D16: case F_D15S1: case F_D16_LOCKABLE: case F_INDEX16: return 2;
        case F_A8: case F_L8: case F_A4L4: case F_R3G3B2: return 1;
    }
    return 4;
}
static u32 level_pitch(u32 f, u32 w) {
    if (is_dxt(f)) return std::max(1u, (w + 3) / 4) * (f == F_DXT1 ? 8 : 16);
    return w * fmt_bpp(f);
}
static u32 level_rows(u32 f, u32 h) { return is_dxt(f) ? std::max(1u, (h + 3) / 4) : h; }

// ---- resources ----------------------------------------------------------------------------
enum ResKind { R_TEXTURE, R_SURFACE, R_VB, R_IB };
struct Level { u32 w, h, pitch, addr; bool dirty; };
struct Res {
    ResKind kind;
    int refs = 1;
    u32 obj = 0;          // guest COM object
    u32 fmt = 0, usage = 0, pool = 0;
    // textures
    std::vector<Level> levels;
    GLuint tex = 0;
    GLuint fbo = 0, depth_rb = 0;  // for render target textures
    bool render_target = false;
    u32 lod = 0;
    // surfaces
    int tex_res = -1, tex_level = 0;  // surface of a texture level
    bool backbuffer = false, depth = false;
    u32 w = 0, h = 0, addr = 0, pitch = 0;  // standalone surface storage
    // buffers
    u32 size = 0, fvf = 0, buf = 0;
};
static std::vector<Res*> res;
static u32 vt_d3d, vt_dev, vt_tex, vt_surf, vt_vb, vt_ib;
static u32 d3d_obj, dev_obj;

static int new_res(Res* r, u32 vtbl) {
    int id = (int)res.size();
    res.push_back(r);
    r->obj = make_com_object(vtbl, (u32)id);
    return id;
}
static Res* R(u32 obj) { return obj ? res[com_id(obj)] : nullptr; }
static int RID(u32 obj) { return obj ? (int)com_id(obj) : -1; }

// ---- device state ---------------------------------------------------------------------------
struct Light { u8 raw[104]; bool enabled; bool set; };
struct State {
    u32 rs[256];
    u32 tss[8][32];
    float world[16], view[16], proj[16], texm[8][16];
    u8 material[68];
    std::map<u32, Light> lights;
    u32 vp[6];
    int textures[8];
    u32 fvf;
    int stream_vb; u32 stream_stride;
    int ib; u32 base_vertex;
};
static State st;
static u32 bb_w = 640, bb_h = 480, bb_fmt = F_X8R8G8B8;
static bool windowed = true;
static GLuint bb_fbo = 0, bb_color = 0, bb_depth = 0;
// Framebuffer binds are cached: gl4es checks glGetError after each one, which stalls WebGL.
static GLuint bound_fb = ~0u;
static void bind_fb(GLuint fb) {
    if (fb == bound_fb) return;
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb);
    bound_fb = fb;
}
static int bb_surf = -1, depth_surf = -1;
static int cur_rt = -1, cur_ds = -1;  // surface ids (-1 => default back buffer)
static GLuint white_tex = 0;
static u32 rt_w = 640, rt_h = 480;  // logical (guest-visible) render target size
// The back buffer is rendered at bb_scale x the game's resolution and downscaled on present,
// so 3D geometry is sharp on high-DPI displays. Guest-visible sizes stay unscaled.
static u32 bb_scale = 1;
static u32 rt_scale = 1;  // scale of the currently bound render target
static u32 frame_no = 0;

static const float IDENT[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static float u2f(u32 v) { float f; memcpy(&f, &v, 4); return f; }

static void default_state() {
    memset(&st, 0, sizeof(st.rs) + sizeof(st.tss));
    u32* rs = st.rs;
    rs[7] = 1; rs[8] = 3; rs[9] = 2; rs[14] = 1; rs[16] = 1; rs[19] = 2; rs[20] = 1; rs[22] = 3; rs[23] = 4;
    rs[25] = 8; rs[34] = 0; rs[37] = 0x3F800000; rs[38] = 0x3F800000; rs[53] = 1; rs[54] = 1; rs[55] = 1;
    rs[56] = 8; rs[58] = 0xFFFFFFFF; rs[59] = 0xFFFFFFFF; rs[60] = 0xFFFFFFFF; rs[136] = 1; rs[137] = 1;
    rs[141] = 1; rs[142] = 1; rs[145] = 1; rs[146] = 2; rs[154] = 0x3F800000; rs[155] = 0; rs[158] = 0x3F800000;
    rs[162] = 0xFFFFFFFF; rs[166] = 0x42800000; rs[168] = 0xF; rs[171] = 1;
    for (int s = 0; s < 8; s++) {
        u32* t = st.tss[s];
        t[1] = s == 0 ? 4 : 1; t[2] = 2; t[3] = 1; t[4] = s == 0 ? 2 : 1; t[5] = 2; t[6] = 1;
        t[11] = s; t[13] = 1; t[14] = 1; t[25] = 1; t[16] = 1; t[17] = 1; t[18] = 0; t[21] = 1;
        t[26] = 1; t[27] = 1; t[28] = 1;
        memcpy(st.texm[s], IDENT, 64);
        st.textures[s] = -1;
    }
    memcpy(st.world, IDENT, 64); memcpy(st.view, IDENT, 64); memcpy(st.proj, IDENT, 64);
    memset(st.material, 0, sizeof(st.material));
    st.lights.clear();
    st.vp[0] = 0; st.vp[1] = 0; st.vp[2] = bb_w; st.vp[3] = bb_h; st.vp[4] = 0; st.vp[5] = 0x3F800000;
    st.fvf = 0; st.stream_vb = -1; st.stream_stride = 0; st.ib = -1; st.base_vertex = 0;
}

// ---- refcounting helpers ------------------------------------------------------------------------
static void tex_destroy(Res* r) {
    if (r->tex) glDeleteTextures(1, &r->tex);
    if (r->fbo) { if (bound_fb == r->fbo) bound_fb = ~0u; glDeleteFramebuffersEXT(1, &r->fbo); }
    if (r->depth_rb) glDeleteRenderbuffersEXT(1, &r->depth_rb);
    r->tex = r->fbo = r->depth_rb = 0;
    for (auto& l : r->levels) if (l.addr) gfree(l.addr);
    r->levels.clear();
}
static void release_res(int id) {
    if (id < 0) return;
    Res* r = res[id];
    if (--r->refs > 0) return;
    if (r->refs < 0) { r->refs = 0; return; }
    for (int s = 0; s < 8; s++) if (st.textures[s] == id) st.textures[s] = -1;
    switch (r->kind) {
        case R_TEXTURE: tex_destroy(r); break;
        case R_SURFACE:
            if (r->tex_res >= 0) release_res(r->tex_res);
            else if (r->addr) { gfree(r->addr); r->addr = 0; }
            break;
        case R_VB: case R_IB: if (r->buf) { gfree(r->buf); r->buf = 0; } break;
    }
}
static void addref_res(int id) { if (id >= 0) res[id]->refs++; }

// ---- pixel conversion -------------------------------------------------------------------------
static void convert_row(u32 fmt, const u8* src, u8* dst, u32 w) {
    for (u32 x = 0; x < w; x++, dst += 4) {
        u32 r = 255, g = 255, b = 255, a = 255;
        switch (fmt) {
            case F_A8R8G8B8: b = src[x * 4]; g = src[x * 4 + 1]; r = src[x * 4 + 2]; a = src[x * 4 + 3]; break;
            case F_X8R8G8B8: b = src[x * 4]; g = src[x * 4 + 1]; r = src[x * 4 + 2]; break;
            case F_R8G8B8: b = src[x * 3]; g = src[x * 3 + 1]; r = src[x * 3 + 2]; break;
            case F_R5G6B5: { u16 p = ((const u16*)src)[x]; r = (p >> 11) * 255 / 31; g = ((p >> 5) & 63) * 255 / 63; b = (p & 31) * 255 / 31; break; }
            case F_X1R5G5B5: case F_A1R5G5B5: {
                u16 p = ((const u16*)src)[x];
                r = ((p >> 10) & 31) * 255 / 31; g = ((p >> 5) & 31) * 255 / 31; b = (p & 31) * 255 / 31;
                a = fmt == F_A1R5G5B5 ? ((p >> 15) ? 255 : 0) : 255; break;
            }
            case F_A4R4G4B4: case F_X4R4G4B4: {
                u16 p = ((const u16*)src)[x];
                a = fmt == F_A4R4G4B4 ? (p >> 12) * 17 : 255; r = ((p >> 8) & 15) * 17; g = ((p >> 4) & 15) * 17; b = (p & 15) * 17; break;
            }
            case F_A8: r = g = b = 255; a = src[x]; break;
            case F_L8: r = g = b = src[x]; break;
            case F_A8L8: r = g = b = src[x * 2]; a = src[x * 2 + 1]; break;
            case F_A4L4: r = g = b = (src[x] & 15) * 17; a = (src[x] >> 4) * 17; break;
            case F_R3G3B2: { u8 p = src[x]; r = (p >> 5) * 255 / 7; g = ((p >> 2) & 7) * 255 / 7; b = (p & 3) * 85; break; }
        }
        dst[0] = (u8)r; dst[1] = (u8)g; dst[2] = (u8)b; dst[3] = (u8)a;
    }
}

static void tex_upload(Res* r) {
    if (!r->tex) {
        glGenTextures(1, &r->tex);
        glBindTexture(GL_TEXTURE_2D, r->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)r->levels.size() - 1);
        for (auto& l : r->levels) l.dirty = true;
    } else glBindTexture(GL_TEXTURE_2D, r->tex);
    static std::vector<u8> tmp;
    for (size_t i = 0; i < r->levels.size(); i++) {
        Level& l = r->levels[i];
        if (!l.dirty) continue;
        l.dirty = false;
        if (is_dxt(r->fmt)) {
            GLenum gf = r->fmt == F_DXT1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT : (r->fmt == F_DXT2 || r->fmt == F_DXT3) ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
            glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)i, gf, l.w, l.h, 0, l.pitch * level_rows(r->fmt, l.h), g2h(l.addr));
            continue;
        }
        tmp.resize((size_t)l.w * l.h * 4);
        for (u32 y = 0; y < l.h; y++) convert_row(r->fmt, g2h(l.addr + y * l.pitch), &tmp[(size_t)y * l.w * 4], l.w);
        glTexImage2D(GL_TEXTURE_2D, (GLint)i, GL_RGBA8, l.w, l.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
    }
}

// Read back a render target texture into its guest level memory (for Lock on RT textures).
static void rt_readback(Res* r) {
    if (!r->fbo || r->levels.empty()) return;
    Level& l = r->levels[0];
    GLint prev; glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &prev);
    bind_fb(r->fbo);
    std::vector<u8> tmp((size_t)l.w * l.h * 4);
    glReadPixels(0, 0, l.w, l.h, GL_BGRA, GL_UNSIGNED_BYTE, tmp.data());
    for (u32 y = 0; y < l.h; y++) memcpy(g2h(l.addr + y * l.pitch), &tmp[(size_t)(l.h - 1 - y) * l.w * 4], std::min(l.pitch, l.w * 4));
    bind_fb(prev);
}

// ---- framebuffer -----------------------------------------------------------------------------------
static u32 choose_scale() {
    if (const char* e = getenv("HB_RENDER_SCALE")) return std::max(1, std::min(8, atoi(e)));
    SDL_DisplayMode dm;
    int disp = SDL_GetWindowDisplayIndex(plat::window);
    float ddpi = 0;
    int px_h = 1080;
    if (SDL_GetDesktopDisplayMode(disp < 0 ? 0 : disp, &dm) == 0) px_h = dm.h;
    // desktop mode height is in points on macOS; account for Retina backing scale
    int ww, wh, dw, dh; SDL_GetWindowSize(plat::window, &ww, &wh); plat::drawable_size(dw, dh);
    float backing = wh ? (float)dh / wh : 1.0f;
    (void)ddpi;
    u32 k = (u32)ceilf(px_h * backing / bb_h);
#if HB_GLES
    return std::max(1u, std::min(2u, k));  // mobile GPUs / WebGL: cap supersampling at 2x
#else
    return std::max(1u, std::min(4u, k));
#endif
}
static void glcheck(const char* where) {
    for (GLenum e; (e = glGetError()) != GL_NO_ERROR;) logf("GL error %x after %s", e, where);
}
static void create_backbuffer() {
    bound_fb = ~0u;
    bb_scale = choose_scale();
    glcheck("before back buffer");
    logf("render scale %ux (%ux%u)", bb_scale, bb_w * bb_scale, bb_h * bb_scale);
    if (bb_fbo) {
        glDeleteFramebuffersEXT(1, &bb_fbo); glDeleteTextures(1, &bb_color); glDeleteRenderbuffersEXT(1, &bb_depth);
    }
    glGenTextures(1, &bb_color);
    glBindTexture(GL_TEXTURE_2D, bb_color);
    // Non-power-of-two: ES 2 / WebGL 1 only sample it unmipmapped with clamped wrapping, and
    // gl4es decides how to store it at upload time, so set that first.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bb_w * bb_scale, bb_h * bb_scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glcheck("color texture");
    // depth/stencil: packed 24/8 where supported; OpenGL ES devices may only take depth alone.
    // Each attempt gets a fresh framebuffer: WebGL rejects one left with conflicting attachments.
    struct { GLenum fmt; bool stencil; } tries[] = {{GL_DEPTH24_STENCIL8_EXT, true}, {GL_DEPTH_COMPONENT24, false}, {GL_DEPTH_COMPONENT16, false}};
    GLenum s = 0;
    for (auto& t : tries) {
        glGenFramebuffersEXT(1, &bb_fbo);
        bind_fb(bb_fbo);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, bb_color, 0);
        glGenRenderbuffersEXT(1, &bb_depth);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, bb_depth);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, t.fmt, bb_w * bb_scale, bb_h * bb_scale);
#ifdef __EMSCRIPTEN__
        if (t.stencil)  // WebGL 1 takes a packed buffer only on the combined attachment point
            glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, 0x821A /*GL_DEPTH_STENCIL_ATTACHMENT*/, GL_RENDERBUFFER_EXT, bb_depth);
        else
#endif
        {
            glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, bb_depth);
            glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, t.stencil ? bb_depth : 0);
        }
        glcheck("depth buffer");
        s = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
        if (s == GL_FRAMEBUFFER_COMPLETE_EXT) break;
        logf("back buffer: depth format %x not usable (status %x)", t.fmt, s);
        bind_fb(0);
        glDeleteFramebuffersEXT(1, &bb_fbo);
        glDeleteRenderbuffersEXT(1, &bb_depth);
    }
    glcheck("status check");
    if (s != GL_FRAMEBUFFER_COMPLETE_EXT) fatal("back buffer FBO incomplete: %x", s);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    rt_w = bb_w; rt_h = bb_h; rt_scale = bb_scale;
}
static void bind_render_target() {
    if (cur_rt < 0 || res[cur_rt]->backbuffer) { bind_fb(bb_fbo); rt_w = bb_w; rt_h = bb_h; rt_scale = bb_scale; return; }
    Res* s = res[cur_rt];
    if (s->tex_res >= 0) {
        Res* t = res[s->tex_res];
        if (!t->fbo) {
            tex_upload(t);
            glGenFramebuffersEXT(1, &t->fbo);
            bind_fb(t->fbo);
            glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, t->tex, 0);
            glGenRenderbuffersEXT(1, &t->depth_rb);
            glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, t->depth_rb);
            glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, t->levels[0].w, t->levels[0].h);
            glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, t->depth_rb);
            glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, t->depth_rb);
        }
        bind_fb(t->fbo);
        rt_w = t->levels[0].w; rt_h = t->levels[0].h; rt_scale = 1;
        return;
    }
    bind_fb(bb_fbo);
    rt_w = bb_w; rt_h = bb_h; rt_scale = bb_scale;
}

static void apply_viewport() {
    u32 x = st.vp[0], y = st.vp[1], w = st.vp[2], h = st.vp[3];
    GLint k = (GLint)rt_scale;
    glViewport((GLint)x * k, ((GLint)rt_h - (GLint)(y + h)) * k, (GLsizei)w * k, (GLsizei)h * k);
    glDepthRange(u2f(st.vp[4]), u2f(st.vp[5]));
}

// ---- state application ---------------------------------------------------------------------------
static GLenum gl_cmp(u32 f) {
    static const GLenum m[] = {GL_ALWAYS, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    return f <= 8 ? m[f] : GL_ALWAYS;
}
static GLenum gl_blend(u32 b) {
    switch (b) {
        case 1: return GL_ZERO; case 2: return GL_ONE; case 3: return GL_SRC_COLOR; case 4: return GL_ONE_MINUS_SRC_COLOR;
        case 5: return GL_SRC_ALPHA; case 6: return GL_ONE_MINUS_SRC_ALPHA; case 7: return GL_DST_ALPHA;
        case 8: return GL_ONE_MINUS_DST_ALPHA; case 9: return GL_DST_COLOR; case 10: return GL_ONE_MINUS_DST_COLOR;
        case 11: return GL_SRC_ALPHA_SATURATE;
    }
    return GL_ONE;
}
static GLenum gl_stencil_op(u32 o) {
    switch (o) {
        case 1: return GL_KEEP; case 2: return GL_ZERO; case 3: return GL_REPLACE; case 4: return GL_INCR;
        case 5: return GL_DECR; case 6: return GL_INVERT; case 7: return GL_INCR_WRAP; case 8: return GL_DECR_WRAP;
    }
    return GL_KEEP;
}
static void color4(u32 argb, float* out) {
    out[0] = ((argb >> 16) & 255) / 255.0f; out[1] = ((argb >> 8) & 255) / 255.0f;
    out[2] = (argb & 255) / 255.0f; out[3] = (argb >> 24) / 255.0f;
}

static void apply_raster_state(bool rhw) {
    u32* rs = st.rs;
    // depth
    if (rs[7]) { glEnable(GL_DEPTH_TEST); glDepthFunc(gl_cmp(rs[23])); } else glDisable(GL_DEPTH_TEST);
    glDepthMask(rs[14] ? GL_TRUE : GL_FALSE);
    if (rs[47]) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-1.0f, -(float)rs[47]); } else glDisable(GL_POLYGON_OFFSET_FILL);
    // cull (D3D front faces are clockwise)
    glFrontFace(GL_CW);
    if (rs[22] == 1) glDisable(GL_CULL_FACE);
    else { glEnable(GL_CULL_FACE); glCullFace(rs[22] == 2 ? GL_FRONT : GL_BACK); }
    glPolygonMode(GL_FRONT_AND_BACK, rs[8] == 1 ? GL_POINT : rs[8] == 2 ? GL_LINE : GL_FILL);
    glLineWidth((float)rt_scale);
    glPointSize((float)rt_scale);
    glShadeModel(rs[9] == 1 ? GL_FLAT : GL_SMOOTH);
    // alpha test
    if (rs[15]) { glEnable(GL_ALPHA_TEST); glAlphaFunc(gl_cmp(rs[25]), (rs[24] & 255) / 255.0f); } else glDisable(GL_ALPHA_TEST);
    // blending
    if (rs[27]) {
        glEnable(GL_BLEND);
        GLenum s = gl_blend(rs[19]), d = gl_blend(rs[20]);
        if (rs[19] == 12) { s = GL_SRC_ALPHA; d = GL_ONE_MINUS_SRC_ALPHA; }
        if (rs[19] == 13) { s = GL_ONE_MINUS_SRC_ALPHA; d = GL_SRC_ALPHA; }
        glBlendFunc(s, d);
        GLenum eq = GL_FUNC_ADD;
        switch (rs[171]) { case 2: eq = GL_FUNC_SUBTRACT; break; case 3: eq = GL_FUNC_REVERSE_SUBTRACT; break; case 4: eq = GL_MIN; break; case 5: eq = GL_MAX; break; }
        glBlendEquation(eq);
    } else glDisable(GL_BLEND);
    // stencil
    if (rs[52]) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(gl_cmp(rs[56]), (GLint)rs[57], rs[58]);
        glStencilOp(gl_stencil_op(rs[53]), gl_stencil_op(rs[54]), gl_stencil_op(rs[55]));
        glStencilMask(rs[59]);
    } else glDisable(GL_STENCIL_TEST);
    u32 cw = rs[168];
    glColorMask(cw & 1, (cw >> 1) & 1, (cw >> 2) & 1, (cw >> 3) & 1);
    // fog
    if (rs[28] && (rs[35] || rs[140] || rhw)) {
        glEnable(GL_FOG);
        float fc[4]; color4(rs[34], fc);
        glFogfv(GL_FOG_COLOR, fc);
        u32 mode = rs[35] ? rs[35] : rs[140];
        if (rhw && !rs[35]) {
            // fog factor supplied in specular alpha
            glFogi(GL_FOG_COORDINATE_SOURCE, GL_FOG_COORDINATE);
            glFogi(GL_FOG_MODE, GL_LINEAR);
            glFogf(GL_FOG_START, 1.0f); glFogf(GL_FOG_END, 0.0f);
        } else {
            glFogi(GL_FOG_COORDINATE_SOURCE, GL_FRAGMENT_DEPTH);
            glFogi(GL_FOG_MODE, mode == 1 ? GL_EXP : mode == 2 ? GL_EXP2 : GL_LINEAR);
            glFogf(GL_FOG_START, u2f(rs[36])); glFogf(GL_FOG_END, u2f(rs[37])); glFogf(GL_FOG_DENSITY, u2f(rs[38]));
        }
    } else glDisable(GL_FOG);
}

static void set_combiner(bool alpha, u32 op, u32 a1, u32 a2, bool has_tex) {
    GLenum combine = alpha ? GL_COMBINE_ALPHA : GL_COMBINE_RGB;
    GLenum srcs[3] = {(GLenum)(alpha ? GL_SOURCE0_ALPHA : GL_SOURCE0_RGB), (GLenum)(alpha ? GL_SOURCE1_ALPHA : GL_SOURCE1_RGB), (GLenum)(alpha ? GL_SOURCE2_ALPHA : GL_SOURCE2_RGB)};
    GLenum opers[3] = {(GLenum)(alpha ? GL_OPERAND0_ALPHA : GL_OPERAND0_RGB), (GLenum)(alpha ? GL_OPERAND1_ALPHA : GL_OPERAND1_RGB), (GLenum)(alpha ? GL_OPERAND2_ALPHA : GL_OPERAND2_RGB)};
    auto src = [&](int slot, u32 arg, bool force_alpha = false, bool complement = false) {
        GLenum s;
        switch (arg & 0xF) {
            case 0: s = GL_PRIMARY_COLOR; break;
            case 1: s = GL_PREVIOUS; break;
            case 2: s = GL_TEXTURE; break;
            case 3: s = GL_CONSTANT; break;
            case 4: s = GL_PRIMARY_COLOR; break;  // specular: approximated
            default: s = GL_PREVIOUS; break;
        }
        glTexEnvi(GL_TEXTURE_ENV, srcs[slot], s);
        bool comp = ((arg & 0x10) != 0) ^ complement;
        bool alp = alpha || (arg & 0x20) || force_alpha;
        GLenum o = alp ? (comp ? GL_ONE_MINUS_SRC_ALPHA : GL_SRC_ALPHA) : (comp ? GL_ONE_MINUS_SRC_COLOR : GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, opers[slot], o);
    };
    GLenum scale = alpha ? GL_ALPHA_SCALE : GL_RGB_SCALE;
    glTexEnvf(GL_TEXTURE_ENV, scale, 1.0f);
    (void)has_tex;
    switch (op) {
        case 2: glTexEnvi(GL_TEXTURE_ENV, combine, GL_REPLACE); src(0, a1); break;
        case 3: glTexEnvi(GL_TEXTURE_ENV, combine, GL_REPLACE); src(0, a2); break;
        case 4: case 5: case 6:
            glTexEnvi(GL_TEXTURE_ENV, combine, GL_MODULATE); src(0, a1); src(1, a2);
            glTexEnvf(GL_TEXTURE_ENV, scale, op == 4 ? 1.0f : op == 5 ? 2.0f : 4.0f);
            break;
        case 7: glTexEnvi(GL_TEXTURE_ENV, combine, GL_ADD); src(0, a1); src(1, a2); break;
        case 8: case 9:
            glTexEnvi(GL_TEXTURE_ENV, combine, GL_ADD_SIGNED); src(0, a1); src(1, a2);
            if (op == 9) glTexEnvf(GL_TEXTURE_ENV, scale, 2.0f);
            break;
        case 10: glTexEnvi(GL_TEXTURE_ENV, combine, GL_SUBTRACT); src(0, a1); src(1, a2); break;
        case 11:  // ADDSMOOTH: a1 + a2 - a1*a2 ~= a1 + a2*(1-a1)  -> interpolate(1, a2, a1)... approximate with ADD
            glTexEnvi(GL_TEXTURE_ENV, combine, GL_ADD); src(0, a1); src(1, a2); break;
        case 12: case 13: case 14: case 16: {
            // BLEND*ALPHA: a1*alpha + a2*(1-alpha)
            glTexEnvi(GL_TEXTURE_ENV, combine, GL_INTERPOLATE); src(0, a1); src(1, a2);
            u32 asrc = op == 12 ? 0 : op == 13 ? 2 : op == 14 ? 3 : 1;
            src(2, asrc, true);
            break;
        }
        case 15:  // BLENDTEXTUREALPHAPM: a1 + a2*(1-texalpha) -> approximate with interpolate
            glTexEnvi(GL_TEXTURE_ENV, combine, GL_INTERPOLATE); src(0, a1); src(1, a2); src(2, 2, true); break;
        case 24:
            if (!alpha) { glTexEnvi(GL_TEXTURE_ENV, combine, GL_DOT3_RGBA); src(0, a1); src(1, a2); }
            else { glTexEnvi(GL_TEXTURE_ENV, combine, GL_REPLACE); src(0, a1); }
            break;
        case 18:  // MODULATEALPHA_ADDCOLOR: a1.rgb + a1.a*a2.rgb (approximate)
            glTexEnvi(GL_TEXTURE_ENV, combine, GL_ADD); src(0, a1); src(1, a2); break;
        default:
            glTexEnvi(GL_TEXTURE_ENV, combine, GL_MODULATE); src(0, a1); src(1, a2); break;
    }
}

static void apply_sampler(int stage, Res* t) {
    u32* ts = st.tss[stage];
    auto addr = [](u32 m) -> GLint {
        switch (m) { case 2: case 5: return GL_MIRRORED_REPEAT; case 3: return GL_CLAMP_TO_EDGE; case 4: return GL_CLAMP_TO_BORDER; }
        return GL_REPEAT;
    };
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, addr(ts[13]));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, addr(ts[14]));
    float bc[4]; color4(ts[15], bc);
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, bc);
    u32 mag = ts[16], min = ts[17], mip = ts[18];
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag <= 1 ? GL_NEAREST : GL_LINEAR);
    bool has_mips = t && t->levels.size() > 1;
    GLint mf;
    if (!has_mips || mip == 0) mf = min <= 1 ? GL_NEAREST : GL_LINEAR;
    else if (mip == 1) mf = min <= 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_NEAREST;
    else mf = min <= 1 ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mf);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, min == 3 ? (float)std::max(1u, ts[21]) : 1.0f);
    if (t) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, (GLint)std::min<u32>(std::max(t->lod, ts[20]), (u32)t->levels.size() - 1));
    glTexEnvf(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS, u2f(ts[19]));
}

// Converted vertex layout
struct HVtx {
    float pos[4];
    float nrm[3];
    u8 col[4];
    u8 spec[4];
    float fog;
    float tc[4][4];
};
static int g_max_units = 4;
static int active_units = 0;

static void apply_texture_stages(bool rhw, int ntc) {
    float tf[4]; color4(st.rs[60], tf);
    int last = 0;
    for (int s = 0; s < g_max_units; s++) {
        u32* ts = st.tss[s];
        glActiveTexture(GL_TEXTURE0 + s);
        glClientActiveTexture(GL_TEXTURE0 + s);
        bool enabled = ts[1] != 1 && (s == 0 || last == s);
        if (!enabled) {
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_TEXTURE_GEN_S); glDisable(GL_TEXTURE_GEN_T); glDisable(GL_TEXTURE_GEN_R); glDisable(GL_TEXTURE_GEN_Q);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            continue;
        }
        last = s + 1;
        glEnable(GL_TEXTURE_2D);
        Res* t = st.textures[s] >= 0 ? res[st.textures[s]] : nullptr;
        if (t && t->kind == R_TEXTURE && !t->levels.empty()) {
            if (t->render_target && t->tex) glBindTexture(GL_TEXTURE_2D, t->tex);
            else tex_upload(t);
        } else {
            glBindTexture(GL_TEXTURE_2D, white_tex);
            t = nullptr;
        }
        apply_sampler(s, t);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, tf);
        u32 aop = ts[4], aa1 = ts[5], aa2 = ts[6];
        if (!t && aop == 2 && (aa1 & 0xF) == 2) { aop = 2; aa1 = s == 0 ? 0 : 1; }  // missing texture: pass alpha through
        if (aop == 1) { aop = 2; aa1 = 1; }  // alpha disabled: keep current
        set_combiner(false, ts[1], ts[2], ts[3], t != nullptr);
        set_combiner(true, aop, aa1, aa2, t != nullptr);

        // texture coordinates
        u32 tci = ts[11];
        u32 gen = tci & 0xFFFF0000, idx = tci & 0xFFFF;
        glDisable(GL_TEXTURE_GEN_S); glDisable(GL_TEXTURE_GEN_T); glDisable(GL_TEXTURE_GEN_R); glDisable(GL_TEXTURE_GEN_Q);
        if (gen && !rhw) {
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            GLint mode = gen == 0x10000 ? GL_NORMAL_MAP : gen == 0x30000 ? GL_REFLECTION_MAP : GL_EYE_LINEAR;
            if (mode == GL_EYE_LINEAR) {
                // eye-space position: planes must be specified with identity modelview
                glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
                float ps[4] = {1, 0, 0, 0}, pt[4] = {0, 1, 0, 0}, pr[4] = {0, 0, 1, 0};
                glTexGenfv(GL_S, GL_EYE_PLANE, ps); glTexGenfv(GL_T, GL_EYE_PLANE, pt); glTexGenfv(GL_R, GL_EYE_PLANE, pr);
                glPopMatrix();
            }
            glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, mode); glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, mode); glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, mode);
            glEnable(GL_TEXTURE_GEN_S); glEnable(GL_TEXTURE_GEN_T); glEnable(GL_TEXTURE_GEN_R);
        } else if ((int)idx < ntc) {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        } else {
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glMultiTexCoord4f(GL_TEXTURE0 + s, 0, 0, 0, 1);
        }
        // texture transform
        glMatrixMode(GL_TEXTURE);
        u32 ttf = ts[24];
        if ((ttf & 0xFF) && !rhw) {
            float m[16]; memcpy(m, st.texm[s], 64);
            bool two_comp_input = !gen;  // 2D coords: D3D uses row 3 as translation
            if (two_comp_input) { for (int k = 0; k < 4; k++) { m[12 + k] = m[8 + k]; m[8 + k] = 0; } }
            if (!(ttf & 256)) {
                // non-projected: force q = 1
                m[3] = m[7] = m[11] = 0; m[15] = 1;
            }
            glLoadMatrixf(m);
        } else glLoadIdentity();
    }
    for (int s = last; s < g_max_units; s++) {
        glActiveTexture(GL_TEXTURE0 + s);
        glDisable(GL_TEXTURE_2D);
    }
    active_units = last;
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
}

static void apply_lighting(bool has_normal, bool has_diffuse) {
    u32* rs = st.rs;
    if (!rs[137]) { glDisable(GL_LIGHTING); return; }
    glEnable(GL_LIGHTING);
    float amb[4]; color4(rs[139], amb);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, rs[142] ? 1 : 0);
    glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SEPARATE_SPECULAR_COLOR);
    if (rs[143]) glEnable(GL_NORMALIZE); else glDisable(GL_NORMALIZE);
    // material
    const float* m = (const float*)st.material;
    float zero[4] = {0, 0, 0, 0};
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, m);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, m + 4);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, rs[29] ? m + 8 : zero);
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, m + 12);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, std::min(128.0f, std::max(0.0f, m[16])));
    bool cv = rs[141] && has_diffuse;
    bool dif = cv && rs[145] == 1, ambc = cv && rs[147] == 1, emi = cv && rs[148] == 1;
    if (dif && ambc) { glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE); glEnable(GL_COLOR_MATERIAL); }
    else if (dif) { glColorMaterial(GL_FRONT_AND_BACK, GL_DIFFUSE); glEnable(GL_COLOR_MATERIAL); }
    else if (ambc) { glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT); glEnable(GL_COLOR_MATERIAL); }
    else if (emi) { glColorMaterial(GL_FRONT_AND_BACK, GL_EMISSION); glEnable(GL_COLOR_MATERIAL); }
    else glDisable(GL_COLOR_MATERIAL);
    // lights are given in world space: load view matrix only
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(st.view);
    int gl_i = 0;
    for (auto& kv : st.lights) {
        if (!kv.second.enabled || gl_i >= 8) continue;
        const Light& L = kv.second;
        u32 type; memcpy(&type, L.raw, 4);
        const float* f = (const float*)(L.raw + 4);
        GLenum li = GL_LIGHT0 + gl_i++;
        glEnable(li);
        glLightfv(li, GL_DIFFUSE, f);
        glLightfv(li, GL_SPECULAR, rs[29] ? f + 4 : zero);
        glLightfv(li, GL_AMBIENT, f + 8);
        const float* pos = f + 12; const float* dir = f + 15;
        float range = f[18], falloff = f[19], a0 = f[20], a1 = f[21], a2 = f[22], theta = f[23], phi = f[24];
        (void)range; (void)theta;
        if (type == 3) {  // directional
            float p[4] = {-dir[0], -dir[1], -dir[2], 0};
            glLightfv(li, GL_POSITION, p);
            glLightf(li, GL_SPOT_CUTOFF, 180);
            glLightf(li, GL_CONSTANT_ATTENUATION, 1); glLightf(li, GL_LINEAR_ATTENUATION, 0); glLightf(li, GL_QUADRATIC_ATTENUATION, 0);
        } else {
            float p[4] = {pos[0], pos[1], pos[2], 1};
            glLightfv(li, GL_POSITION, p);
            glLightf(li, GL_CONSTANT_ATTENUATION, a0 == 0 && a1 == 0 && a2 == 0 ? 1 : a0);
            glLightf(li, GL_LINEAR_ATTENUATION, a1);
            glLightf(li, GL_QUADRATIC_ATTENUATION, a2);
            if (type == 2) {
                glLightfv(li, GL_SPOT_DIRECTION, dir);
                glLightf(li, GL_SPOT_CUTOFF, std::min(90.0f, phi * 0.5f * 180.0f / (float)M_PI));
                glLightf(li, GL_SPOT_EXPONENT, std::min(128.0f, falloff));
            } else glLightf(li, GL_SPOT_CUTOFF, 180);
        }
    }
    for (; gl_i < 8; gl_i++) glDisable(GL_LIGHT0 + gl_i);
    (void)has_normal;
}

// ---- vertex conversion & drawing ------------------------------------------------------------------
static std::vector<HVtx> vbuf;
static std::vector<u32> ibuf;

struct FvfInfo { bool rhw; int nweights; bool normal, psize, diffuse, specular; int ntc; int tcsize[8]; u32 stride; };
static FvfInfo parse_fvf(u32 fvf) {
    FvfInfo f = {};
    u32 pos = fvf & 0xE;
    u32 off = 0;
    if (pos == 0x4) { f.rhw = true; off = 16; }
    else if (pos == 0x2) off = 12;
    else if (pos >= 0x6) { f.nweights = (int)((pos - 0x6) / 2 + 1); off = 12 + f.nweights * 4; }
    f.normal = fvf & 0x10; f.psize = fvf & 0x20; f.diffuse = fvf & 0x40; f.specular = fvf & 0x80;
    if (f.normal) off += 12;
    if (f.psize) off += 4;
    if (f.diffuse) off += 4;
    if (f.specular) off += 4;
    f.ntc = (int)((fvf >> 8) & 0xF);
    for (int i = 0; i < f.ntc; i++) {
        u32 s = (fvf >> (16 + i * 2)) & 3;
        f.tcsize[i] = s == 0 ? 2 : s == 1 ? 3 : s == 2 ? 4 : 1;
        off += f.tcsize[i] * 4;
    }
    f.stride = off;
    return f;
}

static void convert_vertices(const FvfInfo& f, const u8* src, u32 stride, u32 count) {
    vbuf.resize(count);
    float vx = (float)st.vp[0], vy = (float)st.vp[1];
    (void)vx; (void)vy;
    for (u32 i = 0; i < count; i++) {
        const u8* p = src + (size_t)i * stride;
        HVtx& v = vbuf[i];
        const float* fp = (const float*)p;
        u32 off;
        if (f.rhw) {
            float rhw = fp[3];
            float w = (rhw != 0 && std::isfinite(rhw)) ? 1.0f / rhw : 1.0f;
            // D3D doesn't depth-clip pre-transformed vertices (the game's menu backdrop uses z up to 480)
            float z = std::min(1.0f, std::max(0.0f, fp[2]));
            v.pos[0] = fp[0] * w; v.pos[1] = fp[1] * w; v.pos[2] = z * w; v.pos[3] = w;
            off = 16;
        } else {
            v.pos[0] = fp[0]; v.pos[1] = fp[1]; v.pos[2] = fp[2]; v.pos[3] = 1;
            off = 12 + f.nweights * 4;
        }
        if (f.normal) { memcpy(v.nrm, p + off, 12); off += 12; } else { v.nrm[0] = v.nrm[1] = 0; v.nrm[2] = 0; }
        if (f.psize) off += 4;
        if (f.diffuse) { const u8* c = p + off; v.col[0] = c[2]; v.col[1] = c[1]; v.col[2] = c[0]; v.col[3] = c[3]; off += 4; }
        else { v.col[0] = v.col[1] = v.col[2] = v.col[3] = 255; }
        if (f.specular) { const u8* c = p + off; v.spec[0] = c[2]; v.spec[1] = c[1]; v.spec[2] = c[0]; v.spec[3] = c[3]; off += 4; }
        else { v.spec[0] = v.spec[1] = v.spec[2] = 0; v.spec[3] = 255; }
        v.fog = v.spec[3] / 255.0f;
        for (int t = 0; t < f.ntc && t < 4; t++) {
            const float* tc = (const float*)(p + off);
            v.tc[t][0] = tc[0]; v.tc[t][1] = f.tcsize[t] > 1 ? tc[1] : 0;
            v.tc[t][2] = f.tcsize[t] > 2 ? tc[2] : 0; v.tc[t][3] = f.tcsize[t] > 3 ? tc[3] : 1;
            off += f.tcsize[t] * 4;
        }
        for (int t = f.ntc; t < f.ntc + 0 && t < 4; t++) {}
    }
}

static GLenum gl_prim(u32 t, u32 n, u32& verts) {
    switch (t) {
        case 1: verts = n; return GL_POINTS;
        case 2: verts = n * 2; return GL_LINES;
        case 3: verts = n + 1; return GL_LINE_STRIP;
        case 4: verts = n * 3; return GL_TRIANGLES;
        case 5: verts = n + 2; return GL_TRIANGLE_STRIP;
        case 6: verts = n + 2; return GL_TRIANGLE_FAN;
    }
    verts = 0;
    return GL_POINTS;
}

static void draw(u32 prim, u32 nprims, const u8* vdata, u32 stride, u32 nverts, const void* idx, bool idx32, u32 nidx) {
    if (!nprims) return;
    u32 fvf = st.fvf;
    if (fvf > 0xFFFF) { static bool once = false; if (!once) { logf("draw with vertex shader handle %08x ignored", fvf); once = true; } return; }
    FvfInfo f = parse_fvf(fvf);
    if (!stride) stride = f.stride;
    u32 count; GLenum mode = gl_prim(prim, nprims, count);

    // index range
    u32 vmin = 0, vmax = nverts ? nverts - 1 : 0;
    if (idx) {
        ibuf.resize(nidx);
        vmin = 0xFFFFFFFF; vmax = 0;
        for (u32 i = 0; i < nidx; i++) {
            u32 v = idx32 ? ((const u32*)idx)[i] : ((const u16*)idx)[i];
            ibuf[i] = v; vmin = std::min(vmin, v); vmax = std::max(vmax, v);
        }
        for (auto& v : ibuf) v -= vmin;
    }
    convert_vertices(f, vdata + (size_t)vmin * stride, stride, vmax - vmin + 1);

    bind_render_target();
    apply_raster_state(f.rhw);
    apply_texture_stages(f.rhw, f.ntc);

    if (f.rhw) {
        glDisable(GL_LIGHTING);
        glViewport(0, 0, rt_w * rt_scale, rt_h * rt_scale);
        glDepthRange(0, 1);
        glMatrixMode(GL_PROJECTION);
        // screen (pixel) coords -> NDC with D3D's pixel-centre convention; z in [0,1] -> [-1,1]
        float sx = 2.0f / rt_w, sy = -2.0f / rt_h;
        float m[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, 2, 0, -1 + 0.5f * sx, 1 + 0.5f * sy, -1, 1};
        glLoadMatrixf(m);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
    } else {
        apply_viewport();
        apply_lighting(f.normal, f.diffuse);
        glMatrixMode(GL_PROJECTION);
        static const float zfix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2, 0, 0, 0, -1, 1};
        glLoadMatrixf(zfix);
        glMultMatrixf(st.proj);
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(st.view);
        glMultMatrixf(st.world);
    }

    // secondary (specular) colour sum when specular enabled without lighting
    if (st.rs[29] && f.specular && (!st.rs[137] || f.rhw)) glEnable(GL_COLOR_SUM); else glDisable(GL_COLOR_SUM);

#if HB_GLES
    // Stream the converted vertices and indices through buffer objects: WebGL has no client
    // arrays (Emscripten's emulation uploads too few vertices for indexed draws), and it's
    // faster on ES drivers too.
    static GLuint vbo = 0, ibo = 0;
    if (!vbo) { glGenBuffers(1, &vbo); glGenBuffers(1, &ibo); }
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((vmax - vmin + 1) * sizeof(HVtx)), vbuf.data(), GL_STREAM_DRAW);
    const char* base = nullptr;  // attribute pointers below become offsets into the buffer
#else
    const char* base = (const char*)vbuf.data();
#endif
#define VATTR(m) (base + offsetof(HVtx, m))
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(4, GL_FLOAT, sizeof(HVtx), VATTR(pos));
    if (f.normal && !f.rhw) { glEnableClientState(GL_NORMAL_ARRAY); glNormalPointer(GL_FLOAT, sizeof(HVtx), VATTR(nrm)); }
    else { glDisableClientState(GL_NORMAL_ARRAY); glNormal3f(0, 0, 0); }
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(HVtx), VATTR(col));
    glEnableClientState(GL_SECONDARY_COLOR_ARRAY);
    glSecondaryColorPointer(3, GL_UNSIGNED_BYTE, sizeof(HVtx), VATTR(spec));
    if (f.rhw && st.rs[28] && !st.rs[35]) { glEnableClientState(GL_FOG_COORDINATE_ARRAY); glFogCoordPointer(GL_FLOAT, sizeof(HVtx), VATTR(fog)); }
    else glDisableClientState(GL_FOG_COORDINATE_ARRAY);
    for (int s = 0; s < active_units; s++) {
        u32 tci = st.tss[s][11] & 0xFFFF;
        glClientActiveTexture(GL_TEXTURE0 + s);
        if ((int)tci < f.ntc && tci < 4) glTexCoordPointer(4, GL_FLOAT, sizeof(HVtx), VATTR(tc) + tci * sizeof(float) * 4);
    }
    glClientActiveTexture(GL_TEXTURE0);

#if HB_GLES
    if (idx) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(nidx * 4), ibuf.data(), GL_STREAM_DRAW);
        glDrawElements(mode, (GLsizei)nidx, GL_UNSIGNED_INT, nullptr);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    } else {
        glDrawArrays(mode, 0, (GLsizei)count);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
#else
    if (idx) glDrawElements(mode, (GLsizei)nidx, GL_UNSIGNED_INT, ibuf.data());
    else glDrawArrays(mode, 0, (GLsizei)count);
#endif
}

// ---- IDirect3D8 ------------------------------------------------------------------------------------
static void init_vtables() {
    vt_d3d = make_vtable("IDirect3D8", {"QueryInterface", "AddRef", "Release", "RegisterSoftwareDevice", "GetAdapterCount",
                                        "GetAdapterIdentifier", "GetAdapterModeCount", "EnumAdapterModes", "GetAdapterDisplayMode",
                                        "CheckDeviceType", "CheckDeviceFormat", "CheckDeviceMultiSampleType", "CheckDepthStencilMatch",
                                        "GetDeviceCaps", "GetAdapterMonitor", "CreateDevice"});
    vt_dev = make_vtable("IDirect3DDevice8", {
        "QueryInterface", "AddRef", "Release", "TestCooperativeLevel", "GetAvailableTextureMem", "ResourceManagerDiscardBytes",
        "GetDirect3D", "GetDeviceCaps", "GetDisplayMode", "GetCreationParameters", "SetCursorProperties", "SetCursorPosition",
        "ShowCursor", "CreateAdditionalSwapChain", "Reset", "Present", "GetBackBuffer", "GetRasterStatus", "SetGammaRamp",
        "GetGammaRamp", "CreateTexture", "CreateVolumeTexture", "CreateCubeTexture", "CreateVertexBuffer", "CreateIndexBuffer",
        "CreateRenderTarget", "CreateDepthStencilSurface", "CreateImageSurface", "CopyRects", "UpdateTexture", "GetFrontBuffer",
        "SetRenderTarget", "GetRenderTarget", "GetDepthStencilSurface", "BeginScene", "EndScene", "Clear", "SetTransform",
        "GetTransform", "MultiplyTransform", "SetViewport", "GetViewport", "SetMaterial", "GetMaterial", "SetLight", "GetLight",
        "LightEnable", "GetLightEnable", "SetClipPlane", "GetClipPlane", "SetRenderState", "GetRenderState", "BeginStateBlock",
        "EndStateBlock", "ApplyStateBlock", "CaptureStateBlock", "DeleteStateBlock", "CreateStateBlock", "SetClipStatus",
        "GetClipStatus", "GetTexture", "SetTexture", "GetTextureStageState", "SetTextureStageState", "ValidateDevice", "GetInfo",
        "SetPaletteEntries", "GetPaletteEntries", "SetCurrentTexturePalette", "GetCurrentTexturePalette", "DrawPrimitive",
        "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "ProcessVertices", "CreateVertexShader",
        "SetVertexShader", "GetVertexShader", "DeleteVertexShader", "SetVertexShaderConstant", "GetVertexShaderConstant",
        "GetVertexShaderDeclaration", "GetVertexShaderFunction", "SetStreamSource", "GetStreamSource", "SetIndices", "GetIndices",
        "CreatePixelShader", "SetPixelShader", "GetPixelShader", "DeletePixelShader", "SetPixelShaderConstant",
        "GetPixelShaderConstant", "GetPixelShaderFunction", "DrawRectPatch", "DrawTriPatch", "DeletePatch"});
    vt_tex = make_vtable("IDirect3DTexture8", {"QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData", "GetPrivateData",
                                               "FreePrivateData", "SetPriority", "GetPriority", "PreLoad", "GetType", "SetLOD", "GetLOD",
                                               "GetLevelCount", "GetLevelDesc", "GetSurfaceLevel", "LockRect", "UnlockRect", "AddDirtyRect"});
    vt_surf = make_vtable("IDirect3DSurface8", {"QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData", "GetPrivateData",
                                                "FreePrivateData", "GetContainer", "GetDesc", "LockRect", "UnlockRect"});
    vt_vb = make_vtable("IDirect3DVertexBuffer8", {"QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData", "GetPrivateData",
                                                   "FreePrivateData", "SetPriority", "GetPriority", "PreLoad", "GetType", "Lock", "Unlock", "GetDesc"});
    vt_ib = make_vtable("IDirect3DIndexBuffer8", {"QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData", "GetPrivateData",
                                                  "FreePrivateData", "SetPriority", "GetPriority", "PreLoad", "GetType", "Lock", "Unlock", "GetDesc"});
}

API(d3d8, Direct3DCreate8, 1) {
    if (!vt_d3d) init_vtables();
    if (!d3d_obj) d3d_obj = make_com_object(vt_d3d, 0);
    return d3d_obj;
}
COM(IDirect3D8, QueryInterface, 3) { wr32(c.arg(2), c.arg(0)); return 0; }
COM(IDirect3D8, AddRef, 1) { return 1; }
COM(IDirect3D8, Release, 1) { return 0; }
COM(IDirect3D8, GetAdapterCount, 1) { return 1; }
COM(IDirect3D8, GetAdapterIdentifier, 4) {
    u32 p = c.arg(3);
    memset(g2h(p), 0, 1024 + 8 + 16 + 16 + 4);
    gstrcpy(p, "hbwrap.dll", 512);
    gstrcpy(p + 512, "OpenGL (hbwrap)", 512);
    wr32(p + 1024, 0x0001000A); wr32(p + 1028, 0x00060000);
    wr32(p + 1032, 0x10DE); wr32(p + 1036, 0x0181);  // looks like a GeForce4 to the game
    return 0;
}
static const u32 modes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 960}, {1280, 1024}, {1600, 1200}};
COM(IDirect3D8, GetAdapterModeCount, 2) { return sizeof(modes) / sizeof(modes[0]); }
COM(IDirect3D8, EnumAdapterModes, 4) {
    u32 i = c.arg(2), p = c.arg(3);
    if (i >= sizeof(modes) / sizeof(modes[0])) return D3DERR_INVALIDCALL;
    wr32(p, modes[i][0]); wr32(p + 4, modes[i][1]); wr32(p + 8, 60); wr32(p + 12, F_X8R8G8B8);
    return 0;
}
COM(IDirect3D8, GetAdapterDisplayMode, 3) {
    u32 p = c.arg(2);
    wr32(p, 1600); wr32(p + 4, 1200); wr32(p + 8, 60); wr32(p + 12, F_X8R8G8B8);
    return 0;
}
COM(IDirect3D8, CheckDeviceType, 6) { return 0; }
COM(IDirect3D8, CheckDeviceFormat, 7) {
    u32 fmt = c.arg(6);
    switch (fmt) {
        case F_A8R8G8B8: case F_X8R8G8B8: case F_R5G6B5: case F_X1R5G5B5: case F_A1R5G5B5: case F_A4R4G4B4:
        case F_A8: case F_L8: case F_A8L8: case F_DXT1: case F_DXT3: case F_DXT5: case F_D16: case F_D24S8: case F_D24X8:
        case F_R8G8B8: case F_X4R4G4B4: case F_DXT2: case F_DXT4: case F_D32:
            return 0;
    }
    return D3DERR_NOTAVAILABLE;
}
COM(IDirect3D8, CheckDeviceMultiSampleType, 6) { return c.arg(5) == 0 ? 0 : D3DERR_NOTAVAILABLE; }
COM(IDirect3D8, CheckDepthStencilMatch, 6) { return 0; }
static void fill_caps(u32 p) {
    memset(g2h(p), 0, 212);
    wr32(p + 0, 1);                       // HAL
    wr32(p + 8, 0x00020000);              // Caps: READ_SCANLINE
    wr32(p + 12, 0x20000000 | 0x00020000);// Caps2: CANRENDERWINDOWED | FULLSCREENGAMMA
    wr32(p + 16, 0x20);                   // Caps3: ALPHA_FULLSCREEN_FLIP_OR_DISCARD
    wr32(p + 20, 0x8000000F);             // presentation intervals
    wr32(p + 28, 0x0001FFF0);             // DevCaps (incl. HWTRANSFORMANDLIGHT 0x10000, DRAWPRIMTLVERTEX)
    wr32(p + 32, 0x00000FFF);             // PrimitiveMiscCaps
    wr32(p + 36, 0x00FFFFFF & ~0x00000200);// RasterCaps
    wr32(p + 40, 0xFF); wr32(p + 44, 0x1FFF); wr32(p + 48, 0x1FFF); wr32(p + 52, 0xFF);
    wr32(p + 56, 0x000FFFFF);             // ShadeCaps
    wr32(p + 60, 0x00000005 | 0x00000004 | 0x00000040 | 0x00000400 | 0x00010000);  // PERSPECTIVE|ALPHA|MIPMAP|PROJECTED|...
    wr32(p + 64, 0x03030303); wr32(p + 68, 0x03030303); wr32(p + 72, 0x03030303);
    wr32(p + 76, 0x3F); wr32(p + 80, 0x3F); wr32(p + 84, 0x1F);
    wr32(p + 88, 4096); wr32(p + 92, 4096); wr32(p + 96, 256); wr32(p + 100, 8192); wr32(p + 104, 4096); wr32(p + 108, 16);
    wrf(p + 112, 1e10f);
    wrf(p + 116, -1e8f); wrf(p + 120, -1e8f); wrf(p + 124, 1e8f); wrf(p + 128, 1e8f);
    wr32(p + 136, 0xFF);                  // StencilCaps
    wr32(p + 140, 0x00080008);            // FVFCaps: 8 texcoords
    wr32(p + 144, 0x03FFFFFF);            // TextureOpCaps
    wr32(p + 148, 8); wr32(p + 152, (u32)g_max_units);
    wr32(p + 156, 0x3B);                  // VertexProcessingCaps
    wr32(p + 160, 8); wr32(p + 164, 0); wr32(p + 168, 0); wr32(p + 172, 0);
    wrf(p + 176, 64.0f);
    wr32(p + 180, 0xFFFFF); wr32(p + 184, 0xFFFFF); wr32(p + 188, 1); wr32(p + 192, 255);
    wr32(p + 196, 0xFFFE0000);            // no vertex shaders
    wr32(p + 200, 0);
    wr32(p + 204, 0xFFFF0000);            // no pixel shaders
    wrf(p + 208, 0.0f);
}
COM(IDirect3D8, GetDeviceCaps, 4) { fill_caps(c.arg(3)); return 0; }
COM(IDirect3D8, GetAdapterMonitor, 2) { return 0x4D000001; }

static void read_present_params(u32 pp) {
    u32 w = rd32(pp), h = rd32(pp + 4);
    windowed = rd32(pp + 28) != 0;
    if (!w || !h) { w = plat::client_w; h = plat::client_h; }
    bb_w = w; bb_h = h;
    if (rd32(pp + 8)) bb_fmt = rd32(pp + 8);
    logf("D3D8 present params: %ux%u fmt=%u windowed=%d autodepth=%u depthfmt=%u", w, h, rd32(pp + 8), windowed, rd32(pp + 32), rd32(pp + 36));
    plat::ensure_window(w, h);
    plat::set_client_size(w, h);
    bool want_fs = !windowed && !getenv("HB_WINDOWED");
    bool is_fs = SDL_GetWindowFlags(plat::window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (want_fs != is_fs) SDL_SetWindowFullscreen(plat::window, want_fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

COM(IDirect3D8, CreateDevice, 7) {
    read_present_params(c.arg(5));
    SDL_GL_MakeCurrent(plat::window, plat::gl);
    logf("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    GLint units; glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    g_max_units = std::min(4, (int)units);
    glGenTextures(1, &white_tex);
    glBindTexture(GL_TEXTURE_2D, white_tex);
    u32 white = 0xFFFFFFFF;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    create_backbuffer();
    default_state();
    Res* bb = new Res(); bb->kind = R_SURFACE; bb->backbuffer = true; bb->w = bb_w; bb->h = bb_h; bb->fmt = bb_fmt; bb->refs = 1000000;
    bb_surf = new_res(bb, vt_surf);
    Res* ds = new Res(); ds->kind = R_SURFACE; ds->depth = true; ds->w = bb_w; ds->h = bb_h; ds->fmt = F_D24S8; ds->refs = 1000000;
    depth_surf = new_res(ds, vt_surf);
    cur_rt = bb_surf; cur_ds = depth_surf;
    dev_obj = make_com_object(vt_dev, 0);
    wr32(c.arg(6), dev_obj);
    return 0;
}

// ---- IDirect3DDevice8 ---------------------------------------------------------------------------------
COM(IDirect3DDevice8, QueryInterface, 3) { wr32(c.arg(2), c.arg(0)); return 0; }
COM(IDirect3DDevice8, AddRef, 1) { return 2; }
COM(IDirect3DDevice8, Release, 1) { return 1; }
COM(IDirect3DDevice8, TestCooperativeLevel, 1) { return 0; }
COM(IDirect3DDevice8, GetAvailableTextureMem, 1) { return 256u << 20; }
COM(IDirect3DDevice8, ResourceManagerDiscardBytes, 2) { return 0; }
COM(IDirect3DDevice8, GetDirect3D, 2) { wr32(c.arg(1), d3d_obj); return 0; }
COM(IDirect3DDevice8, GetDeviceCaps, 2) { fill_caps(c.arg(1)); return 0; }
COM(IDirect3DDevice8, GetDisplayMode, 2) {
    u32 p = c.arg(1);
    wr32(p, bb_w); wr32(p + 4, bb_h); wr32(p + 8, 60); wr32(p + 12, bb_fmt);
    return 0;
}
COM(IDirect3DDevice8, GetCreationParameters, 2) {
    u32 p = c.arg(1);
    wr32(p, 0); wr32(p + 4, 1); wr32(p + 8, plat::main_hwnd); wr32(p + 12, 0x40);
    return 0;
}
COM(IDirect3DDevice8, SetCursorProperties, 4) { return 0; }
COM(IDirect3DDevice8, SetCursorPosition, 4) { return 0; }
COM(IDirect3DDevice8, ShowCursor, 2) { return 0; }
COM(IDirect3DDevice8, Reset, 2) {
    read_present_params(c.arg(1));
    create_backbuffer();
    res[bb_surf]->w = bb_w; res[bb_surf]->h = bb_h;
    res[depth_surf]->w = bb_w; res[depth_surf]->h = bb_h;
    default_state();
    cur_rt = bb_surf; cur_ds = depth_surf;
    return 0;
}

void script_tick(u32 frame);
void d3d_screenshot(const char* path) {
    GLint prev; glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &prev);
    bind_fb(bb_fbo);
    u32 W = bb_w * bb_scale, H = bb_h * bb_scale;
    std::vector<u8> px((size_t)W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    bind_fb(prev);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", W, H);
    for (int yy = (int)H - 1; yy >= 0; yy--) for (u32 xx = 0; xx < W; xx++) fwrite(&px[((size_t)yy * W + xx) * 4], 1, 3, f);
    fclose(f);
    logf("screenshot -> %s", path);
}
static void present_frame() {
    int x, y, w, h;
    plat::present_rect(x, y, w, h);
    int dw, dh; plat::drawable_size(dw, dh);
#if HB_GLES
    // OpenGL ES 2 has no framebuffer blit: draw the back buffer texture as a quad instead.
    // (Every draw call sets the state it needs, so nothing here has to be restored.)
    bind_fb(0);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST); glDisable(GL_CULL_FACE); glDisable(GL_FOG); glDisable(GL_LIGHTING); glDisable(GL_COLOR_SUM);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, dw, dh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    for (int u = 1; u < g_max_units; u++) { glActiveTexture(GL_TEXTURE0 + u); glDisable(GL_TEXTURE_2D); }
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, bb_color);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glMatrixMode(GL_TEXTURE); glLoadIdentity();
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, dw, 0, dh, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisableClientState(GL_VERTEX_ARRAY); glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_SECONDARY_COLOR_ARRAY); glDisableClientState(GL_FOG_COORDINATE_ARRAY);
    glClientActiveTexture(GL_TEXTURE0); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f((float)x, (float)y);
    glTexCoord2f(1, 0); glVertex2f((float)(x + w), (float)y);
    glTexCoord2f(1, 1); glVertex2f((float)(x + w), (float)(y + h));
    glTexCoord2f(0, 1); glVertex2f((float)x, (float)(y + h));
    glEnd();
    glFlush();  // gl4es queues immediate-mode draws; SDL swaps through EGL without telling it
#else
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, bb_fbo);
    glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, 0);
    bound_fb = ~0u;
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, dw, dh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebufferEXT(0, 0, bb_w * bb_scale, bb_h * bb_scale, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
#endif
    SDL_GL_SwapWindow(plat::window);
#ifdef __EMSCRIPTEN__
    web_yield(0);  // the browser shows the canvas once we return to it
    web_frame_done();
#endif
    bind_fb(bb_fbo);
    frame_no++;
    static u32 fps_t0 = SDL_GetTicks(), fps_n = 0;
    fps_n++;
    if (getenv("HB_FPS") && SDL_GetTicks() - fps_t0 >= 2000) {
        logf("fps: %.1f", fps_n * 1000.0 / (SDL_GetTicks() - fps_t0));
        fps_t0 = SDL_GetTicks(); fps_n = 0;
    }
    script_tick(frame_no);
}
COM(IDirect3DDevice8, Present, 5) {
    present_frame();
    platform_pump();
    return 0;
}
COM(IDirect3DDevice8, GetBackBuffer, 4) {
    res[bb_surf]->refs++;
    wr32(c.arg(3), res[bb_surf]->obj);
    return 0;
}
COM(IDirect3DDevice8, GetRasterStatus, 2) { wr32(c.arg(1), 1); wr32(c.arg(1) + 4, 0); return 0; }
COM(IDirect3DDevice8, SetGammaRamp, 3) { return 0; }
COM(IDirect3DDevice8, GetGammaRamp, 2) {
    u32 p = c.arg(1);
    for (int i = 0; i < 256; i++) { wr16(p + i * 2, (u16)(i * 257)); wr16(p + 512 + i * 2, (u16)(i * 257)); wr16(p + 1024 + i * 2, (u16)(i * 257)); }
    return 0;
}

COM(IDirect3DDevice8, CreateTexture, 8) {
    u32 w = c.arg(1), h = c.arg(2), levels = c.arg(3), usage = c.arg(4), fmt = c.arg(5), pool = c.arg(6);
    if (!w || !h) return D3DERR_INVALIDCALL;
    Res* r = new Res(); r->kind = R_TEXTURE; r->fmt = fmt; r->usage = usage; r->pool = pool;
    r->render_target = usage & 1;
    if (levels == 0) { levels = 1; u32 a = w, b = h; while (a > 1 || b > 1) { a = std::max(1u, a / 2); b = std::max(1u, b / 2); levels++; } }
    if (r->render_target) levels = 1;
    u32 lw = w, lh = h;
    for (u32 i = 0; i < levels; i++) {
        Level l; l.w = lw; l.h = lh; l.pitch = level_pitch(fmt, lw);
        l.addr = galloc(l.pitch * level_rows(fmt, lh) + 16);
        l.dirty = true;
        r->levels.push_back(l);
        lw = std::max(1u, lw / 2); lh = std::max(1u, lh / 2);
    }
    int id = new_res(r, vt_tex);
    wr32(c.arg(7), r->obj);
    (void)id;
    return 0;
}
COM(IDirect3DDevice8, CreateVolumeTexture, 9) { return D3DERR_NOTAVAILABLE; }
COM(IDirect3DDevice8, CreateCubeTexture, 7) { return D3DERR_NOTAVAILABLE; }
COM(IDirect3DDevice8, CreateVertexBuffer, 6) {
    Res* r = new Res(); r->kind = R_VB; r->size = c.arg(1); r->usage = c.arg(2); r->fvf = c.arg(3); r->pool = c.arg(4);
    r->buf = galloc(r->size + 16);
    new_res(r, vt_vb);
    wr32(c.arg(5), r->obj);
    return 0;
}
COM(IDirect3DDevice8, CreateIndexBuffer, 6) {
    Res* r = new Res(); r->kind = R_IB; r->size = c.arg(1); r->usage = c.arg(2); r->fmt = c.arg(3); r->pool = c.arg(4);
    r->buf = galloc(r->size + 16);
    new_res(r, vt_ib);
    wr32(c.arg(5), r->obj);
    return 0;
}
static u32 make_surface(u32 w, u32 h, u32 fmt) {
    Res* r = new Res(); r->kind = R_SURFACE; r->w = w; r->h = h; r->fmt = fmt;
    r->pitch = level_pitch(fmt, w);
    r->addr = galloc(r->pitch * level_rows(fmt, h) + 16);
    new_res(r, vt_surf);
    return r->obj;
}
COM(IDirect3DDevice8, CreateRenderTarget, 7) {
    // Offscreen render targets: create as a 1-level render-target texture and hand out its surface.
    Res* t = new Res(); t->kind = R_TEXTURE; t->fmt = c.arg(3); t->render_target = true;
    Level l; l.w = c.arg(1); l.h = c.arg(2); l.pitch = l.w * 4; l.addr = galloc(l.pitch * l.h); l.dirty = true;
    t->levels.push_back(l);
    int tid = new_res(t, vt_tex);
    Res* s = new Res(); s->kind = R_SURFACE; s->tex_res = tid; s->tex_level = 0; s->w = l.w; s->h = l.h; s->fmt = t->fmt;
    new_res(s, vt_surf);
    wr32(c.arg(6), s->obj);
    return 0;
}
COM(IDirect3DDevice8, CreateDepthStencilSurface, 6) {
    Res* r = new Res(); r->kind = R_SURFACE; r->depth = true; r->w = c.arg(1); r->h = c.arg(2); r->fmt = c.arg(3);
    new_res(r, vt_surf);
    wr32(c.arg(5), r->obj);
    return 0;
}
COM(IDirect3DDevice8, CreateImageSurface, 5) {
    wr32(c.arg(4), make_surface(c.arg(1), c.arg(2), c.arg(3)));
    return 0;
}

// Surface storage accessors
struct SurfMem { u32 addr, pitch, w, h, fmt; Level* level; Res* tex; };
static bool surf_mem(Res* s, SurfMem& m) {
    if (s->tex_res >= 0) {
        Res* t = res[s->tex_res];
        if (s->tex_level >= (int)t->levels.size()) return false;
        Level& l = t->levels[s->tex_level];
        m = {l.addr, l.pitch, l.w, l.h, t->fmt, &l, t};
        return true;
    }
    if (s->backbuffer) {
        // materialise back buffer contents into guest memory on demand
        if (!s->addr) { s->pitch = bb_w * 4; s->addr = galloc(s->pitch * bb_h); }
        GLint prev; glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &prev);
        bind_fb(bb_fbo);
        u32 W = bb_w * bb_scale, H = bb_h * bb_scale;
        std::vector<u8> tmp((size_t)W * H * 4);
        glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, tmp.data());
        for (u32 y = 0; y < bb_h; y++)
            for (u32 x = 0; x < bb_w; x++)
                memcpy(g2h(s->addr + y * s->pitch + x * 4), &tmp[((size_t)(H - 1 - y * bb_scale) * W + x * bb_scale) * 4], 4);
        bind_fb(prev);
        m = {s->addr, s->pitch, bb_w, bb_h, F_X8R8G8B8, nullptr, nullptr};
        return true;
    }
    if (!s->addr) return false;
    m = {s->addr, s->pitch, s->w, s->h, s->fmt, nullptr, nullptr};
    return true;
}

static void copy_rect(const SurfMem& src, const SurfMem& dst, int sx, int sy, int w, int h, int dx, int dy) {
    if (src.fmt != dst.fmt && !(is_dxt(src.fmt) || is_dxt(dst.fmt))) {
        // format conversion via RGBA8 -> only to 32-bit targets supported
        if (dst.fmt == F_A8R8G8B8 || dst.fmt == F_X8R8G8B8) {
            std::vector<u8> row(w * 4);
            for (int y = 0; y < h; y++) {
                convert_row(src.fmt, g2h(src.addr + (sy + y) * src.pitch) + sx * fmt_bpp(src.fmt), row.data(), w);
                u8* d = g2h(dst.addr + (dy + y) * dst.pitch) + dx * 4;
                for (int x = 0; x < w; x++) { d[x * 4] = row[x * 4 + 2]; d[x * 4 + 1] = row[x * 4 + 1]; d[x * 4 + 2] = row[x * 4]; d[x * 4 + 3] = row[x * 4 + 3]; }
            }
            return;
        }
        logf("CopyRects: unsupported format conversion %u -> %u", src.fmt, dst.fmt);
        return;
    }
    if (is_dxt(src.fmt)) {
        int rows = (int)level_rows(src.fmt, h), rowbytes = (int)level_pitch(src.fmt, w);
        for (int y = 0; y < rows; y++)
            memcpy(g2h(dst.addr + (dy / 4 + y) * dst.pitch) + (dx / 4) * (src.fmt == F_DXT1 ? 8 : 16),
                   g2h(src.addr + (sy / 4 + y) * src.pitch) + (sx / 4) * (src.fmt == F_DXT1 ? 8 : 16), rowbytes);
        return;
    }
    u32 bpp = fmt_bpp(src.fmt);
    for (int y = 0; y < h; y++)
        memcpy(g2h(dst.addr + (dy + y) * dst.pitch) + dx * bpp, g2h(src.addr + (sy + y) * src.pitch) + sx * bpp, w * bpp);
}

COM(IDirect3DDevice8, CopyRects, 6) {
    Res* s = R(c.arg(1)); Res* d = R(c.arg(4));
    u32 rects = c.arg(2), n = c.arg(3), pts = c.arg(5);
    if (!s || !d) return D3DERR_INVALIDCALL;
    if (d->backbuffer || (d->tex_res >= 0 && res[d->tex_res]->render_target)) {
        // upload into the GL render target via a temporary texture draw
        SurfMem sm; if (!surf_mem(s, sm)) return D3DERR_INVALIDCALL;
        GLuint tmp; glGenTextures(1, &tmp); glBindTexture(GL_TEXTURE_2D, tmp);
        std::vector<u8> px((size_t)sm.w * sm.h * 4);
        for (u32 y = 0; y < sm.h; y++) convert_row(sm.fmt, g2h(sm.addr + y * sm.pitch), &px[(size_t)y * sm.w * 4], sm.w);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sm.w, sm.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        u32 cnt = n ? n : 1;
        int prev_rt = cur_rt; cur_rt = RID(c.arg(4)); bind_render_target();
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST); glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE);
        glDisable(GL_FOG); glDisable(GL_STENCIL_TEST);
        glActiveTexture(GL_TEXTURE0); glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, tmp);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glViewport(0, 0, rt_w * rt_scale, rt_h * rt_scale);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, rt_w, rt_h, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glMatrixMode(GL_TEXTURE); glLoadIdentity();
        glDisableClientState(GL_VERTEX_ARRAY); glDisableClientState(GL_COLOR_ARRAY);
        glColor4f(1, 1, 1, 1);
        for (u32 i = 0; i < cnt; i++) {
            int sx = 0, sy = 0, w = (int)sm.w, h = (int)sm.h, dx = 0, dy = 0;
            if (n && rects) { sx = rd32(rects + i * 16); sy = rd32(rects + i * 16 + 4); w = (int)rd32(rects + i * 16 + 8) - sx; h = (int)rd32(rects + i * 16 + 12) - sy; }
            if (n && pts) { dx = rd32(pts + i * 8); dy = rd32(pts + i * 8 + 4); } else { dx = sx; dy = sy; }
            float u0 = (float)sx / sm.w, v0 = (float)sy / sm.h, u1 = (float)(sx + w) / sm.w, v1 = (float)(sy + h) / sm.h;
            glBegin(GL_QUADS);
            glTexCoord2f(u0, v0); glVertex2f((float)dx, (float)dy);
            glTexCoord2f(u1, v0); glVertex2f((float)(dx + w), (float)dy);
            glTexCoord2f(u1, v1); glVertex2f((float)(dx + w), (float)(dy + h));
            glTexCoord2f(u0, v1); glVertex2f((float)dx, (float)(dy + h));
            glEnd();
        }
        glPopAttrib();
        glDeleteTextures(1, &tmp);
        cur_rt = prev_rt;
        return 0;
    }
    SurfMem sm, dm;
    if (!surf_mem(s, sm) || !surf_mem(d, dm)) return D3DERR_INVALIDCALL;
    if (!n || !rects) copy_rect(sm, dm, 0, 0, (int)std::min(sm.w, dm.w), (int)std::min(sm.h, dm.h), 0, 0);
    else for (u32 i = 0; i < n; i++) {
        int l = rd32(rects + i * 16), t = rd32(rects + i * 16 + 4), r = rd32(rects + i * 16 + 8), b = rd32(rects + i * 16 + 12);
        int dx = pts ? (int)rd32(pts + i * 8) : l, dy = pts ? (int)rd32(pts + i * 8 + 4) : t;
        copy_rect(sm, dm, l, t, r - l, b - t, dx, dy);
    }
    if (dm.level) dm.level->dirty = true;
    return 0;
}
COM(IDirect3DDevice8, UpdateTexture, 3) {
    Res* s = R(c.arg(1)); Res* d = R(c.arg(2));
    if (!s || !d || s->kind != R_TEXTURE || d->kind != R_TEXTURE) return D3DERR_INVALIDCALL;
    for (size_t i = 0; i < d->levels.size() && i < s->levels.size(); i++) {
        Level& sl = s->levels[i]; Level& dl = d->levels[i];
        u32 rows = level_rows(s->fmt, sl.h);
        for (u32 y = 0; y < rows; y++) memcpy(g2h(dl.addr + y * dl.pitch), g2h(sl.addr + y * sl.pitch), std::min(sl.pitch, dl.pitch));
        dl.dirty = true;
    }
    return 0;
}
COM(IDirect3DDevice8, GetFrontBuffer, 2) { return 0; }
COM(IDirect3DDevice8, SetRenderTarget, 3) {
    int rt = RID(c.arg(1)), ds = RID(c.arg(2));
    if (rt >= 0) {
        if (cur_rt >= 0 && cur_rt != rt) {
            Res* old = res[cur_rt];
            if (old->tex_res >= 0) { Res* t = res[old->tex_res]; (void)t; }
        }
        cur_rt = rt;
    }
    cur_ds = ds;
    bind_render_target();
    // D3D resets the viewport to the full render target
    st.vp[0] = st.vp[1] = 0; st.vp[2] = rt_w; st.vp[3] = rt_h;
    return 0;
}
COM(IDirect3DDevice8, GetRenderTarget, 2) {
    int id = cur_rt >= 0 ? cur_rt : bb_surf;
    addref_res(id);
    wr32(c.arg(1), res[id]->obj);
    return 0;
}
COM(IDirect3DDevice8, GetDepthStencilSurface, 2) {
    if (cur_ds < 0) { wr32(c.arg(1), 0); return 0x8876086A; }
    addref_res(cur_ds);
    wr32(c.arg(1), res[cur_ds]->obj);
    return 0;
}
COM(IDirect3DDevice8, BeginScene, 1) { return 0; }
COM(IDirect3DDevice8, EndScene, 1) { return 0; }
COM(IDirect3DDevice8, Clear, 7) {
    u32 count = c.arg(1), rects = c.arg(2), flags = c.arg(3), color = c.arg(4);
    float z = c.farg(5); u32 stencil = c.arg(6);
    bind_render_target();
    GLbitfield mask = 0;
    if (flags & 1) { float col[4]; color4(color, col); glClearColor(col[0], col[1], col[2], col[3]); mask |= GL_COLOR_BUFFER_BIT; }
    if (flags & 2) { glClearDepth(z); mask |= GL_DEPTH_BUFFER_BIT; }
    if (flags & 4) { glClearStencil((GLint)stencil); mask |= GL_STENCIL_BUFFER_BIT; }
    glColorMask(1, 1, 1, 1); glDepthMask(GL_TRUE); glStencilMask(0xFFFFFFFF);
    glEnable(GL_SCISSOR_TEST);
    if (count && rects) {
        for (u32 i = 0; i < count; i++) {
            i32 x1 = (i32)rd32(rects + i * 16), y1 = (i32)rd32(rects + i * 16 + 4), x2 = (i32)rd32(rects + i * 16 + 8), y2 = (i32)rd32(rects + i * 16 + 12);
            GLint k = (GLint)rt_scale;
            glScissor(x1 * k, ((GLint)rt_h - y2) * k, (x2 - x1) * k, (y2 - y1) * k);
            glClear(mask);
        }
    } else {
        GLint k = (GLint)rt_scale;
        glScissor((GLint)st.vp[0] * k, ((GLint)rt_h - (GLint)(st.vp[1] + st.vp[3])) * k, (GLsizei)st.vp[2] * k, (GLsizei)st.vp[3] * k);
        glClear(mask);
    }
    glDisable(GL_SCISSOR_TEST);
    return 0;
}

// ---- state setters (with state-block recording) -----------------------------------------------------
struct StateBlock {
    State s;
    std::vector<bool> rs_set, tss_set, tex_set, light_set;
    bool world, view, proj, texm[8], material, vp, fvf, stream, indices;
    bool all;
};
static std::map<u32, StateBlock*> blocks;
static u32 next_block = 1;
static StateBlock* recording = nullptr;

static StateBlock* new_block(bool all) {
    StateBlock* b = new StateBlock();
    b->rs_set.assign(256, all); b->tss_set.assign(8 * 32, all); b->tex_set.assign(8, all);
    b->world = b->view = b->proj = b->material = b->vp = b->fvf = b->stream = b->indices = all;
    for (int i = 0; i < 8; i++) b->texm[i] = all;
    b->all = all;
    return b;
}
static State& target_state() { return recording ? recording->s : st; }

static float* transform_ptr(State& s, u32 t) {
    if (t == 2) return s.view;
    if (t == 3) return s.proj;
    if (t >= 16 && t < 24) return s.texm[t - 16];
    if (t == 256) return s.world;
    return nullptr;
}
static void mark_transform(u32 t) {
    if (!recording) return;
    if (t == 2) recording->view = true; else if (t == 3) recording->proj = true;
    else if (t >= 16 && t < 24) recording->texm[t - 16] = true; else if (t == 256) recording->world = true;
}
COM(IDirect3DDevice8, SetTransform, 3) {
    float* m = transform_ptr(target_state(), c.arg(1));
    if (m) { memcpy(m, g2h(c.arg(2)), 64); mark_transform(c.arg(1)); }
    return 0;
}
COM(IDirect3DDevice8, GetTransform, 3) {
    float* m = transform_ptr(st, c.arg(1));
    memcpy(g2h(c.arg(2)), m ? m : IDENT, 64);
    return 0;
}
static void matmul(const float* a, const float* b, float* out) {  // row-vector convention: out = a * b
    float r[16];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0; for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
        r[i * 4 + j] = s;
    }
    memcpy(out, r, 64);
}
COM(IDirect3DDevice8, MultiplyTransform, 3) {
    float* m = transform_ptr(target_state(), c.arg(1));
    if (m) { matmul(m, (const float*)g2h(c.arg(2)), m); mark_transform(c.arg(1)); }
    return 0;
}
COM(IDirect3DDevice8, SetViewport, 2) {
    memcpy(target_state().vp, g2h(c.arg(1)), 24);
    if (recording) recording->vp = true;
    return 0;
}
COM(IDirect3DDevice8, GetViewport, 2) { memcpy(g2h(c.arg(1)), st.vp, 24); return 0; }
COM(IDirect3DDevice8, SetMaterial, 2) {
    memcpy(target_state().material, g2h(c.arg(1)), 68);
    if (recording) recording->material = true;
    return 0;
}
COM(IDirect3DDevice8, GetMaterial, 2) { memcpy(g2h(c.arg(1)), st.material, 68); return 0; }
COM(IDirect3DDevice8, SetLight, 3) {
    Light& L = target_state().lights[c.arg(1)];
    memcpy(L.raw, g2h(c.arg(2)), 104);
    L.set = true;
    return 0;
}
COM(IDirect3DDevice8, GetLight, 3) {
    auto it = st.lights.find(c.arg(1));
    if (it == st.lights.end()) return D3DERR_INVALIDCALL;
    memcpy(g2h(c.arg(2)), it->second.raw, 104);
    return 0;
}
COM(IDirect3DDevice8, LightEnable, 3) {
    Light& L = target_state().lights[c.arg(1)];
    if (!L.set) {
        // default light: white directional along +z
        memset(L.raw, 0, 104);
        u32 t = 3; memcpy(L.raw, &t, 4);
        float* f = (float*)(L.raw + 4); f[0] = f[1] = f[2] = 1; f[17] = 1;
        L.set = true;
    }
    L.enabled = c.arg(2) != 0;
    return 0;
}
COM(IDirect3DDevice8, GetLightEnable, 3) {
    auto it = st.lights.find(c.arg(1));
    wr32(c.arg(2), it != st.lights.end() && it->second.enabled ? 1 : 0);
    return 0;
}
COM(IDirect3DDevice8, SetClipPlane, 3) { return 0; }
COM(IDirect3DDevice8, GetClipPlane, 3) { memset(g2h(c.arg(2)), 0, 16); return 0; }
COM(IDirect3DDevice8, SetRenderState, 3) {
    u32 s = c.arg(1);
    if (s < 256) { target_state().rs[s] = c.arg(2); if (recording) recording->rs_set[s] = true; }
    return 0;
}
COM(IDirect3DDevice8, GetRenderState, 3) { wr32(c.arg(2), c.arg(1) < 256 ? st.rs[c.arg(1)] : 0); return 0; }
COM(IDirect3DDevice8, SetTextureStageState, 4) {
    u32 s = c.arg(1), t = c.arg(2);
    if (s < 8 && t < 32) { target_state().tss[s][t] = c.arg(3); if (recording) recording->tss_set[s * 32 + t] = true; }
    return 0;
}
COM(IDirect3DDevice8, GetTextureStageState, 4) {
    u32 s = c.arg(1), t = c.arg(2);
    wr32(c.arg(3), s < 8 && t < 32 ? st.tss[s][t] : 0);
    return 0;
}
COM(IDirect3DDevice8, SetTexture, 3) {
    u32 s = c.arg(1);
    if (s >= 8) return D3DERR_INVALIDCALL;
    int id = RID(c.arg(2));
    if (recording) { recording->s.textures[s] = id; recording->tex_set[s] = true; return 0; }
    if (st.textures[s] == id) return 0;
    addref_res(id);
    int old = st.textures[s];
    st.textures[s] = id;
    release_res(old);
    return 0;
}
COM(IDirect3DDevice8, GetTexture, 3) {
    u32 s = c.arg(1);
    int id = s < 8 ? st.textures[s] : -1;
    if (id >= 0) { addref_res(id); wr32(c.arg(2), res[id]->obj); } else wr32(c.arg(2), 0);
    return 0;
}

static void apply_block(StateBlock* b) {
    State& s = b->s;
    for (int i = 0; i < 256; i++) if (b->rs_set[i]) st.rs[i] = s.rs[i];
    for (int i = 0; i < 8 * 32; i++) if (b->tss_set[i]) st.tss[i / 32][i % 32] = s.tss[i / 32][i % 32];
    for (int i = 0; i < 8; i++) if (b->tex_set[i] && st.textures[i] != s.textures[i]) {
        addref_res(s.textures[i]); int old = st.textures[i]; st.textures[i] = s.textures[i]; release_res(old);
    }
    if (b->world) memcpy(st.world, s.world, 64);
    if (b->view) memcpy(st.view, s.view, 64);
    if (b->proj) memcpy(st.proj, s.proj, 64);
    for (int i = 0; i < 8; i++) if (b->texm[i]) memcpy(st.texm[i], s.texm[i], 64);
    if (b->material) memcpy(st.material, s.material, 68);
    if (b->vp) memcpy(st.vp, s.vp, 24);
    if (b->fvf) st.fvf = s.fvf;
    if (b->stream) { st.stream_vb = s.stream_vb; st.stream_stride = s.stream_stride; }
    if (b->indices) { st.ib = s.ib; st.base_vertex = s.base_vertex; }
    for (auto& kv : s.lights) st.lights[kv.first] = kv.second;
}
static void capture_block(StateBlock* b) {
    State& s = b->s;
    for (int i = 0; i < 256; i++) if (b->rs_set[i]) s.rs[i] = st.rs[i];
    for (int i = 0; i < 8 * 32; i++) if (b->tss_set[i]) s.tss[i / 32][i % 32] = st.tss[i / 32][i % 32];
    for (int i = 0; i < 8; i++) if (b->tex_set[i]) s.textures[i] = st.textures[i];
    if (b->world) memcpy(s.world, st.world, 64);
    if (b->view) memcpy(s.view, st.view, 64);
    if (b->proj) memcpy(s.proj, st.proj, 64);
    for (int i = 0; i < 8; i++) if (b->texm[i]) memcpy(s.texm[i], st.texm[i], 64);
    if (b->material) memcpy(s.material, st.material, 68);
    if (b->vp) memcpy(s.vp, st.vp, 24);
    if (b->fvf) s.fvf = st.fvf;
    if (b->stream) { s.stream_vb = st.stream_vb; s.stream_stride = st.stream_stride; }
    if (b->indices) { s.ib = st.ib; s.base_vertex = st.base_vertex; }
    if (b->all) s.lights = st.lights;
    else for (auto& kv : s.lights) if (st.lights.count(kv.first)) kv.second = st.lights[kv.first];
}
COM(IDirect3DDevice8, BeginStateBlock, 1) {
    recording = new_block(false);
    recording->s = st;
    recording->s.lights.clear();
    return 0;
}
COM(IDirect3DDevice8, EndStateBlock, 2) {
    if (!recording) return D3DERR_INVALIDCALL;
    u32 id = next_block++;
    blocks[id] = recording;
    recording = nullptr;
    wr32(c.arg(1), id);
    return 0;
}
COM(IDirect3DDevice8, ApplyStateBlock, 2) {
    auto it = blocks.find(c.arg(1));
    if (it != blocks.end()) apply_block(it->second);
    return 0;
}
COM(IDirect3DDevice8, CaptureStateBlock, 2) {
    auto it = blocks.find(c.arg(1));
    if (it != blocks.end()) capture_block(it->second);
    return 0;
}
COM(IDirect3DDevice8, DeleteStateBlock, 2) {
    auto it = blocks.find(c.arg(1));
    if (it != blocks.end()) { delete it->second; blocks.erase(it); }
    return 0;
}
COM(IDirect3DDevice8, CreateStateBlock, 3) {
    StateBlock* b = new_block(true);
    capture_block(b);
    u32 id = next_block++;
    blocks[id] = b;
    wr32(c.arg(2), id);
    return 0;
}
COM(IDirect3DDevice8, SetClipStatus, 2) { return 0; }
COM(IDirect3DDevice8, GetClipStatus, 2) { memset(g2h(c.arg(1)), 0, 24); return 0; }
COM(IDirect3DDevice8, ValidateDevice, 2) { wr32(c.arg(1), 1); return 0; }
COM(IDirect3DDevice8, GetInfo, 4) { return 1; }
COM(IDirect3DDevice8, SetPaletteEntries, 3) { return 0; }
COM(IDirect3DDevice8, GetPaletteEntries, 3) { return 0; }
COM(IDirect3DDevice8, SetCurrentTexturePalette, 2) { return 0; }
COM(IDirect3DDevice8, GetCurrentTexturePalette, 2) { wr32(c.arg(1), 0); return 0; }

COM(IDirect3DDevice8, DrawPrimitive, 4) {
    if (st.stream_vb < 0) return D3DERR_INVALIDCALL;
    Res* vb = res[st.stream_vb];
    u32 count; gl_prim(c.arg(1), c.arg(3), count);
    u32 stride = st.stream_stride;
    draw(c.arg(1), c.arg(3), g2h(vb->buf) + (size_t)c.arg(2) * stride, stride, count, nullptr, false, 0);
    return 0;
}
COM(IDirect3DDevice8, DrawIndexedPrimitive, 6) {
    if (st.stream_vb < 0 || st.ib < 0) return D3DERR_INVALIDCALL;
    Res* vb = res[st.stream_vb]; Res* ib = res[st.ib];
    u32 count; gl_prim(c.arg(1), c.arg(5), count);
    bool i32b = ib->fmt == F_INDEX32;
    const u8* idx = g2h(ib->buf) + (size_t)c.arg(4) * (i32b ? 4 : 2);
    draw(c.arg(1), c.arg(5), g2h(vb->buf) + (size_t)st.base_vertex * st.stream_stride, st.stream_stride, 0, idx, i32b, count);
    return 0;
}
COM(IDirect3DDevice8, DrawPrimitiveUP, 5) {
    u32 count; gl_prim(c.arg(1), c.arg(2), count);
    draw(c.arg(1), c.arg(2), g2h(c.arg(3)), c.arg(4), count, nullptr, false, 0);
    // D3D8 resets stream 0 after UP draws
    if (st.stream_vb >= 0) { release_res(st.stream_vb); st.stream_vb = -1; }
    return 0;
}
COM(IDirect3DDevice8, DrawIndexedPrimitiveUP, 9) {
    u32 count; gl_prim(c.arg(1), c.arg(4), count);
    draw(c.arg(1), c.arg(4), g2h(c.arg(7)), c.arg(8), 0, g2h(c.arg(5)), c.arg(6) == F_INDEX32, count);
    if (st.stream_vb >= 0) { release_res(st.stream_vb); st.stream_vb = -1; }
    if (st.ib >= 0) { release_res(st.ib); st.ib = -1; }
    return 0;
}
COM(IDirect3DDevice8, ProcessVertices, 6) { logf("ProcessVertices not implemented"); return D3DERR_INVALIDCALL; }
COM(IDirect3DDevice8, CreateVertexShader, 5) {
    logf("CreateVertexShader requested (function=%08x) - not supported", c.arg(2));
    return D3DERR_INVALIDCALL;
}
COM(IDirect3DDevice8, SetVertexShader, 2) {
    target_state().fvf = c.arg(1);
    if (recording) recording->fvf = true;
    return 0;
}
COM(IDirect3DDevice8, GetVertexShader, 2) { wr32(c.arg(1), st.fvf); return 0; }
COM(IDirect3DDevice8, DeleteVertexShader, 2) { return 0; }
COM(IDirect3DDevice8, SetVertexShaderConstant, 4) { return 0; }
COM(IDirect3DDevice8, SetStreamSource, 4) {
    if (c.arg(1) != 0) return 0;
    int id = RID(c.arg(2));
    if (recording) { recording->s.stream_vb = id; recording->s.stream_stride = c.arg(3); recording->stream = true; return 0; }
    if (id != st.stream_vb) { addref_res(id); release_res(st.stream_vb); }
    st.stream_vb = id; st.stream_stride = c.arg(3);
    return 0;
}
COM(IDirect3DDevice8, GetStreamSource, 4) {
    if (st.stream_vb >= 0) { addref_res(st.stream_vb); wr32(c.arg(2), res[st.stream_vb]->obj); } else wr32(c.arg(2), 0);
    wr32(c.arg(3), st.stream_stride);
    return 0;
}
COM(IDirect3DDevice8, SetIndices, 3) {
    int id = RID(c.arg(1));
    if (recording) { recording->s.ib = id; recording->s.base_vertex = c.arg(2); recording->indices = true; return 0; }
    if (id != st.ib) { addref_res(id); release_res(st.ib); }
    st.ib = id; st.base_vertex = c.arg(2);
    return 0;
}
COM(IDirect3DDevice8, GetIndices, 3) {
    if (st.ib >= 0) { addref_res(st.ib); wr32(c.arg(1), res[st.ib]->obj); } else wr32(c.arg(1), 0);
    wr32(c.arg(2), st.base_vertex);
    return 0;
}
COM(IDirect3DDevice8, CreatePixelShader, 3) { return D3DERR_INVALIDCALL; }
COM(IDirect3DDevice8, SetPixelShader, 2) { return 0; }
COM(IDirect3DDevice8, GetPixelShader, 2) { wr32(c.arg(1), 0); return 0; }
COM(IDirect3DDevice8, DeletePixelShader, 2) { return 0; }
COM(IDirect3DDevice8, SetPixelShaderConstant, 4) { return 0; }

// ---- resources: shared IUnknown/IDirect3DResource8 ------------------------------------------------------
#define RESOURCE_COMMON(I)                                                                       \
    COM(I, QueryInterface, 3) { addref_res(RID(c.arg(0))); wr32(c.arg(2), c.arg(0)); return 0; } \
    COM(I, AddRef, 1) { Res* r = R(c.arg(0)); return ++r->refs; }                                \
    COM(I, Release, 1) { int id = RID(c.arg(0)); release_res(id); return (u32)std::max(0, res[id]->refs); } \
    COM(I, GetDevice, 2) { wr32(c.arg(1), dev_obj); return 0; }                                   \
    COM(I, SetPrivateData, 5) { return 0; }                                                       \
    COM(I, GetPrivateData, 4) { return 0x88760866; }                                              \
    COM(I, FreePrivateData, 2) { return 0; }

RESOURCE_COMMON(IDirect3DTexture8)
RESOURCE_COMMON(IDirect3DSurface8)
RESOURCE_COMMON(IDirect3DVertexBuffer8)
RESOURCE_COMMON(IDirect3DIndexBuffer8)

COM(IDirect3DTexture8, SetPriority, 2) { return 0; }
COM(IDirect3DTexture8, GetPriority, 1) { return 0; }
COM(IDirect3DTexture8, PreLoad, 1) { return 0; }
COM(IDirect3DTexture8, GetType, 1) { return 3; }
COM(IDirect3DTexture8, SetLOD, 2) { Res* r = R(c.arg(0)); u32 o = r->lod; r->lod = c.arg(1); return o; }
COM(IDirect3DTexture8, GetLOD, 1) { return R(c.arg(0))->lod; }
COM(IDirect3DTexture8, GetLevelCount, 1) { return (u32)R(c.arg(0))->levels.size(); }
COM(IDirect3DTexture8, GetLevelDesc, 3) {
    Res* r = R(c.arg(0)); u32 lv = c.arg(1), p = c.arg(2);
    if (lv >= r->levels.size()) return D3DERR_INVALIDCALL;
    Level& l = r->levels[lv];
    wr32(p, r->fmt); wr32(p + 4, 1 /*SURFACE*/); wr32(p + 8, r->usage); wr32(p + 12, r->pool);
    wr32(p + 16, l.pitch * level_rows(r->fmt, l.h)); wr32(p + 20, 0); wr32(p + 24, l.w); wr32(p + 28, l.h);
    return 0;
}
COM(IDirect3DTexture8, GetSurfaceLevel, 3) {
    int tid = RID(c.arg(0)); Res* t = res[tid];
    if (c.arg(1) >= t->levels.size()) return D3DERR_INVALIDCALL;
    Res* s = new Res(); s->kind = R_SURFACE; s->tex_res = tid; s->tex_level = (int)c.arg(1);
    s->w = t->levels[c.arg(1)].w; s->h = t->levels[c.arg(1)].h; s->fmt = t->fmt;
    t->refs++;
    new_res(s, vt_surf);
    wr32(c.arg(2), s->obj);
    return 0;
}
static void lock_level(Res* t, Level& l, u32 out, u32 rect) {
    if (t->render_target && t->fbo) rt_readback(t);
    u32 off = 0;
    if (rect) {
        u32 x = rd32(rect), y = rd32(rect + 4);
        if (is_dxt(t->fmt)) off = (y / 4) * l.pitch + (x / 4) * (t->fmt == F_DXT1 ? 8 : 16);
        else off = y * l.pitch + x * fmt_bpp(t->fmt);
    }
    wr32(out, l.pitch);
    wr32(out + 4, l.addr + off);
}
COM(IDirect3DTexture8, LockRect, 5) {
    Res* t = R(c.arg(0));
    if (c.arg(1) >= t->levels.size()) return D3DERR_INVALIDCALL;
    lock_level(t, t->levels[c.arg(1)], c.arg(2), c.arg(3));
    return 0;
}
COM(IDirect3DTexture8, UnlockRect, 2) {
    Res* t = R(c.arg(0));
    if (c.arg(1) < t->levels.size()) t->levels[c.arg(1)].dirty = true;
    if (t->render_target && t->tex) { glBindTexture(GL_TEXTURE_2D, t->tex); }
    return 0;
}
COM(IDirect3DTexture8, AddDirtyRect, 2) { for (auto& l : R(c.arg(0))->levels) l.dirty = true; return 0; }

COM(IDirect3DSurface8, GetContainer, 3) {
    Res* s = R(c.arg(0));
    if (s->tex_res >= 0) { res[s->tex_res]->refs++; wr32(c.arg(2), res[s->tex_res]->obj); return 0; }
    wr32(c.arg(2), dev_obj);
    return 0;
}
COM(IDirect3DSurface8, GetDesc, 2) {
    Res* s = R(c.arg(0)); u32 p = c.arg(1);
    u32 usage = s->backbuffer ? 1 : s->depth ? 2 : 0;
    wr32(p, s->fmt); wr32(p + 4, 1); wr32(p + 8, usage); wr32(p + 12, 0);
    wr32(p + 16, level_pitch(s->fmt, s->w) * level_rows(s->fmt, s->h)); wr32(p + 20, 0); wr32(p + 24, s->w); wr32(p + 28, s->h);
    return 0;
}
COM(IDirect3DSurface8, LockRect, 4) {
    Res* s = R(c.arg(0));
    if (s->tex_res >= 0) {
        Res* t = res[s->tex_res];
        lock_level(t, t->levels[s->tex_level], c.arg(1), c.arg(2));
        return 0;
    }
    SurfMem m;
    if (!surf_mem(s, m)) return D3DERR_INVALIDCALL;
    u32 off = 0;
    if (c.arg(2)) off = rd32(c.arg(2) + 4) * m.pitch + rd32(c.arg(2)) * fmt_bpp(m.fmt);
    wr32(c.arg(1), m.pitch);
    wr32(c.arg(1) + 4, m.addr + off);
    return 0;
}
COM(IDirect3DSurface8, UnlockRect, 1) {
    Res* s = R(c.arg(0));
    if (s->tex_res >= 0) res[s->tex_res]->levels[s->tex_level].dirty = true;
    return 0;
}

#define BUFFER_METHODS(I)                                                                          \
    COM(I, SetPriority, 2) { return 0; }                                                           \
    COM(I, GetPriority, 1) { return 0; }                                                           \
    COM(I, PreLoad, 1) { return 0; }                                                               \
    COM(I, Lock, 5) { Res* r = R(c.arg(0)); wr32(c.arg(3), r->buf + c.arg(1)); return 0; }       \
    COM(I, Unlock, 1) { return 0; }

BUFFER_METHODS(IDirect3DVertexBuffer8)
BUFFER_METHODS(IDirect3DIndexBuffer8)
COM(IDirect3DVertexBuffer8, GetType, 1) { return 6; }
COM(IDirect3DIndexBuffer8, GetType, 1) { return 7; }
COM(IDirect3DVertexBuffer8, GetDesc, 2) {
    Res* r = R(c.arg(0)); u32 p = c.arg(1);
    wr32(p, 100 /*D3DFMT_VERTEXDATA*/); wr32(p + 4, 6); wr32(p + 8, r->usage); wr32(p + 12, r->pool); wr32(p + 16, r->size); wr32(p + 20, r->fvf);
    return 0;
}
COM(IDirect3DIndexBuffer8, GetDesc, 2) {
    Res* r = R(c.arg(0)); u32 p = c.arg(1);
    wr32(p, r->fmt); wr32(p + 4, 7); wr32(p + 8, r->usage); wr32(p + 12, r->pool); wr32(p + 16, r->size);
    return 0;
}
