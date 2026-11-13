// pl_mpeg must come first (single-header MPEG-1 video decoder, downloaded to src/pl_mpeg.h)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {
        // true while a troll is on screen, so we never stack two at once
        bool g_trollActive = false;

        std::string lowerExt(std::filesystem::path const& p) {
                auto ext = p.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return ext;
        }

        bool isAudio(std::string const& ext) {
                return ext == ".mp3" || ext == ".ogg" || ext == ".wav";
        }

        // Only MPEG-1 video can be played inside the game.
        bool isVideo(std::string const& ext) {
                return ext == ".mpg" || ext == ".mpeg";
        }

        // <GD folder>/geode/config/kiril.percent-troll/media  (created if missing)
        std::filesystem::path mediaDir() {
                auto dir = Mod::get()->getConfigDir() / "media";
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                return dir;
        }

        // For clip.mpg this finds clip.mp3 / clip.ogg / clip.wav next to it.
        std::filesystem::path findSidecarAudio(std::filesystem::path const& video) {
                for (auto const* ext : { ".mp3", ".ogg", ".wav" }) {
                        auto p = video;
                        p.replace_extension(ext);
                        std::error_code ec;
                        if (std::filesystem::exists(p, ec)) return p;
                }
                return {};
        }

        bool hasVideoSibling(std::filesystem::path const& audio) {
                for (auto const* ext : { ".mpg", ".mpeg" }) {
                        auto p = audio;
                        p.replace_extension(ext);
                        std::error_code ec;
                        if (std::filesystem::exists(p, ec)) return true;
                }
                return false;
        }

        // Uses the file named in settings, or a random supported file from the folder.
        std::filesystem::path pickMedia() {
                auto dir = mediaDir();
                auto name = Mod::get()->getSettingValue<std::string>("media-file");

                if (!name.empty()) {
                        auto p = dir / std::filesystem::path(name);
                        std::error_code ec;
                        if (std::filesystem::exists(p, ec)) return p;
                        log::warn("Percent Troll: '{}' was not found in {}", name, dir.string());
                        return {};
                }

                std::vector<std::filesystem::path> files;
                std::error_code ec;
                for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
                        if (!entry.is_regular_file()) continue;
                        auto ext = lowerExt(entry.path());
                        if (isVideo(ext)) files.push_back(entry.path());
                        // an audio file that belongs to a video is not a separate troll
                        else if (isAudio(ext) && !hasVideoSibling(entry.path())) files.push_back(entry.path());
                }
                if (files.empty()) return {};

                static std::mt19937 rng{std::random_device{}()};
                std::uniform_int_distribution<size_t> pick(0, files.size() - 1);
                return files[pick(rng)];
        }

        std::string buildText(int percent) {
                auto msg = Mod::get()->getSettingValue<std::string>("message");
                std::string const token = "{percent}";
                auto pos = msg.find(token);
                while (pos != std::string::npos) {
                        auto num = std::to_string(percent);
                        msg.replace(pos, token.size(), num);
                        pos = msg.find(token, pos + num.size());
                }
                return msg;
        }

        FILE* openFileBinary(std::filesystem::path const& p) {
#ifdef GEODE_IS_WINDOWS
                return _wfopen(p.wstring().c_str(), L"rb");
#else
                return std::fopen(p.string().c_str(), "rb");
#endif
        }
}

// Shows the video (if any) and text on top of the scene, freezes the level,
// and unfreezes it when time is up. It lives on the scene (not on the PlayLayer),
// so it keeps running while the level is frozen.
class TrollOverlay : public CCNode {
protected:
        Ref<PlayLayer> m_playLayer;
        bool m_freeze = false;
        float m_duration = 0.f;
        plm_t* m_plm = nullptr;
        CCSprite* m_videoSprite = nullptr;
        std::vector<uint8_t> m_rgba;

        static void onVideoFrame(plm_t*, plm_frame_t* frame, void* user) {
                static_cast<TrollOverlay*>(user)->showFrame(frame);
        }

        void showFrame(plm_frame_t* frame) {
                if (!m_videoSprite || !frame) return;
                int w = static_cast<int>(frame->width);
                int h = static_cast<int>(frame->height);
                if (w <= 0 || h <= 0) return;

                m_rgba.resize(static_cast<size_t>(w) * h * 4);
                plm_frame_to_rgba(frame, m_rgba.data(), w * 4);

                auto tex = new CCTexture2D();
                if (tex->initWithData(m_rgba.data(), kCCTexture2DPixelFormat_RGBA8888,
                        w, h, CCSize{ static_cast<float>(w), static_cast<float>(h) })) {
                        m_videoSprite->setTexture(tex);
                        m_videoSprite->setTextureRect(CCRect{ 0.f, 0.f, static_cast<float>(w), static_cast<float>(h) });

                        // fit the video inside the screen, centered
                        auto winSize = CCDirector::sharedDirector()->getWinSize();
                        float scale = std::min(winSize.width / w, winSize.height / h);
                        m_videoSprite->setScale(scale);
                        m_videoSprite->setPosition({ winSize.width / 2.f, winSize.height / 2.f });
                        m_videoSprite->setVisible(true);
                }
                tex->release();
        }

public:
        ~TrollOverlay() override {
                if (m_plm) {
                        plm_destroy(m_plm);
                        m_plm = nullptr;
                }
                g_trollActive = false;
        }

        static TrollOverlay* create(PlayLayer* pl, std::string const& text, bool freeze,
                float duration, std::filesystem::path const& video) {
                auto ret = new TrollOverlay();
                if (ret->init(pl, text, freeze, duration, video)) {
                        ret->autorelease();
                        return ret;
                }
                delete ret;
                return nullptr;
        }

        bool init(PlayLayer* pl, std::string const& text, bool freeze,
                float duration, std::filesystem::path const& video) {
                if (!CCNode::init()) return false;

                m_playLayer = pl;
                m_freeze = freeze;
                m_duration = duration;

                auto winSize = CCDirector::sharedDirector()->getWinSize();

                if (!video.empty()) {
                        if (auto fh = openFileBinary(video)) {
                                m_plm = plm_create_with_file(fh, TRUE); // closes the file when destroyed
                        }
                        if (m_plm) {
                                plm_set_audio_enabled(m_plm, FALSE); // sound comes from the matching mp3/ogg/wav
                                plm_set_video_decode_callback(m_plm, &TrollOverlay::onVideoFrame, this);
                                double len = plm_get_duration(m_plm);
                                if (len > 0.0) m_duration = static_cast<float>(len) + 0.3f;

                                this->addChild(CCLayerColor::create(ccColor4B{ 0, 0, 0, 255 }));
                                m_videoSprite = CCSprite::create();
                                m_videoSprite->setVisible(false);
                                this->addChild(m_videoSprite);
                        }
                        else {
                                log::warn("Percent Troll: could not open '{}' as MPEG-1 video. Convert it with the ffmpeg command from the mod description.",
                                        video.filename().string());
                        }
                }

                if (!text.empty()) {
                        auto label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
                        label->setPosition({ winSize.width / 2.f, winSize.height - 28.f });
                        label->setScale(0.7f);
                        this->addChild(label, 10);
                }

                g_trollActive = true;
                return true;
        }

        void onEnter() override {
                CCNode::onEnter();
                if (m_freeze && m_playLayer) {
                        m_playLayer->pauseSchedulerAndActions();
                }
                if (m_plm) this->scheduleUpdate();
                this->scheduleOnce(schedule_selector(TrollOverlay::finish), m_duration);
        }

        void update(float dt) override {
                if (!m_plm) return;
                plm_decode(m_plm, static_cast<double>(std::min(dt, 0.1f)));
        }

        void finish(float) {
                if (m_freeze && m_playLayer && m_playLayer->getParent()) {
                        m_playLayer->resumeSchedulerAndActions();
                }
                g_trollActive = false;
                this->removeFromParentAndCleanup(true);
        }
};

class $modify(TrollPlayLayer, PlayLayer) {
        static void onModify(auto& self) {
                // Run before other mods' hooks so we read the death percent before an
                // instant-respawn style mod can reset the level back to 0%.
                (void)self.setHookPriority("PlayLayer::destroyPlayer", -100);
        }

        void destroyPlayer(PlayerObject* player, GameObject* object) override {
                // Snapshot before anything else runs.
                float percent = this->getCurrentPercent();
                bool wasAlive = !m_player1->m_isDead;

                PlayLayer::destroyPlayer(player, object);

                if (!wasAlive || g_trollActive) return;

                // A real death: the player is dead, or the level was already reset
                // back (instant respawn). A blocked death (noclip) leaves both unchanged.
                bool died = m_player1->m_isDead || this->getCurrentPercent() < percent - 0.01f;
                if (!died) return;

                auto mod = Mod::get();
                if (m_isPracticeMode && !mod->getSettingValue<bool>("practice-mode")) return;

                int target = static_cast<int>(mod->getSettingValue<int64_t>("target-percent"));
                if (percent < static_cast<float>(target) || percent >= static_cast<float>(target + 1)) return;

                this->startTroll(static_cast<int>(percent));
        }

        void startTroll(int percent) {
                auto mod = Mod::get();
                auto scene = CCDirector::sharedDirector()->getRunningScene();
                if (!scene) return;

                float duration = static_cast<float>(mod->getSettingValue<double>("duration-seconds"));
                float volume = static_cast<float>(mod->getSettingValue<double>("volume"));
                duration = std::clamp(duration, 1.f, 60.f);

                std::filesystem::path videoPath;
                std::filesystem::path audioPath;

                auto media = pickMedia();
                if (media.empty()) {
                        log::warn("Percent Troll: no media found. Put an .mpg video or an mp3/ogg/wav in {}", mediaDir().string());
                }
                else {
                        auto ext = lowerExt(media);
                        if (isVideo(ext)) {
                                videoPath = media;
                                audioPath = findSidecarAudio(media);
                        }
                        else {
                                audioPath = media;
                        }
                }

                if (!audioPath.empty()) {
                        FMODAudioEngine::sharedEngine()->playEffect(audioPath.string(), 1.f, 0.f, volume);
                }

                std::string text = mod->getSettingValue<bool>("show-text") ? buildText(percent) : std::string();
                bool freeze = mod->getSettingValue<bool>("freeze-game");

                auto overlay = TrollOverlay::create(this, text, freeze, duration, videoPath);
                if (overlay) scene->addChild(overlay, 100000);
        }
};

// Adds a "Troll Folder" button to the pause menu that opens the media folder.
class $modify(TrollPauseLayer, PauseLayer) {
        void customSetup() {
                PauseLayer::customSetup();

                auto menu = this->getChildByID("right-button-menu");
                if (!menu) return;

                auto spr = ButtonSprite::create("Troll Folder");
                spr->setScale(0.6f);
                auto btn = CCMenuItemExt::createSpriteExtra(spr, [](CCObject*) {
                        auto res = utils::file::openFolder(mediaDir());
                        if (!res) {
                                log::warn("Percent Troll: could not open the media folder");
                        }
                });
                menu->addChild(btn);
                menu->updateLayout();
        }
};

// Make sure the media folder exists as soon as the mod loads, and log where it is.
$on_mod(Loaded) {
        log::info("Percent Troll media folder: {}", mediaDir().string());
}
