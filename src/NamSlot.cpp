#include "NamSlot.h"

#include "NAM/convnet.h"
#include "NAM/linear.h"
#include "NAM/lstm.h"
#include "NAM/wavenet/model.h"

namespace ampcade
{
// Each NAM architecture registers its config parser from a static initializer in
// its own translation unit. Those TUs end up as members of AmpCade_SharedCode.a,
// and a linker only pulls an archive member that resolves an undefined symbol —
// so in the *plugin* builds every architecture was silently stripped and any
// capture failed with "No config parser registered for architecture: WaveNet".
//
// v1 of this fix touched the function addresses through a volatile sink, which
// forces the TUs into the link but still trusts each TU's static registrar to
// run — and those registrars are zero-size objects in anonymous namespaces that
// Xcode 15.4 folds into unnamed init code (which also broke the post-build
// symbol check on CI). So register EXPLICITLY: calling create_config by name
// both drags the TU into the link and puts the parser in the registry with no
// static-initializer trust involved. has() guards make it a no-op wherever the
// TU's own registrar already ran. Do not "clean this up".
//
// The offline smoke test never caught the original bug: it is a console app,
// so it links the nam_core objects directly instead of through the archive.
// scripts/check-nam-linked.sh greps every built plugin binary for the
// create_config symbols POST_BUILD.
void keepNamArchitecturesLinked() noexcept
{
    auto& reg = nam::ConfigParserRegistry::instance();
    if (! reg.has ("WaveNet")) reg.registerParser ("WaveNet", &nam::wavenet::create_config);
    if (! reg.has ("LSTM"))    reg.registerParser ("LSTM",    &nam::lstm::create_config);
    if (! reg.has ("ConvNet")) reg.registerParser ("ConvNet", &nam::convnet::create_config);
    if (! reg.has ("Linear"))  reg.registerParser ("Linear",  &nam::linear::create_config);
}

juce::String NamSlot::load (const juce::File& namFile)
{
    keepNamArchitecturesLinked();

    if (! namFile.existsAsFile())
        return "File not found: " + namFile.getFullPathName();

    std::shared_ptr<ResamplingNam> fresh;

    try
    {
        nam::DspLoadOptions options;
        options.prewarm = false; // reset() below prewarms once, at the right rate
        auto dsp = nam::get_dsp (std::filesystem::path (namFile.getFullPathName().toStdString()), options);

        if (dsp == nullptr)
            return "Couldn't parse this .nam file";
        if (dsp->NumInputChannels() != 1 || dsp->NumOutputChannels() != 1)
            return "Only mono NAM captures are supported";

        fresh = std::make_shared<ResamplingNam> (std::move (dsp));

        // Measure the capture's inherent level offset once, here on the loader
        // thread: half a second of 220 Hz at 0.1 peak (a realistic guitar level,
        // same signal the smoke test uses). Captures trained hot measured
        // 15-22 dB below their own input; this fixed number replaces the old
        // runtime servo so DRIVE and pedal order keep their real-life level
        // dynamics (see NOTES.md).
        {
            constexpr int block = 512;

            // Size the net for the block we are about to push through it, NOT for
            // hostBlock. Every internal Eigen matrix in a NAM model is allocated
            // with exactly maxBufferSize columns, and the only guard on the far
            // side is assert(num_frames <= mMaxBufferSize) — which compiles out of
            // Release, so an over-long block silently writes past all of them and
            // smashes the heap. hostBlock is the player's device buffer and is
            // routinely far below 512 (a 16-sample interface setting sized the net
            // to 32 columns and this loop then wrote 512 into it, corrupting the
            // heap until some unrelated allocation on the message thread died).
            // The reset below puts it back to hostBlock for the audio thread.
            fresh->reset (hostRate, juce::jmax (hostBlock, block));

            const int total = (int) (hostRate / 2.0);
            double phase = 0.0, inSq = 0.0, outSq = 0.0;
            const double inc = juce::MathConstants<double>::twoPi * 220.0 / hostRate;
            float buf[block];
            int counted = 0;

            for (int done = 0; done < total; done += block)
            {
                for (int i = 0; i < block; ++i)
                {
                    buf[i] = 0.1f * (float) std::sin (phase);
                    phase += inc;
                }
                const bool settle = done < total / 3; // let the net warm up first
                if (! settle)
                    for (int i = 0; i < block; ++i)
                        inSq += (double) buf[i] * buf[i];

                fresh->process (buf, block);

                if (! settle)
                {
                    for (int i = 0; i < block; ++i)
                        outSq += (double) buf[i] * buf[i];
                    counted += block;
                }
            }

            const double inRms = std::sqrt (inSq / juce::jmax (1, counted));
            const double outRms = std::sqrt (outSq / juce::jmax (1, counted));
            if (inRms > 1.0e-6 && outRms > 1.0e-6)
                fresh->unityDb = juce::jlimit (kUnityMinDb, kUnityMaxDb,
                                               (float) juce::Decibels::gainToDecibels (inRms / outRms));

            // Back down to the real device block: resizes the net for the audio
            // thread and clears the measurement out of it in one go.
            fresh->reset (hostRate, hostBlock);
        }
    }
    catch (const std::exception& e)
    {
        return juce::String ("Couldn't load model: ") + e.what();
    }

    std::shared_ptr<ResamplingNam> old;
    {
        const juce::SpinLock::ScopedLockType l (lock);
        old = std::move (active);
        active = std::move (fresh);
    }

    if (old != nullptr)
    {
        const juce::ScopedLock g (graveyardLock);
        graveyard.push_back (std::move (old));
    }

    return {};
}

void NamSlot::clear()
{
    std::shared_ptr<ResamplingNam> old;
    {
        const juce::SpinLock::ScopedLockType l (lock);
        old = std::move (active);
        active = nullptr;
    }

    if (old != nullptr)
    {
        const juce::ScopedLock g (graveyardLock);
        graveyard.push_back (std::move (old));
    }
}

bool NamSlot::process (float* buffer, int numFrames, float driveDb, float levelDb, Makeup makeup)
{
    std::shared_ptr<ResamplingNam> local;
    {
        const juce::SpinLock::ScopedTryLockType l (lock);
        if (! l.isLocked())
            return false; // model being swapped right now — pass through this block
        local = active;
    }

    if (local == nullptr)
        return false;

    driveGain.setTargetValue (juce::Decibels::decibelsToGain (driveDb));
    driveGain.applyGain (buffer, numFrames);

    // A NAM net's matrices are sized by reset() and its only bounds check is an
    // assert() that compiles out of Release, so a block longer than we prepared
    // for writes straight past them and smashes the heap. processBlock grows its
    // own scratch when a host overruns the prepared size, but a net cannot be
    // resized on the audio thread — so feed it in prepared-size chunks. The
    // normal case (numFrames <= hostBlock) is one pass and costs nothing.
    // The in/out energies feed the stacking floor below.
    double inSq = 0.0, outSq = 0.0;
    const int chunk = juce::jmax (1, hostBlock);
    for (int off = 0; off < numFrames; off += chunk)
    {
        const int c = juce::jmin (chunk, numFrames - off);
        for (int i = 0; i < c; ++i)
            inSq += (double) buffer[off + i] * buffer[off + i];
        local->process (buffer + off, c);
        for (int i = 0; i < c; ++i)
            outSq += (double) buffer[off + i] * buffer[off + i];
    }

    float post = levelDb;

    if (makeup == Makeup::loudness && local->hasLoudness)
    {
        post += kLoudnessTargetDb - (float) local->loudnessDb;
    }
    else if (makeup == Makeup::unity)
    {
        // Fixed per-capture correction measured at load (see NamSlot::load).
        // Deliberately NOT a runtime servo: with the servo, a cranked DRIVE and a
        // boosted pedal upstream were both normalized away before the next block
        // ever heard them, so swapping a Klon and a Screamer around each other
        // barely changed the sound. Now the level that leaves a pedal actually
        // depends on how hard it was hit — like the physical pedals do.
        //
        // One exception, and it is one-sided: unityDb was measured at reference
        // level, but a capture hit HARDER than that clamps against its trained
        // ceiling — a low-gain Screamer after a cranked Klon ate the Klon's boost
        // and dropped the level of the whole chain ("adding a pedal removed
        // gain"). So when the capture is measurably eating level beyond its
        // static correction, make up the difference — slowly, so dynamics inside
        // a phrase survive — and never in the other direction: a pedal that
        // boosts keeps its boost, order and drive still matter, but stacking
        // pedals can only ever ADD gain, exactly like stacking real ones with
        // their level knobs set right.
        const float inRms = (float) std::sqrt (inSq / (double) juce::jmax (1, numFrames));
        const float outRms = (float) std::sqrt (outSq / (double) juce::jmax (1, numFrames));
        if (inRms > 1.0e-3f && outRms > 1.0e-6f) // only track while actually playing
        {
            const float deficit = juce::Decibels::gainToDecibels (inRms / outRms, -60.0f);
            const float want = juce::jlimit (0.0f, kStackCompMaxDb, deficit - local->unityDb);
            // ~300 ms one-pole: fast enough to catch "kicked on a boost", slow
            // enough that pick attack and palm mutes keep their level movement.
            const float alpha = 1.0f - std::exp ((float) -numFrames / ((float) hostRate * 0.3f));
            stackCompDb += alpha * (want - stackCompDb);
        }
        post += local->unityDb + stackCompDb;
    }

    levelGain.setTargetValue (juce::Decibels::decibelsToGain (post));
    levelGain.applyGain (buffer, numFrames);
    return true;
}

void NamSlot::purgeGraveyard()
{
    const juce::ScopedLock g (graveyardLock);
    graveyard.erase (std::remove_if (graveyard.begin(), graveyard.end(),
                                     [] (auto& p) { return p.use_count() == 1; }),
                     graveyard.end());
}
} // namespace ampcade
