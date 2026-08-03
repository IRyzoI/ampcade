// Offline sanity checks for the AmpCade audio path. Usage:
//   ampcade-smoke <path-to-NAM-example_models-dir>
// Exit 0 = pass. Keeps output to a handful of lines.

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_dsp/juce_dsp.h>

#include "FxPedals.h"
#include "IrLoader.h"
#include "Looper.h"
#include "NamSlot.h"
#include "ToneStack.h"
#include "Tone3000Client.h"

using namespace ampcade;

static int failures = 0;

static void check (bool ok, const char* what)
{
    if (! ok)
    {
        ++failures;
        std::cout << "FAIL: " << what << "\n";
    }
}

static double rmsOf (const std::vector<float>& v, size_t from)
{
    double acc = 0;
    size_t n = 0;
    for (size_t i = from; i < v.size(); ++i, ++n)
        acc += (double) v[i] * v[i];
    return n > 0 ? std::sqrt (acc / (double) n) : 0.0;
}

static bool allFinite (const std::vector<float>& v)
{
    for (auto s : v)
        if (! std::isfinite (s))
            return false;
    return true;
}

// Run one second of 220 Hz sine through a NamSlot at the given host rate.
static void runModel (NamSlot& slot, double sr, const char* label)
{
    const int block = 512;
    slot.prepare (sr, block);

    std::vector<float> out;
    out.reserve ((size_t) sr + block);

    double phase = 0.0;
    const double inc = juce::MathConstants<double>::twoPi * 220.0 / sr;
    float buf[512];

    for (int done = 0; done < (int) sr; done += block)
    {
        for (int i = 0; i < block; ++i)
        {
            buf[i] = 0.1f * (float) std::sin (phase);
            phase += inc;
        }
        const bool processed = slot.process (buf, block, 0.0f, 0.0f, NamSlot::Makeup::none);
        check (processed, "slot processed a block");
        out.insert (out.end(), buf, buf + block);
    }

    const auto rms = rmsOf (out, out.size() / 2);
    check (allFinite (out), "output finite");
    check (rms > 1.0e-5, "output not silent");
    std::cout << label << ": rms=" << juce::String (rms, 5) << " latency=" << slot.getLatency() << "\n";
}

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::cout << "usage: ampcade-smoke <example_models dir> | --authurl\n";
        return 2;
    }

    juce::MessageManager::getInstance();

    // Print the exact OAuth authorize URL the plugin will hand to the webview
    // (used to verify against the live API without clicking through the GUI).
    if (juce::String (argv[1]) == "--authurl")
    {
        ampcade::Tone3000Client client;
        std::cout << client.beginAuth().url << "\n";
        juce::MessageManager::deleteInstance();
        juce::DeletedAtShutdown::deleteAll();
        return 0;
    }

    const bool levelsOnly = (argc > 2 && juce::String (argv[2]) == "--levels");

    const juce::File modelsDir { juce::String (argv[1]) };

    // Gain-staging report: what a realistic -20 dBFS guitar level actually becomes
    // at each stage. Used to set the parameter ranges and the loudness target from
    // measurement instead of guesswork; see "weak input" in NOTES.md.
    if (levelsOnly)
    {
        const auto dbfs = [] (double lin) { return lin > 1.0e-9 ? 20.0 * std::log10 (lin) : -120.0; };

        for (auto name : { "wavenet.nam", "lstm.nam" })
        {
            auto f = modelsDir.getChildFile (name);
            if (! f.existsAsFile())
                continue;

            NamSlot slot;
            slot.prepare (48000.0, 512);
            if (slot.load (f).isNotEmpty())
                continue;

            const int block = 512, blocks = 94; // ~1 s
            for (float driveDb : { 0.0f, 12.0f, 24.0f })
                for (auto makeup : { NamSlot::Makeup::none, NamSlot::Makeup::loudness, NamSlot::Makeup::unity })
                {
                    std::vector<float> out;
                    double phase = 0.0;
                    const double inc = juce::MathConstants<double>::twoPi * 220.0 / 48000.0;
                    float buf[512];
                    double inSq = 0.0;

                    for (int b = 0; b < blocks; ++b)
                    {
                        for (int i = 0; i < block; ++i)
                        {
                            buf[i] = 0.1f * (float) std::sin (phase); // -23 dBFS rms
                            inSq += (double) buf[i] * buf[i];
                            phase += inc;
                        }
                        slot.process (buf, block, driveDb, 0.0f, makeup);
                        out.insert (out.end(), buf, buf + block);
                    }

                    const auto inRms = std::sqrt (inSq / (double) out.size());
                    const auto outRms = rmsOf (out, out.size() / 2);
                    double peak = 0.0;
                    for (size_t i = out.size() / 2; i < out.size(); ++i)
                        peak = juce::jmax (peak, (double) std::abs (out[i]));

                    std::cout << name << "  drive=" << juce::String (driveDb, 0) << "dB"
                              << "  makeup=" << (makeup == NamSlot::Makeup::none ? "none    "
                                                       : makeup == NamSlot::Makeup::loudness ? "loudness" : "unity   ")
                              << "  in=" << juce::String (dbfs (inRms), 1) << "dBFS"
                              << "  out=" << juce::String (dbfs (outRms), 1) << "dBFS"
                              << "  peak=" << juce::String (dbfs (peak), 1) << "dBFS"
                              << "  gain=" << juce::String (dbfs (outRms) - dbfs (inRms), 1) << "dB\n";
                }

            std::cout << name << "  hasLoudness=" << (slot.captureHasLoudness() ? "yes" : "no")
                      << " loudness=" << juce::String (slot.captureLoudnessDb(), 2) << "dB\n\n";
            slot.clear();
            slot.purgeGraveyard();
        }

        juce::MessageManager::deleteInstance();
        juce::DeletedAtShutdown::deleteAll();
        return 0;
    }

    // --- NAM models: native rate + resampled rate --------------------------
    for (auto name : { "wavenet.nam", "lstm.nam" })
    {
        auto f = modelsDir.getChildFile (name);
        if (! f.existsAsFile())
        {
            std::cout << "SKIP missing " << name << "\n";
            continue;
        }

        NamSlot slot;
        slot.prepare (48000.0, 512);
        const auto err = slot.load (f);
        check (err.isEmpty(), ("load " + juce::String (name) + " — " + err).toRawUTF8());
        if (err.isNotEmpty())
            continue;

        runModel (slot, 48000.0, (juce::String (name) + " @48k").toRawUTF8());
        runModel (slot, 44100.0, (juce::String (name) + " @44.1k (resampled)").toRawUTF8());

        // Pedal slots run Makeup::unity so that stacking pedals cannot bleed level
        // (raw captures measured 15-22 dB down). Since v0.8 the correction is a
        // FIXED per-capture number measured at load — not a runtime servo — so a
        // cranked DRIVE genuinely leaves the pedal hotter and hits the next block
        // harder (that level interaction is what makes pedal order audible).
        {
            const auto levelAt = [&slot] (float driveDb)
            {
                slot.prepare (48000.0, 512);
                std::vector<float> out;
                double phase = 0.0;
                float buf[512];
                double inSq = 0.0;
                for (int b = 0; b < 94; ++b)
                {
                    for (int i = 0; i < 512; ++i)
                    {
                        buf[i] = 0.1f * (float) std::sin (phase);
                        inSq += (double) buf[i] * buf[i];
                        phase += juce::MathConstants<double>::twoPi * 220.0 / 48000.0;
                    }
                    slot.process (buf, 512, driveDb, 0.0f, NamSlot::Makeup::unity);
                    out.insert (out.end(), buf, buf + 512);
                }
                const auto inRms = std::sqrt (inSq / (double) out.size());
                return 20.0 * std::log10 (juce::jmax (1.0e-9, rmsOf (out, out.size() / 2) / inRms));
            };

            const auto flat = levelAt (0.0f), driven = levelAt (24.0f);
            check (std::abs (flat) < 2.0, "unity makeup holds a pedal at input level");
            check (driven > flat - 1.5, "a driven pedal never comes out quieter");
            std::cout << name << " unity: drive0=" << juce::String (flat, 1)
                      << "dB drive24=" << juce::String (driven, 1) << "dB\n";
        }

        slot.clear();
        slot.purgeGraveyard();
    }

    // --- Pedal ORDER must be audible -----------------------------------------
    // Two different captures, one of them driven hard: A→B and B→A have to come
    // out measurably different. The old runtime unity servo normalized every
    // pedal's output back to its input level, so the chain sounded near-identical
    // in either order — exactly the bug this guards against.
    {
        auto fa = modelsDir.getChildFile ("wavenet.nam");
        auto fb = modelsDir.getChildFile ("lstm.nam");
        if (fa.existsAsFile() && fb.existsAsFile())
        {
            NamSlot a, b;
            a.prepare (48000.0, 512);
            b.prepare (48000.0, 512);
            if (a.load (fa).isEmpty() && b.load (fb).isEmpty())
            {
                const auto renderChain = [&] (NamSlot& first, float firstDrive,
                                              NamSlot& second, float secondDrive)
                {
                    first.prepare (48000.0, 512);
                    second.prepare (48000.0, 512);
                    std::vector<float> out;
                    double phase = 0.0;
                    float buf[512];
                    for (int blk = 0; blk < 94; ++blk)
                    {
                        for (int i = 0; i < 512; ++i)
                        {
                            buf[i] = 0.1f * (float) std::sin (phase);
                            phase += juce::MathConstants<double>::twoPi * 220.0 / 48000.0;
                        }
                        first.process (buf, 512, firstDrive, 0.0f, NamSlot::Makeup::unity);
                        second.process (buf, 512, secondDrive, 0.0f, NamSlot::Makeup::unity);
                        out.insert (out.end(), buf, buf + 512);
                    }
                    return out;
                };

                // Steady-state LEVELS converge by design now (the stacking floor:
                // a pedal never eats the chain's level, see NamSlot::process), so
                // "order matters" is checked on the WAVEFORM — different order,
                // different saturation, measurably different signal.
                const auto outAB = renderChain (a, 24.0f, b, 0.0f);
                const auto outBA = renderChain (b, 0.0f, a, 24.0f);
                const auto half = outAB.size() / 2;
                double diffSq = 0.0, refSq = 0.0;
                for (size_t i = half; i < outAB.size(); ++i)
                {
                    const double d = (double) outAB[i] - (double) outBA[i];
                    diffSq += d * d;
                    refSq += (double) outAB[i] * outAB[i];
                }
                const auto diffDb = 10.0 * std::log10 (juce::jmax (1.0e-12, diffSq)
                                                       / juce::jmax (1.0e-12, refSq));
                check (diffDb > -30.0, "pedal order changes the output");

                // And the feature that changed the old level check: a low-gain
                // pedal AFTER a driven one must not drag the chain down. Compare
                // the driven pedal alone vs driven→low-gain — adding the second
                // pedal may only ever ADD gain (small tolerance for tone shift).
                const auto rmsDb = [&] (const std::vector<float>& v)
                { return 20.0 * std::log10 (juce::jmax (1.0e-9, rmsOf (v, v.size() / 2))); };
                std::vector<float> aloneV;
                {
                    a.prepare (48000.0, 512);
                    double phase = 0.0;
                    float buf[512];
                    for (int blk = 0; blk < 94; ++blk)
                    {
                        for (int i = 0; i < 512; ++i)
                        {
                            buf[i] = 0.1f * (float) std::sin (phase);
                            phase += juce::MathConstants<double>::twoPi * 220.0 / 48000.0;
                        }
                        a.process (buf, 512, 24.0f, 0.0f, NamSlot::Makeup::unity);
                        aloneV.insert (aloneV.end(), buf, buf + 512);
                    }
                }
                const auto alone = rmsDb (aloneV), stacked = rmsDb (outAB);
                check (stacked > alone - 1.5, "stacking a low-gain pedal never reduces the chain's level");
                std::cout << "order: waveform-diff=" << juce::String (diffDb, 1)
                          << "dB  A(drv24) alone=" << juce::String (alone, 2)
                          << "dB  A->B stacked=" << juce::String (stacked, 2) << "dB\n";
            }
            a.clear(); b.clear();
            a.purgeGraveyard(); b.purgeGraveyard();
        }
    }

    // --- Tone stack: bass boost must lift a 100 Hz tone --------------------
    {
        ToneStack ts;
        ts.prepare (48000.0, 512);

        auto energy = [&] (float bassKnob)
        {
            ts.prepare (48000.0, 512);
            std::vector<float> out;
            double phase = 0;
            float buf[512];
            for (int b = 0; b < 40; ++b)
            {
                for (int i = 0; i < 512; ++i)
                {
                    buf[i] = 0.1f * (float) std::sin (phase);
                    phase += juce::MathConstants<double>::twoPi * 100.0 / 48000.0;
                }
                ts.process (buf, 512, bassKnob, 5.0f, 5.0f);
                out.insert (out.end(), buf, buf + 512);
            }
            return rmsOf (out, out.size() / 2);
        };

        const auto flat = energy (5.0f);
        const auto boosted = energy (10.0f);
        check (boosted > flat * 1.5, "tone stack bass boost works");
        std::cout << "tonestack: flat=" << juce::String (flat, 5) << " bass10=" << juce::String (boosted, 5) << "\n";
    }

    // --- Convolution with a synthetic delayed-spike IR ---------------------
    {
        juce::dsp::Convolution conv;
        conv.prepare ({ 48000.0, 512, 1 });

        const int delay = 1000;
        juce::AudioBuffer<float> ir (1, delay + 64);
        ir.clear();
        ir.setSample (0, delay, 0.9f);
        conv.loadImpulseResponse (std::move (ir), 48000.0,
                                  juce::dsp::Convolution::Stereo::no,
                                  juce::dsp::Convolution::Trim::no,
                                  juce::dsp::Convolution::Normalise::no);

        juce::Thread::sleep (300); // IR install happens on a background thread

        bool sawDelayedSpike = false;
        std::vector<float> tail;
        for (int b = 0; b < 300 && ! sawDelayedSpike; ++b)
        {
            float buf[512] = {};
            if (b % 8 == 0)
                buf[0] = 1.0f; // impulse train
            float* ch[1] = { buf };
            auto block = juce::dsp::AudioBlock<float> (ch, 1, 512);
            conv.process (juce::dsp::ProcessContextReplacing<float> (block));
            check (std::isfinite (buf[0]), "conv output finite");

            // impulse enters at sample 0 of block b%8==0; expect energy at +1000 samples
            if (b % 8 == 1 && std::abs (buf[delay - 512]) > 0.4f)
                sawDelayedSpike = true;
        }
        check (sawDelayedSpike, "convolution engaged (delayed spike found)");
        std::cout << "convolution: engaged=" << (sawDelayedSpike ? "yes" : "no") << "\n";
    }

    // --- IR triage: a speaker cab and a reverb IR are not the same animal ---
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("ampcade-smoke-irs");
        dir.createDirectory();

        // Writes a decaying-noise IR of the given length at a known peak;
        // padSeconds of digital silence after it (how "mix ready" cab packs pad
        // their files out — the case the old length-based triage got wrong).
        const auto writeIr = [&dir] (const juce::String& name, double seconds, float peak, int channels,
                                     double padSeconds = 0.0)
        {
            auto file = dir.getChildFile (name);
            const int len = (int) (seconds * 48000.0);
            const int total = len + (int) (padSeconds * 48000.0); // trailing silence
            juce::AudioBuffer<float> buf (channels, total);
            buf.clear();
            juce::Random rng (99);
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < len; ++i)
                    buf.setSample (ch, i, (i == 0 ? 1.0f : (rng.nextFloat() * 2.0f - 1.0f) * 0.5f)
                                              * std::exp (-(float) i / (0.25f * (float) len)) * peak);

            juce::WavAudioFormat fmt;
            file.deleteFile();
            if (auto out = std::unique_ptr<juce::FileOutputStream> (file.createOutputStream()))
                if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (
                        fmt.createWriterFor (out.get(), 48000.0, (unsigned int) channels, 24, {}, 0)))
                {
                    out.release();
                    writer->writeFromAudioSampleBuffer (buf, 0, total);
                }
            return file;
        };

        const auto cabFile = writeIr ("cab.wav", 0.08, 0.7f, 1);
        const auto revFile = writeIr ("reverb.wav", 1.5, 0.05f, 2);  // quiet + stereo, like real space IRs
        const auto hugeFile = writeIr ("hall.wav", 6.0, 0.4f, 1);

        // A speaker padded out to a long file must still read as a speaker: the
        // triage looks at where the energy lives (t99), not at the file length.
        const auto paddedCab = writeIr ("padded_cab.wav", 0.06, 0.7f, 1, 0.7);
        const auto cabInfo = ampcade::ir::analyze (cabFile);
        const auto padInfo = ampcade::ir::analyze (paddedCab);
        const auto revInfo = ampcade::ir::analyze (revFile);
        check (! ampcade::ir::isSpace (cabInfo), "80 ms IR reads as a cab");
        check (! ampcade::ir::isSpace (padInfo), "silence-padded 0.76 s cab IR still reads as a cab");
        check (padInfo.seconds > ampcade::ir::kSpaceSeconds, "padded cab really is a long file");
        check (ampcade::ir::isSpace (revInfo), "1.5 s IR reads as a reverb");

        juce::AudioBuffer<float> buf;
        double sr = 0.0;
        check (ampcade::ir::readSpace (revFile, buf, sr), "space IR reads");
        check (buf.getNumChannels() == 1, "space IR comes back mono");
        check (std::abs (sr - 48000.0) < 1.0, "space IR keeps its sample rate");
        // Unit energy: convolution loudness follows the IR's total energy, so
        // this is what makes the wet path land at ~dry level no matter how hot
        // or quiet the file was rendered. (Peak normalisation here put a long
        // tail +20..30 dB over dry — the speaker-blowing bug.)
        double energy = 0.0;
        for (int i = 0; i < buf.getNumSamples(); ++i)
            energy += (double) buf.getSample (0, i) * buf.getSample (0, i);
        check (std::abs (energy - 1.0) < 0.01, "space IR is energy-normalised");

        juce::AudioBuffer<float> huge;
        check (ampcade::ir::readSpace (hugeFile, huge, sr), "long space IR reads");
        const auto seconds = (double) huge.getNumSamples() / sr;
        check (seconds <= ampcade::ir::kMaxSpaceSeconds + 0.01, "long space IR is truncated");
        check (std::abs (huge.getSample (0, huge.getNumSamples() - 1)) < 1.0e-4f,
               "truncated space IR is faded out (no click)");

        std::cout << "ir triage: cab t99=" << juce::String (cabInfo.t99 * 1000.0, 1) << "ms"
                  << " padded(" << juce::String (padInfo.seconds, 2) << "s) t99="
                  << juce::String (padInfo.t99 * 1000.0, 1) << "ms"
                  << " reverb t99=" << juce::String (revInfo.t99, 2) << "s"
                  << " energy=" << juce::String (energy, 3)
                  << " hall kept=" << juce::String (seconds, 2) << "s\n";
        dir.deleteRecursively();
    }

    // --- Removing an IR must actually remove it ----------------------------
    // The v0.5 trap: juce::dsp::Convolution only installs a queued IR from inside
    // process(), so "removing" one by stopping the block left the old IR in the
    // engine. With a reverb IR in the cab slot that meant the wash never went
    // away — not on clear, not on loading a preset — until another IR was loaded
    // to displace it. clearCabIr() queues an inert 1-sample IR and keeps the
    // convolution running (output discarded) until the swap has really happened.
    {
        juce::dsp::Convolution conv;
        conv.prepare ({ 48000.0, 512, 1 });

        // Stand-in for a reverb IR: half a second of decaying noise.
        juce::Random rng (20260726);
        juce::AudioBuffer<float> ir (1, 24000);
        for (int i = 0; i < ir.getNumSamples(); ++i)
            ir.setSample (0, i, (i == 0 ? 1.0f : (rng.nextFloat() * 2.0f - 1.0f) * 0.4f)
                                    * std::exp (-(float) i / 7000.0f));
        conv.loadImpulseResponse (std::move (ir), 48000.0,
                                  juce::dsp::Convolution::Stereo::no,
                                  juce::dsp::Convolution::Trim::no,
                                  juce::dsp::Convolution::Normalise::no);

        const auto pump = [&conv] (int blocks, bool impulseFirst)
        {
            double energy = 0.0;
            for (int b = 0; b < blocks; ++b)
            {
                float buf[512] = {};
                if (b == 0 && impulseFirst)
                    buf[0] = 1.0f;
                float* ch[1] = { buf };
                auto block = juce::dsp::AudioBlock<float> (ch, 1, 512);
                conv.process (juce::dsp::ProcessContextReplacing<float> (block));
                if (b >= 2) // past the direct hit: this is tail only
                    for (int i = 0; i < 512; ++i)
                        energy += (double) buf[i] * buf[i];
            }
            return energy;
        };

        juce::Thread::sleep (300); // install happens on a background thread
        const auto wetEnergy = pump (8, true);
        check (wetEnergy > 1.0e-4, "long IR convolves a tail");

        juce::AudioBuffer<float> unit (1, 1);
        unit.setSample (0, 0, 1.0f);
        conv.loadImpulseResponse (std::move (unit), 48000.0,
                                  juce::dsp::Convolution::Stereo::no,
                                  juce::dsp::Convolution::Trim::no,
                                  juce::dsp::Convolution::Normalise::no);
        juce::Thread::sleep (300);
        const int sizeWithoutPumping = conv.getCurrentIRSize();
        check (sizeWithoutPumping > 1, "queued IR does NOT install while the block is skipped");

        int guard = 0;
        while (conv.getCurrentIRSize() > 1 && guard++ < 400)
            pump (1, false);
        check (conv.getCurrentIRSize() <= 1, "inert IR installs once the block keeps running");

        pump (60, false); // let the crossfade and the old tail finish
        const auto dryEnergy = pump (8, true);
        check (dryEnergy < wetEnergy * 1.0e-3, "a cleared cab stops convolving");
        std::cout << "cab clear: wet=" << juce::String (wetEnergy, 6)
                  << " stale-size=" << sizeWithoutPumping
                  << " drain-blocks=" << guard
                  << " after=" << juce::String (dryEnergy, 9) << "\n";
    }

    // --- Stereo delay: impulse must come back ~380ms later on both sides ----
    // (v0.6 made it a straight echo: both channels on the same tap — the old
    // 1.15x right-channel offset turned everything into a ping-pongy wash.)
    {
        StereoDelay dly;
        dly.prepare (48000.0, 512);

        std::vector<float> L (48000, 0.0f), R (48000, 0.0f);
        L[0] = R[0] = 1.0f;

        for (int off = 0; off < 48000; off += 512)
            dly.process (L.data() + off, R.data() + off, 512, 380.0f, 0.35f, 0.5f, true);

        auto peakNear = [] (const std::vector<float>& v, int center)
        {
            float p = 0;
            for (int i = juce::jmax (0, center - 200); i < juce::jmin ((int) v.size(), center + 200); ++i)
                p = juce::jmax (p, std::abs (v[i]));
            return p;
        };

        const int tap = (int) (0.380 * 48000);
        check (allFinite (L) && allFinite (R), "delay output finite");
        check (peakNear (L, tap) > 0.05f, "delay L echo present");
        check (peakNear (R, tap) > 0.05f, "delay R echo present");
        std::cout << "delay: L@380ms=" << juce::String (peakNear (L, tap), 3)
                  << " R@380ms=" << juce::String (peakNear (R, tap), 3) << "\n";

        // The tempo-sync ceiling: a half note at 60 BPM = 2000 ms must survive
        // the clamp and come back as a real echo (the line holds 2.2 s).
        StereoDelay dly2;
        dly2.prepare (48000.0, 512);
        std::vector<float> L2 (120000, 0.0f), R2 (120000, 0.0f);
        L2[0] = R2[0] = 1.0f;
        for (int off = 0; off + 512 <= (int) L2.size(); off += 512)
            dly2.process (L2.data() + off, R2.data() + off, 512, 2000.0f, 0.0f, 0.5f, true);
        check (peakNear (L2, (int) (2.0 * 48000)) > 0.05f, "delay echoes at the 2000ms sync ceiling");
    }

    // --- Tri-chorus: lush, stable, and never a level change ------------------
    {
        ChorusPedal cho;
        cho.prepare (48000.0, 512);
        std::vector<float> out;
        double phase = 0.0;
        float buf[512];
        for (int blk = 0; blk < 188; ++blk) // ~2 s
        {
            for (int i = 0; i < 512; ++i)
            {
                buf[i] = 0.3f * (float) std::sin (phase);
                phase += juce::MathConstants<double>::twoPi * 220.0 / 48000.0;
            }
            cho.process (buf, 512, 0.6f, 5.0f, 0.5f, true);
            out.insert (out.end(), buf, buf + 512);
        }
        check (allFinite (out), "chorus output finite");
        const auto rmsDb = 20.0 * std::log10 (juce::jmax (1.0e-9, rmsOf (out, out.size() / 2)));
        const auto inDb = 20.0 * std::log10 (0.3 / std::sqrt (2.0));
        check (std::abs (rmsDb - inDb) < 3.0, "chorus at defaults holds the chain's level");

        // extreme settings must stay bounded (no runaway, no NaN)
        ChorusPedal ext;
        ext.prepare (48000.0, 512);
        std::vector<float> out2;
        phase = 0.0;
        for (int blk = 0; blk < 94; ++blk)
        {
            for (int i = 0; i < 512; ++i)
            {
                buf[i] = 0.5f * (float) std::sin (phase);
                phase += juce::MathConstants<double>::twoPi * 220.0 / 48000.0;
            }
            ext.process (buf, 512, 8.0f, 10.0f, 1.0f, true);
            out2.insert (out2.end(), buf, buf + 512);
        }
        check (allFinite (out2), "chorus extremes stay finite");
        std::cout << "chorus: default rms=" << juce::String (rmsDb, 1) << "dB (dry "
                  << juce::String (inDb, 1) << "dB)\n";
    }

    // --- Compressor: squeezes dynamics, bypass is bit-exact ------------------
    {
        const auto gainAt = [] (float amp)
        {
            CompPedal c;
            c.prepare (48000.0, 512);
            std::vector<float> v (48000);
            double phase = 0.0;
            for (int off = 0; off + 512 <= (int) v.size(); off += 512)
            {
                float buf[512];
                for (int i = 0; i < 512; ++i)
                {
                    buf[i] = amp * (float) std::sin (phase);
                    phase += juce::MathConstants<double>::twoPi * 220.0 / 48000.0;
                }
                c.process (buf, 512, 6.0f, 3.0f, 0.0f, true);
                std::copy (buf, buf + 512, v.begin() + off);
            }
            return rmsOf (v, v.size() / 2) / (amp / std::sqrt (2.0));
        };
        const auto gLoud = gainAt (0.5f), gQuiet = gainAt (0.02f);
        check (gQuiet > gLoud * 1.5, "compressor squeezes loud harder than quiet");

        CompPedal off;
        off.prepare (48000.0, 512);
        float buf[512], ref[512];
        for (int i = 0; i < 512; ++i)
            ref[i] = buf[i] = 0.4f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * i / 48000.0);
        off.process (buf, 512, 8.0f, 2.0f, 6.0f, false);
        check (std::equal (buf, buf + 512, ref), "compressor off is a straight wire");
        std::cout << "compressor: gain@-6dB=" << juce::String (gLoud, 2)
                  << " gain@-34dB=" << juce::String (gQuiet, 2) << "\n";
    }

    // --- Clean boost: exact gain, level-exact bypass -------------------------
    {
        BoostPedal b;
        b.prepare (48000.0, 512);
        std::vector<float> v (48000);
        double phase = 0.0;
        for (int off = 0; off + 512 <= (int) v.size(); off += 512)
        {
            float buf[512];
            for (int i = 0; i < 512; ++i)
            {
                buf[i] = 0.1f * (float) std::sin (phase);
                phase += juce::MathConstants<double>::twoPi * 220.0 / 48000.0;
            }
            b.process (buf, 512, 6.0f, true);
            std::copy (buf, buf + 512, v.begin() + off);
        }
        const auto gainDb = 20.0 * std::log10 (rmsOf (v, v.size() / 2) / (0.1 / std::sqrt (2.0)));
        check (std::abs (gainDb - 6.0) < 0.1, "boost at +6 dB gains exactly +6 dB");

        BoostPedal off;
        off.prepare (48000.0, 512);
        float buf[512], ref[512];
        for (int i = 0; i < 512; ++i)
            ref[i] = buf[i] = 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * i / 48000.0);
        off.process (buf, 512, 12.0f, false);
        check (std::equal (buf, buf + 512, ref), "boost off is a straight wire");
        std::cout << "boost: +6dB knob -> " << juce::String (gainDb, 2) << "dB\n";
    }

    // --- AIR EQ: shapes the tail, bypasses exactly when flat ----------------
    {
        AirEq eq;
        eq.prepare (48000.0, 512);

        // flat = bit-exact bypass
        std::vector<float> L (4096), R (4096), refL, refR;
        juce::Random rng (7);
        for (auto* v : { &L, &R })
            for (auto& s : *v)
                s = rng.nextFloat() * 2.0f - 1.0f;
        refL = L; refR = R;
        eq.process (L.data(), R.data(), (int) L.size(), 5.0f, 5.0f);
        check (L == refL && R == refR, "air EQ at flat is a straight wire");

        // bass knob at 0 shelves a 60 Hz tone by ~12 dB
        AirEq eq2;
        eq2.prepare (48000.0, 512);
        std::vector<float> lo (96000), loR (96000);
        for (size_t i = 0; i < lo.size(); ++i)
            lo[i] = loR[i] = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 60.0 * (double) i / 48000.0);
        for (int off = 0; off + 512 <= (int) lo.size(); off += 512)
            eq2.process (lo.data() + off, loR.data() + off, 512, 0.0f, 5.0f);
        const auto cutDb = 20.0 * std::log10 (juce::jmax (1.0e-9, rmsOf (lo, lo.size() / 2))
                                              / (0.5 / std::sqrt (2.0)));
        check (cutDb < -9.0, "air EQ bass at 0 tames a 60 Hz boom");
        std::cout << "airEq: 60Hz @ bass=0 -> " << juce::String (cutDb, 1) << "dB\n";
    }

    // --- Looper transparency: what comes back must be BIT-EXACTLY what went
    // in — any coloration players hear is not this class's doing.
    {
        Looper lp;
        lp.prepare (48000.0, 512);
        lp.setConfig (120, 4, true /*play after rec*/, false /*no count-in*/);
        std::vector<float> fed;
        double phase = 0.0;
        float L[512], R[512];
        lp.tap(); // -> Record
        for (int blk = 0; blk < 10; ++blk)
        {
            for (int i = 0; i < 512; ++i)
            {
                L[i] = R[i] = 0.4f * (float) std::sin (phase);
                phase += juce::MathConstants<double>::twoPi * 220.0 / 48000.0;
            }
            fed.insert (fed.end(), L, L + 512);
            lp.process (L, R, 512);
        }
        lp.tap(); // close -> Play
        std::vector<float> played;
        for (int blk = 0; blk < 10; ++blk)
        {
            std::fill (L, L + 512, 0.0f);
            std::fill (R, R + 512, 0.0f);
            lp.process (L, R, 512);
            played.insert (played.end(), L, L + 512);
        }
        bool exact = played.size() == fed.size();
        for (size_t i = 0; exact && i < fed.size(); ++i)
            exact = played[i] == fed[i];
        check (exact, "loop playback is bit-exactly the recorded signal");
        lp.erase();
        std::cout << "looper transparency: " << (exact ? "bit-exact" : "MISMATCH") << "\n";
    }

    // --- Looper: tap → count-in → record → close → overdub/play ------------
    auto layersTmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("ampcade-smoke-loops");
    layersTmp.deleteRecursively();
    juce::File savedDir; // the one session we keep, asserted on after ~Looper
    {
        Looper lp;
        lp.setLayersRoot (layersTmp); // every pass below must land on disk as a stem
        lp.prepare (48000.0, 512);
        lp.setConfig (120, 4, false); // 4 beats @120 = 2 s count-in

        std::vector<float> L (512, 0.1f), R (512, 0.1f);
        const auto pump = [&] (int blocks)
        {
            double energy = 0.0;
            for (int b = 0; b < blocks; ++b)
            {
                std::fill (L.begin(), L.end(), 0.1f);
                std::fill (R.begin(), R.end(), 0.1f);
                lp.process (L.data(), R.data(), 512);
                for (auto s : L) energy += (double) s * s;
            }
            return energy;
        };

        check (lp.getState() == Looper::Idle, "looper starts idle");
        lp.tap();
        pump (1);
        check (lp.getState() == Looper::Count, "tap starts the count-in");
        // ~0.5 s window: passthrough energy is 46*512*0.1^2 ≈ 235.5, one decaying
        // click adds ~16 on top — assert the click is there, not that it's loud.
        const double passthrough = 46.0 * 512.0 * 0.01;
        const auto countEnergy = pump (46);
        check (countEnergy > passthrough + 5.0, "count-in clicks are audible");
        pump (188);                            // ride out the rest of the 2 s
        check (lp.getState() == Looper::Record, "count-in ends in record");
        pump (94);                             // more recording (~24k samples began during the pumps above)
        lp.tap();
        pump (1);
        check (lp.getState() == Looper::Overdub, "closing the loop lands in overdub");
        // 235+94+1 blocks minus the 96000-sample count-in ≈ 1.51 s recorded
        check (lp.lengthSeconds() > 1.4 && lp.lengthSeconds() < 1.65, "loop length ≈ recorded time");
        lp.tap();
        pump (1);
        check (lp.getState() == Looper::Play, "tap toggles overdub → play");
        // playback must add the recorded material to the live signal
        std::fill (L.begin(), L.end(), 0.0f);
        std::fill (R.begin(), R.end(), 0.0f);
        lp.process (L.data(), R.data(), 512);
        double playEnergy = 0.0;
        for (auto s : L) playEnergy += (double) s * s;
        check (playEnergy > 1.0e-4, "playback adds the loop to the output");
        lp.erase();
        pump (1);
        check (lp.getState() == Looper::Idle, "erase returns to idle");

        // Sessions are throwaways until saved: the erase above must delete the
        // run's stem folder from Loops/Unsaved (writer thread, so wait for it).
        const auto unsaved = layersTmp.getChildFile ("Unsaved");
        const auto unsavedCount = [&unsaved]
        {
            juce::Array<juce::File> dirs;
            unsaved.findChildFiles (dirs, juce::File::findDirectories, false);
            return dirs.size();
        };
        // Wait for the BACKUP to appear — that is the discard's real
        // postcondition. ("Unsaved is empty" is racy: before the writer thread
        // has drained the first open marker, Unsaved is empty already.)
        const auto backup = layersTmp.getChildFile (Looper::kBackupFolderName);
        for (int i = 0; i < 120 && ! backup.isDirectory(); ++i)
            juce::Thread::sleep (25);
        check (backup.isDirectory() && backup.getNumberOfChildFiles (juce::File::findFiles, "*.wav") >= 2,
               "erased take survives as the rolling auto-backup (with its stems)");
        check (unsavedCount() == 0, "erase discards the unsaved session folder");

        // playAfterRec + count-in OFF + save: this take is a keeper
        lp.setConfig (240, 2, true, false);   // no pre-roll
        lp.tap();
        pump (1);
        check (lp.getState() == Looper::Record, "count-in off: tap goes straight to record");
        pump (20);
        lp.tap();
        pump (1);
        check (lp.getState() == Looper::Play, "playAfterRec lands in play");

        // ---- overdub-layer undo/redo: dub a pass, lift it out, put it back
        const auto lapEnergy = [&]
        {
            // four laps with silent live input (the output IS the loop) — the
            // ceil() phase remainder is then noise, not a tolerance hazard
            const int blocks = 4 * (int) std::ceil (lp.lengthSeconds() * 48000.0 / 512.0);
            double e = 0.0;
            for (int b = 0; b < blocks; ++b)
            {
                std::fill (L.begin(), L.end(), 0.0f);
                std::fill (R.begin(), R.end(), 0.0f);
                lp.process (L.data(), R.data(), 512);
                for (auto s : L) e += (double) s * s;
            }
            return e;
        };
        lp.tap();       // Play -> Overdub
        pump (10);      // add a layer over ~half the loop
        lp.tap();       // -> Play
        pump (1);
        const double withDub = lapEnergy();
        lp.undoLayer();
        pump (1);
        const double undone = lapEnergy();
        check (undone < withDub * 0.8, "undo lifts the last overdub layer out");
        lp.redoLayer();
        pump (1);
        const double redone = lapEnergy();
        check (std::abs (redone - withDub) < withDub * 0.05, "redo puts the layer back");

        savedDir = lp.saveSession();
        check (savedDir != juce::File{} && savedDir.isDirectory(), "saveSession returns the kept folder");
        check (savedDir.getParentDirectory() == layersTmp, "saved session moves out of Unsaved");
        check (savedDir.getChildFile ("Loop (mix).wav").existsAsFile(), "saved session contains the mix wav");
        check (savedDir.getChildFile ("Layer 01 (base).wav").existsAsFile(), "saved session keeps its layer stems");

        lp.erase();
        pump (1);
        juce::Thread::sleep (300);            // writer processes the discard marker
        check (savedDir.getChildFile ("Loop (mix).wav").existsAsFile(), "erase never deletes a SAVED session");
        std::cout << "looper: state machine + save/discard ok, saved=" << savedDir.getFileName() << "\n";
    }   // ~Looper joins the writer thread (an unsaved session would die here too)

    // --- Saved session contents: mix + base stem hold audio ----------------
    {
        juce::Array<juce::File> wavs;
        savedDir.findChildFiles (wavs, juce::File::findFiles, false, "*.wav");
        check (wavs.size() >= 2, "saved folder holds the mix plus the stems");

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        int nonEmpty = 0;
        for (const auto& f : wavs)
            if (auto reader = std::unique_ptr<juce::AudioFormatReader> (fm.createReaderFor (f)))
                if (reader->lengthInSamples > 0)
                    ++nonEmpty;
        check (nonEmpty == wavs.size(), "saved wavs contain audio");
        std::cout << "looper save: " << wavs.size() << " wavs kept in " << savedDir.getFileName() << "\n";
        layersTmp.deleteRecursively();
    }

    // --- Reverb: impulse must grow a tail ---------------------------------
    {
        ReverbPedal rev;
        rev.prepare (48000.0, 512);

        std::vector<float> L (48000, 0.0f), R (48000, 0.0f);
        L[0] = R[0] = 1.0f;

        for (int off = 0; off < 48000; off += 512)
            rev.process (L.data() + off, R.data() + off, 512, 6.0f, 0.5f, true);

        double tail = 0;
        for (int i = 12000; i < 36000; ++i)
            tail += (double) L[i] * L[i] + (double) R[i] * R[i];

        check (allFinite (L) && allFinite (R), "reverb output finite");
        check (tail > 1.0e-4, "reverb tail present");
        std::cout << "reverb: tail energy=" << juce::String (tail, 5) << "\n";
    }

    juce::MessageManager::deleteInstance();
    juce::DeletedAtShutdown::deleteAll();

    std::cout << (failures == 0 ? "SMOKE PASS\n" : "SMOKE FAIL\n");
    return failures == 0 ? 0 : 1;
}
