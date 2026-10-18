#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace waveform {
constexpr size_t SpectrogramBudget = 64 * 1024 * 1024;
constexpr std::array<unsigned, 5> WindowSizes{512, 1024, 2048, 4096, 8192};
enum class Colormap { Magma, Viridis, Inferno, Grayscale };
unsigned windowSizeFromSetting(std::string_view value);
Colormap colormapFromSetting(std::string_view value);
std::array<uint8_t, 4> spectralColor(uint8_t intensity, Colormap map);

struct FrequencyRange {
    double minimum = 0, maximum = 0;
    bool valid() const { return maximum > minimum; }
};
FrequencyRange frequencyRange(double minimum, double maximum, double sampleRate);
double frequencyPosition(double frequency, FrequencyRange range, double scale);
double frequencyAtPosition(double position, FrequencyRange range, double scale);

// Each column covers [column * hopFrames, (column + 1) * hopFrames).
// STFT windows are centered on the beginning of their original time slice.
// Coarsening changes only temporal detail, never the song's timestamps.
class Spectrogram {
    std::vector<uint8_t> m_values;
    uint64_t m_originalSlices = 0;
    size_t m_maxColumns = 0;
    void coarsen();
public:
    unsigned windowSize = 0;
    double sampleRate = 0;
    uint64_t frames = 0, hopFrames = 0;
    size_t bins = 0;

    Spectrogram(unsigned window, double rate, size_t budget = SpectrogramBudget);
    Spectrogram(Spectrogram const&) = delete;
    Spectrogram& operator=(Spectrogram const&) = delete;
    Spectrogram(Spectrogram&&) noexcept = default;
    Spectrogram& operator=(Spectrogram&&) noexcept = default;
    size_t columns() const { return bins ? m_values.size() / bins : 0; }
    size_t bytes() const { return sizeof(*this) + m_values.capacity(); }
    double binHz() const { return sampleRate / windowSize; }
    double duration() const { return sampleRate > 0 ? frames / sampleRate : 0; }
    void appendSlice(std::span<uint8_t const> values);
    bool timePeaks(double begin, double end, std::span<uint8_t> result) const;
    std::optional<uint8_t> peak(double begin, double end, double lowHz, double highHz) const;
};

// Streaming interleaved PCM -> Hann-windowed real FFTs. Working memory is
// bounded by the FFT size/channel count; no full-song PCM is retained.
class SpectralAnalysis {
    struct Impl;
    std::unique_ptr<Impl> m_impl;
public:
    SpectralAnalysis(unsigned window, double rate, unsigned channels,
        size_t budget = SpectrogramBudget, std::atomic<bool> const* cancelled = nullptr);
    ~SpectralAnalysis();
    bool pushFrame(std::span<float const> channels);
    std::shared_ptr<Spectrogram const> finish();
};

// Time edges come from the editor's timeForPos, including speed changes.
// Output rows run bottom to top. Missing time/frequency ranges are transparent.
std::vector<uint8_t> rasterSpectrogram(Spectrogram const& spectrum,
    std::span<double const> timeEdges, unsigned rows, FrequencyRange range,
    double scale, Colormap map);
}
