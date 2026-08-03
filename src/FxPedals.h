#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <vector>

namespace ampcade
{
// Built-in post-cab delay: a plain straight echo, both channels on the same
// tap (the old ~15% right-channel offset made everything a ping-pongy wash).
// Trails: stomping it off ramps the wet mix to zero but the line keeps
// running, so echoes ring out and re-engaging never clicks.
class StereoDelay
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        sr = sampleRate;
        line.setMaximumDelayInSamples ((int) std::ceil (2.2 * sampleRate));
        line.prepare ({ sampleRate, (juce::uint32) maxBlock, 2 });
        line.reset();

        timeSm.reset (sampleRate, 0.25);
        wetSm.reset (sampleRate, 0.05);
        fbSm.reset (sampleRate, 0.05);
        wetSm.setCurrentAndTargetValue (0.0f);

        lpL = lpR = 0.0f;
        lpCoeff = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 4500.0f / (float) sampleRate);
    }

    void process (float* L, float* R, int n, float timeMs, float feedback, float mix, bool on)
    {
        // 2000 ms ceiling: a half note at 60 BPM. The line holds 2.2 s.
        timeSm.setTargetValue (juce::jlimit (60.0f, 2000.0f, timeMs));
        wetSm.setTargetValue (on ? juce::jlimit (0.0f, 1.0f, mix) : 0.0f);
        fbSm.setTargetValue (juce::jlimit (0.0f, 0.95f, feedback));

        const float msToSamples = (float) (sr / 1000.0);

        for (int i = 0; i < n; ++i)
        {
            const float t = timeSm.getNextValue();
            const float wet = wetSm.getNextValue();
            const float fb = fbSm.getNextValue();

            const float yl = line.popSample (0, t * msToSamples, true);
            const float yr = line.popSample (1, t * msToSamples, true);

            // darken the repeats like an analog unit
            lpL += lpCoeff * (yl - lpL);
            lpR += lpCoeff * (yr - lpR);

            line.pushSample (0, L[i] + lpL * fb);
            line.pushSample (1, R[i] + lpR * fb);

            L[i] += yl * wet;
            R[i] += yr * wet;
        }
    }

private:
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> line;
    juce::SmoothedValue<float> timeSm { 380.0f }, wetSm { 0.0f }, fbSm { 0.35f };
    float lpL = 0.0f, lpR = 0.0f, lpCoeff = 0.5f;
    double sr = 48000.0;
};

//==============================================================================
// Built-in mono chorus, a chain block like the drive pedal. This one exists
// because NAM captures are static waveshapers — a chorus/flanger/vibe literally
// cannot be captured, so TONE3000 will never have one.
//
// A tri-chorus: three taps on ONE LFO, phase-locked 120° apart. The previous
// two-voice design ran the second voice at rate×1.13 so the phase relationship
// drifted — every few seconds both voices bent pitch the same direction and
// the "wide" chorus turned into plain vibrato. That was the warble. With three
// voices locked in symmetric phase the summed pitch deviation is zero at every
// instant by construction — the swirl can never line up — which is the fat
// standing-still shimmer of the 80s studio tri-chorus racks (the Purple Rain
// texture). Staggered base delays keep the three combs from stacking; the wet
// path is high-passed at ~100 Hz so the low end stays anchored instead of
// going seasick. MIX 0.5 = classic, 1.0 = wet-only doubling; bypass is a short
// ramp so stomping never clicks.
class ChorusPedal
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        sr = sampleRate;
        line.setMaximumDelayInSamples ((int) std::ceil (0.05 * sampleRate));
        line.prepare ({ sampleRate, (juce::uint32) maxBlock, 1 });
        line.reset();
        ph = 0.0f;
        lpWet = lpDry = 0.0f;
        lpCoeff = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 100.0f / (float) sampleRate);
        rateSm.reset (sampleRate, 0.1);
        depthSm.reset (sampleRate, 0.1);
        wetSm.reset (sampleRate, 0.05);
        wetSm.setCurrentAndTargetValue (0.0f);
    }

    void process (float* m, int n, float rateHz, float depth0to10, float mix, bool on)
    {
        wetSm.setTargetValue (on ? juce::jlimit (0.0f, 1.0f, mix) : 0.0f);
        if (! on && wetSm.getCurrentValue() < 1.0e-4f)
        {
            wetSm.skip (n);
            return; // fully bypassed and settled — free
        }

        rateSm.setTargetValue (juce::jlimit (0.05f, 8.0f, rateHz));
        // 0..~1.5 ms of excursion. Depth 5 ≈ 0.75 ms — a handful of cents per
        // voice at the default rate; chorus lives well below vibrato territory.
        depthSm.setTargetValue (juce::jlimit (0.0f, 10.0f, depth0to10) * 0.15f);

        const float msToSamples = (float) (sr / 1000.0);
        const float twoPi = juce::MathConstants<float>::twoPi;

        for (int i = 0; i < n; ++i)
        {
            const float rate = rateSm.getNextValue();
            const float depthMs = depthSm.getNextValue();
            const float w = wetSm.getNextValue();

            ph += rate / (float) sr;
            if (ph >= 1.0f) ph -= 1.0f;

            const float d1 = (8.0f  + depthMs * std::sin (twoPi * ph)) * msToSamples;
            const float d2 = (13.0f + depthMs * std::sin (twoPi * (ph + 1.0f / 3.0f))) * msToSamples;
            const float d3 = (18.0f + depthMs * std::sin (twoPi * (ph + 2.0f / 3.0f))) * msToSamples;

            line.pushSample (0, m[i]);
            const float v1 = line.popSample (0, d1, false);
            const float v2 = line.popSample (0, d2, false);
            const float v3 = line.popSample (0, d3, true);
            // 1/√3, not 1/3: three staggered voices are decorrelated on real
            // program material, so their powers add — this keeps the wet path
            // at the dry level and stomping the pedal on is not a volume drop.
            float wetS = (v1 + v2 + v3) * 0.57735f;

            // The wet voice carries the DRY signal's lows: below ~100 Hz a
            // modulated tap reads as seasick, not lush, so the shimmer rides on
            // top of an anchored low end — even at full MIX.
            lpWet += lpCoeff * (wetS - lpWet);
            lpDry += lpCoeff * (m[i] - lpDry);
            wetS = (wetS - lpWet) + lpDry;

            m[i] += (wetS - m[i]) * w;
        }
    }

private:
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> line;
    juce::SmoothedValue<float> rateSm { 0.6f }, depthSm { 0.75f }, wetSm { 0.0f };
    float ph = 0.0f;
    float lpWet = 0.0f, lpDry = 0.0f, lpCoeff = 0.02f;
    double sr = 48000.0;
};

//==============================================================================
// Built-in compressor: the classic guitar squeeze. One COMP knob drives
// threshold, ratio AND makeup together (like a Dyna-style SUSTAIN control),
// ATTACK sets how much pick lands before the clamp, LEVEL trims. A floater
// like the EQ/boosts: mono before the fan-out, stereo-LINKED after (one gain
// for both channels, so the image never wanders). Bypass is a crossfade.
class CompPedal
{
public:
    void prepare (double sampleRate, int)
    {
        sr = sampleRate;
        env = 0.0f;
        rel = 1.0f - std::exp (-1.0f / (0.15f * (float) sampleRate)); // ~150 ms release
        mixSm.reset (sampleRate, 0.03);
        mixSm.setCurrentAndTargetValue (0.0f);
    }

    void process (float* m, int n, float comp, float att, float levelDb, bool on)
    {
        if (idle (on)) { mixSm.skip (n); return; }
        setup (comp, att, levelDb);
        for (int i = 0; i < n; ++i)
        {
            const float g = gainFor (std::abs (m[i]));
            const float w = mixSm.getNextValue();
            m[i] += (m[i] * g - m[i]) * w;
        }
    }

    void processStereo (float* L, float* R, int n, float comp, float att, float levelDb, bool on)
    {
        if (idle (on)) { mixSm.skip (n); return; }
        setup (comp, att, levelDb);
        for (int i = 0; i < n; ++i)
        {
            const float g = gainFor (juce::jmax (std::abs (L[i]), std::abs (R[i])));
            const float w = mixSm.getNextValue();
            L[i] += (L[i] * g - L[i]) * w;
            R[i] += (R[i] * g - R[i]) * w;
        }
    }

private:
    bool idle (bool on)
    {
        mixSm.setTargetValue (on ? 1.0f : 0.0f);
        if (on || mixSm.getCurrentValue() > 1.0e-4f)
            return false;
        env = 0.0f; // fully bypassed: the detector starts fresh next engage
        return true;
    }

    void setup (float comp, float att, float levelDb)
    {
        const float c = juce::jlimit (0.0f, 10.0f, comp) * 0.1f;
        threshDb = -8.0f - 24.0f * c;                  // 0 -> -8 dB, 10 -> -32 dB
        slope = 1.0f - 1.0f / (2.0f + 4.0f * c);       // ratio 2:1 .. 6:1
        // COMP also makes up what it takes (the Dyna-style sustain feel);
        // LEVEL trims from there.
        makeup = juce::Decibels::decibelsToGain (5.0f * c + juce::jlimit (-12.0f, 12.0f, levelDb));
        const float attMs = 1.0f + 2.4f * juce::jlimit (0.0f, 10.0f, att); // 1..25 ms
        atk = 1.0f - std::exp (-1.0f / (attMs * 0.001f * (float) sr));
    }

    float gainFor (float d)
    {
        env += (d > env ? atk : rel) * (d - env);
        const float envDb = juce::Decibels::gainToDecibels (env, -80.0f);
        const float grDb = juce::jmin (0.0f, (threshDb - envDb) * slope);
        return juce::Decibels::decibelsToGain (grDb) * makeup;
    }

    float env = 0.0f, atk = 0.01f, rel = 0.001f;
    float threshDb = -20.0f, slope = 0.5f, makeup = 1.0f;
    juce::SmoothedValue<float> mixSm { 0.0f };
    double sr = 48000.0;
};

//==============================================================================
// Built-in clean boost: one knob of smoothed gain, nothing else. Three
// instances exist (a front bump, a post-drive push, a lead lift) and a boost
// may sit anywhere in the chain — before the amp it runs mono, after the
// stereo fan-out it runs on both channels with one shared gain ramp. The knob
// swings negative too, so a "boost" doubles as a volume cut.
class BoostPedal
{
public:
    void prepare (double sampleRate, int)
    {
        g.reset (sampleRate, 0.03);
        g.setCurrentAndTargetValue (1.0f);
    }

    void process (float* m, int n, float db, bool on)
    {
        if (idle (db, on)) { g.skip (n); return; }
        for (int i = 0; i < n; ++i)
            m[i] *= g.getNextValue();
    }

    void processStereo (float* L, float* R, int n, float db, bool on)
    {
        if (idle (db, on)) { g.skip (n); return; }
        for (int i = 0; i < n; ++i)
        {
            const float v = g.getNextValue();
            L[i] *= v;
            R[i] *= v;
        }
    }

private:
    bool idle (float db, bool on)
    {
        g.setTargetValue (on ? juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 24.0f, db)) : 1.0f);
        return ! g.isSmoothing() && std::abs (g.getCurrentValue() - 1.0f) < 1.0e-4f;
    }

    juce::SmoothedValue<float> g { 1.0f };
};

//==============================================================================
// Built-in stereo reverb (wet-only juce::Reverb blended in), same trails
// behaviour as the delay.
class ReverbPedal
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        verb.setSampleRate (sampleRate);
        verb.reset();
        wl.assign ((size_t) maxBlock, 0.0f);
        wr.assign ((size_t) maxBlock, 0.0f);
        wetSm.reset (sampleRate, 0.05);
        wetSm.setCurrentAndTargetValue (0.0f);
        lastRoom = -1.0f;
    }

    void process (float* L, float* R, int n, float size, float mix, bool on)
    {
        if ((size_t) n > wl.size())
        {
            wl.resize ((size_t) n);
            wr.resize ((size_t) n);
        }

        const float room = juce::jlimit (0.0f, 1.0f, size / 10.0f);
        if (std::abs (room - lastRoom) > 0.001f)
        {
            lastRoom = room;
            juce::Reverb::Parameters p;
            p.roomSize = room;
            p.damping = 0.45f;
            p.width = 1.0f;
            p.wetLevel = 1.0f;
            p.dryLevel = 0.0f;
            p.freezeMode = 0.0f;
            verb.setParameters (p);
        }

        std::copy (L, L + n, wl.data());
        std::copy (R, R + n, wr.data());
        verb.processStereo (wl.data(), wr.data(), n);

        wetSm.setTargetValue (on ? 0.65f * juce::jlimit (0.0f, 1.0f, mix) : 0.0f);
        for (int i = 0; i < n; ++i)
        {
            const float w = wetSm.getNextValue();
            L[i] += wl[(size_t) i] * w;
            R[i] += wr[(size_t) i] * w;
        }
    }

private:
    juce::Reverb verb;
    std::vector<float> wl, wr;
    juce::SmoothedValue<float> wetSm { 0.0f };
    float lastRoom = -1.0f;
};
} // namespace ampcade
