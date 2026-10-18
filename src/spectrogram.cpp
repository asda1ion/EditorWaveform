#include "spectrogram.hpp"
#include "palettes.hpp"
#include <kiss_fftr.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>

namespace waveform {
namespace {
bool validWindow(unsigned size) {
    return std::find(WindowSizes.begin(), WindowSizes.end(), size) != WindowSizes.end();
}
double safeScale(double scale) { return std::isfinite(scale) ? std::clamp(scale, 0.0, 10.0) : 10.0; }
unsigned checkedChannels(unsigned count) {
    if (!count || count > 32) throw std::invalid_argument("Invalid spectrogram channel count");
    return count;
}
}
unsigned windowSizeFromSetting(std::string_view value) {
    for (auto size : WindowSizes) if (value == std::to_string(size)) return size;
    return 2048;
}
Colormap colormapFromSetting(std::string_view value) {
    if (value == "Viridis") return Colormap::Viridis;
    if (value == "Inferno") return Colormap::Inferno;
    if (value == "Grayscale") return Colormap::Grayscale;
    return Colormap::Magma;
}
std::array<uint8_t, 4> spectralColor(uint8_t intensity, Colormap map) {
    if (map == Colormap::Grayscale) return {intensity, intensity, intensity, 255};
    auto const& colors = map == Colormap::Viridis ? palettes::Viridis : map == Colormap::Inferno ? palettes::Inferno : palettes::Magma;
    auto color = colors[intensity];
    return {color[0], color[1], color[2], 255};
}
FrequencyRange frequencyRange(double minimum, double maximum, double sampleRate) {
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || !std::isfinite(sampleRate) || sampleRate <= 0) return {};
    minimum = std::clamp(minimum, 0.0, 16000.0);
    maximum = std::clamp(maximum, 0.0, 16000.0);
    if (minimum > maximum) std::swap(minimum, maximum);
    auto nyquist = sampleRate / 2;
    return {std::min(minimum, nyquist), std::min(maximum, nyquist)};
}
double frequencyPosition(double frequency, FrequencyRange range, double scale) {
    if (!range.valid() || !std::isfinite(frequency)) return 0;
    frequency = std::clamp(frequency, range.minimum, range.maximum);
    auto linear = (frequency - range.minimum) / (range.maximum - range.minimum);
    auto lower = std::log1p(range.minimum / 20);
    auto logarithmic = (std::log1p(frequency / 20) - lower) / (std::log1p(range.maximum / 20) - lower);
    auto blend = safeScale(scale) / 10;
    return (1 - blend) * linear + blend * logarithmic;
}
double frequencyAtPosition(double position, FrequencyRange range, double scale) {
    if (!range.valid() || !std::isfinite(position)) return range.minimum;
    position = std::clamp(position, 0.0, 1.0);
    if (position == 0) return range.minimum;
    if (position == 1) return range.maximum;
    auto low = range.minimum, high = range.maximum;
    for (int i = 0; i < 40; ++i) {
        auto middle = (low + high) / 2;
        if (frequencyPosition(middle, range, scale) < position) low = middle;
        else high = middle;
    }
    return (low + high) / 2;
}

Spectrogram::Spectrogram(unsigned window, double rate, size_t budget) : windowSize(window), sampleRate(rate) {
    if (!validWindow(window) || !std::isfinite(rate) || rate <= 0) throw std::invalid_argument("Invalid spectrogram metadata");
    hopFrames = window / 4;
    bins = static_cast<size_t>(std::ceil(std::min(16000.0, rate / 2) * window / rate)) + 1;
    m_maxColumns = budget > sizeof(*this) ? (budget - sizeof(*this)) / bins : 0;
    if (!m_maxColumns) throw std::invalid_argument("Spectrogram memory budget is too small");
    // Reserve once: growth otherwise temporarily keeps both old and new large
    // allocations alive. Unwritten capacity does not need to be initialized.
    m_values.reserve(m_maxColumns * bins);
    if (bytes() > budget) throw std::runtime_error("Spectrogram allocation exceeded its budget");
}
void Spectrogram::coarsen() {
    auto count = columns();
    for (size_t column = 0; column < (count + 1) / 2; ++column) {
        for (size_t bin = 0; bin < bins; ++bin) {
            auto value = m_values[(column * 2) * bins + bin];
            if (column * 2 + 1 < count) value = std::max(value, m_values[(column * 2 + 1) * bins + bin]);
            m_values[column * bins + bin] = value;
        }
    }
    m_values.resize(((count + 1) / 2) * bins);
    if (hopFrames > std::numeric_limits<uint64_t>::max() / 2) throw std::overflow_error("Spectrogram duration overflow");
    hopFrames *= 2;
}
void Spectrogram::appendSlice(std::span<uint8_t const> values) {
    if (values.size() != bins) throw std::invalid_argument("Invalid spectrum bin count");
    auto group = hopFrames / (windowSize / 4);
    auto column = m_originalSlices / group;
    while (column >= m_maxColumns) {
        coarsen(); group *= 2; column = m_originalSlices / group;
    }
    if (column >= columns()) {
        m_values.resize((column + 1) * bins);
        std::copy(values.begin(), values.end(), m_values.begin() + column * bins);
    } else {
        for (size_t bin = 0; bin < bins; ++bin) {
            auto& value = m_values[column * bins + bin];
            value = std::max(value, values[bin]);
        }
    }
    ++m_originalSlices;
}
std::optional<uint8_t> Spectrogram::peak(double begin, double end, double lowHz, double highHz) const {
    if (!columns() || !frames || !std::isfinite(begin) || !std::isfinite(end) ||
        !std::isfinite(lowHz) || !std::isfinite(highHz) || end <= begin || highHz <= lowHz ||
        end <= 0 || begin >= duration() || highHz < 0 || lowHz > std::min(16000.0, sampleRate / 2)) return std::nullopt;
    auto first = static_cast<size_t>(std::min(double(columns()), std::floor(std::max(0.0, begin) * sampleRate / hopFrames)));
    auto last = static_cast<size_t>(std::min(double(columns()), std::ceil(std::min(duration(), end) * sampleRate / hopFrames)));
    auto firstBin = static_cast<size_t>(std::min(double(bins - 1), std::floor(std::max(0.0, lowHz) / binHz())));
    auto lastBin = static_cast<size_t>(std::min(double(bins), std::ceil(std::max(0.0, highHz) / binHz()) + 1));
    if (first >= last || firstBin >= lastBin) return std::nullopt;
    uint8_t result = 0;
    for (auto column = first; column < last; ++column) {
        auto start = m_values.begin() + column * bins;
        result = std::max(result, *std::max_element(start + firstBin, start + lastBin));
    }
    return result;
}
bool Spectrogram::timePeaks(double begin, double end, std::span<uint8_t> result) const {
    if (result.size() != bins) throw std::invalid_argument("Invalid temporal spectrum bin count");
    if (!columns() || !frames || !std::isfinite(begin) || !std::isfinite(end) ||
        end <= begin || end <= 0 || begin >= duration()) return false;
    auto first = static_cast<size_t>(std::min(double(columns()), std::floor(std::max(0.0, begin) * sampleRate / hopFrames)));
    auto last = static_cast<size_t>(std::min(double(columns()), std::ceil(std::min(duration(), end) * sampleRate / hopFrames)));
    if (first >= last) return false;
    std::copy_n(m_values.data() + first * bins, bins, result.data());
    for (auto column = first + 1; column < last; ++column) {
        auto source = m_values.data() + column * bins;
        for (size_t bin = 0; bin < bins; ++bin) result[bin] = std::max(result[bin], source[bin]);
    }
    return true;
}

struct SpectralAnalysis::Impl {
    Spectrogram spectrum;
    unsigned channels;
    std::atomic<bool> const* cancelled;
    std::vector<float> ring, window, input, powers;
    std::vector<kiss_fft_cpx> output;
    std::vector<uint8_t> intensities;
    kiss_fftr_cfg fft = nullptr;
    uint64_t fed = 0, realFrames = 0, nextCenter = 0;
    double windowSum = 0;
    bool finished = false;

    Impl(unsigned size, double rate, unsigned count, size_t budget, std::atomic<bool> const* flag)
      : spectrum(size, rate, budget), channels(checkedChannels(count)), cancelled(flag), ring(size * channels),
        window(size), input(size), powers(spectrum.bins), output(size / 2 + 1), intensities(spectrum.bins) {
        for (unsigned n = 0; n < size; ++n) {
            window[n] = float(.5 * (1 - std::cos(2 * std::numbers::pi * n / size)));
            windowSum += window[n];
        }
        fft = kiss_fftr_alloc(static_cast<int>(size), 0, nullptr, nullptr);
        if (!fft) throw std::bad_alloc();
    }
    ~Impl() { kiss_fftr_free(fft); }
    bool stopped() const { return cancelled && cancelled->load(std::memory_order_relaxed); }
    bool transform() {
        std::fill(powers.begin(), powers.end(), 0.f);
        auto size = spectrum.windowSize;
        for (unsigned channel = 0; channel < channels; ++channel) {
            if (stopped()) return false;
            for (unsigned n = 0; n < size; ++n) {
                // fed is the exclusive end of the centered window. Negative
                // positions at the song's start read untouched (zero) slots.
                auto index = (fed + n) % size;
                input[n] = ring[index * channels + channel] * window[n];
            }
            kiss_fftr(fft, input.data(), output.data());
            for (size_t bin = 0; bin < spectrum.bins; ++bin) {
                auto factor = (bin == 0 || bin == size / 2) ? 1.0 / windowSum : 2.0 / windowSum;
                auto value = output[bin];
                powers[bin] += float((double(value.r) * value.r + double(value.i) * value.i) * factor * factor / channels);
            }
        }
        for (size_t bin = 0; bin < spectrum.bins; ++bin) {
            auto db = 10 * std::log10(std::max(double(powers[bin]), 1e-9));
            intensities[bin] = static_cast<uint8_t>(std::lround(std::clamp((db + 90) / 90, 0.0, 1.0) * 255));
        }
        spectrum.appendSlice(intensities);
        nextCenter += size / 4;
        return true;
    }
    bool feed(std::span<float const> frame) {
        if (stopped()) return false;
        auto offset = (fed % spectrum.windowSize) * channels;
        for (unsigned channel = 0; channel < channels; ++channel) {
            auto value = frame.empty() ? 0.f : frame[channel];
            ring[offset + channel] = std::isfinite(value) ? value : 0.f;
        }
        ++fed;
        if (fed == nextCenter + spectrum.windowSize / 2) return transform();
        return true;
    }
};
SpectralAnalysis::SpectralAnalysis(unsigned window, double rate, unsigned channels, size_t budget, std::atomic<bool> const* cancelled)
  : m_impl(std::make_unique<Impl>(window, rate, channels, budget, cancelled)) {}
SpectralAnalysis::~SpectralAnalysis() = default;
bool SpectralAnalysis::pushFrame(std::span<float const> channels) {
    auto& impl = *m_impl;
    if (impl.finished || channels.size() != impl.channels) throw std::invalid_argument("Invalid spectrogram PCM frame");
    ++impl.realFrames;
    return impl.feed(channels);
}
std::shared_ptr<Spectrogram const> SpectralAnalysis::finish() {
    auto& impl = *m_impl;
    if (impl.finished) throw std::logic_error("Spectrogram already finished");
    impl.finished = true;
    while (impl.nextCenter < impl.realFrames) if (!impl.feed({})) return nullptr;
    if (impl.stopped()) return nullptr;
    impl.spectrum.frames = impl.realFrames;
    return std::make_shared<Spectrogram>(std::move(impl.spectrum));
}

std::vector<uint8_t> rasterSpectrogram(Spectrogram const& spectrum, std::span<double const> timeEdges,
    unsigned rows, FrequencyRange range, double scale, Colormap map) {
    if (timeEdges.size() < 2 || timeEdges.size() > 2049 || !rows || rows > 512) return {};
    auto columns = timeEdges.size() - 1;
    std::vector<uint8_t> image(columns * rows * 4);
    if (!range.valid()) return image;
    std::vector<double> frequencies(rows + 1);
    for (unsigned row = 0; row <= rows; ++row) frequencies[row] = frequencyAtPosition(double(row) / rows, range, scale);
    std::vector<std::pair<size_t, size_t>> binRanges(rows);
    for (unsigned row = 0; row < rows; ++row) {
        auto low = frequencies[row], high = frequencies[row + 1];
        if (high <= low || high < 0 || low > std::min(16000.0, spectrum.sampleRate / 2)) continue;
        binRanges[row] = {
            static_cast<size_t>(std::min(double(spectrum.bins - 1), std::floor(std::max(0.0, low) / spectrum.binHz()))),
            static_cast<size_t>(std::min(double(spectrum.bins), std::ceil(std::max(0.0, high) / spectrum.binHz()) + 1))
        };
    }
    std::vector<uint8_t> temporal(spectrum.bins);
    for (size_t column = 0; column < columns; ++column) {
        // Pool time once, then resample frequencies. Repeated low-frequency
        // rows on a log axis must not repeatedly scan the song's time slices.
        if (!spectrum.timePeaks(timeEdges[column], timeEdges[column + 1], temporal)) continue;
        for (unsigned row = 0; row < rows; ++row) {
            auto [first, last] = binRanges[row];
            if (first >= last) continue;
            auto peak = *std::max_element(temporal.begin() + first, temporal.begin() + last);
            auto color = spectralColor(peak, map);
            std::copy(color.begin(), color.end(), image.begin() + (row * columns + column) * 4);
        }
    }
    return image;
}
}
