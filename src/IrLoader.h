#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <memory>

namespace ampcade::ir
{
// The cab slot takes any impulse response, and there are two completely different
// animals in there. A speaker cab is a few tens of milliseconds and belongs at
// 100% wet with energy normalisation (so swapping cabs is not a volume change).
// A reverb/delay IR is seconds long, is an effect, and needs a dry path plus a
// mix. Its buffer is normalised to unit energy: convolution loudness follows the
// IR's total energy, not its peak, so a peak-normalised 3 s tail comes out
// +20..30 dB over dry (the "almost blew my speakers" bug). Unit energy puts the
// wet path at roughly the dry level for sustained material; AIR's LEVEL knob
// trims from there. (The old all-tail "1000% wet" complaint was the space IR
// sitting in the 100%-wet cab slot, not energy normalisation itself.)
constexpr double kSpaceSeconds = 0.4;    // a file shorter than this is always a speaker
constexpr double kMaxSpaceSeconds = 3.0; // and this is all a zero-latency convolution should carry
// Cab vs space is decided by where the ENERGY lives, not by file length: a
// commercial "mix ready" cab IR is often padded to 0.5-1 s with near-silence,
// but 99% of its energy still sits in the first few tens of ms. A reverb's
// energy is spread over hundreds of ms at minimum. (The Fender Deluxe cab IR
// that motivated this: 0.731 s long, t99 = 12.6 ms.)
constexpr double kSpaceT99Seconds = 0.2; // energy tail longer than this = space

struct Info
{
    double seconds = 0.0; // raw file length
    double t99 = 0.0;     // time containing 99% of the IR's energy
    // False means "do not hand this file to a convolution". A single non-finite
    // sample is enough: it multiplies through every tap, and because the engine
    // downstream is recursive the NaN never leaves again. It is also invisible
    // to the energy tests below — juce::jmax is "a < b ? b : a" and "0.0 < NaN"
    // is false, so a NaN energy reads as an empty IR rather than a broken one.
    bool valid = false;
};

// True if any sample in the buffer is NaN or Inf.
inline bool hasNonFinite (const juce::AudioBuffer<float>& b, int len)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        const auto* p = b.getReadPointer (ch);
        for (int i = 0; i < len; ++i)
            if (! std::isfinite (p[i]))
                return true;
    }
    return false;
}

inline Info analyze (const juce::File& file)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    Info info;
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return info;

    info.seconds = (double) reader->lengthInSamples / reader->sampleRate;

    // 12 s is far beyond any IR that matters here; keeps a mislabeled song file
    // from being slurped whole.
    const auto len = (int) juce::jmin ((double) reader->lengthInSamples, 12.0 * reader->sampleRate);
    juce::AudioBuffer<float> raw ((int) juce::jmin<juce::uint32> (2, reader->numChannels), len);
    if (! reader->read (&raw, 0, len, 0, true, raw.getNumChannels() > 1))
        return info;
    if (hasNonFinite (raw, len))
        return info; // valid stays false — callers must refuse the file

    info.valid = true;

    std::vector<double> e ((size_t) len, 0.0);
    double total = 0.0;
    for (int ch = 0; ch < raw.getNumChannels(); ++ch)
    {
        const auto* p = raw.getReadPointer (ch);
        for (int i = 0; i < len; ++i)
        {
            const double v = (double) p[i] * p[i];
            e[(size_t) i] += v;
            total += v;
        }
    }
    if (total <= 0.0)
        return info;

    double cum = 0.0;
    for (int i = 0; i < len; ++i)
    {
        cum += e[(size_t) i];
        if (cum >= 0.99 * total)
        {
            info.t99 = (double) i / reader->sampleRate;
            break;
        }
    }
    return info;
}

inline bool isSpace (const Info& info)
{
    if (info.seconds <= kSpaceSeconds)
        return false;             // short file: always a speaker
    return info.t99 > kSpaceT99Seconds;
}

// Reads a long IR down to a unit-energy buffer, truncated (with a fade so
// the cut cannot click) to maxSeconds. keepStereo=false mixes to mono (the cab
// slot's convolution is mono); true keeps a stereo IR's two channels (the air
// slot runs stereo). False means "no usable audio" — callers fall back to
// handing the file straight to juce::dsp::Convolution.
inline bool readSpace (const juce::File& file, juce::AudioBuffer<float>& out, double& srOut,
                       double maxSeconds = kMaxSpaceSeconds, bool keepStereo = false)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return false;

    const auto len = (int) juce::jmin ((double) reader->lengthInSamples, maxSeconds * reader->sampleRate);
    if (len <= 0)
        return false;

    juce::AudioBuffer<float> raw ((int) juce::jmin<juce::uint32> (2, reader->numChannels), len);
    if (! reader->read (&raw, 0, len, 0, true, raw.getNumChannels() > 1))
        return false;
    if (hasNonFinite (raw, len))
        return false;

    const bool stereoOut = keepStereo && raw.getNumChannels() > 1;
    out.setSize (stereoOut ? 2 : 1, len, false, false, true);
    out.copyFrom (0, 0, raw, 0, 0, len);
    if (stereoOut)
    {
        out.copyFrom (1, 0, raw, 1, 0, len);
    }
    else if (raw.getNumChannels() > 1)
    {
        out.addFrom (0, 0, raw, 1, 0, len);
        out.applyGain (0.5f);
    }

    if ((double) len < (double) reader->lengthInSamples)
    {
        const int fade = juce::jmin (len / 4, (int) (0.02 * reader->sampleRate));
        if (fade > 0)
            for (int ch = 0; ch < out.getNumChannels(); ++ch)
                out.applyGainRamp (ch, len - fade, fade, 1.0f, 0.0f);
    }

    // Unit energy on the loudest channel (one shared factor keeps the stereo
    // balance): sustained input then convolves through at ~dry loudness, which
    // is the 0 dB reference the AIR LEVEL knob trims around. Peak normalisation
    // here made a long tail +20..30 dB hot.
    double maxEnergy = 0.0;
    for (int ch = 0; ch < out.getNumChannels(); ++ch)
    {
        const auto* p = out.getReadPointer (ch);
        double e = 0.0;
        for (int i = 0; i < len; ++i)
            e += (double) p[i] * p[i];
        maxEnergy = juce::jmax (maxEnergy, e);
    }
    if (maxEnergy <= 1.0e-10)
        return false;
    out.applyGain ((float) (1.0 / std::sqrt (maxEnergy)));

    srOut = reader->sampleRate;
    return true;
}
} // namespace ampcade::ir
