#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <memory>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

#include "NamCompat.h"
#include "dsp/ResamplingContainer/ResamplingContainer.h"

namespace ampcade
{
// Wraps a nam::DSP so it can run at any host sample rate.
// Port of ResamplingNAM from the official NeuralAmpModelerPlugin
// (MIT, © 2022 Steven Atkinson) adapted to the current core API.
class ResamplingNam
{
public:
    ResamplingNam (std::unique_ptr<nam::DSP> encapsulated)
        : model (std::move (encapsulated)),
          resampler (namSampleRate())
    {
        if (model->HasLoudness())
        {
            hasLoudness = true;
            loudnessDb = model->GetLoudness();
        }
    }

    // Call off the audio thread before first use and on host SR/block changes.
    void reset (double hostSampleRate, int maxBlockSize)
    {
        externalSampleRate = hostSampleRate;
        maxExternalBlock = maxBlockSize;

        if (needsResampling())
            resampler.Reset (hostSampleRate, maxBlockSize);

        const auto upRatio = hostSampleRate / namSampleRate();
        const auto maxInnerBlock = (int) std::ceil ((double) maxBlockSize / upRatio) + 16;
        model->Reset (namSampleRate(), std::max (maxInnerBlock, maxBlockSize));
    }

    void process (float* buffer, int numFrames)
    {
        float* ins[1] = { buffer };
        float* outs[1] = { buffer };

        if (! needsResampling())
        {
            model->process (ins, outs, numFrames);
        }
        else
        {
            resampler.ProcessBlock (ins, outs, numFrames,
                                    [this] (float** i, float** o, int n) { model->process (i, o, n); });
        }
    }

    int getLatency() const { return needsResampling() ? resampler.GetLatency() : 0; }

    bool needsResampling() const
    {
        return std::abs (externalSampleRate - namSampleRate()) > 1.0;
    }

    double namSampleRate() const
    {
        const auto sr = model->GetExpectedSampleRate();
        return sr > 0 ? sr : 48000.0; // ancient .nam files don't store a rate
    }

    bool hasLoudness = false;
    double loudnessDb = -18.0;

    // Fixed correction for the capture's inherent level deficit (they are trained
    // at a hotter reference level than an interface delivers), measured ONCE at
    // load time in NamSlot::load. This used to be a runtime RMS servo that
    // re-matched output level to input level continuously — which also erased the
    // level interactions that make pedal ORDER and DRIVE audible (a driven pedal
    // never actually hit the next pedal any harder). Static means: correct the
    // capture, keep the dynamics.
    float unityDb = 0.0f;

private:
    std::unique_ptr<nam::DSP> model;
    dsp::ResamplingContainer<float, 1, 12> resampler;
    double externalSampleRate = 48000.0;
    int maxExternalBlock = 0;
};

//==============================================================================
// One pedalboard slot holding a hot-swappable NAM model.
// Audio thread uses tryLock + a local shared_ptr copy; retired models go to a
// graveyard that the message thread purges (never free a net on audio thread).
class NamSlot
{
public:
    void prepare (double sampleRate, int maxBlockSize)
    {
        hostRate = sampleRate;
        hostBlock = maxBlockSize;
        stackCompDb = 0.0f;
        scratch.setSize (1, maxBlockSize, false, true);

        const juce::SpinLock::ScopedLockType l (lock);
        if (active != nullptr)
            active->reset (sampleRate, maxBlockSize);
        driveGain.reset (sampleRate, 0.02);
        levelGain.reset (sampleRate, 0.02);
    }

    // Message/worker thread. Returns error string, or empty on success.
    juce::String load (const juce::File& namFile);

    void clear();

    bool isLoaded() const
    {
        const juce::SpinLock::ScopedLockType l (lock);
        return active != nullptr;
    }

    int getLatency() const
    {
        const juce::SpinLock::ScopedLockType l (lock);
        return active != nullptr ? active->getLatency() : 0;
    }

    // How a slot's output level is managed.
    enum class Makeup
    {
        none,      // raw capture output
        loudness,  // trim toward kLoudnessTargetDb using the capture's stored loudness
        unity      // measure and match input level, so the slot neither adds nor eats gain
    };

    // Audio thread. Returns false when nothing was processed (empty slot or mid-swap).
    bool process (float* buffer, int numFrames, float driveDb, float levelDb, Makeup makeup);

    // Loudness metadata of the loaded capture, for gain-staging checks.
    // hasLoudness=false means the capture stored none, so normalize is a no-op.
    bool captureHasLoudness() const
    {
        const juce::SpinLock::ScopedLockType l (lock);
        return active != nullptr && active->hasLoudness;
    }

    double captureLoudnessDb() const
    {
        const juce::SpinLock::ScopedLockType l (lock);
        return active != nullptr ? active->loudnessDb : 0.0;
    }

    // Message thread, periodically: frees retired models.
    void purgeGraveyard();

private:
    mutable juce::SpinLock lock;
    std::shared_ptr<ResamplingNam> active;
    std::vector<std::shared_ptr<ResamplingNam>> graveyard;
    juce::CriticalSection graveyardLock;

    juce::AudioBuffer<float> scratch;
    juce::SmoothedValue<float> driveGain { 1.0f }, levelGain { 1.0f };
    double hostRate = 48000.0;
    int hostBlock = 512;
    // Adaptive one-sided stacking makeup (audio thread only, see process()):
    // only ever positive, only when the capture is eating level beyond unityDb.
    float stackCompDb = 0.0f;

    static constexpr float kLoudnessTargetDb = -18.0f;
    // Bounds on the unity correction: enough to rescue a capture that bleeds 20 dB,
    // not enough to turn a silent slot into a noise generator.
    static constexpr float kUnityMinDb = -12.0f, kUnityMaxDb = 24.0f;
    static constexpr float kStackCompMaxDb = 24.0f;
};
} // namespace ampcade
