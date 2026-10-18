#include "waveform.hpp"
#ifndef WAVEFORM_AUDIO_ONLY
#include <Geode/utils/async.hpp>
#include <Geode/loader/Log.hpp>
#endif
#include <Geode/fmod/fmod.hpp>
#include <Geode/fmod/fmod_errors.h>

namespace waveform {
SourceAudio decodeAudio(FMOD::System* system, std::string const& path, std::atomic<bool> const& cancelled, AnalysisOptions options) {
    SourceAudio result;
    if (cancelled.load(std::memory_order_relaxed)) return result;
    FMOD::Sound* raw = nullptr;
    auto status = system->createSound(path.c_str(), FMOD_OPENONLY | FMOD_CREATESTREAM, nullptr, &raw);
    if (status != FMOD_OK) { result.error = FMOD_ErrorString(status); return result; }
    auto release = [](FMOD::Sound* sound) { sound->release(); };
    std::unique_ptr<FMOD::Sound, decltype(release)> sound(raw, release);
    FMOD_SOUND_FORMAT format;
    int channels = 0;
    float rate = 0;
    if (sound->getFormat(nullptr, &format, &channels, nullptr) != FMOD_OK ||
        sound->getDefaults(&rate, nullptr) != FMOD_OK || !std::isfinite(rate) || rate <= 0 || channels <= 0 || channels > 32) {
        result.error = "Invalid audio metadata"; return result;
    }
    PCMFormat pcm;
    switch (format) {
        case FMOD_SOUND_FORMAT_PCM8: pcm = PCMFormat::Signed8; break;
        case FMOD_SOUND_FORMAT_PCM16: pcm = PCMFormat::Signed16; break;
        case FMOD_SOUND_FORMAT_PCM24: pcm = PCMFormat::Signed24; break;
        case FMOD_SOUND_FORMAT_PCM32: pcm = PCMFormat::Signed32; break;
        case FMOD_SOUND_FORMAT_PCMFLOAT: pcm = PCMFormat::Float32; break;
        default: result.error = "Unsupported decoded audio format"; return result;
    }
    result.sampleRate = rate;
    std::unique_ptr<SpectralAnalysis> spectrum;
    if (options.windowSize) {
        try { spectrum = std::make_unique<SpectralAnalysis>(options.windowSize, rate, channels, SpectrogramBudget, &cancelled); }
        catch (std::exception const& error) { result.spectralError = error.what(); }
    }
    auto sampleBytes = bytesPerSample(pcm);
    auto frameBytes = sampleBytes * channels;
    std::array<uint8_t, 65536> buffer{};
    auto request = buffer.size() / frameBytes * frameBytes;
    float peak = 0;
    unsigned binFrames = 0;
    std::array<float, 32> frame{};
    while (!cancelled.load(std::memory_order_relaxed)) {
        unsigned bytes = 0;
        status = sound->readData(buffer.data(), static_cast<unsigned>(request), &bytes);
        if (status != FMOD_OK && status != FMOD_ERR_FILE_EOF) {
            result.error = FMOD_ErrorString(status); break;
        }
        if (bytes > request || bytes % frameBytes) { result.error = "Truncated PCM frame"; break; }
        for (size_t offset = 0; offset < bytes; offset += frameBytes) {
            if (cancelled.load(std::memory_order_relaxed)) return result;
            for (int channel = 0; channel < channels; ++channel) {
                auto value = pcmSample(std::span(buffer).subspan(offset + channel * sampleBytes, sampleBytes), pcm);
                frame[channel] = value;
                if (options.buildWaveform) peak = std::max(peak, std::abs(value));
            }
            if (spectrum) {
                try { if (!spectrum->pushFrame(std::span(frame).first(channels))) return result; }
                catch (std::exception const& error) { result.spectralError = error.what(); spectrum.reset(); }
            }
            ++result.frames;
            if (options.buildWaveform && ++binFrames == BinFrames) {
                if (!result.envelope.append(peak)) {
                    result.error = "Main-song waveform reached its memory limit";
                    options.buildWaveform = false;
                }
                binFrames = 0; peak = 0;
            }
        }
        if (status == FMOD_ERR_FILE_EOF || !bytes) break;
    }
    if (options.buildWaveform && binFrames && !result.envelope.append(peak)) result.error = "Main-song waveform reached its memory limit";
    if (!result.frames && result.error.empty()) result.error = "Audio file is empty";
    if (spectrum && !cancelled.load(std::memory_order_relaxed)) {
        try { result.spectrogram = spectrum->finish(); }
        catch (std::exception const& error) { result.spectralError = error.what(); }
    }
    return result;
}

#ifndef WAVEFORM_AUDIO_ONLY
struct SourceLoader::Impl {
    struct Request {
        FMOD::System* system;
        std::string path;
        AnalysisOptions options;
        uint64_t generation;
    };
    geode::async::TaskHolder<SourceAudio> task;
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::shared_ptr<SourceAudio const> audio;
    std::shared_ptr<Spectrogram const> spectrum;
    std::optional<Request> pending;
    uint64_t generation = 0;
    bool running = false;

    void start() {
        if (running || !pending) return;
        auto request = std::move(*pending); pending.reset();
        running = true;
        auto flag = cancelled = std::make_shared<std::atomic<bool>>(false);
        auto serial = request.generation;
        task.spawn(geode::async::runtime().spawnBlocking<SourceAudio>([request = std::move(request), flag] {
            try { return decodeAudio(request.system, request.path, *flag, request.options); }
            catch (std::exception const& error) { SourceAudio result; result.error = error.what(); return result; }
        }), [this, serial](SourceAudio result) {
            running = false;
            if (serial == generation) {
                if (!result.error.empty()) geode::log::warn("Main-song waveform: {}", result.error);
                if (!result.spectralError.empty()) geode::log::warn("Main-song spectrogram: {}", result.spectralError);
                spectrum = std::move(result.spectrogram);
                // Spectral buffers live separately so a rebuild can release
                // them without copying or discarding the finished waveform.
                if (!audio) audio = std::make_shared<SourceAudio>(std::move(result));
            }
            result.spectrogram.reset(); // release stale storage before the next allocation
            start();
        });
    }
};
SourceLoader::SourceLoader() : m_impl(std::make_unique<Impl>()) {}
SourceLoader::~SourceLoader() { cancel(); }
void SourceLoader::cancel() {
    ++m_impl->generation;
    m_impl->pending.reset();
    if (m_impl->cancelled) m_impl->cancelled->store(true, std::memory_order_relaxed);
    // Let the completion callback retire this job before starting its successor.
    // TaskHolder cancels callbacks when the loader itself is destroyed.
    m_impl->audio.reset(); m_impl->spectrum.reset();
}
void SourceLoader::load(FMOD::System* system, std::string path, unsigned windowSize, bool preserveWaveform) {
    ++m_impl->generation;
    if (m_impl->cancelled) m_impl->cancelled->store(true, std::memory_order_relaxed);
    m_impl->spectrum.reset();
    if (!preserveWaveform || (m_impl->audio && !m_impl->audio->envelope.size())) m_impl->audio.reset();
    if (!windowSize && m_impl->audio) { m_impl->pending.reset(); return; }
    m_impl->pending = Impl::Request{system, std::move(path), {windowSize, !m_impl->audio}, m_impl->generation};
    m_impl->start();
}
std::shared_ptr<SourceAudio const> SourceLoader::audio() const { return m_impl->audio; }
std::shared_ptr<Spectrogram const> SourceLoader::spectrogram() const { return m_impl->spectrum; }
#endif

}
