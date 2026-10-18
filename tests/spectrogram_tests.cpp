#include "spectrogram.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <string_view>

static void check(bool condition, char const* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
static std::shared_ptr<waveform::Spectrogram const> tone(unsigned window, double rate, double frequency,
    double amplitude, unsigned channels = 1, unsigned frames = 0, size_t budget = waveform::SpectrogramBudget) {
    waveform::SpectralAnalysis analysis(window, rate, channels, budget);
    if (!frames) frames = window * 4;
    for (unsigned frame = 0; frame < frames; ++frame) {
        auto value = float(amplitude * std::sin(2 * std::numbers::pi * frequency * frame / rate));
        std::array<float, 2> pcm{value, -value};
        check(analysis.pushFrame(std::span(pcm).first(channels)), "tone input accepted");
    }
    return analysis.finish();
}
static void benchmark() {
    for (auto window : {512u, 8192u}) for (auto minutes : {5u, 30u}) {
        constexpr double rate = 44100;
        auto begin = std::chrono::steady_clock::now();
        waveform::SpectralAnalysis analysis(window, rate, 2);
        std::array<float, 2> pcm{};
        auto frames = uint64_t(rate * 60 * minutes);
        for (uint64_t i = 0; i < frames; ++i) {
            pcm[0] = float(.5 * std::sin(2 * std::numbers::pi * 440 * i / rate)); pcm[1] = -pcm[0];
            analysis.pushFrame(pcm);
        }
        auto spectrum = analysis.finish();
        auto end = std::chrono::steady_clock::now();
        std::vector<double> edges(961);
        for (size_t i = 0; i < edges.size(); ++i) edges[i] = spectrum->duration() * i / (edges.size() - 1);
        auto image = waveform::rasterSpectrogram(*spectrum, edges, 120, waveform::frequencyRange(0, 16000, rate), 10, waveform::Colormap::Magma);
        auto rasterEnd = std::chrono::steady_clock::now();
        for (size_t i = 0; i < edges.size(); ++i) edges[i] = spectrum->duration() / 2 + 10.0 * i / (edges.size() - 1);
        auto viewportImage = waveform::rasterSpectrogram(*spectrum, edges, 120, waveform::frequencyRange(0, 16000, rate), 10, waveform::Colormap::Magma);
        auto viewportEnd = std::chrono::steady_clock::now();
        check(spectrum->bytes() <= waveform::SpectrogramBudget, "benchmark memory limit");
        std::cout << minutes << " min, N=" << window << ": analysis "
            << std::chrono::duration<double>(end - begin).count() << " s, whole-song raster "
            << std::chrono::duration<double, std::milli>(rasterEnd - end).count() << " ms, 10-second viewport "
            << std::chrono::duration<double, std::milli>(viewportEnd - rasterEnd).count() << " ms, capacity "
            << spectrum->bytes() << " bytes, hop " << spectrum->hopFrames << ", duration " << spectrum->duration() << " s\n";
    }
}
int main(int argc, char** argv) {
    using namespace waveform;
    if (argc > 1 && std::string_view(argv[1]) == "--benchmark") { benchmark(); return 0; }
    for (auto window : WindowSizes) {
        auto hz = 48000.0 / window * 16; // exactly on a bin for every window
        auto mono = tone(window, 48000, hz, .5);
        auto stereo = tone(window, 48000, hz, .5, 2);
        auto atTone = mono->peak(window / 48000.0, 2 * window / 48000.0, hz - 1, hz + 1);
        check(atTone && std::abs(int(*atTone) - 238) <= 1, "Hann coherent-gain normalization is window independent");
        check(stereo->peak(window / 48000.0, 2 * window / 48000.0, hz - 1, hz + 1) == atTone, "opposite-phase stereo must not cancel");
        check(mono->peak(window / 48000.0, 2 * window / 48000.0, hz * 2, hz * 2 + mono->binHz()) == 0, "bin-centered tone does not leak into distant frequencies");
        check(mono->columns() == 16 && mono->frames == window * 4, "centered windows cover the entire song");
        check(mono->bytes() <= SpectrogramBudget, "spectral memory bound");
        auto silent = tone(window, 24000, hz, 0, 1, 13);
        check(silent->columns() == 1 && silent->frames == 13, "short clips are padded and retain exact duration");
        check(silent->peak(0, silent->duration(), 0, 12000) == 0, "padded silence remains silent");
        check(!silent->peak(-2, -1, 0, 1000), "negative time is transparent");
        check(!silent->peak(silent->duration(), 2, 0, 1000), "past-song time is transparent");
        check(!silent->peak(NAN, 1, 0, 1000), "nonfinite time rejected");
    }
    check(windowSizeFromSetting("8192") == 8192 && windowSizeFromSetting("1023") == 2048, "window choices and fallback");
    auto range = frequencyRange(16000, 0, 24000);
    check(range.minimum == 0 && range.maximum == 12000, "reversed borders and Nyquist clamp");
    check(!frequencyRange(15000, 16000, 24000).valid(), "range above Nyquist is empty");
    check(!frequencyRange(1000, 1000, 48000).valid(), "equal borders are empty");
    for (auto scale : {0.0, 2.5, 5.0, 10.0}) {
        double previous = -1;
        for (int i = 0; i <= 100; ++i) {
            auto frequency = frequencyAtPosition(i / 100.0, range, scale);
            check(frequency > previous, "frequency axis is strictly monotonic");
            check(std::abs(frequencyPosition(frequency, range, scale) - i / 100.0) < 1e-8, "frequency axis inverse matches gradual scale");
            previous = frequency;
        }
        check(frequencyAtPosition(0, range, scale) == 0 && frequencyAtPosition(1, range, scale) == 12000, "frequency endpoints include zero");
    }
    check(std::abs(frequencyAtPosition(.5, range, 0) - 6000) < 1e-6, "linear midpoint");
    check(frequencyAtPosition(.5, range, 10) < 1000, "log midpoint emphasizes low frequencies");
    for (bool nyquist : {false, true}) {
        SpectralAnalysis boundary(512, 24000, 1);
        std::array<float, 1> pcm{};
        for (unsigned i = 0; i < 2048; ++i) { pcm[0] = nyquist && i % 2 ? -.25f : .25f; boundary.pushFrame(pcm); }
        auto spectrum = boundary.finish();
        auto intensity = spectrum->peak(.03, .06, nyquist ? 11999 : 0, nyquist ? 12000 : 1);
        check(intensity && std::abs(int(*intensity) - 221) <= 1, "DC and Nyquist use the single-sided endpoint normalization");
    }

    // Five slots deliberately exercise odd tails when coarsening in place.
    auto bins = size_t(257);
    auto budget = sizeof(Spectrogram) + bins * 5;
    Spectrogram bounded(512, 24000, budget);
    std::vector<uint8_t> slice(bins);
    std::vector<uint8_t> reference;
    for (unsigned i = 0; i < 77; ++i) {
        std::fill(slice.begin(), slice.end(), 0);
        slice[12] = uint8_t((i * 73) % 256); reference.push_back(slice[12]);
        bounded.appendSlice(slice);
        bounded.frames += 128;
        check(bounded.bytes() <= budget && bounded.columns() <= 5, "coarsening stays in capacity budget");
        auto group = bounded.hopFrames / 128;
        for (size_t column = 0; column < bounded.columns(); ++column) {
            auto first = column * group, last = std::min<uint64_t>(reference.size(), (column + 1) * group);
            auto expected = *std::max_element(reference.begin() + first, reference.begin() + last);
            auto actual = bounded.peak(first * 128.0 / 24000, last * 128.0 / 24000, 560, 565);
            check(actual == expected, "coarsening preserves maxima, odd tails and timestamps");
        }
    }
    auto coarsenedTone = tone(512, 24000, 750, .5, 2, 3000, budget);
    check(coarsenedTone->frames == 3000 && coarsenedTone->hopFrames > 128, "streaming analysis coarsens without truncation");
    check(coarsenedTone->peak(0, coarsenedTone->duration(), 749, 751) == 238, "coarsening retains tone amplitude");
    auto lastImpulse = [](unsigned count) {
        SpectralAnalysis analysis(512, 24000, 1);
        std::array<float, 1> pcm{};
        for (unsigned i = 0; i < count; ++i) { pcm[0] = i == count - 1 ? 1.f : 0.f; analysis.pushFrame(pcm); }
        return analysis.finish();
    }(333);
    check(lastImpulse->columns() == 3 && lastImpulse->peak(256.0 / 24000, 333.0 / 24000, 500, 2000).value_or(0) > 0, "EOF padding keeps the final transient");

    auto spectrum = tone(512, 24000, 750, .5);
    std::array<double, 5> edges{-1, 0, .01, spectrum->duration(), spectrum->duration() + 1};
    auto image = rasterSpectrogram(*spectrum, edges, 8, range, 5, Colormap::Magma);
    check(image.size() == 4 * 8 * 4, "bounded RGBA raster dimensions");
    for (unsigned row = 0; row < 8; ++row) {
        check(image[(row * 4) * 4 + 3] == 0 && image[(row * 4 + 3) * 4 + 3] == 0, "out-of-song raster columns are transparent");
        check(image[(row * 4 + 1) * 4 + 3] == 255, "valid raster columns are opaque before display opacity");
        auto low = frequencyAtPosition(double(row) / 8, range, 5);
        auto high = frequencyAtPosition(double(row + 1) / 8, range, 5);
        for (unsigned column = 1; column < 3; ++column) {
            auto expected = spectralColor(*spectrum->peak(edges[column], edges[column + 1], low, high), Colormap::Magma);
            check(std::equal(expected.begin(), expected.end(), image.begin() + (row * 4 + column) * 4), "temporal pooling matches exact rectangular spectral peaks");
        }
    }
    auto empty = rasterSpectrogram(*spectrum, edges, 8, {}, 0, Colormap::Grayscale);
    check(std::all_of(empty.begin(), empty.end(), [](uint8_t value) { return !value; }), "empty frequency range is transparent");
    check(spectralColor(0, Colormap::Magma) == std::array<uint8_t, 4>{0, 0, 4, 255}, "Magma lower endpoint");
    check(spectralColor(255, Colormap::Grayscale) == std::array<uint8_t, 4>{255, 255, 255, 255}, "grayscale upper endpoint");
    check(colormapFromSetting("Viridis") == Colormap::Viridis && colormapFromSetting("bad") == Colormap::Magma, "palette choices and fallback");
    std::atomic<bool> cancelled{true};
    SpectralAnalysis cancelledAnalysis(512, 24000, 1, budget, &cancelled);
    std::array<float, 1> pcm{1};
    check(!cancelledAnalysis.pushFrame(pcm) && !cancelledAnalysis.finish(), "analysis cancellation discards output");
    cancelled = false;
    SpectralAnalysis interruptedAnalysis(512, 24000, 1, budget, &cancelled);
    for (unsigned i = 0; i < 1024; ++i) check(interruptedAnalysis.pushFrame(pcm), "analysis runs before cancellation");
    cancelled = true;
    check(!interruptedAnalysis.pushFrame(pcm) && !interruptedAnalysis.finish(), "mid-analysis cancellation discards partial spectra");
    SpectralAnalysis nonfinite(512, 24000, 1);
    pcm[0] = NAN; nonfinite.pushFrame(pcm); pcm[0] = INFINITY; nonfinite.pushFrame(pcm);
    auto finite = nonfinite.finish();
    check(finite->peak(0, finite->duration(), 0, 12000) == 0, "nonfinite PCM becomes silence");
    std::cout << "Spectral analysis, mappings, palettes, rasterization and memory budgets passed\n";
}
