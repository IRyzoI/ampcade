#pragma once
#include <juce_dsp/juce_dsp.h>

#include <cmath>

namespace ampcade
{
// Post-amp 3-band EQ. Knobs are 0..10 (5 = flat) mapped to ±12 dB:
// low shelf 120 Hz, mid peak 650 Hz, high shelf 3.2 kHz.
class ToneStack
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        sr = sampleRate;
        const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlock, 1 };
        for (auto* f : { &low, &mid, &high })
        {
            f->prepare (spec);
            f->reset();
        }
        // Every coefficient set we can ever need is built ONCE, here.
        // juce::dsp::IIR::Coefficients<float>::makeLowShelf and friends are
        // literally "return *new Coefficients (...)", and update() is called
        // from process() — i.e. from the audio thread — so building them on
        // demand put a malloc in the signal path every time a knob moved or the
        // host automated one, contending with the model loader's own
        // allocations for the same heap lock.
        for (int i = 0; i <= kSteps; ++i)
        {
            const float k = knobForStep (i);
            lowTab[i]  = juce::dsp::IIR::Coefficients<float>::makeLowShelf   (sr, 120.0f,  0.707f, knobToGain (k));
            midTab[i]  = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sr, 650.0f,  0.6f,   knobToGain (k));
            highTab[i] = juce::dsp::IIR::Coefficients<float>::makeHighShelf  (sr, 3200.0f, 0.707f, knobToGain (k));
        }
        lastB = lastM = lastT = -1.0f;
        update (5.0f, 5.0f, 5.0f);
    }

    void process (float* buffer, int numFrames, float bass, float midK, float treble)
    {
        if (bass != lastB || midK != lastM || treble != lastT)
            update (bass, midK, treble);

        auto block = juce::dsp::AudioBlock<float> (&buffer, 1, (size_t) numFrames);
        auto ctx = juce::dsp::ProcessContextReplacing<float> (block);
        low.process (ctx);
        mid.process (ctx);
        high.process (ctx);
    }

private:
    // 0.05 of a knob unit — 0.12 dB per step, well under the resolution anyone
    // can hear, and finer than the steps a host's automation curve already
    // arrives in.
    static constexpr int kSteps = 200;
    static float knobForStep (int i) { return (float) i * (10.0f / (float) kSteps); }

    static int stepFor (float knob)
    {
        if (! std::isfinite (knob))
            knob = 5.0f;
        return juce::jlimit (0, kSteps,
                             juce::roundToInt (juce::jlimit (0.0f, 10.0f, knob) * ((float) kSteps / 10.0f)));
    }

    static float knobToGain (float knob) // 0..10 -> dB gain factor
    {
        return juce::Decibels::decibelsToGain ((knob - 5.0f) * (12.0f / 5.0f));
    }

    void update (float b, float m, float t)
    {
        lastB = b; lastM = m; lastT = t;
        // Copying into the existing Coefficients reuses its array capacity, so
        // this stays allocation-free.
        *low.coefficients  = *lowTab[stepFor (b)];
        *mid.coefficients  = *midTab[stepFor (m)];
        *high.coefficients = *highTab[stepFor (t)];
    }

    juce::dsp::IIR::Filter<float> low, mid, high;
    juce::dsp::IIR::Coefficients<float>::Ptr lowTab[kSteps + 1], midTab[kSteps + 1], highTab[kSteps + 1];
    double sr = 48000.0;
    float lastB = -1, lastM = -1, lastT = -1;
};

//==============================================================================
// Wet-only bass/treble for the AIR convolution, stereo. Knobs are 0..10
// (5 = flat) mapped to ±12 dB. The low shelf sits at 180 Hz — reverb IRs that
// misbehave do it as endless low-end bloom, and 180 catches the boom without
// thinning the body of the tail. Both knobs at centre = exact bypass, so an
// untouched EQ colours nothing.
class AirEq
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        sr = sampleRate;
        const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlock, 1 };
        for (auto* f : { &lowL, &lowR, &highL, &highR })
        {
            f->prepare (spec);
            f->reset();
        }
        // Same reason as ToneStack: the make* factories allocate, and update()
        // runs from the audio thread.
        for (int i = 0; i <= kSteps; ++i)
        {
            const float k = knobForStep (i);
            lowTab[i]  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, 180.0f,  0.707f, knobToGain (k));
            highTab[i] = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 3500.0f, 0.707f, knobToGain (k));
        }
        lastB = lastT = -1.0f;
        update (5.0f, 5.0f);
    }

    void process (float* L, float* R, int n, float bass, float treble)
    {
        if (std::abs (bass - 5.0f) < 0.05f && std::abs (treble - 5.0f) < 0.05f)
            return;

        if (bass != lastB || treble != lastT)
            update (bass, treble);

        auto run = [n] (juce::dsp::IIR::Filter<float>& f, float* buf)
        {
            auto block = juce::dsp::AudioBlock<float> (&buf, 1, (size_t) n);
            auto ctx = juce::dsp::ProcessContextReplacing<float> (block);
            f.process (ctx);
        };
        run (lowL, L); run (highL, L);
        run (lowR, R); run (highR, R);
    }

private:
    static constexpr int kSteps = 200;
    static float knobForStep (int i) { return (float) i * (10.0f / (float) kSteps); }

    static int stepFor (float knob)
    {
        if (! std::isfinite (knob))
            knob = 5.0f;
        return juce::jlimit (0, kSteps,
                             juce::roundToInt (juce::jlimit (0.0f, 10.0f, knob) * ((float) kSteps / 10.0f)));
    }

    static float knobToGain (float knob)
    {
        return juce::Decibels::decibelsToGain ((knob - 5.0f) * (12.0f / 5.0f));
    }

    void update (float b, float t)
    {
        lastB = b; lastT = t;
        const auto& lowC  = lowTab[stepFor (b)];
        const auto& highC = highTab[stepFor (t)];
        *lowL.coefficients = *lowC;
        *lowR.coefficients = *lowC;
        *highL.coefficients = *highC;
        *highR.coefficients = *highC;
    }

    juce::dsp::IIR::Filter<float> lowL, lowR, highL, highR;
    juce::dsp::IIR::Coefficients<float>::Ptr lowTab[kSteps + 1], highTab[kSteps + 1];
    double sr = 48000.0;
    float lastB = -1, lastT = -1;
};
} // namespace ampcade
