// DirectSound + BASS (music) replacement, mixed by an SDL audio callback.
// DirectSound buffer memory lives in guest memory, so Lock() hands out direct pointers
// and the mixer reads the samples from there.
#include "platform.h"
#include <libopenmpt/libopenmpt.h>
#include <mutex>
#include <cmath>

static const int MIX_RATE = 44100;
static std::recursive_mutex mix_lock;
static SDL_AudioDeviceID audio_dev = 0;

struct SoundData {  // shared between duplicated buffers
    u32 addr = 0, bytes = 0;
    int channels = 1, bits = 16, rate = 22050;
    int refs = 1;
};
struct SoundBuf {
    SoundData* data;
    bool primary = false;
    bool playing = false, looping = false;
    double pos = 0;       // in frames
    int freq = 0;         // 0 = original
    i32 volume = 0;       // hundredths of dB
    i32 pan = 0;
    u32 caps = 0;
    bool alive = true;
};
static std::vector<SoundBuf*> bufs;

struct Music {
    openmpt_module* mod = nullptr;
    bool playing = false;
    float volume = 1.0f;
    float pan = 0;
};
static std::vector<Music*> musics;
static float bass_global_music_vol = 1.0f;

static void mix_cb(void*, Uint8* stream, int len) {
    int frames = len / 8;
    float* out = (float*)stream;
    memset(out, 0, len);
    std::lock_guard<std::recursive_mutex> g(mix_lock);
    static std::vector<float> tmp;
    for (auto* m : musics) {
        if (!m->playing || !m->mod) continue;
        tmp.resize(frames * 2);
        size_t n = openmpt_module_read_interleaved_float_stereo(m->mod, MIX_RATE, frames, tmp.data());
        float lv = m->volume * bass_global_music_vol * (m->pan > 0 ? 1 - m->pan : 1);
        float rv = m->volume * bass_global_music_vol * (m->pan < 0 ? 1 + m->pan : 1);
        for (size_t i = 0; i < n; i++) { out[i * 2] += tmp[i * 2] * lv; out[i * 2 + 1] += tmp[i * 2 + 1] * rv; }
        if (n < (size_t)frames) m->playing = false;
    }
    for (auto* b : bufs) {
        if (!b->alive || !b->playing || b->primary) continue;
        SoundData* d = b->data;
        int bpf = d->channels * d->bits / 8;
        u32 nframes = d->bytes / bpf;
        if (!nframes) { b->playing = false; continue; }
        double step = (double)(b->freq ? b->freq : d->rate) / MIX_RATE;
        float gain = b->volume <= -10000 ? 0 : powf(10.0f, b->volume / 2000.0f);
        float lg = gain, rg = gain;
        if (b->pan < 0) rg *= powf(10.0f, b->pan / 2000.0f);
        if (b->pan > 0) lg *= powf(10.0f, -b->pan / 2000.0f);
        const u8* base = g2h(d->addr);
        for (int i = 0; i < frames; i++) {
            u32 f = (u32)b->pos;
            if (f >= nframes) {
                if (b->looping) { b->pos = fmod(b->pos, nframes); f = (u32)b->pos; }
                else { b->playing = false; b->pos = 0; break; }
            }
            float l, r;
            const u8* s = base + f * bpf;
            if (d->bits == 16) {
                l = ((const int16_t*)s)[0] / 32768.0f;
                r = d->channels == 2 ? ((const int16_t*)s)[1] / 32768.0f : l;
            } else {
                l = (s[0] - 128) / 128.0f;
                r = d->channels == 2 ? (s[1] - 128) / 128.0f : l;
            }
            out[i * 2] += l * lg;
            out[i * 2 + 1] += r * rg;
            b->pos += step;
        }
    }
    for (int i = 0; i < frames * 2; i++) out[i] = out[i] > 1 ? 1 : out[i] < -1 ? -1 : out[i];
    // HB_AUDIO_DUMP=path: append raw float32 stereo 44.1k samples (for testing)
    static FILE* dump = nullptr;
    static bool dump_checked = false;
    if (!dump_checked) { dump_checked = true; if (const char* p = getenv("HB_AUDIO_DUMP")) dump = fopen(p, "wb"); }
    if (dump) fwrite(out, 4, frames * 2, dump);
}

static void ensure_audio() {
    if (audio_dev) return;
    SDL_AudioSpec want = {}, have;
    want.freq = MIX_RATE; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 1024; want.callback = mix_cb;
    audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!audio_dev) { logf("audio: SDL_OpenAudioDevice failed: %s", SDL_GetError()); return; }
    SDL_PauseAudioDevice(audio_dev, 0);
}

// ---- DirectSound ----------------------------------------------------------------------------
static u32 vt_ds = 0, vt_buf = 0;
static u32 new_buffer_object(SoundBuf* b) {
    std::lock_guard<std::recursive_mutex> g(mix_lock);
    bufs.push_back(b);
    return make_com_object(vt_buf, (u32)bufs.size() - 1);
}
static SoundBuf* buf(Ctx& c) { return bufs[com_id(c.arg(0))]; }

API(dsound, DirectSoundCreate, 3) {
    ensure_audio();
    if (!vt_ds) {
        vt_ds = make_vtable("IDirectSound", {"QueryInterface", "AddRef", "Release", "CreateSoundBuffer", "GetCaps",
                                             "DuplicateSoundBuffer", "SetCooperativeLevel", "Compact", "GetSpeakerConfig",
                                             "SetSpeakerConfig", "Initialize"});
        vt_buf = make_vtable("IDirectSoundBuffer", {"QueryInterface", "AddRef", "Release", "GetCaps", "GetCurrentPosition",
                                                    "GetFormat", "GetVolume", "GetPan", "GetFrequency", "GetStatus", "Initialize",
                                                    "Lock", "Play", "SetCurrentPosition", "SetFormat", "SetVolume", "SetPan",
                                                    "SetFrequency", "Stop", "Unlock", "Restore"});
    }
    wr32(c.arg(1), make_com_object(vt_ds, 0));
    return 0;
}
COM(IDirectSound, QueryInterface, 3) { wr32(c.arg(2), c.arg(0)); return 0; }
COM(IDirectSound, AddRef, 1) { return 1; }
COM(IDirectSound, Release, 1) { return 0; }
COM(IDirectSound, SetCooperativeLevel, 3) { return 0; }
COM(IDirectSound, Compact, 1) { return 0; }
COM(IDirectSound, GetSpeakerConfig, 2) { wr32(c.arg(1), 4); return 0; }
COM(IDirectSound, GetCaps, 2) {
    u32 p = c.arg(1);
    memset(g2h(p + 4), 0, rd32(p) - 4);
    wr32(p + 4, 0xF5F);  // all formats, continuous rate, secondary buffers
    wr32(p + 8, 100); wr32(p + 12, 100000);
    return 0;
}
COM(IDirectSound, CreateSoundBuffer, 4) {
    u32 desc = c.arg(1);
    u32 flags = rd32(desc + 4), bytes = rd32(desc + 8), fmt = rd32(desc + 16);
    SoundBuf* b = new SoundBuf();
    b->data = new SoundData();
    b->caps = flags;
    if (flags & 1) {  // DSBCAPS_PRIMARYBUFFER
        b->primary = true;
        b->data->bytes = 0;
    } else {
        if (fmt) {
            b->data->channels = rd16(fmt + 2);
            b->data->rate = (int)rd32(fmt + 4);
            b->data->bits = rd16(fmt + 14);
        }
        b->data->bytes = bytes;
        b->data->addr = galloc(bytes + 16);
        if (b->data->bits == 8) memset(g2h(b->data->addr), 0x80, bytes);
    }
    wr32(c.arg(2), new_buffer_object(b));
    return 0;
}
COM(IDirectSound, DuplicateSoundBuffer, 3) {
    SoundBuf* src = bufs[com_id(c.arg(1))];
    SoundBuf* b = new SoundBuf(*src);
    b->playing = false; b->pos = 0;
    b->data->refs++;
    wr32(c.arg(2), new_buffer_object(b));
    return 0;
}

COM(IDirectSoundBuffer, QueryInterface, 3) { wr32(c.arg(2), c.arg(0)); return 0; }
COM(IDirectSoundBuffer, AddRef, 1) { return 1; }
COM(IDirectSoundBuffer, Release, 1) {
    std::lock_guard<std::recursive_mutex> g(mix_lock);
    SoundBuf* b = buf(c);
    if (b->alive) {
        b->alive = false; b->playing = false;
        if (--b->data->refs == 0 && b->data->addr) { gfree(b->data->addr); b->data->addr = 0; b->data->bytes = 0; }
    }
    return 0;
}
COM(IDirectSoundBuffer, GetCaps, 2) {
    u32 p = c.arg(1);
    wr32(p + 4, buf(c)->caps); wr32(p + 8, buf(c)->data->bytes); wr32(p + 12, 0); wr32(p + 16, 0);
    return 0;
}
COM(IDirectSoundBuffer, GetCurrentPosition, 3) {
    std::lock_guard<std::recursive_mutex> g(mix_lock);
    SoundBuf* b = buf(c);
    int bpf = b->data->channels * b->data->bits / 8;
    u32 play = (u32)b->pos * bpf;
    if (b->data->bytes) play %= b->data->bytes;
    if (c.arg(1)) wr32(c.arg(1), play);
    if (c.arg(2)) wr32(c.arg(2), b->data->bytes ? (play + 2048 * bpf) % b->data->bytes : 0);
    return 0;
}
COM(IDirectSoundBuffer, GetFormat, 4) {
    SoundBuf* b = buf(c);
    u32 p = c.arg(1);
    if (p) {
        wr16(p, 1); wr16(p + 2, b->data->channels); wr32(p + 4, b->data->rate);
        wr32(p + 8, b->data->rate * b->data->channels * b->data->bits / 8);
        wr16(p + 12, b->data->channels * b->data->bits / 8); wr16(p + 14, b->data->bits);
    }
    if (c.arg(3)) wr32(c.arg(3), 16);
    return 0;
}
COM(IDirectSoundBuffer, GetVolume, 2) { wr32(c.arg(1), buf(c)->volume); return 0; }
COM(IDirectSoundBuffer, GetPan, 2) { wr32(c.arg(1), buf(c)->pan); return 0; }
COM(IDirectSoundBuffer, GetFrequency, 2) { wr32(c.arg(1), buf(c)->freq ? buf(c)->freq : buf(c)->data->rate); return 0; }
COM(IDirectSoundBuffer, GetStatus, 2) {
    SoundBuf* b = buf(c);
    wr32(c.arg(1), b->playing ? (1 | (b->looping ? 4 : 0)) : 0);
    return 0;
}
COM(IDirectSoundBuffer, Lock, 8) {
    SoundBuf* b = buf(c);
    u32 off = c.arg(1), n = c.arg(2), flags = c.arg(7);
    u32 total = b->data->bytes;
    if (flags & 2 /*DSBLOCK_ENTIREBUFFER*/) { off = 0; n = total; }
    if (flags & 1 /*FROMWRITECURSOR*/) off = 0;
    if (!total) return 0x88780032;
    off %= total;
    if (n > total) n = total;
    u32 first = std::min(n, total - off);
    wr32(c.arg(3), b->data->addr + off); wr32(c.arg(4), first);
    if (c.arg(5)) wr32(c.arg(5), n > first ? b->data->addr : 0);
    if (c.arg(6)) wr32(c.arg(6), n > first ? n - first : 0);
    return 0;
}
COM(IDirectSoundBuffer, Unlock, 5) { return 0; }
COM(IDirectSoundBuffer, Play, 4) {
    std::lock_guard<std::recursive_mutex> g(mix_lock);
    SoundBuf* b = buf(c);
    b->playing = true;
    b->looping = c.arg(3) & 1;
    return 0;
}
COM(IDirectSoundBuffer, Stop, 1) { std::lock_guard<std::recursive_mutex> g(mix_lock); buf(c)->playing = false; return 0; }
COM(IDirectSoundBuffer, SetCurrentPosition, 2) {
    std::lock_guard<std::recursive_mutex> g(mix_lock);
    SoundBuf* b = buf(c);
    b->pos = c.arg(1) / (b->data->channels * b->data->bits / 8);
    return 0;
}
COM(IDirectSoundBuffer, SetFormat, 2) { return 0; }
COM(IDirectSoundBuffer, SetVolume, 2) { buf(c)->volume = std::max(-10000, std::min(0, c.iarg(1))); return 0; }
COM(IDirectSoundBuffer, SetPan, 2) { buf(c)->pan = std::max(-10000, std::min(10000, c.iarg(1))); return 0; }
COM(IDirectSoundBuffer, SetFrequency, 2) { buf(c)->freq = (int)c.arg(1); return 0; }
COM(IDirectSoundBuffer, Restore, 1) { return 0; }

// ---- BASS (music only: MO3/MOD via libopenmpt) ------------------------------------------------
static u32 bass_error = 0;
API(bass, BASS_SetConfig, 2) {
    if (c.arg(0) == 1 /*BASS_CONFIG_GVOL_MUSIC? (2.0: MUSIC=2)*/) {}
    return 1;
}
API(bass, BASS_Init, 5) { ensure_audio(); return 1; }
API(bass, BASS_Free, 0) { return 1; }
API(bass, BASS_Start, 0) { return 1; }
API(bass, BASS_Stop, 0) { return 1; }
API(bass, BASS_ErrorGetCode, 0) { return bass_error; }
API(bass, BASS_MusicLoad, 6) {
    u32 mem = c.arg(0), file = c.arg(1), offset = c.arg(2), length = c.arg(3);
    std::vector<u8> data;
    if (mem) data.assign(g2h(file + offset), g2h(file + offset) + length);
    else {
        extern std::string host_path(const char*);
        std::string p = host_path(gstr(file));
        FILE* f = fopen(p.c_str(), "rb");
        if (!f) { bass_error = 2; logf("BASS_MusicLoad: cannot open %s", p.c_str()); return 0; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, offset, SEEK_SET);
        if (length == 0 || length > (u32)n - offset) length = (u32)n - offset;
        data.resize(length);
        if (fread(data.data(), 1, length, f) != length) {}
        fclose(f);
    }
    openmpt_module* mod = openmpt_module_create_from_memory2(data.data(), data.size(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!mod) { bass_error = 41; logf("BASS_MusicLoad: libopenmpt failed"); return 0; }
    openmpt_module_set_repeat_count(mod, -1);
    Music* m = new Music();
    m->mod = mod;
    std::lock_guard<std::recursive_mutex> g(mix_lock);
    musics.push_back(m);
    logf("BASS_MusicLoad: %u orders, flags=%x", openmpt_module_get_num_orders(mod), c.arg(4));
    return 0x80000000u | (u32)(musics.size() - 1);
}
static Music* music(u32 h) {
    if (!(h & 0x80000000u)) return nullptr;
    u32 i = h & 0x7FFFFFFF;
    return i < musics.size() ? musics[i] : nullptr;
}
API(bass, BASS_MusicPlayEx, 4) {
    std::lock_guard<std::recursive_mutex> g(mix_lock);
    Music* m = music(c.arg(0));
    if (!m) { bass_error = 5; return 0; }
    u32 pos = c.arg(1);
    if (pos != 0xFFFFFFFF) openmpt_module_set_position_order_row(m->mod, pos & 0xFFFF, (pos >> 16) & 0xFFFF);
    m->playing = true;
    return 1;
}
API(bass, BASS_ChannelStop, 1) {
    std::lock_guard<std::recursive_mutex> g(mix_lock);
    if (Music* m = music(c.arg(0))) m->playing = false;
    return 1;
}
API(bass, BASS_ChannelSetAttributes, 4) {
    std::lock_guard<std::recursive_mutex> g(mix_lock);
    Music* m = music(c.arg(0));
    if (!m) return 0;
    i32 vol = c.iarg(2), pan = c.iarg(3);
    if (vol >= 0) m->volume = std::min(vol, 100) / 100.0f;
    if (pan != -101) m->pan = std::max(-100, std::min(100, pan)) / 100.0f;
    return 1;
}
