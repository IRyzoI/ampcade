#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

namespace ampcade
{
// Built-in gear, so a fresh install makes a good noise before anything is
// downloaded: a TS-style drive pedal, a tweed-flavoured clean amp and a 1x12
// cab voicing. Each is deliberately simple DSP — the moment a NAM capture or
// IR is loaded it takes over and these step aside.

//==============================================================================
// TS-style overdrive: lows stay clean, highs get gained into an asymmetric
// soft clipper, then a one-pole tone rolls the fizz off. The on/off is a short
// wet/dry crossfade so kicking it on never clicks.
class ScreamerDrive
{
public:
    void prepare (double sampleRate, int /*maxBlock*/)
    {
        sr = sampleRate;
        wetSm.reset (sampleRate, 0.012);
        wetSm.setCurrentAndTargetValue (0.0f);
        driveSm.reset (sampleRate, 0.02);
        levelSm.reset (sampleRate, 0.02);
        toneSm.reset (sampleRate, 0.02);
        lpSplit = lpTone = dcX = dcY = 0.0f;
        splitCoeff = onePole (720.0f);
    }

    void process (float* m, int n, float driveKnob, float toneKnob, float levelDb, bool on)
    {
        wetSm.setTargetValue (on ? 1.0f : 0.0f);
        if (! on && wetSm.getCurrentValue() < 1.0e-4f)
        {
            wetSm.skip (n);
            return; // fully bypassed and settled — free
        }

        // knob 0..10 -> 6..34 dB into the clipper; makeup keeps engaging it
        // roughly level-neutral instead of a volume jump.
        driveSm.setTargetValue (juce::Decibels::decibelsToGain (6.0f + juce::jlimit (0.0f, 10.0f, driveKnob) * 2.8f));
        levelSm.setTargetValue (juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 24.0f, levelDb)));
        toneSm.setTargetValue (onePole (650.0f * std::pow (2.0f, juce::jlimit (0.0f, 10.0f, toneKnob) * 0.32f)));

        const float R = 1.0f - (float) (juce::MathConstants<double>::twoPi * 12.0 / sr);

        for (int i = 0; i < n; ++i)
        {
            const float x = m[i];
            const float g = driveSm.getNextValue();
            const float lvl = levelSm.getNextValue();
            const float tc = toneSm.getNextValue();
            const float wet = wetSm.getNextValue();

            lpSplit += splitCoeff * (x - lpSplit);      // lows kept clean (the TS trick)
            const float hi = x - lpSplit;

            // asymmetric soft clip; static offset removed, envelope DC handled below
            const float d = std::tanh (1.1f * hi * g + 0.12f) - kAsymRest;
            const float makeup = 1.0f / std::pow (g, 0.62f);

            float y = lpSplit + d * makeup * 1.9f;

            lpTone += tc * (y - lpTone);                // tone: darker <-> brighter
            y = lpTone * lvl;

            // one-pole DC blocker — the asymmetric clipper rectifies a little
            const float dcOut = y - dcX + R * dcY;
            dcX = y;
            dcY = dcOut;

            m[i] = x + (dcOut - x) * wet;
        }
    }

private:
    float onePole (float hz) const
    {
        return 1.0f - std::exp (-juce::MathConstants<float>::twoPi * hz / (float) sr);
    }

    static inline const float kAsymRest = std::tanh (0.12f);

    juce::SmoothedValue<float> wetSm { 0.0f }, driveSm { 1.0f }, levelSm { 1.0f }, toneSm { 0.1f };
    float lpSplit = 0.0f, lpTone = 0.0f, dcX = 0.0f, dcY = 0.0f;
    float splitCoeff = 0.1f;
    double sr = 48000.0;
};

//==============================================================================
// Tweed-flavoured clean amp: bright-cap sparkle into a soft squash, then a
// gentle mid scoop. GAIN keeps small-signal level steady while adding squash
// and hair, so cranking it is more compression and edge, not just louder.
// The plugin's ToneStack runs after this, exactly as it does for a capture.
class TweedClean
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlock, 1 };
        for (auto* f : { &bright, &scoop })
        {
            f->prepare (spec);
            f->reset();
        }
        *bright.coefficients = *juce::dsp::IIR::Coefficients<float>::makeHighShelf (sampleRate, 2400.0f, 0.707f,
                                                                                   juce::Decibels::decibelsToGain (2.5f));
        *scoop.coefficients = *juce::dsp::IIR::Coefficients<float>::makePeakFilter (sampleRate, 500.0f, 0.8f,
                                                                                   juce::Decibels::decibelsToGain (-3.0f));
        volSm.reset (sampleRate, 0.02);
        preSm.reset (sampleRate, 0.02);
    }

    void process (float* m, int n, float gainDb, float volDb)
    {
        auto block = juce::dsp::AudioBlock<float> (&m, 1, (size_t) n);
        auto ctx = juce::dsp::ProcessContextReplacing<float> (block);
        bright.process (ctx);

        preSm.setTargetValue (juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 24.0f, gainDb)) * 2.6f);
        volSm.setTargetValue (juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 24.0f, volDb)));

        for (int i = 0; i < n; ++i)
        {
            const float pre = preSm.getNextValue();
            // tanh(pre·x)/sqrt(pre): more pre = more squash, small-signal gain
            // grows only as sqrt, normalised so the default lands near unity.
            const float y = std::tanh (pre * m[i]) / std::sqrt (juce::jmax (0.05f, pre));
            m[i] = y * kNorm * volSm.getNextValue();
        }

        scoop.process (ctx);
    }

private:
    static constexpr float kNorm = 0.62f; // unity small-signal at the default GAIN
    juce::dsp::IIR::Filter<float> bright, scoop;
    juce::SmoothedValue<float> volSm { 1.0f }, preSm { 2.6f };
};

//==============================================================================
// Fixed 1x12 open-back voicing as a biquad cascade — no IR file needed, no
// convolution latency. Thump around 100 Hz, boxiness dip, presence lift, and a
// steep top-end rolloff that does most of what "a speaker" means.
class TweedCab
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlock, 1 };
        using C = juce::dsp::IIR::Coefficients<float>;
        const auto db = [] (float d) { return juce::Decibels::decibelsToGain (d); };

        // makeLowPass caps its frequency below Nyquist internally, but keep the
        // design sensible at 44.1k too.
        const float lp1 = (float) juce::jmin (4900.0, sampleRate * 0.45);
        const float lp2 = (float) juce::jmin (5400.0, sampleRate * 0.45);

        f[0].coefficients = C::makeHighPass (sampleRate, 78.0f, 0.85f);
        f[1].coefficients = C::makePeakFilter (sampleRate, 105.0f, 1.1f, db (2.5f));
        f[2].coefficients = C::makePeakFilter (sampleRate, 380.0f, 1.4f, db (-3.0f));
        f[3].coefficients = C::makePeakFilter (sampleRate, 2400.0f, 0.9f, db (3.0f));
        f[4].coefficients = C::makeLowPass (sampleRate, lp1, 0.75f);
        f[5].coefficients = C::makeLowPass (sampleRate, lp2, 1.05f);

        for (auto& filt : f)
        {
            filt.prepare (spec);
            filt.reset();
        }
    }

    void process (float* m, int n)
    {
        auto block = juce::dsp::AudioBlock<float> (&m, 1, (size_t) n);
        auto ctx = juce::dsp::ProcessContextReplacing<float> (block);
        for (auto& filt : f)
            filt.process (ctx);
    }

private:
    juce::dsp::IIR::Filter<float> f[6];
};
} // namespace ampcade
