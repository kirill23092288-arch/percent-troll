// Percent Troll v1.3.1 - plays .mp4 directly through Windows Media Foundation
// (links: mfplat, mfreadwrite, mfuuid, ole32)
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstdlib>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#include <shellapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#endif

using namespace geode::prelude;
namespace fs = std::filesystem;

// ---------------------------------------------------------------- helpers

static fs::path mediaDir() {
    fs::path p = Mod::get()->getConfigDir();
    p /= "media";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

static fs::path cacheDir() {
    fs::path p = Mod::get()->getConfigDir();
    p /= "cache";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

static std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return e;
}

static bool isVideoExt(const std::string& e) {
    return e == ".mp4" || e == ".m4v" || e == ".mov" || e == ".wmv" || e == ".avi";
}

static bool isAudioExt(const std::string& e) {
    return e == ".mp3" || e == ".ogg" || e == ".wav";
}

static fs::path pickMedia() {
    std::error_code ec;
    auto dir = mediaDir();

    std::string chosen = Mod::get()->getSettingValue<std::string>("media-file");
    if (!chosen.empty()) {
        fs::path a = fs::path(chosen);
        if (a.is_absolute() && fs::exists(a, ec)) return a;
        fs::path b = dir / chosen;
        if (fs::exists(b, ec)) return b;
    }

    std::vector<fs::path> files;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        auto e = lowerExt(entry.path());
        if (isVideoExt(e) || isAudioExt(e)) files.push_back(entry.path());
    }
    if (files.empty()) return fs::path();

    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<size_t> dist(0, files.size() - 1);
    return files[dist(rng)];
}

static std::string replacePercent(std::string text, int pct) {
    std::string val = std::to_string(pct);
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    const std::string key = "{percent}";
    size_t pos = 0;
    while ((pos = lower.find(key, pos)) != std::string::npos) {
        text.replace(pos, key.size(), val);
        lower.replace(pos, key.size(), val);
        pos += val.size();
    }
    return text;
}

static void openFolderInExplorer(const fs::path& p) {
#ifdef GEODE_IS_WINDOWS
    ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

static void playSoundFile(const fs::path& p) {
    float vol = (float)Mod::get()->getSettingValue<double>("volume");
    FMODAudioEngine::sharedEngine()->playEffect(geode::utils::string::pathToString(p), 1.f, 0.f, vol);
}

// ---------------------------------------------------------------- Windows Media Foundation

#ifdef GEODE_IS_WINDOWS

static bool ensureMF() {
    static bool tried = false;
    static bool ok = false;
    if (!tried) {
        tried = true;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED); // may say "already initialised": ignore
        ok = SUCCEEDED(MFStartup(MF_VERSION));
    }
    return ok;
}

struct VideoDecoder {
    IMFSourceReader* reader = nullptr;
    int w = 0;
    int h = 0;
    LONG stride = 0;
    double duration = 0.0;
    double par = 1.0;
    bool flip = false;
    bool ended = false;
    double curTime = -1.0;
    std::vector<uint8_t> rgba;

    ~VideoDecoder() {
        if (reader) reader->Release();
    }

    bool open(const fs::path& p) {
        if (!ensureMF()) return false;

        IMFAttributes* attr = nullptr;
        if (FAILED(MFCreateAttributes(&attr, 1))) return false;
        attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        HRESULT hr = MFCreateSourceReaderFromURL(p.wstring().c_str(), attr, &reader);
        attr->Release();
        if (FAILED(hr) || !reader) {
            reader = nullptr;
            return false;
        }

        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (FAILED(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE))) return false;

        IMFMediaType* want = nullptr;
        if (FAILED(MFCreateMediaType(&want))) return false;
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want);
        want->Release();
        if (FAILED(hr)) return false;

        IMFMediaType* cur = nullptr;
        if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)) || !cur) return false;
        UINT32 uw = 0, uh = 0;
        MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &uw, &uh);
        UINT32 s32 = 0;
        if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &s32))) {
            stride = (LONG)(INT32)s32;
        } else {
            LONG st = 0;
            if (SUCCEEDED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, uw, &st))) stride = st;
            else stride = -(LONG)(uw * 4);
        }
        UINT32 pn = 1, pd = 1;
        if (SUCCEEDED(MFGetAttributeRatio(cur, MF_MT_PIXEL_ASPECT_RATIO, &pn, &pd)) && pn > 0 && pd > 0) {
            par = (double)pn / (double)pd;
        }
        cur->Release();
        log::info("Percent Troll: video {}x{} pixel aspect {}", uw, uh, par);
        if (uw == 0 || uh == 0) return false;
        w = (int)uw;
        h = (int)uh;
        if (stride == 0) stride = -(LONG)(w * 4);

        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var))) {
            duration = (double)var.uhVal.QuadPart / 10000000.0;
        }
        PropVariantClear(&var);
        if (duration <= 0.0) duration = 5.0;
        return true;
    }

    bool convert(IMFSample* s) {
        IMFMediaBuffer* buf = nullptr;
        if (FAILED(s->ConvertToContiguousBuffer(&buf)) || !buf) return false;
        BYTE* data = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(buf->Lock(&data, &maxLen, &curLen)) || !data) {
            buf->Release();
            return false;
        }

        size_t pitch = (size_t)(stride < 0 ? -stride : stride);
        bool ok = curLen >= pitch * (size_t)h && pitch >= (size_t)w * 4;
        if (ok) {
            rgba.resize((size_t)w * h * 4);
            bool bottomUp = stride < 0;
            bool doFlip = (bottomUp != flip);
            for (int y = 0; y < h; y++) {
                int srcRow = doFlip ? (h - 1 - y) : y;
                const uint8_t* src = data + (size_t)srcRow * pitch;
                uint8_t* dst = rgba.data() + (size_t)y * w * 4;
                for (int x = 0; x < w; x++) {
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = 255;
                }
            }
        }
        buf->Unlock();
        buf->Release();
        return ok;
    }

    // Decode forward until the frame time reaches t. Returns true when rgba holds a new frame.
    bool advanceTo(double t) {
        if (!reader || ended) return false;
        IMFSample* last = nullptr;
        for (int i = 0; i < 6 && curTime < t; i++) {
            DWORD si = 0, fl = 0;
            LONGLONG ts = 0;
            IMFSample* s = nullptr;
            HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &si, &fl, &ts, &s);
            if (FAILED(hr)) {
                ended = true;
                if (s) s->Release();
                break;
            }
            if (fl & MF_SOURCE_READERF_ENDOFSTREAM) {
                ended = true;
                if (s) s->Release();
                break;
            }
            if (s) {
                if (last) last->Release();
                last = s;
                curTime = (double)ts / 10000000.0;
            }
        }
        bool ok = false;
        if (last) {
            ok = convert(last);
            last->Release();
        }
        return ok;
    }
};

// Reads the audio of a video and saves it as a 44.1 kHz 16-bit stereo .wav
static bool extractAudio(const fs::path& in, const fs::path& out) {
    if (!ensureMF()) return false;

    IMFSourceReader* r = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(in.wstring().c_str(), nullptr, &r)) || !r) return false;

    r->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(r->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE))) {
        r->Release();
        return false;
    }

    IMFMediaType* want = nullptr;
    if (FAILED(MFCreateMediaType(&want))) {
        r->Release();
        return false;
    }
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    want->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    want->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
    want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    want->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    want->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 176400);
    HRESULT hr = r->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, want);
    want->Release();
    if (FAILED(hr)) {
        r->Release();
        return false;
    }

    std::vector<char> pcm;
    const size_t maxBytes = 200u * 1024u * 1024u;
    while (pcm.size() < maxBytes) {
        DWORD si = 0, fl = 0;
        LONGLONG ts = 0;
        IMFSample* s = nullptr;
        hr = r->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &si, &fl, &ts, &s);
        if (FAILED(hr)) {
            if (s) s->Release();
            break;
        }
        if (fl & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (s) s->Release();
            break;
        }
        if (!s) continue;
        IMFMediaBuffer* buf = nullptr;
        if (SUCCEEDED(s->ConvertToContiguousBuffer(&buf)) && buf) {
            BYTE* data = nullptr;
            DWORD maxLen = 0, curLen = 0;
            if (SUCCEEDED(buf->Lock(&data, &maxLen, &curLen)) && data) {
                pcm.insert(pcm.end(), (char*)data, (char*)data + curLen);
                buf->Unlock();
            }
            buf->Release();
        }
        s->Release();
    }
    r->Release();

    if (pcm.empty()) return false;

    std::ofstream f(out, std::ios::binary);
    if (!f) return false;
    auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto w16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    uint32_t dataSize = (uint32_t)pcm.size();
    f.write("RIFF", 4);
    w32(36 + dataSize);
    f.write("WAVEfmt ", 8);
    w32(16);
    w16(1);
    w16(2);
    w32(44100);
    w32(176400);
    w16(4);
    w16(16);
    f.write("data", 4);
    w32(dataSize);
    f.write(pcm.data(), (std::streamsize)pcm.size());
    f.close();
    return f.good() || true;
}

#else

struct VideoDecoder {
    int w = 0;
    int h = 0;
    double duration = 0.0;
    double par = 1.0;
    bool flip = false;
    std::vector<uint8_t> rgba;
    bool open(const fs::path&) { return false; }
    bool advanceTo(double) { return false; }
};

static bool extractAudio(const fs::path&, const fs::path&) { return false; }

#endif

// ---------------------------------------------------------------- overlay

static bool g_active = false;

class TrollOverlay : public CCNode {
protected:
    PlayLayer* m_pl = nullptr;
    bool m_freeze = false;
    float m_elapsed = 0.f;
    float m_total = 3.f;
    std::unique_ptr<VideoDecoder> m_video;
    CCSprite* m_sprite = nullptr;
    CCSize m_win;

    bool init(PlayLayer* pl, const fs::path& media, const std::string& text, bool freeze) {
        if (!CCNode::init()) return false;
        m_pl = pl;
        m_freeze = freeze;
        m_win = CCDirector::get()->getWinSize();
        m_total = (float)Mod::get()->getSettingValue<double>("duration-seconds");

        if (!media.empty()) {
            std::string ext = lowerExt(media);
            if (isVideoExt(ext)) {
                m_video = std::make_unique<VideoDecoder>();
                m_video->flip = Mod::get()->getSettingValue<bool>("flip-video");
                if (m_video->open(media)) {
                    m_total = (float)m_video->duration + 0.3f;

                    auto bg = CCLayerColor::create(ccc4(0, 0, 0, 255), m_win.width, m_win.height);
                    this->addChild(bg);

                    m_sprite = CCSprite::create();
                    m_sprite->setPosition(ccp(m_win.width / 2, m_win.height / 2));
                    m_sprite->setVisible(false);
                    this->addChild(m_sprite);

                    std::error_code ec;
                    auto size = fs::file_size(media, ec);
                    fs::path wav = cacheDir() / (media.stem().string() + "_" + std::to_string((unsigned long long)size) + ".wav");
                    if (!fs::exists(wav, ec)) {
                        if (!extractAudio(media, wav)) {
                            fs::remove(wav, ec);
                            log::warn("Percent Troll: video has no usable audio");
                        }
                    }
                    if (fs::exists(wav, ec)) playSoundFile(wav);
                } else {
                    log::warn("Percent Troll: Windows could not open the video: {}", geode::utils::string::pathToString(media));
                    m_video.reset();
                }
            } else if (isAudioExt(ext)) {
                playSoundFile(media);
            }
        }

        if (!text.empty()) {
            auto label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
            label->limitLabelWidth(m_win.width - 40.f, 1.f, 0.2f);
            label->setPosition(ccp(m_win.width / 2, m_win.height - 35.f));
            this->addChild(label, 10);
        }

        if (m_total < 0.5f) m_total = 0.5f;
        if (m_total > 120.f) m_total = 120.f;

        this->scheduleUpdate();
        return true;
    }

    void showFrame() {
        auto* tex = new CCTexture2D();
        bool ok = tex->initWithData(
            m_video->rgba.data(), kCCTexture2DPixelFormat_RGBA8888,
            (unsigned int)m_video->w, (unsigned int)m_video->h,
            CCSize((float)m_video->w, (float)m_video->h)
        );
        if (!ok) {
            tex->release();
            return;
        }
        m_sprite->setTexture(tex);
        m_sprite->setTextureRect(CCRect(0, 0, (float)m_video->w, (float)m_video->h));
        tex->release();
        float ws = (float)(m_video->par * Mod::get()->getSettingValue<double>("video-width-scale"));
        if (ws < 0.05f) ws = 1.f;
        float sx = m_win.width / ((float)m_video->w * ws);
        float sy = m_win.height / (float)m_video->h;
        float s = sx < sy ? sx : sy;
        m_sprite->setScaleX(s * ws);
        m_sprite->setScaleY(s);
        m_sprite->setVisible(true);
    }

    void finish() {
        this->unscheduleUpdate();
        if (m_freeze && m_pl && PlayLayer::get() == m_pl) {
            m_pl->resumeSchedulerAndActions();
        }
        g_active = false;
        this->removeFromParentAndCleanup(true);
    }

public:
    ~TrollOverlay() { g_active = false; }

    static TrollOverlay* create(PlayLayer* pl, const fs::path& media, const std::string& text, bool freeze) {
        auto* ret = new TrollOverlay();
        if (ret && ret->init(pl, media, text, freeze)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void update(float dt) override {
        m_elapsed += dt;
        if (m_video && m_sprite) {
            if (m_video->advanceTo((double)m_elapsed)) this->showFrame();
        }
        if (m_elapsed >= m_total) this->finish();
    }
};

static void startTroll(PlayLayer* pl, int pct) {
    auto media = pickMedia();
    std::string text;
    if (Mod::get()->getSettingValue<bool>("show-text")) {
        text = replacePercent(Mod::get()->getSettingValue<std::string>("message"), pct);
    }
    bool freeze = Mod::get()->getSettingValue<bool>("freeze-game");

    auto scene = CCDirector::get()->getRunningScene();
    if (!scene) return;

    auto overlay = TrollOverlay::create(pl, media, text, freeze);
    if (!overlay) return;

    g_active = true;
    scene->addChild(overlay, 100000);
    if (freeze) pl->pauseSchedulerAndActions();
}

// ---------------------------------------------------------------- hooks

class $modify(TrollPlayLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        float pct = this->getCurrentPercent();
        bool wasBusy = g_active;
        PlayLayer::destroyPlayer(player, obj);

        bool startpos = this->m_startPosObject != nullptr;
        log::info("Percent Troll: death at {}% (startpos {}, practice {})", pct, startpos, this->m_isPracticeMode);
        if (wasBusy || g_active) return;

        if (this->m_isPracticeMode && !Mod::get()->getSettingValue<bool>("practice-mode")) return;
        if (startpos && !Mod::get()->getSettingValue<bool>("startpos-mode")) return;

        int64_t target = Mod::get()->getSettingValue<int64_t>("target-percent");
        if (pct >= (float)target && pct < (float)(target + 1)) {
            startTroll(this, (int)pct);
        }
    }
};

class $modify(TrollPauseLayer, PauseLayer) {
    void onTrollFolder(CCObject*) {
        openFolderInExplorer(mediaDir());
    }

    void customSetup() {
        PauseLayer::customSetup();

        auto spr = ButtonSprite::create("Troll Folder", 100, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(TrollPauseLayer::onTrollFolder));

        auto menu = this->getChildByID("right-button-menu");
        if (menu) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            auto win = CCDirector::get()->getWinSize();
            auto m = CCMenu::create();
            m->addChild(btn);
            m->setPosition(ccp(win.width - 70.f, 25.f));
            this->addChild(m, 10);
        }
    }
};
