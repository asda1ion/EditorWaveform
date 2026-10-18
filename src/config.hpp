#pragma once

#include <Geode/loader/Mod.hpp>
#include <algorithm>
#include <cmath>
#include "spectrogram.hpp"

namespace waveform {
struct Config {
    bool enabled;
    bool lockToCamera;
    float verticalOffset;
    float verticalScale;
    int lineWidth;
    int opacity;
    bool spectrogramEnabled;
    float spectrogramOffset;
    float spectrogramHeight;
    int spectrogramOpacity;
    unsigned windowSize;
    float frequencyScale;
    float minimumFrequency;
    float maximumFrequency;
    Colormap colormap;

    static Config get() {
        auto mod = geode::Mod::get();
        auto finite = [](double value, float fallback) {
            return std::isfinite(value) ? static_cast<float>(std::clamp(value, -1e6, 1e6)) : fallback;
        };
        return {
            mod->getSettingValue<bool>("enabled"),
            mod->getSettingValue<bool>("lock-to-camera"),
            finite(mod->getSettingValue<double>("vertical-offset"), 270.f),
            std::max(0.f, finite(mod->getSettingValue<double>("vertical-scale"), 90.f)),
            static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("line-width"), 1, 10)),
            static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("opacity"), 0, 255)),
            mod->getSettingValue<bool>("spectrogram-enabled"),
            finite(mod->getSettingValue<double>("spectrogram-vertical-offset"), 90.f),
            std::max(0.f, finite(mod->getSettingValue<double>("spectrogram-height"), 120.f)),
            static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("spectrogram-opacity"), 0, 255)),
            windowSizeFromSetting(mod->getSettingValue<std::string>("spectrogram-window-size")),
            std::clamp(finite(mod->getSettingValue<double>("spectrogram-frequency-scale"), 0.f), 0.f, 10.f),
            std::clamp(finite(mod->getSettingValue<double>("spectrogram-min-frequency"), 50.f), 0.f, 16000.f),
            std::clamp(finite(mod->getSettingValue<double>("spectrogram-max-frequency"), 8000.f), 0.f, 16000.f),
            colormapFromSetting(mod->getSettingValue<std::string>("spectrogram-colormap"))
        };
    }
};
}
