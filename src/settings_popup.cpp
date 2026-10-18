#include "settings_popup.hpp"
#include "spectrogram.hpp"

#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/Scrollbar.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/Keyboard.hpp>
#include <eclipse.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <vector>

using namespace geode::prelude;

namespace waveform {
namespace {
// Eclipse stores menu.toggleKey as its key enum in the low byte and Geode
// modifiers in the next byte. Translate that public config value to a Geode
// input event to dismiss the overlay before presenting a Cocos modal. Sending
// the event directly does not dispatch a key to the editor/gameplay controls.
enumKeyCodes eclipseKeyboardKey(int key) {
    if (key >= 1 && key <= 26) return enumKeyCodes(KEY_A + key - 1);
    if (key >= 41 && key <= 50) return enumKeyCodes(KEY_Zero + key - 41);
    if (key >= 51 && key <= 74) return enumKeyCodes(KEY_F1 + key - 51);
    if (key >= 76 && key <= 85) return enumKeyCodes(KEY_NumPad0 + key - 76);
    constexpr std::array symbols{KEY_Space, KEY_Apostrophe, KEY_OEMComma, KEY_OEMMinus,
        KEY_OEMPeriod, KEY_Slash, KEY_Semicolon, KEY_Equal, KEY_LeftBracket, KEY_Backslash,
        KEY_RightBracket, KEY_GraveAccent, KEY_World1, KEY_World2};
    if (key >= 27 && key <= 40) return symbols[key - 27];
    constexpr std::array special{KEY_Decimal, KEY_Divide, KEY_Multiply, KEY_Subtract, KEY_Add,
        KEY_NumEnter, KEY_OEMEqual, KEY_ApplicationsKey, KEY_Escape, KEY_Enter, KEY_Tab,
        KEY_Backspace, KEY_Insert, KEY_Delete, KEY_Home, KEY_End, KEY_PageUp, KEY_PageDown,
        KEY_CapsLock, KEY_ScrollLock, KEY_Numlock, KEY_PrintScreen, KEY_Pause, KEY_Up,
        KEY_Down, KEY_Left, KEY_Right, KEY_LeftShift, KEY_LeftControl, KEY_LeftMenu,
        KEY_LeftWindowsKey, KEY_RightShift, KEY_RightControl, KEY_RightMenu, KEY_RightWindowsKey};
    if (key >= 86 && key <= 120) return special[key - 86];
    constexpr std::array controller{CONTROLLER_A, CONTROLLER_B, CONTROLLER_X, CONTROLLER_Y,
        CONTROLLER_Up, CONTROLLER_Down, CONTROLLER_Left, CONTROLLER_Right, CONTROLLER_LB,
        CONTROLLER_RB, CONTROLLER_LT, CONTROLLER_RT, CONTROLLER_Back, CONTROLLER_Start};
    if (key >= 126 && key <= 139) return controller[key - 126];
    return KEY_None;
}
void dismissEclipse() {
    int binding = eclipse::config::getInternal<int>("menu.toggleKey", 96); // Tab
    int key = binding & 0xff;
    auto modifiers = KeyboardModifier(uint8_t((binding >> 8) & 0xff));
    auto timestamp = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (key >= 121 && key <= 125) {
        auto button = MouseInputData::Button(key - 121);
        MouseInputData down(button, MouseInputData::Action::Press, timestamp, modifiers);
        MouseInputEvent().send(down);
        MouseInputData up(button, MouseInputData::Action::Release, timestamp, modifiers);
        MouseInputEvent().send(up);
    } else if (auto code = eclipseKeyboardKey(key); code != KEY_None) {
        KeyboardInputData down(code, KeyboardInputData::Action::Press, {}, timestamp, modifiers);
        KeyboardInputEvent(code).send(down);
        KeyboardInputData up(code, KeyboardInputData::Action::Release, {}, timestamp, modifiers);
        KeyboardInputEvent(code).send(up);
    }
}
std::string numberText(double value) {
    auto text = fmt::format("{:.2f}", value);
    while (text.ends_with('0')) text.pop_back();
    if (text.ends_with('.')) text.pop_back();
    return text == "-0" ? "0" : text;
}

// ScrollLayer clips drawing, but menus also need to reject touches outside it.
class SettingsMenu final : public CCMenu {
    ScrollLayer* m_viewport = nullptr;
    bool ccTouchBegan(CCTouch* touch, CCEvent* event) override {
        auto point = m_viewport->convertToNodeSpace(touch->getLocation());
        if (!CCRect({0, 0}, m_viewport->getContentSize()).containsPoint(point)) return false;
        return CCMenu::ccTouchBegan(touch, event);
    }
public:
    static SettingsMenu* create(ScrollLayer* viewport) {
        auto menu = new SettingsMenu;
        if (menu->init()) {
            menu->m_viewport = viewport;
            menu->setPosition({0, 0});
            menu->setTouchPriority(viewport->getTouchPriority() - 1);
            menu->autorelease();
            return menu;
        }
        delete menu;
        return nullptr;
    }
};

bool rowInViewport(CCNode* row, ScrollLayer* viewport) {
    if (row->getParent() != viewport->m_contentLayer || !cocos::nodeIsVisible(row)) return false;
    auto bottom = row->getPositionY() + viewport->m_contentLayer->getPositionY();
    return bottom >= 0 && bottom + row->getContentHeight() <= viewport->getContentHeight();
}

class SettingsInput final : public TextInput {
    ScrollLayer* m_viewport = nullptr;
    bool allowTextInput(CCTextInputNode*) override {
        return rowInViewport(getParent(), m_viewport);
    }
public:
    static SettingsInput* create(float width, ScrollLayer* viewport) {
        auto input = new SettingsInput;
        if (input->init(width, "0", "bigFont.fnt")) {
            input->m_viewport = viewport;
            input->autorelease();
            return input;
        }
        delete input;
        return nullptr;
    }
};

class SettingsPopup final : public Popup {
    struct Number {
        std::string key;
        double minimum, maximum;
        bool integer;
        Slider* slider;
        TextInput* input;
        // Large editor-coordinate ranges need finer control around the origin.
        double axis(double value) const {
            if (key == "spectrogram-window-size") return std::log2(value);
            return maximum >= 1e6 ? std::copysign(std::log1p(std::abs(value) / 10), value) : value;
        }
        float position(double value) const {
            return float((axis(value) - axis(minimum)) / (axis(maximum) - axis(minimum)));
        }
        double value(float position) const {
            auto mapped = axis(minimum) + std::clamp(position, 0.f, 1.f) * (axis(maximum) - axis(minimum));
            if (key == "spectrogram-window-size") return std::exp2(mapped);
            return maximum >= 1e6 ? std::copysign(10 * std::expm1(std::abs(mapped)), mapped) : mapped;
        }
        double stored() const {
            auto mod = Mod::get();
            if (key == "spectrogram-window-size")
                return windowSizeFromSetting(mod->getSettingValue<std::string>(key));
            return integer ? double(mod->getSettingValue<int64_t>(key)) : mod->getSettingValue<double>(key);
        }
        void store(double value) const {
            value = std::clamp(value, minimum, maximum);
            auto mod = Mod::get();
            if (key == "spectrogram-window-size") {
                auto window = 1u << unsigned(std::clamp(std::round(std::log2(value)), 9., 13.));
                auto text = std::to_string(window);
                if (mod->getSettingValue<std::string>(key) != text) mod->setSettingValue<std::string>(key, text);
            } else if (integer) {
                auto rounded = int64_t(std::round(value));
                if (mod->getSettingValue<int64_t>(key) != rounded) mod->setSettingValue<int64_t>(key, rounded);
            } else if (mod->getSettingValue<double>(key) != value) mod->setSettingValue<double>(key, value);
        }
    };
    static constexpr float RowWidth = 374;
    static constexpr float ViewportHeight = 244;
    static constexpr float RowHeight = 28;
    static constexpr float HeaderHeight = 28;
    static constexpr float ChoiceWidth = 110;
    struct Row {
        std::string key;
        Ref<CCNode> node;
        CCMenu* menu;
        CCSprite* resetIcon;
        bool interactive = false;
    };
    struct Section {
        Ref<CCNode> header;
        CCMenu* menu;
        CCSprite* arrow;
        std::vector<size_t> rows;
        bool expanded = true;
    };
    struct Choice { std::string key; std::vector<std::string> values; CCLabelBMFont* label; };
    std::vector<Row> m_rows;
    std::vector<Section> m_sections;
    std::vector<Number> m_numbers;
    std::vector<Choice> m_choices;
    ScrollLayer* m_scroll = nullptr;
    Scrollbar* m_scrollbar = nullptr;
    bool m_shiftHidden = false;
    GLubyte m_backdropOpacity = 0;

    static void setMenuInteractive(CCMenu* menu, bool interactive) {
        if (!interactive && menu->m_eState == kCCMenuStateTrackingTouch)
            menu->CCMenu::ccTouchCancelled(nullptr, nullptr);
        menu->setEnabled(interactive);
        menu->setTouchEnabled(interactive);
    }

    void updateShiftVisibility() {
        bool hidden = CCKeyboardDispatcher::get()->getShiftKeyPressed();
        if (hidden == m_shiftHidden) return;
        m_shiftHidden = hidden;
        // Keep the modal alive and blocking editor input while previewing the grid.
        // Poll the modifier state so hiding never depends on receiving a key-up.
        m_mainLayer->setVisible(!hidden);
        setOpacity(hidden ? 0 : m_backdropOpacity);
        setMenuInteractive(m_buttonMenu, !hidden);
        for (auto const& section : m_sections)
            setMenuInteractive(section.menu, !hidden);
        if (hidden) {
            for (auto const& number : m_numbers) number.input->defocus();
            m_scroll->m_contentLayer->stopAllActions();
            static_cast<CCLayer*>(m_scrollbar)->ccTouchCancelled(nullptr, nullptr);
        }
        m_scroll->setTouchEnabled(!hidden);
        m_scroll->enableScrollWheel(!hidden);
        m_scrollbar->setTouchEnabled(!hidden && m_scrollbar->isVisible());
        updateInteraction();
    }

    void keyDown(enumKeyCodes key, double timestamp) override {
        updateShiftVisibility();
        if (!m_shiftHidden) Popup::keyDown(key, timestamp);
    }

    void keyBackClicked() override {
        updateShiftVisibility();
        if (!m_shiftHidden) Popup::keyBackClicked();
    }

    CCLabelBMFont* label(CCNode* parent, std::string const& text, CCPoint position,
        float scale = .3f, char const* font = "bigFont.fnt") {
        auto result = CCLabelBMFont::create(text.c_str(), font);
        result->setScale(scale);
        result->setAnchorPoint({0, .5f});
        result->setPosition(position);
        parent->addChild(result);
        return result;
    }
    void addSection(std::string const& title) {
        auto header = CCNode::create();
        header->setContentSize({RowWidth, HeaderHeight});
        header->setAnchorPoint({0, 0});
        header->setID(title + "-section");
        auto menu = SettingsMenu::create(m_scroll);
        header->addChild(menu);
        auto background = CCLayerColor::create({0, 0, 0, 45}, RowWidth, HeaderHeight);
        header->addChild(background, -1);
        auto face = CCNode::create();
        face->setContentSize({RowWidth, HeaderHeight});
        auto button = CCMenuItemSpriteExtra::create(face, this, menu_selector(SettingsPopup::onSection));
        button->m_animationEnabled = false;
        button->setTag(int(m_sections.size()));
        button->setPosition({RowWidth / 2, HeaderHeight / 2});
        button->setID("collapse-button");
        menu->addChild(button);
        auto titleLabel = label(face, title, {10, HeaderHeight / 2}, .42f, "goldFont.fnt");
        titleLabel->limitLabelWidth(RowWidth - 40, .42f, .3f);
        auto arrow = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        arrow->setScale(.32f);
        arrow->setRotation(-90);
        arrow->setPosition({RowWidth - 15, HeaderHeight / 2});
        face->addChild(arrow);
        m_sections.push_back({header, menu, arrow, {}});
    }
    Row& addRow(std::string const& key, std::string const& title) {
        auto node = CCNode::create();
        node->setContentSize({RowWidth, RowHeight});
        node->setAnchorPoint({0, 0});
        node->setID(key);
        auto background = CCLayerColor::create(
            {0, 0, 0, uint8_t(m_sections.back().rows.size() % 2 ? 35 : 15)}, RowWidth, RowHeight);
        node->addChild(background);
        auto name = label(node, title, {24, RowHeight / 2}, .28f);
        name->limitLabelWidth(113, .28f, .22f);
        auto menu = SettingsMenu::create(m_scroll);
        menu->setEnabled(false);
        node->addChild(menu);
        auto index = m_rows.size();
        auto resetIcon = CCSprite::createWithSpriteFrameName("geode.loader/reset-gold.png");
        resetIcon->setScale(.5f);
        auto reset = CCMenuItemSpriteExtra::create(resetIcon, this, menu_selector(SettingsPopup::onReset));
        reset->setPosition({337, RowHeight / 2});
        reset->setTag(int(index));
        reset->setID("reset-button");
        menu->addChild(reset);
        auto infoIcon = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
        infoIcon->setScale(.35f);
        auto info = CCMenuItemSpriteExtra::create(infoIcon, this, menu_selector(SettingsPopup::onHelp));
        info->setPosition({361, RowHeight / 2});
        info->setTag(int(index));
        info->setID("info-button");
        menu->addChild(info);
        m_rows.push_back({key, node, menu, resetIcon});
        m_sections.back().rows.push_back(index);
        return m_rows.back();
    }
    void addNumber(std::string key, std::string title,
        double minimum, double maximum, bool integer = false) {
        auto& controls = addRow(key, title);
        auto index = m_numbers.size();
        auto slider = Slider::create(this, menu_selector(SettingsPopup::onSlider), .7f);
        // Slider is a CCLayer with a screen-sized content box. Its artwork is
        // centered at the origin; using that box's default anchor displaces it.
        slider->ignoreAnchorPointForPosition(false);
        slider->setAnchorPoint({0, 0});
        slider->setScale(100.f / slider->m_groove->getScaledContentWidth());
        slider->setPosition({197, RowHeight / 2});
        slider->getThumb()->setTag(int(index));
        slider->setLiveDragging(true);
        slider->m_touchLogic->setTouchPriority(m_scroll->getTouchPriority() - 1);
        slider->disableTouch();
        slider->setID("slider");
        controls.node->addChild(slider);
        auto input = SettingsInput::create(85, m_scroll);
        input->setScale(.65f);
        input->setPosition({287, RowHeight / 2});
        input->setCommonFilter(integer ? CommonFilter::Int : CommonFilter::Float);
        input->setMaxCharCount(12);
        input->getInputNode()->setTouchPriority(m_scroll->getTouchPriority() - 1);
        input->setEnabled(false);
        input->setID("value-input");
        controls.node->addChild(input);
        m_numbers.push_back({key, minimum, maximum, integer, slider, input});
        refreshNumber(m_numbers.back());
        input->setCallback([this, index](std::string const& text) {
            auto& row = m_numbers[index];
            if (!rowInViewport(row.input->getParent(), m_scroll)) return;
            char* end = nullptr;
            auto value = std::strtod(text.c_str(), &end);
            bool valid = end != text.c_str() && end == text.c_str() + text.size() && std::isfinite(value);
            row.input->getBGSprite()->setColor(valid ? ccBLACK : ccc3(255, 100, 100));
            if (!valid) return;
            row.store(value);
            row.slider->setValue(row.position(row.stored()));
            refreshResets();
        });
    }
    void addChoice(std::string key, std::string title, std::vector<std::string> values) {
        auto& controls = addRow(key, title);
        auto field = NineSlice::create("square02b_001.png");
        field->setContentSize({ChoiceWidth + 6, 19.5f});
        field->setColor(ccBLACK);
        field->setOpacity(90);
        field->setPosition({231, RowHeight / 2});
        controls.node->addChild(field);
        auto value = label(controls.node, Mod::get()->getSettingValue<std::string>(key),
            {231, RowHeight / 2}, .33f);
        value->setAnchorPoint({.5f, .5f});
        value->limitLabelWidth(ChoiceWidth, .33f, .2f);
        auto index = m_choices.size();
        m_choices.push_back({key, std::move(values), value});
        for (int direction = 0; direction < 2; ++direction) {
            auto arrow = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
            arrow->setScale(.35f);
            arrow->setFlipX(direction == 1);
            auto button = CCMenuItemSpriteExtra::create(arrow, this, menu_selector(SettingsPopup::onChoice));
            button->setTag(int(index) * 2 + direction);
            button->setPosition({direction ? 306.f : 157.f, RowHeight / 2});
            controls.menu->addChild(button);
        }
    }
    void refreshNumber(Number const& row) {
        row.slider->setValue(row.position(row.stored()));
        auto text = numberText(row.stored());
        if (row.input->getString() != text) row.input->setString(text);
        row.input->getBGSprite()->setColor(ccBLACK);
    }
    void refreshResets() {
        for (auto const& row : m_rows) {
            bool isDefault = Mod::get()->getSetting(row.key)->isDefaultValue();
            row.resetIcon->setColor(isDefault ? ccGRAY : ccWHITE);
            row.resetIcon->setOpacity(isDefault ? 140 : 255);
        }
    }
    void updateInteraction() {
        for (auto& row : m_rows) {
            bool interactive = rowInViewport(row.node, m_scroll);
            if (row.interactive == interactive) continue;
            row.interactive = interactive;
            setMenuInteractive(row.menu, interactive);
            // A partially clipped row remains drawn, but cannot begin editing.
            for (auto const& number : m_numbers) {
                if (number.input->getParent() != row.node.data()) continue;
                if (!interactive) number.input->defocus();
                number.input->setEnabled(interactive);
                if (!interactive && number.slider->getThumb()->isSelected())
                    number.slider->m_touchLogic->CCMenu::ccTouchCancelled(nullptr, nullptr);
                number.slider->m_touchLogic->setTouchEnabled(interactive);
            }
        }
    }
    void layoutSections() {
        auto content = m_scroll->m_contentLayer;
        auto previousHeight = content->getContentHeight();
        auto previousY = content->getPositionY();
        float height = 0;
        for (auto const& section : m_sections)
            height += HeaderHeight + (section.expanded ? RowHeight * section.rows.size() : 0);
        content->setContentSize({RowWidth, std::max(height, m_scroll->getContentHeight())});
        float y = content->getContentHeight();
        for (auto& section : m_sections) {
            if (!section.header->getParent()) content->addChild(section.header);
            y -= HeaderHeight;
            section.header->setPosition({0, y});
            section.arrow->setRotation(section.expanded ? -90 : 180);
            for (auto index : section.rows) {
                auto& row = m_rows[index];
                if (section.expanded) {
                    if (!row.node->getParent()) content->addChild(row.node);
                    y -= RowHeight;
                    row.node->setPosition({0, y});
                } else if (row.node->getParent()) {
                    // Retain detached rows so values and callbacks survive reopening.
                    row.node->removeFromParentAndCleanup(false);
                }
            }
        }
        auto minimumY = m_scroll->getContentHeight() - content->getContentHeight();
        content->setPositionY(std::clamp(previousY + previousHeight - content->getContentHeight(), minimumY, 0.f));
        m_scrollbar->setVisible(content->getContentHeight() > m_scroll->getContentHeight());
        updateInteraction();
    }
    void onSection(CCObject* sender) {
        auto& section = m_sections.at(static_cast<CCNode*>(sender)->getTag());
        section.expanded = !section.expanded;
        for (auto const& row : m_numbers) row.input->defocus();
        layoutSections();
    }
    void onReset(CCObject* sender) {
        auto const& controls = m_rows.at(static_cast<CCNode*>(sender)->getTag());
        if (!rowInViewport(controls.node, m_scroll)) return;
        auto setting = Mod::get()->getSetting(controls.key);
        if (!setting->isDefaultValue()) setting->reset();
        for (auto const& row : m_numbers) {
            if (row.key != controls.key) continue;
            row.input->defocus();
            refreshNumber(row);
        }
        for (auto const& row : m_choices) {
            if (row.key != controls.key) continue;
            row.label->setString(Mod::get()->getSettingValue<std::string>(row.key).c_str());
            row.label->limitLabelWidth(ChoiceWidth, .33f, .2f);
        }
        refreshResets();
    }
    void onSlider(CCObject* sender) {
        auto& row = m_numbers.at(static_cast<CCNode*>(sender)->getTag());
        if (!rowInViewport(row.input->getParent(), m_scroll)) return;
        row.input->defocus();
        auto value = row.value(row.slider->getValue());
        row.store(row.integer ? std::round(value) : std::round(value * 100) / 100);
        refreshNumber(row);
        refreshResets();
    }
    void onChoice(CCObject* sender) {
        auto tag = static_cast<CCNode*>(sender)->getTag();
        auto& row = m_choices.at(tag / 2);
        if (!rowInViewport(row.label->getParent(), m_scroll)) return;
        auto current = Mod::get()->getSettingValue<std::string>(row.key);
        auto selected = std::find(row.values.begin(), row.values.end(), current);
        int index = selected == row.values.end() ? 0 : int(selected - row.values.begin());
        index = (index + (tag % 2 ? 1 : -1) + int(row.values.size())) % int(row.values.size());
        Mod::get()->setSettingValue<std::string>(row.key, row.values[index]);
        row.label->setString(row.values[index].c_str());
        row.label->limitLabelWidth(ChoiceWidth, .33f, .2f);
        refreshResets();
    }
    void onHelp(CCObject* sender) {
        auto const& row = m_rows.at(static_cast<CCNode*>(sender)->getTag());
        if (!rowInViewport(row.node, m_scroll)) return;
        for (auto const& number : m_numbers) number.input->defocus();
        auto setting = Mod::get()->getSetting(row.key);
        FLAlertLayer::create(setting->getDisplayName().c_str(), setting->getDescription().value_or(""), "OK")->show();
    }
    void update(float) override {
        updateShiftVisibility();
        updateInteraction();
        for (auto const& row : m_numbers) {
            if (row.input->getInputNode()->m_selected)
                row.slider->setValue(row.position(row.stored()));
            else refreshNumber(row);
        }
        for (auto const& row : m_choices) {
            auto value = Mod::get()->getSettingValue<std::string>(row.key);
            if (row.label->getString() != value) {
                row.label->setString(value.c_str());
                row.label->limitLabelWidth(ChoiceWidth, .33f, .2f);
            }
        }
        refreshResets();
    }
    bool init() override {
        if (!Popup::init(420, 310)) return false;
        m_backdropOpacity = getOpacity();
        setID("waveform-settings"_spr);
        auto title = label(m_mainLayer, "Editor Waveform", {26, 286}, .65f, "goldFont.fnt");
        title->limitLabelWidth(360, .65f, .4f);
        auto panel = CCLayerColor::create({0, 0, 0, 75}, RowWidth, ViewportHeight);
        panel->setPosition({16, 28});
        m_mainLayer->addChild(panel);
        m_scroll = ScrollLayer::create(CCSize{RowWidth, ViewportHeight});
        m_scroll->ignoreAnchorPointForPosition(false);
        m_scroll->setAnchorPoint({0, 0});
        m_scroll->setPosition({16, 28});
        m_scroll->setStealingTouches(true);
        m_scroll->setID("settings-scroll");
        m_mainLayer->addChild(m_scroll);
        m_scrollbar = Scrollbar::create(m_scroll);
        m_scrollbar->setAnchorPoint({.5f, .5f});
        m_scrollbar->setPosition({401, 150});
        m_mainLayer->addChild(m_scrollbar);
        m_buttonMenu->setTouchPriority(m_scroll->getTouchPriority() - 1);
        auto note = label(m_mainLayer, "Changes apply immediately | Hold Shift to hide", {16, 14}, .23f);
        note->limitLabelWidth(384, .23f, .2f);
        addSection("Waveform");
        addNumber("vertical-offset", "Offset", -1e6, 1e6);
        addNumber("vertical-scale", "Height", 0, 1e6);
        addNumber("line-width", "Line width (px)", 1, 10, true);
        addNumber("opacity", "Opacity", 0, 255, true);
        addSection("Spectrogram");
        addNumber("spectrogram-vertical-offset", "Offset", -1e6, 1e6);
        addNumber("spectrogram-height", "Height", 0, 1e6);
        addNumber("spectrogram-opacity", "Opacity", 0, 255, true);
        addNumber("spectrogram-frequency-scale", "Linear / log", 0, 10);
        addNumber("spectrogram-min-frequency", "Min frequency (Hz)", 0, 16000);
        addNumber("spectrogram-max-frequency", "Max frequency (Hz)", 0, 16000);
        addNumber("spectrogram-window-size", "Window size", 512, 8192, true);
        addChoice("spectrogram-colormap", "Colormap", {"Magma", "Viridis", "Inferno", "Grayscale"});
        layoutSections();
        refreshResets();
        updateShiftVisibility();
        updateInteraction();
        scheduleUpdate();
        return true;
    }
public:
    static SettingsPopup* create() {
        auto popup = new SettingsPopup;
        if (popup->init()) { popup->autorelease(); return popup; }
        delete popup;
        return nullptr;
    }
};
}

void showSettingsFromEclipse() {
    static WeakRef<SettingsPopup> active;
    if (active.lock()) return;
    if (auto popup = SettingsPopup::create()) {
        dismissEclipse();
        active = popup;
        popup->show();
    }
}
}
