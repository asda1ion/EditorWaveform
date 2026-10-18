#pragma once

#include <Geode/Geode.hpp>
#include "waveform.hpp"
#include "config.hpp"
#include <filesystem>

namespace waveform {
class Timeline final : public cocos2d::CCNode {
    LevelEditorLayer* m_editor = nullptr;
    geode::Ref<cocos2d::CCDrawNode> m_overlay;
    geode::Ref<cocos2d::CCSprite> m_spectrogramSprite;
    SourceLoader m_source;
    std::string m_path;
    std::filesystem::file_time_type m_modified{};
    float m_songOffset = 0;
    float m_pollTimer = 0;
    bool m_enabled = false;
    bool m_suspended = false;
    float m_worldBaseline = 0;
    float m_spectralWorldBaseline = 0;
    float m_rasterCooldown = 0;
    unsigned m_analysisWindow = 0;
    uint64_t m_sourceRevision = 0;
    bool m_analysisChanged = false;
    struct RasterView {
        uint64_t revision = 0;
        unsigned rows = 0;
        double minimum = 0, maximum = 0, scale = 0;
        Colormap colormap = Colormap::Magma;
        std::vector<double> timeEdges;
        bool operator==(RasterView const&) const = default;
    } m_rasterView;
    float m_rasterLeft = 0, m_rasterWidth = 0;

    bool init(LevelEditorLayer* editor);
    void pollSong();
    void renderGrid(Config const& config);
    void renderSpectrogram(Config const& config);
    void clearSpectrogram();
public:
    static Timeline* create(LevelEditorLayer* editor);
    void update(float dt) override;
    void onExit() override;
    void onEnter() override;
};
}
