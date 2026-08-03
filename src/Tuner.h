#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <cmath>

namespace ampcade
{
// Chromatic tuner tapped off the input (post input-gain, pre gate — the gate
// would eat the quiet tail of a string you're trying to tune). YIN pitch
// detection on a 4x-decimated signal: at 48 kHz that's 12 kHz, window 1024,
// lags up to ~330 (covers down to ~36 Hz — below a bass low B), a few hundred
// thousand mult-adds every ~43 ms and only while the tuner is engaged.
// freq.load() == 0 means "no confident pitch".
class Tuner
{
public:
    void prepare (double sampleRate, int /*maxBlock*/)
    {
        decim = juce::jmax (1, (int) std::round (sampleRate / 12000.0));
        decRate = sampleRate / decim;
        maxTau = juce::jmin (kBuf / 2, (int) (decRate / 36.0)); // lowest ~36 Hz
        fill = 0;
        decPhase = 0;
        decAcc = 0.0f;
        sinceDetect = 0;
        holdLeft = 0;
        candFreq = 0.0f;
        freq.store (0.0f);
    }

    // Audio thread. Call only while the tuner is engaged.
    void push (const float* m, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            decAcc += m[i];
            if (++decPhase >= decim)
            {
                // averaging decimator: cheap and enough anti-aliasing for pitch
                buf[writeAt (fill++)] = decAcc / (float) decim;
                decAcc = 0.0f;
                decPhase = 0;
            }
        }

        sinceDetect += n;
        const int hop = (int) (decRate * decim * 0.045); // ~45 ms between updates
        if (fill >= kBuf && sinceDetect >= hop)
        {
            sinceDetect = 0;
            detect();
        }
    }

    std::atomic<float> freq { 0.0f }; // Hz; 0 = no pitch

private:
    static constexpr int kBuf = 1024; // decimated ring
    static constexpr int kWin = 512;  // YIN integration window

    int writeAt (long long idx) const { return (int) (idx % kBuf); }

    void detect()
    {
        // unwrap the newest kBuf samples so x[] is contiguous, oldest first
        float x[kBuf];
        const long long start = fill - kBuf;
        for (int i = 0; i < kBuf; ++i)
            x[i] = buf[writeAt (start + i)];

        // Gate on level: don't chase noise. -75 dBFS — a decaying string on a
        // quiet interface input sits well below the old -60 dB line, which is
        // why the note readout kept blinking out mid-tune.
        double sq = 0.0;
        for (int i = 0; i < kBuf; ++i)
            sq += (double) x[i] * x[i];
        if (std::sqrt (sq / kBuf) < 1.78e-4) // ~ -75 dBFS
        {
            noPitch();
            return;
        }

        // YIN difference + cumulative-mean normalization, over the FULL lag
        // range. The early-out "first dip under threshold wins" variant flapped
        // on the A string: as a pluck decays, its 2nd harmonic takes over and
        // the half-period dip slips under the threshold on some frames only,
        // so the readout alternated 110/220 Hz. Computing every lag lets the
        // deepest dip overrule a shallow early one.
        float d[kBuf / 2 + 1];
        d[0] = 1.0f;
        float running = 0.0f;
        for (int tau = 1; tau <= maxTau; ++tau)
        {
            float sum = 0.0f;
            for (int i = 0; i < kWin; ++i)
            {
                const float diff = x[i] - x[i + tau];
                sum += diff * diff;
            }
            running += sum;
            d[tau] = running > 0.0f ? sum * (float) tau / running : 1.0f;
        }

        // smallest-lag local minimum under the threshold (classic YIN)…
        int best = -1;
        float bestVal = 1.0f;
        int deepest = -1;
        float deepestVal = 1.0f;
        for (int tau = 2; tau < maxTau; ++tau)
        {
            if (d[tau] < deepestVal)
            {
                deepestVal = d[tau];
                deepest = tau;
            }
            if (best < 0 && d[tau] < 0.12f && d[tau] <= d[tau - 1] && d[tau] <= d[tau + 1])
            {
                best = tau;
                bestVal = d[tau];
            }
        }
        // …but a clearly deeper dip at a longer lag is the real period (the
        // early dip was a harmonic).
        if (best < 0 || (deepest > best && deepestVal < bestVal - 0.05f))
        {
            best = deepest;
            bestVal = deepestVal;
        }

        if (best <= 1 || bestVal > 0.3f)
        {
            noPitch();
            return;
        }

        // parabolic interpolation around the minimum for sub-sample precision
        float tauF = (float) best;
        if (best > 1 && best < maxTau)
        {
            const float a = d[best - 1], b = d[best], c = d[best + 1];
            const float denom = a - 2.0f * b + c;
            if (std::abs (denom) > 1.0e-12f)
                tauF += 0.5f * (a - c) / denom;
        }

        const float f = (float) (decRate / tauF);

        // Consistency gate: a big jump (an octave flip, a stray harmonic) must
        // show up on two consecutive detections before the display follows it.
        // Small drifts (tuning a string) pass straight through.
        const float shown = freq.load();
        const bool nearShown = shown > 0.0f && std::abs (f - shown) / shown < 0.06f;
        const bool nearCand = candFreq > 0.0f && std::abs (f - candFreq) / candFreq < 0.06f;
        candFreq = f;
        if (! nearShown && ! nearCand)
        {
            noPitch();  // hold what's shown; publish if the next detect agrees
            return;
        }

        holdLeft = kHoldDetects;
        freq.store (f);
    }

    // Hold the last confident pitch across brief dropouts (a beat between
    // plucks, the dip while a bend settles) instead of blanking the display.
    void noPitch()
    {
        if (holdLeft > 0)
            --holdLeft;
        else
            freq.store (0.0f);
    }

    static constexpr int kHoldDetects = 24; // ~1.1 s at the ~45 ms hop

    float buf[kBuf] = {};
    long long fill = 0;
    int holdLeft = 0;
    float candFreq = 0.0f; // last raw detection, for the consistency gate
    int decim = 4, maxTau = 330, decPhase = 0, sinceDetect = 0;
    float decAcc = 0.0f;
    double decRate = 12000.0;
};
} // namespace ampcade
