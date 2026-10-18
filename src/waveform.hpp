#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "spectrogram.hpp"

namespace FMOD { class System; }

namespace waveform {
constexpr unsigned BinFrames = 64;
constexpr size_t MiB = 1024 * 1024;

enum class PCMFormat { Signed8, Signed16, Signed24, Signed32, Float32 };
inline unsigned bytesPerSample(PCMFormat format) {
    return format == PCMFormat::Signed8 ? 1 : format == PCMFormat::Signed16 ? 2 : format == PCMFormat::Signed24 ? 3 : 4;
}
inline float pcmSample(std::span<uint8_t const> data, PCMFormat format) {
    auto bytes = bytesPerSample(format);
    if (data.size() < bytes) return 0;
    uint32_t raw = 0;
    for (unsigned i = 0; i < bytes; ++i) raw |= uint32_t(data[i]) << (8 * i);
    if (format == PCMFormat::Float32) {
        float value = std::bit_cast<float>(raw);
        return std::isfinite(value) ? value : 0;
    }
    auto bits = bytes * 8;
    int64_t value = raw;
    if (raw & (uint32_t(1) << (bits - 1))) value -= int64_t(1) << bits;
    return static_cast<float>(value / std::ldexp(1.0, bits - 1));
}

// Incremental peak pyramid. Only complete pairs enter higher levels, preserving
// odd tails and allowing exact range queries over the decoded song.
class Envelope {
    std::vector<std::vector<float>> m_levels;
    size_t m_budget;
public:
    explicit Envelope(size_t budget = 24 * MiB) : m_budget(budget) {}
    size_t size() const { return m_levels.empty() ? 0 : m_levels[0].size(); }
    size_t bytes() const {
        size_t result = m_levels.capacity() * sizeof(std::vector<float>);
        for (auto const& level : m_levels) result += level.capacity() * sizeof(float);
        return result;
    }
    bool append(float peak) {
        auto newCount = size() + 1;
        size_t required = 0;
        unsigned levelCount = 0;
        for (auto count = newCount; count; count >>= 1) {
            auto capacity = levelCount < m_levels.size() ? m_levels[levelCount].capacity() : 0;
            required += std::max(capacity, ((count + 1023) / 1024) * 1024) * sizeof(float);
            ++levelCount;
        }
        if (required + levelCount * sizeof(std::vector<float>) > m_budget) return false;
        if (m_levels.size() < levelCount) m_levels.resize(levelCount);
        for (unsigned level = 0; level < levelCount; ++level) {
            auto& values = m_levels[level];
            if (values.size() == values.capacity()) values.reserve(values.capacity() + 1024);
            values.push_back(peak);
            if (values.size() % 2) break;
            peak = std::max(values[values.size() - 2], values.back());
        }
        return true;
    }
    std::optional<float> peak(size_t begin, size_t end) const {
        end = std::min(end, size());
        if (begin >= end) return std::nullopt;
        float result = 0;
        while (begin < end) {
            auto remaining = end - begin;
            auto level = static_cast<unsigned>(std::bit_width(remaining) - 1);
            if (begin) level = std::min(level, static_cast<unsigned>(std::countr_zero(begin)));
            result = std::max(result, m_levels[level][begin >> level]);
            begin += size_t(1) << level;
        }
        return result;
    }
    std::optional<float> atTime(double begin, double end, double rate) const {
        if (!std::isfinite(begin) || !std::isfinite(end) || !std::isfinite(rate) || rate <= 0 || end <= 0 || end <= begin) return std::nullopt;
        auto first = std::floor(std::max(0.0, begin) * rate / BinFrames);
        auto last = std::ceil(std::max(0.0, end) * rate / BinFrames);
        if (first >= size()) return std::nullopt;
        return peak(static_cast<size_t>(first), static_cast<size_t>(std::min(last, double(size()))));
    }
};

struct SourceAudio {
    Envelope envelope;
    double sampleRate = 0;
    uint64_t frames = 0;
    std::string error;
    std::string spectralError;
    std::shared_ptr<Spectrogram const> spectrogram;
    double duration() const { return sampleRate > 0 ? frames / sampleRate : 0; }
};

struct AnalysisOptions {
    unsigned windowSize = 0; // zero skips all spectral analysis
    bool buildWaveform = true;
};
SourceAudio decodeAudio(FMOD::System* system, std::string const& path, std::atomic<bool> const& cancelled,
    AnalysisOptions options = {});

class SourceLoader {
    struct Impl;
    std::unique_ptr<Impl> m_impl;
public:
    SourceLoader();
    ~SourceLoader();
    void load(FMOD::System* system, std::string path, unsigned windowSize = 0, bool preserveWaveform = false);
    void cancel();
    std::shared_ptr<SourceAudio const> audio() const;
    std::shared_ptr<Spectrogram const> spectrogram() const;
};

}
