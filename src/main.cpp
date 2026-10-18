#include <Geode/Geode.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/loader/ModEvent.hpp>
#include <eclipse.hpp>
#include "settings_popup.hpp"
#include "timeline.hpp"

using namespace geode::prelude;

class $modify(WaveformEditor, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        if (!LevelEditorLayer::init(level, noUI)) return false;
        if (!noUI && m_editorUI && m_drawGridLayer) {
            if (auto timeline = waveform::Timeline::create(this)) addChild(timeline);
        }
        return true;
    }
};

namespace {
void registerEclipse() {
    static bool registered = false;
    auto eclipseMod = Loader::get()->getLoadedMod("eclipse.eclipse-menu");
    if (registered || !eclipseMod) return;
    registered = true;
    auto tab = eclipse::MenuTab::find("Editor Waveform");
    auto mod = Mod::get();
    for (auto key : {"enabled", "spectrogram-enabled", "lock-to-camera"})
        tab.addModSettingToggle(mod->getSetting(key));
    tab.addButton("Settings", [] {
        // Leave Eclipse's drawing/input callback before opening a Cocos modal.
        Loader::get()->queueInMainThread([] { waveform::showSettingsFromEclipse(); });
    }).setDescription("Waveform and spectrogram appearance and analysis settings.");
}
}
$on_mod(Loaded) { registerEclipse(); }
$execute {
    if (auto eclipseMod = Loader::get()->getInstalledMod("eclipse.eclipse-menu")) {
        ModStateEvent(ModEventType::Loaded, eclipseMod).listen([] { registerEclipse(); }).leak();
    }
}
