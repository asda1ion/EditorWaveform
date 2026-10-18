#include "timeline.hpp"

using namespace geode::prelude;

namespace waveform {
namespace {
void rectangle(CCDrawNode* node, float x1, float y1, float x2, float y2, ccColor4F color) {
    if (!std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) || !std::isfinite(y2)) return;
    CCPoint points[] = {{x1, y1}, {x2, y1}, {x2, y2}, {x1, y2}};
    node->drawPolygon(points, 4, color, 0, {0, 0, 0, 0});
}
}
Timeline* Timeline::create(LevelEditorLayer* editor) {
    auto result = new Timeline;
    if (result->init(editor)) { result->autorelease(); return result; }
    delete result; return nullptr;
}
bool Timeline::init(LevelEditorLayer* editor) {
    if (!CCNode::init()) return false;
    m_editor = editor;
    setID("editor-waveform"_spr);
    m_overlay = CCDrawNode::create(); m_overlay->setID("editor-waveform-grid"_spr);
    editor->m_drawGridLayer->addChild(m_overlay, 1);
    scheduleUpdate();
    return true;
}
void Timeline::pollSong() {
    auto path = std::string(CCFileUtils::sharedFileUtils()->fullPathForFilename(m_editor->m_level->getAudioFileName().c_str(), false));
    std::error_code error;
    auto modified = std::filesystem::last_write_time(path, error);
    auto changed = path != m_path || modified != m_modified;
    if (changed || m_analysisChanged) {
        m_path = path; m_modified = modified;
        m_source.load(FMODAudioEngine::get()->m_system, path, m_analysisWindow, !changed);
        ++m_sourceRevision;
        m_analysisChanged = false;
        clearSpectrogram();
    }
    m_songOffset = m_editor->m_levelSettings->m_songOffset;
}
void Timeline::update(float dt) {
    if (m_suspended) return;
    auto config = Config::get();
    auto enabled = (config.enabled || config.spectrogramEnabled) && !m_editor->m_levelSettings->m_platformerMode;
    if (enabled != m_enabled) {
        m_enabled = enabled;
        if (!m_enabled) { m_source.cancel(); clearSpectrogram(); }
        else {
            m_path.clear(); m_pollTimer = 0;
        }
    }
    setVisible(m_enabled); m_overlay->setVisible(m_enabled && config.enabled);
    if (!m_enabled) return;
    auto window = config.spectrogramEnabled ? config.windowSize : 0;
    if (window != m_analysisWindow) {
        m_analysisWindow = window; m_analysisChanged = true; m_pollTimer = 0;
    }
    m_songOffset = m_editor->m_levelSettings->m_songOffset;
    m_pollTimer -= dt;
    if (m_pollTimer <= 0) { pollSong(); m_pollTimer = 1; }
    renderGrid(config);
    m_rasterCooldown = std::max(0.f, m_rasterCooldown - dt);
    renderSpectrogram(config);
}
void Timeline::renderGrid(Config const& config) {
    m_overlay->clear();
    if (!config.enabled) return;
    auto source = m_source.audio();
    if (!source || !source->sampleRate) return;
    auto grid = m_editor->m_drawGridLayer;
    auto view = CCDirector::sharedDirector()->getWinSize();
    auto pixels = CCDirector::sharedDirector()->getOpenGLView()->getFrameSize().width * CCDirector::sharedDirector()->getContentScaleFactor();
    auto left = grid->convertToNodeSpace({0, 0});
    auto right = grid->convertToNodeSpace({view.width, 0});
    auto columns = std::clamp(static_cast<int>(pixels / config.lineWidth), 1, 4096);
    auto step = (right.x - left.x) / columns;
    if (!std::isfinite(step) || step <= 0) return;
    auto baseline = config.lockToCamera ? grid->convertToNodeSpace({0, config.verticalOffset}).y : m_worldBaseline + config.verticalOffset;
    if (config.lockToCamera) m_worldBaseline = baseline - config.verticalOffset;
    ccColor4F color{31.f / 255, 31.f / 255, 31.f / 255, config.opacity / 255.f};
    auto order = m_editor->getSelectedEditorOrder();
    auto channel = m_editor->getSelectedOrderChannel();
    for (int column = 0; column < columns; ++column) {
        auto x = left.x + column * step;
        auto begin = grid->timeForPos({x, left.y}, order, channel, false, false, false, 0) + m_songOffset;
        auto end = grid->timeForPos({x + step, left.y}, order, channel, false, false, false, 0) + m_songOffset;
        auto peak = source->envelope.atTime(begin, end, source->sampleRate);
        if (peak) {
            auto amplitude = std::min(*peak, 4.f) * config.verticalScale;
            rectangle(m_overlay, x, baseline - amplitude, x + step * .9f, baseline + amplitude, color);
        }
    }
}
void Timeline::clearSpectrogram() {
    if (m_spectrogramSprite) m_spectrogramSprite->removeFromParentAndCleanup(true);
    m_spectrogramSprite = nullptr;
    m_rasterView = {}; m_rasterCooldown = 0;
}
void Timeline::renderSpectrogram(Config const& config) {
    auto source = m_source.spectrogram();
    auto visible = config.spectrogramEnabled && config.spectrogramHeight > 0 && config.spectrogramOpacity > 0 && source;
    if (m_spectrogramSprite) m_spectrogramSprite->setVisible(visible);
    if (!visible) return;
    auto grid = m_editor->m_drawGridLayer;
    auto director = CCDirector::sharedDirector();
    auto view = director->getWinSize();
    auto frame = director->getOpenGLView()->getFrameSize();
    auto factor = director->getContentScaleFactor();
    auto left = grid->convertToNodeSpace({0, 0});
    auto right = grid->convertToNodeSpace({view.width, 0});
    auto width = right.x - left.x;
    if (!std::isfinite(width) || width <= 0 || view.width <= 0 || view.height <= 0) return;
    auto baseline = config.lockToCamera ? grid->convertToNodeSpace({0, config.spectrogramOffset}).y : m_spectralWorldBaseline + config.spectrogramOffset;
    if (config.lockToCamera) m_spectralWorldBaseline = baseline - config.spectrogramOffset;
    auto bottom = grid->convertToWorldSpace({0, baseline - config.spectrogramHeight / 2});
    auto top = grid->convertToWorldSpace({0, baseline + config.spectrogramHeight / 2});
    auto physicalHeight = std::abs(top.y - bottom.y) * frame.height * factor / view.height;
    auto physicalWidth = frame.width * factor;
    if (!std::isfinite(physicalHeight) || !std::isfinite(physicalWidth)) return;
    auto columns = static_cast<unsigned>(std::clamp(std::ceil(physicalWidth / 2.0), 1.0, 2048.0));
    auto rows = static_cast<unsigned>(std::clamp(std::ceil(physicalHeight / 2.0), 1.0, 512.0));
    auto range = frequencyRange(config.minimumFrequency, config.maximumFrequency, source->sampleRate);
    if (!range.valid()) {
        if (m_spectrogramSprite) m_spectrogramSprite->setVisible(false);
        return;
    }
    RasterView requested{m_sourceRevision, rows, range.minimum, range.maximum, config.frequencyScale, config.colormap, {}};
    requested.timeEdges.reserve(columns + 1);
    auto order = m_editor->getSelectedEditorOrder();
    auto channel = m_editor->getSelectedOrderChannel();
    for (unsigned column = 0; column <= columns; ++column) {
        auto x = left.x + width * column / columns;
        requested.timeEdges.push_back(grid->timeForPos({x, left.y}, order, channel, false, false, false, 0) + m_songOffset);
    }
    if (requested != m_rasterView && m_rasterCooldown <= 0) {
        auto image = rasterSpectrogram(*source, requested.timeEdges, rows, range, config.frequencyScale, config.colormap);
        auto texture = new CCTexture2D;
        if (!texture->initWithData(image.data(), kCCTexture2DPixelFormat_RGBA8888, columns, rows, {float(columns), float(rows)})) {
            texture->release(); return;
        }
        texture->setAliasTexParameters();
        if (!m_spectrogramSprite) {
            m_spectrogramSprite = CCSprite::createWithTexture(texture);
            m_spectrogramSprite->setID("editor-spectrogram-grid"_spr);
            m_spectrogramSprite->setAnchorPoint({0, 0});
            // Cocos image rectangles treat the first uploaded row as the top.
            m_spectrogramSprite->setFlipY(true);
            grid->addChild(m_spectrogramSprite, 0);
        } else {
            m_spectrogramSprite->setTexture(texture);
            auto size = texture->getContentSize();
            m_spectrogramSprite->setTextureRect({0, 0, size.width, size.height});
        }
        texture->release();
        m_rasterView = std::move(requested);
        m_rasterLeft = left.x; m_rasterWidth = width;
        m_rasterCooldown = 1.f / 30;
    }
    if (m_spectrogramSprite) {
        auto size = m_spectrogramSprite->getContentSize();
        m_spectrogramSprite->setVisible(true);
        m_spectrogramSprite->setPosition({m_rasterLeft, baseline - config.spectrogramHeight / 2});
        m_spectrogramSprite->setScaleX(m_rasterWidth / size.width);
        m_spectrogramSprite->setScaleY(config.spectrogramHeight / size.height);
        m_spectrogramSprite->setOpacity(config.spectrogramOpacity);
    }
}
void Timeline::onExit() {
    m_suspended = true; m_source.cancel(); m_overlay->setVisible(false);
    clearSpectrogram();
    CCNode::onExit();
}
void Timeline::onEnter() {
    CCNode::onEnter(); m_suspended = false; m_enabled = false; m_pollTimer = 0;
}
}
