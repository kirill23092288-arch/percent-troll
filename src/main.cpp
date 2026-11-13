#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#ifdef GEODE_IS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>
#endif

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

	bool isVideo(std::string const& ext) {
		return ext == ".mp4" || ext == ".webm" || ext == ".mov" ||
		       ext == ".mkv" || ext == ".avi" || ext == ".wmv";
	}

	// <GD folder>/geode/config/kiril.percent-troll/media  (created if missing)
	std::filesystem::path mediaDir() {
		auto dir = Mod::get()->getConfigDir() / "media";
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		return dir;
	}

	// Uses the file named in settings, or a random supported file from the folder.
	std::filesystem::path pickMedia() {
		auto dir = mediaDir();
		auto name = Mod::get()->getSettingValue<std::string>("media-file");

		if (!name.empty()) {
			auto p = dir / std::filesystem::path(name);
			if (std::filesystem::exists(p)) return p;
			log::warn("Percent Troll: '{}' was not found in {}", name, dir.string());
			return {};
		}

		std::vector<std::filesystem::path> files;
		std::error_code ec;
		for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
			if (!entry.is_regular_file()) continue;
			auto ext = lowerExt(entry.path());
			if (isAudio(ext) || isVideo(ext)) files.push_back(entry.path());
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

	void openWithDefaultPlayer(std::filesystem::path const& p) {
#ifdef GEODE_IS_WINDOWS
		ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
		log::warn("Percent Troll: video playback only works on Windows right now");
#endif
	}
}

// Draws the text at the top, freezes the level, and unfreezes it when time is up.
// It lives on the scene (not on the PlayLayer), so it keeps running while the level is frozen.
class TrollOverlay : public CCNode {
protected:
	Ref<PlayLayer> m_playLayer;
	bool m_freeze = false;
	float m_duration = 0.f;

public:
	~TrollOverlay() override {
		g_trollActive = false;
	}

	static TrollOverlay* create(PlayLayer* pl, std::string const& text, bool freeze, float duration) {
		auto ret = new TrollOverlay();
		if (ret->init(pl, text, freeze, duration)) {
			ret->autorelease();
			return ret;
		}
		delete ret;
		return nullptr;
	}

	bool init(PlayLayer* pl, std::string const& text, bool freeze, float duration) {
		if (!CCNode::init()) return false;

		m_playLayer = pl;
		m_freeze = freeze;
		m_duration = duration;

		if (!text.empty()) {
			auto label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
			auto winSize = CCDirector::sharedDirector()->getWinSize();
			label->setPosition({ winSize.width / 2.f, winSize.height - 28.f });
			label->setScale(0.7f);
			this->addChild(label);
		}

		g_trollActive = true;
		return true;
	}

	void onEnter() override {
		CCNode::onEnter();
		if (m_freeze && m_playLayer) {
			m_playLayer->pauseSchedulerAndActions();
		}
		this->scheduleOnce(schedule_selector(TrollOverlay::finish), m_duration);
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

		auto media = pickMedia();
		if (media.empty()) {
			log::warn("Percent Troll: no media found. Put an mp3, ogg, wav or mp4 in {}", mediaDir().string());
		}
		else {
			auto ext = lowerExt(media);
			if (isVideo(ext)) {
				openWithDefaultPlayer(media);
			}
			else {
				FMODAudioEngine::sharedEngine()->playEffect(media.string(), 1.f, 0.f, volume);
			}
		}

		duration = std::clamp(duration, 1.f, 60.f);

		std::string text = mod->getSettingValue<bool>("show-text") ? buildText(percent) : std::string();
		bool freeze = mod->getSettingValue<bool>("freeze-game");

		auto overlay = TrollOverlay::create(this, text, freeze, duration);
		if (overlay) scene->addChild(overlay, 100000);
	}
};

// Make sure the media folder exists as soon as the mod loads, and log where it is.
$on_mod(Loaded) {
	log::info("Percent Troll media folder: {}", mediaDir().string());
}
