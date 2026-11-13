#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

class $modify(TrollPlayLayer, PlayLayer) {
	void destroyPlayer(PlayerObject* player, GameObject* object) override {
		// Grab the percent and dead state before the original runs,
		// so we only react to the death that actually happens.
		float percent = this->getCurrentPercent();
		bool alreadyDead = m_player1->m_isDead;

		PlayLayer::destroyPlayer(player, object);

		// Ignore if already dead, or if the death was blocked (e.g. noclip).
		if (alreadyDead || !m_player1->m_isDead) return;

		auto mod = Mod::get();

		if (m_isPracticeMode && !mod->getSettingValue<bool>("practice-mode")) return;

		int target = static_cast<int>(mod->getSettingValue<int64_t>("target-percent"));

		// 67 -> triggers from 67.0 up to (but not including) 68.0
		if (percent < target || percent >= target + 1) return;

		auto path = mod->getSettingValue<std::filesystem::path>("sound-file");
		if (path.empty() || !std::filesystem::exists(path)) {
			log::warn("Percent Troll: no valid sound file set");
			return;
		}

		float volume = static_cast<float>(mod->getSettingValue<double>("volume"));
		FMODAudioEngine::sharedEngine()->playEffect(path.string(), 1.f, 0.f, volume);
	}
};
