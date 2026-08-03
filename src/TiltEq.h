#pragma once
#include <juce_dsp/juce_dsp.h>

namespace ampcade
{
// One-knob TONE for a pedal slot: a tilt around 900 Hz. At 5 it is flat, below 5
// it rolls off treble and lifts lows (darker), above 5 it does the reverse
// (brighter/tighter) — the way a single tone control on a real pedal behaves.
// Runs post-capture so it shapes what the pedal produced, not what it is fed.
class TiltEq
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        sr = sampleRate;
        const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlock, 1 };
        for (auto* f : { &low, &high })
        {
            f->prepare (spec);
            f->reset();
        }
        last = -1.0f;
        update (5.0f);
    }

    void reset()
    {
        low.reset();
        high.reset();
    }

    // knob 0..10, 5 = flat. Bypasses entirely when centred so a untouched
    // tone knob costs nothing and colours nothing.
    void process (float* buffer, int numFrames, float knob)
    {
        if (std::abs (knob - 5.0f) < 0.05f)
            return;

        if (knob != last)
            update (knob);

        auto block = juce::dsp::AudioBlock<float> (&buffer, 1, (size_t) numFrames);
        auto ctx = juce::dsp::ProcessContextReplacing<float> (block);
        low.process (ctx);
        high.process (ctx);
    }

private:
    void update (float knob)
    {
        last = knob;
        // ±9 dB of tilt across the knob's travel, shelves moving in opposition
        const auto tiltDb = (knob - 5.0f) * (9.0f / 5.0f);
        *low.coefficients  = *juce::dsp::IIR::Coefficients<float>::makeLowShelf (
            sr, 900.0f, 0.707f, juce::Decibels::decibelsToGain (-tiltDb));
        *high.coefficients = *juce::dsp::IIR::Coefficients<float>::makeHighShelf (
            sr, 900.0f, 0.707f, juce::Decibels::decibelsToGain (tiltDb));
    }

    double sr = 48000.0;
    float last = -1.0f;
    juce::dsp::IIR::Filter<float> low, high;
};
} // namespace ampcade
