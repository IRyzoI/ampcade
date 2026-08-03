#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>

namespace ampcade
{
// Two-button looper at the very end of the chain, so it records exactly what
// you hear. The main tap cycles: empty → count-in (4 clicks) → record →
// overdub → play → overdub → … Stop halts playback but keeps the loop, erase
// clears it, and the loop can be exported as a wav.
//
// Threading: the message thread only posts one-shot commands through `cmd`;
// the audio thread owns the state machine and mirrors state/pos/len into
// atomics for the UI. Nothing allocates on the audio thread.
//
// Layer stems: every record/overdub pass is also streamed to its own wav
// (Loops/Unsaved/<session>/Layer NN.wav) through a lock-free FIFO drained by
// a background thread. Overdub layers are padded with silence up to the loop
// position where the pass began — drop every layer file at the same bar line
// and they line up.
//
// Sessions are THROWAWAY until saved: most loops are noodling, so nothing is
// kept automatically. saveSession() writes the mixed loop into the session
// folder and moves the whole folder out of Unsaved into the Loops root in one
// go; an unsaved session is deleted on erase and on shutdown, and anything
// left behind in Unsaved (a crash) is swept at the next startup.
class Looper : private juce::Thread
{
public:
    enum State { Idle = 0, Count = 1, Record = 2, Overdub = 3, Play = 4, Stopped = 5 };

    static constexpr double kMaxSeconds = 300.0; // 5 minutes (was 60 s)

    Looper() : juce::Thread ("AmpCade loop layer writer") {}

    // The most recent unsaved take is never just deleted: it becomes the one
    // rolling backup, overwritten by the next throwaway. Crash insurance.
    static constexpr const char* kBackupFolderName = "Last loop (auto-backup)";

    ~Looper() override
    {
        signalThreadShouldExit();
        notify();
        stopThread (4000);
        closeLayerFile();
        // quitting without saving: the take becomes the rolling backup
        if (! sessionSaved.load() && writerSessionDir != juce::File{})
            promoteToBackup (writerSessionDir);
    }

    // Message thread, once at startup: where layer sessions get written.
    // Streaming stays off until this is set.
    void setLayersRoot (const juce::File& dir)
    {
        layersRoot = dir;
        if (! isThreadRunning())
            startThread (Priority::background);
    }
    juce::File getLayersRoot() const { return layersRoot; }

    void prepare (double sampleRate, int /*maxBlock*/)
    {
        playG.reset (sampleRate, 0.05);
        playG.setCurrentAndTargetValue (playGainTarget.load());
        baseG.reset (sampleRate, 0.05);
        baseG.setCurrentAndTargetValue (baseGainTarget.load());
        const int wantLen = (int) (kMaxSeconds * sampleRate);
        if (std::abs (sampleRate - sr) > 1.0 || buffer.getNumSamples() != wantLen)
        {
            const bool layerWasOpen = aState == Record || aState == Overdub;
            sr = sampleRate;
            writerRate.store (sampleRate);
            buffer.setSize (2, wantLen, false, true);
            // No clearing: the pass-tracking guarantees we only ever read
            // indices this pass wrote, and clearing would commit ~115 MB of
            // pages for a buffer most sessions barely touch.
            undoBuf.setSize (2, wantLen, false, false, true);
            aState = Idle;
            aPos = aLen = aCount = 0;
            aSessionOpen = false;
            passActive = false;
            haveDub = dubRemoved = false;
            haveBase = eraseHadBase = false;   // an imported bed at the old rate is invalid
            // The erased loop is measured in FRAMES, so it is meaningless once
            // the buffer is rebuilt at a different rate — and worse than
            // meaningless: unerase does "aLen = aErasedLen" with no clamp, so a
            // loop recorded at 96 kHz and unerased after dropping to 44.1 kHz
            // set aLen past the end of the smaller buffer and played whatever
            // heap sat beyond it. Dropping the offer here is the whole fix;
            // publish() below then clears uneraseAvail in the UI.
            aErasedLen = 0;
            // ...and so is one still in flight: cancel a staged import so the
            // audio thread can't adopt wrong-rate buffers after the change
            importPending.store (false);
            int expected7 = 7;
            cmd.compare_exchange_strong (expected7, 0);
            if (layerWasOpen) // device change mid-take: close the stem cleanly
                layerEnd();   // (message thread, but audio is stopped during prepare)
            publish();
        }
        clickLen = (int) (0.04 * sampleRate);
    }

    // -------- message thread --------
    void tap()      { cmd.store (1); }
    void stopLoop() { cmd.store (2); }
    // Loop playback volume (linear). Applied to what the loop plays back,
    // never to what it records — the take is always captured at unity.
    void setPlayGain (float linear) { playGainTarget.store (juce::jlimit (0.0f, 8.0f, linear)); }
    // Imported-bed volume (linear): the imported audio lives on its OWN layer
    // (baseBuf) so it can be balanced against the player's overdubs forever.
    void setBaseGain (float linear) { baseGainTarget.store (juce::jlimit (0.0f, 8.0f, linear)); }
    bool hasImportedBase() const { return baseAvail.load(); }

    void erase()    { cmd.store (3); }
    // Erase is reversible until the next recording starts: the audio is still
    // sitting in the loop buffer, only the length was zeroed — unerase puts
    // the length back and the loop plays again. (Layer undo history and the
    // on-disk stems don't come back; the audible mix does, which is what an
    // "oops" needs.)
    void unerase()  { cmd.store (6); }
    bool canUnerase() const { return uneraseAvail.load(); }
    // One level, like a hardware looper: undo lifts the last overdub layer
    // out of the loop, redo puts it back. A new overdub replaces the layer.
    void undoLayer(){ cmd.store (4); }
    void redoLayer(){ cmd.store (5); }
    bool canUndoLayer() const { return undoAvail.load(); }
    bool canRedoLayer() const { return redoAvail.load(); }

    // Count-in tempo / beats-per-bar, whether there is a count-in at all, and
    // what a closed first loop lands in (overdub keeps layering, play just
    // loops). Read when the first tap arrives.
    void setConfig (int bpmIn, int beatsIn, bool playAfterRecIn, bool countInOn = true,
                    bool ringOutOn = false)
    {
        bpm.store (juce::jlimit (40, 240, bpmIn));
        beats.store (juce::jlimit (2, 7, beatsIn));
        playAfterRec.store (playAfterRecIn);
        countIn.store (countInOn);
        ringOut.store (ringOutOn);
    }
    int getBpm() const { return bpm.load(); }
    int getBeats() const { return beats.load(); }
    bool getPlayAfterRec() const { return playAfterRec.load(); }
    bool getCountIn() const { return countIn.load(); }
    // Ring-out: while the loop is in pure PLAY, the processor routes its
    // playback into the reverb's input instead of after it, so stopping
    // leaves a natural tail instead of a dead cut. Consumed by processBlock;
    // the looper itself doesn't change behaviour.
    bool getRingOut() const { return ringOut.load(); }

    int getState() const { return state.load(); }
    double lengthSeconds() const { return sr > 0 ? len.load() / sr : 0.0; }
    double positionSeconds() const { return sr > 0 ? pos.load() / sr : 0.0; }

    // The folder the current/most recent layer session is writing into ({} if
    // none yet). Message thread; used by "open loops folder".
    juce::File currentSessionDir() const
    {
        const juce::ScopedLock l (sessionDirLock);
        return sessionDir;
    }

    // Message thread. Keep the current session: write the mixed loop into the
    // session folder and move the whole folder (mix + every layer stem) out of
    // Unsaved into the Loops root. Returns the saved folder, {} on failure.
    // Saving again after more overdubs just refreshes the mix in place.
    juce::File saveSession()
    {
        const int st = state.load();
        if (st == Record || st == Overdub || len.load() <= 0)
            return {};

        // let the writer thread finish closing the last layer stem
        for (int i = 0; i < 40 && fifo.getNumReady() > 0; ++i)
            juce::Thread::sleep (25);

        juce::File dir;
        {
            const juce::ScopedLock l (sessionDirLock);
            dir = sessionDir;
        }
        if (dir == juce::File{} || ! dir.isDirectory())
        {
            // An imported bed with zero overdubs has no stem session yet — a
            // playable loop must still save. Make the folder directly in the
            // Loops root; the writer adopts it for any later overdub stems.
            if (layersRoot == juce::File{})
                return {};
            dir = layersRoot.getChildFile (
                "Loop " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M.%S"));
            if (dir.exists())
                dir = dir.getNonexistentSibling();
            if (! dir.createDirectory())
                return {};
            {
                const juce::ScopedLock l (sessionDirLock);
                sessionDir = dir;
                writerDirMoved = true;
            }
            sessionSaved.store (true);
        }

        if (! sessionSaved.load())
        {
            auto dest = layersRoot.getChildFile (dir.getFileName());
            if (dest.exists())
                dest = dest.getNonexistentSibling();
            if (! dir.moveFileTo (dest))
                return {};
            {
                const juce::ScopedLock l (sessionDirLock);
                sessionDir = dest;
                writerDirMoved = true;   // the writer picks the new home up on its next layer
            }
            sessionSaved.store (true);
            dir = dest;
        }

        if (! writeWav (dir.getChildFile ("Loop (mix).wav")))
            return {};
        // the imported source file sits beside the mix and stems, so a saved
        // session folder is the complete story of the take
        if (hasImportedBase() && importSrcFile.existsAsFile())
            importSrcFile.copyFileTo (dir.getChildFile ("Imported - " + importSrcFile.getFileName()));
        return dir;
    }

    // Export the mixed loop. The audio thread may still be playing it back
    // (fine) — the UI disables save while recording/overdubbing, so the copy
    // is stable.
    bool writeWav (const juce::File& file) const
    {
        const int ln = len.load();
        if (ln <= 0)
            return false;

        juce::AudioBuffer<float> copy (2, ln);
        for (int ch = 0; ch < 2; ++ch)
            copy.copyFrom (ch, 0, buffer, ch, 0, ln);
        // What you hear is what exports: bake the bed at the HEARD balance
        // (baseGain relative to playGain), with the dubs normalised to unity
        // as exports always were — loop_vol is monitoring, not mix level.
        if (baseAvail.load() && baseBuf.getNumSamples() >= ln)
        {
            const float pg = juce::jmax (0.01f, playGainTarget.load());
            for (int ch = 0; ch < 2; ++ch)
                copy.addFrom (ch, 0, baseBuf, ch, 0, ln, baseGainTarget.load() / pg);
        }
        // the ratio can push past full scale — keep the wav clean
        const float peak = copy.getMagnitude (0, ln);
        if (peak > 0.98f)
            copy.applyGain (0.98f / peak);

        juce::WavAudioFormat fmt;
        file.deleteFile();
        if (auto out = std::unique_ptr<juce::FileOutputStream> (file.createOutputStream()))
            if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (
                    fmt.createWriterFor (out.get(), sr, 2, 24, {}, 0)))
            {
                out.release();
                return writer->writeFromAudioSampleBuffer (copy, 0, ln);
            }
        return false;
    }

    // Message thread: read an audio file, resample it to the engine rate and
    // adopt it as the current loop (it starts playing). Shared loops from
    // other players, or any wav, become the loop in one step. Returns false
    // with `err` set when it can't. targetRms (linear) is the level to match
    // — the processor passes its live-playing loudness so the import sits
    // against THIS player's rig; 0 = never played yet, use a safe default.
    bool importLoopFile (const juce::File& file, juce::String& err, float targetRms = 0.0f)
    {
        const int st = state.load();
        if (st == Record || st == Overdub || st == Count)
        {
            err = "Finish the current take first";
            return false;
        }
        if (importPending.load())
        {
            err = "An import is already in progress";
            return false;
        }
        if (sr <= 0.0)   // prepare() never ran: no rate to resample to
        {
            err = "Audio isn't running - start it and try again";
            return false;
        }

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0
            || reader->numChannels < 1)
        {
            err = "Couldn't read that audio file";
            return false;
        }

        // resample to the engine rate; cap the source so the RESULT fits the loop
        const double ratio = reader->sampleRate / sr;             // src frames per out frame
        const int srcLen = (int) juce::jmin (reader->lengthInSamples,
                                             (juce::int64) (kMaxSeconds * reader->sampleRate));
        juce::AudioBuffer<float> src ((int) reader->numChannels, srcLen);
        if (! reader->read (&src, 0, srcLen, 0, true, true))
        {
            err = "Couldn't read that audio file";
            return false;
        }

        const int wantLen = (int) (kMaxSeconds * sr);
        const int frames = juce::jlimit (1, wantLen, (int) std::llround (srcLen / ratio));
        juce::AudioBuffer<float> staged;
        staged.setSize (2, wantLen);   // uncleared on purpose: only written pages commit
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* in = src.getReadPointer (juce::jmin (ch, src.getNumChannels() - 1));
            float* out = staged.getWritePointer (ch);
            if (std::abs (ratio - 1.0) < 1.0e-9)
                juce::FloatVectorOperations::copy (out, in, frames);
            else
            {
                juce::LagrangeInterpolator li;
                li.process (ratio, in, out, frames, srcLen, 0);
            }
        }

        // Level-match the import to the PLAYER. Mastered files are dense and
        // loud continuously; a live guitar peaks high but its sustained RMS
        // sits far lower — a fixed "-18 RMS" target buried the guitar and
        // every overdub. Match the player's own recent playing loudness
        // (clamped sane), measured over the file's ACTIVE stretches only so
        // count-ins and silence don't skew it. Boost is capped at +12 dB.
        {
            const float target = juce::jlimit (juce::Decibels::decibelsToGain (-32.0f),
                                               juce::Decibels::decibelsToGain (-14.0f),
                                               targetRms > 0.0f ? targetRms
                                                                : juce::Decibels::decibelsToGain (-22.0f));
            double activeSum = 0.0;
            juce::int64 activeCnt = 0;
            const int blk = 4096;
            for (int start = 0; start < frames; start += blk)
            {
                const int nb = juce::jmin (blk, frames - start);
                double s2 = 0.0;
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float* d = staged.getReadPointer (ch) + start;
                    for (int i = 0; i < nb; ++i)
                        s2 += (double) d[i] * d[i];
                }
                if (std::sqrt (s2 / (nb * 2)) > 3.16e-3)   // block above ~-50 dBFS
                {
                    activeSum += s2;
                    activeCnt += nb * 2;
                }
            }
            const double rms = activeCnt > 0 ? std::sqrt (activeSum / (double) activeCnt) : 0.0;
            if (rms > 1.0e-6)
            {
                const float g = juce::jmin (juce::Decibels::decibelsToGain (12.0f),
                                            (float) (target / rms));
                if (std::abs (g - 1.0f) > 0.01f)
                    staged.applyGain (0, frames, g);
            }
        }

        // The import becomes the BASE layer (its own buffer + its own gain);
        // the main loop buffer restarts empty so overdubs land on a clean
        // slate and stay separately balanceable. Only the loop's actual
        // length gets zeroed — pages past it are never read.
        juce::AudioBuffer<float> stagedDub;
        stagedDub.setSize (2, wantLen);
        stagedDub.clear (0, 0, frames);
        stagedDub.clear (1, 0, frames);

        // hand both to the audio thread: it SWAPS buffers (pointer swaps, no
        // allocation, no copy) — the old storage comes back to us in the
        // staging members and is freed on the next import's move-assign.
        importBuf = std::move (staged);
        importDub = std::move (stagedDub);
        importLen.store (frames);
        importAdopted.store (false);
        importPending.store (true);
        cmd.store (7);
        for (int i = 0; i < 50; ++i)
        {
            if (! importPending.load())
            {
                if (importAdopted.load())
                {
                    importSrcFile = file;   // saveSession copies it beside the mix
                    return true;
                }
                err = "Finish the current take first";
                return false;
            }
            juce::Thread::sleep (10);
        }
        int expected = 7;                       // audio never ran: take the command back
        cmd.compare_exchange_strong (expected, 0);
        importPending.store (false);
        if (importAdopted.load())               // ...unless it landed during the last sleep
        {
            importSrcFile = file;
            return true;
        }
        err = "Audio isn't running - start it and try again"; // ASCII only: no u8() in this header
        return false;
    }

    // -------- audio thread --------
    void process (float* L, float* R, int n)
    {
        applyCommand (cmd.exchange (0));
        playG.setTargetValue (playGainTarget.load());
        baseG.setTargetValue (baseGainTarget.load());

        if (aState == Idle || aState == Stopped)
        {
            playG.skip (n);
            baseG.skip (n);
            publish();
            return;
        }

        auto* b0 = buffer.getWritePointer (0);
        auto* b1 = buffer.getWritePointer (1);
        const float* base0 = haveBase ? baseBuf.getReadPointer (0) : nullptr;
        const float* base1 = haveBase ? baseBuf.getReadPointer (1) : nullptr;
        const int maxLen = buffer.getNumSamples();

        for (int i = 0; i < n; ++i)
        {
            const float pg = playG.getNextValue();
            const float bg = baseG.getNextValue();
            if (aState == Count)
            {
                const int total = aBeats * beatLen;
                const int g = total - aCount;      // elapsed
                const int inBeat = g % beatLen;
                if (inBeat < clickLen)
                {
                    const float f = (g / beatLen == 0) ? 1318.5f : 880.0f;
                    const float t = (float) inBeat / (float) sr;
                    const float click = 0.35f * std::sin (juce::MathConstants<float>::twoPi * f * t)
                                              * std::exp (-t * 90.0f);
                    L[i] += click;
                    R[i] += click;
                }
                if (--aCount <= 0)
                {
                    aState = Record;
                    aPos = 0;
                    layerStart (0);
                }
                continue;
            }

            if (aState == Record)
            {
                b0[aPos] = L[i];
                b1[aPos] = R[i];
                layerPush (L[i], R[i]);
                if (++aPos >= maxLen) // hit the ceiling: close the loop
                {
                    aLen = maxLen;
                    aPos = 0;
                    layerEnd();
                    aState = playAfterRec.load() ? Play : Overdub;
                    if (aState == Overdub)
                    {
                        layerStart (0);
                        beginDubPass();
                    }
                }
                continue;
            }

            // Overdub / Play — aLen is always > 0 here
            const float outL = b0[aPos], outR = b1[aPos];
            if (aState == Overdub)
            {
                // Shadow the added signal so the whole pass can be lifted out
                // again (undo). The pass runs sequentially from its start, so
                // the first aLen samples are first-touches; after a full lap
                // it keeps accumulating on the same indices.
                auto* u0 = undoBuf.getWritePointer (0);
                auto* u1 = undoBuf.getWritePointer (1);
                if (passTouched < aLen)
                {
                    u0[aPos] = L[i];
                    u1[aPos] = R[i];
                    ++passTouched;
                }
                else
                {
                    u0[aPos] += L[i];
                    u1[aPos] += R[i];
                }
                b0[aPos] += L[i];
                b1[aPos] += R[i];
                layerPush (L[i], R[i]);
            }
            L[i] += outL * pg;
            R[i] += outR * pg;
            if (base0 != nullptr)              // the imported bed, own volume
            {
                L[i] += base0[aPos] * bg;
                R[i] += base1[aPos] * bg;
            }
            if (++aPos >= aLen)
                aPos = 0;
        }

        publish();
    }

private:
    void applyCommand (int c)
    {
        if (c == 0)
            return;

        if (c == 3) // erase
        {
            if (aState == Record || aState == Overdub)
                layerEnd();
            aErasedLen = aLen;    // the buffer keeps the mix — unerase restores it
            eraseHadBase = haveBase;   // the imported bed comes back with it
            haveBase = false;
            aState = Idle;
            aLen = aPos = aCount = 0;
            aSessionOpen = false; // the next recording starts a fresh session folder
            passActive = false;
            haveDub = dubRemoved = false;
            sessionDiscard();     // the take rolls into the auto-backup folder
        }
        else if (c == 6) // unerase: bring the just-erased loop back
        {
            if (aState == Idle && aErasedLen > 0)
            {
                // prepare() drops aErasedLen on a rate change, so this should
                // never bind — clamp anyway, because the cost of being wrong
                // here is playing uninitialised heap out of the speakers.
                aLen = juce::jmin (aErasedLen, buffer.getNumSamples());
                aErasedLen = 0;
                aPos = 0;
                haveBase = eraseHadBase;
                eraseHadBase = false;
                aState = Stopped;
            }
        }
        else if (c == 7) // adopt an imported loop staged on the message thread
        {
            if (importPending.load())
            {
                if (aState != Record && aState != Overdub && aState != Count)
                {
                    std::swap (baseBuf, importBuf);  // the imported bed, own layer
                    std::swap (buffer, importDub);   // fresh zeroed loop for overdubs
                    haveBase = true;
                    eraseHadBase = false;
                    aLen = juce::jmin (importLen.load(), buffer.getNumSamples());
                    aPos = aCount = 0;
                    aErasedLen = 0;
                    aSessionOpen = false;   // overdubs on the import start a fresh session
                    passActive = false;
                    haveDub = dubRemoved = false;
                    sessionDiscard();       // the previous take rolls into the auto-backup
                    aState = Play;          // hear it right away
                    importAdopted.store (true);
                }
                importPending.store (false);
            }
        }
        else if (c == 2) // stop
        {
            if (aState == Count) { aState = Idle; aCount = 0; }
            else if (aState == Record)
            {
                aLen = juce::jmax (0, aPos);
                aPos = 0;
                layerEnd();
                aState = aLen > 0 ? Stopped : Idle;
            }
            else if (aState == Overdub || aState == Play)
            {
                if (aState == Overdub)
                {
                    layerEnd();
                    endDubPass();
                }
                aState = Stopped;
            }
        }
        else if (c == 4) // undo: lift the last overdub layer out of the loop
        {
            if (haveDub && ! dubRemoved && aState != Overdub && aLen > 0)
            {
                applyDubLayer (-1.0f);
                dubRemoved = true;
            }
        }
        else if (c == 5) // redo: put it back
        {
            if (haveDub && dubRemoved && aState != Overdub && aLen > 0)
            {
                applyDubLayer (1.0f);
                dubRemoved = false;
            }
        }
        else if (c == 1) // main tap
        {
            switch (aState)
            {
                case Idle:
                    aErasedLen = 0; // recording over the buffer: the erased take is gone for real
                    haveBase = eraseHadBase = false;   // ...and so is the imported bed
                    if (countIn.load())
                    {
                        beatLen = juce::jmax (1, (int) (sr * 60.0 / (double) bpm.load()));
                        aBeats = beats.load();
                        aCount = aBeats * beatLen;
                        aState = Count;
                    }
                    else // no pre-roll: the tap IS beat one
                    {
                        aState = Record;
                        aPos = 0;
                        layerStart (0);
                    }
                    break;
                case Record:
                    aLen = juce::jmax (1, aPos);
                    aPos = 0;
                    layerEnd();
                    aState = playAfterRec.load() ? Play : Overdub;
                    if (aState == Overdub)
                    {
                        layerStart (0);
                        beginDubPass();
                    }
                    break;
                case Overdub:
                    layerEnd();
                    endDubPass();
                    aState = Play;
                    break;
                case Play:
                    layerStart (aPos); // pad to the loop position where the pass begins
                    beginDubPass();
                    aState = Overdub;
                    break;
                case Stopped:
                    if (aLen > 0) { aPos = 0; aState = Play; }
                    else            aState = Idle;
                    break;
                default: break; // Count: taps during the count-in are ignored
            }
        }
        publish();
    }

    void publish()
    {
        state.store (aState, std::memory_order_relaxed);
        pos.store (aState == Count ? aCount : aPos, std::memory_order_relaxed);
        len.store (aLen, std::memory_order_relaxed);
        undoAvail.store (haveDub && ! dubRemoved && aState != Overdub && aLen > 0,
                         std::memory_order_relaxed);
        redoAvail.store (haveDub && dubRemoved && aState != Overdub && aLen > 0,
                         std::memory_order_relaxed);
        uneraseAvail.store (aState == Idle && aErasedLen > 0, std::memory_order_relaxed);
        baseAvail.store (haveBase, std::memory_order_relaxed);
    }

    // ---------------- overdub-layer undo (audio side) ----------------
    void beginDubPass()
    {
        passStart = aPos;
        passTouched = 0;
        passActive = true;
        haveDub = dubRemoved = false; // a new pass replaces the undo layer
    }

    void endDubPass()
    {
        if (! passActive)
            return;
        passActive = false;
        if (passTouched > 0)
        {
            haveDub = true;
            dubRemoved = false;
            dubStart = passStart;
            dubCount = juce::jmin (passTouched, aLen);
        }
    }

    // Add (redo) or subtract (undo) the shadowed pass. One walk over at most
    // aLen samples on the audio thread — instant for musical loop lengths;
    // a full five-minute loop may cost one audible block.
    void applyDubLayer (float sign)
    {
        auto* b0 = buffer.getWritePointer (0);
        auto* b1 = buffer.getWritePointer (1);
        const auto* u0 = undoBuf.getReadPointer (0);
        const auto* u1 = undoBuf.getReadPointer (1);
        for (int i = 0; i < dubCount; ++i)
        {
            const int idx = (dubStart + i) % aLen;
            b0[idx] += sign * u0[idx];
            b1[idx] += sign * u1[idx];
        }
    }

    // ---------------- layer streaming (audio side) ----------------
    // In-band protocol over one stereo-frame FIFO. Audio is guaranteed finite
    // here (processBlock scrubs non-finite samples before the looper), so NaN
    // in L can never be audio and marks a control frame:
    //   (NaN, 1) (pad, newSession) -> open the next layer file, write `pad`
    //                                 silent frames first
    //   (NaN, 0) (0, 0)            -> close the current layer file
    //   (NaN, 2) (0, 0)            -> session over: delete its folder unless
    //                                 it was saved (markers stay in order with
    //                                 the layer opens/closes around them)
    void fifoWrite2 (float a0, float b0v, float a1, float b1v)
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (2, s1, n1, s2, n2);
        if (n1 + n2 < 2) { fifo.finishedWrite (0); return; } // full: drop (writer stalled >5 s)
        auto put = [this] (int idx, float l, float r) { fifoL[(size_t) idx] = l; fifoR[(size_t) idx] = r; };
        if (n1 >= 1) put (s1, a0, b0v);
        if (n1 >= 2) put (s1 + 1, a1, b1v);
        else if (n2 >= 1) put (s2, a1, b1v);
        fifo.finishedWrite (2);
    }

    void layerStart (int padFrames)
    {
        if (layersRoot == juce::File{})
            return;
        const bool newSession = ! aSessionOpen;
        aSessionOpen = true;
        fifoWrite2 (std::numeric_limits<float>::quiet_NaN(), 1.0f,
                    (float) padFrames, newSession ? 1.0f : 0.0f);
        notify();
    }

    void layerEnd()
    {
        if (layersRoot == juce::File{})
            return;
        fifoWrite2 (std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 0.0f);
        notify();
    }

    void sessionDiscard() // audio thread (erase) — the writer does the deleting
    {
        if (layersRoot == juce::File{})
            return;
        fifoWrite2 (std::numeric_limits<float>::quiet_NaN(), 2.0f, 0.0f, 0.0f);
        notify();
    }

    void layerPush (float l, float r)
    {
        if (layersRoot == juce::File{})
            return;
        // The looper sits before the output scrubber, so guarantee the marker
        // protocol's invariant (NaN in L = control frame) ourselves.
        if (! std::isfinite (l)) l = 0.0f;
        if (! std::isfinite (r)) r = 0.0f;
        int s1, n1, s2, n2;
        fifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 < 1) { fifo.finishedWrite (0); return; }
        fifoL[(size_t) s1] = l;
        fifoR[(size_t) s1] = r;
        fifo.finishedWrite (1);
    }

    // ---------------- layer streaming (writer thread) ----------------
    void run() override
    {
        while (! threadShouldExit())
        {
            drainFifo();
            wait (50);
        }
        drainFifo(); // whatever is left when the app closes
    }

    void drainFifo()
    {
        for (;;)
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (fifo.getNumReady(), s1, n1, s2, n2);
            const int total = n1 + n2;
            if (total <= 0) { fifo.finishedRead (0); return; }

            int consumed = 0;
            auto frame = [&] (int k, float& l, float& r)
            {
                const int idx = k < n1 ? s1 + k : s2 + (k - n1);
                l = fifoL[(size_t) idx];
                r = fifoR[(size_t) idx];
            };

            while (consumed < total)
            {
                float l, r;
                frame (consumed, l, r);
                if (std::isnan (l))
                {
                    if (consumed + 1 >= total)
                        break; // marker payload not in the fifo yet — come back
                    float pl, pr;
                    frame (consumed + 1, pl, pr);
                    consumed += 2;
                    const int code = (int) std::lround (r);
                    if (code == 1)
                        openNextLayerFile ((int) pl, pr > 0.5f);
                    else if (code == 2)
                        discardWriterSession();
                    else
                        closeLayerFile();
                }
                else
                {
                    // batch every contiguous audio frame up to the next marker
                    int run = consumed;
                    while (run < total)
                    {
                        float tl, tr;
                        frame (run, tl, tr);
                        if (std::isnan (tl))
                            break;
                        ++run;
                    }
                    writeFrames (consumed, run - consumed, s1, n1, s2);
                    consumed = run;
                }
            }
            fifo.finishedRead (consumed);
            if (consumed == 0)
                return; // half a marker pair — wait for the rest
        }
    }

    void writeFrames (int k0, int count, int s1, int n1, int s2)
    {
        if (layerWriter == nullptr || count <= 0)
            return;
        // contiguous stretches within the fifo's (up to) two regions
        while (count > 0)
        {
            const int idx = k0 < n1 ? s1 + k0 : s2 + (k0 - n1);
            const int regionLeft = k0 < n1 ? n1 - k0 : count;
            const int c = juce::jmin (count, regionLeft);
            const float* chans[2] = { fifoL.data() + idx, fifoR.data() + idx };
            layerWriter->writeFromFloatArrays (chans, 2, c);
            k0 += c;
            count -= c;
        }
    }

    void openNextLayerFile (int padFrames, bool newSession)
    {
        closeLayerFile();

        // saveSession moved the folder out from under us — follow it
        {
            const juce::ScopedLock l (sessionDirLock);
            if (writerDirMoved)
            {
                writerSessionDir = sessionDir;
                writerDirMoved = false;
            }
        }

        if (newSession || writerSessionDir == juce::File{})
        {
            sessionSaved.store (false);
            writerSessionDir = layersRoot.getChildFile ("Unsaved").getChildFile (
                "Loop " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M.%S"));
            if (writerSessionDir.exists()) // erase → re-record inside the same second
                writerSessionDir = writerSessionDir.getNonexistentSibling();
            writerSessionDir.createDirectory();
            writerLayerNum = 0;
            const juce::ScopedLock l (sessionDirLock);
            sessionDir = writerSessionDir;
        }

        ++writerLayerNum;
        const auto name = juce::String::formatted ("Layer %02d%s.wav", writerLayerNum,
                                                   writerLayerNum == 1 ? " (base)" : "");
        auto f = writerSessionDir.getChildFile (name);
        f.deleteFile();

        juce::WavAudioFormat fmt;
        if (auto out = std::unique_ptr<juce::FileOutputStream> (f.createOutputStream()))
            if (auto* w = fmt.createWriterFor (out.get(), writerRate.load(), 2, 24, {}, 0))
            {
                out.release();
                layerWriter.reset (w);
            }

        if (layerWriter != nullptr && padFrames > 0)
        {
            // silence up to the loop position where the overdub began, so every
            // layer file starts at the loop's bar line
            std::vector<float> zeros ((size_t) 4096, 0.0f);
            const float* chans[2] = { zeros.data(), zeros.data() };
            for (int left = padFrames; left > 0;)
            {
                const int c = juce::jmin (left, 4096);
                layerWriter->writeFromFloatArrays (chans, 2, c);
                left -= c;
            }
        }
    }

    void closeLayerFile()
    {
        layerWriter = nullptr; // AudioFormatWriter flushes + closes on destruction
    }

    // Writer thread, on the erase marker: an unsaved session becomes the one
    // rolling auto-backup (crash/regret insurance), overwriting the previous
    // backup. A saved session keeps everything where it is.
    void discardWriterSession()
    {
        closeLayerFile();
        {
            const juce::ScopedLock l (sessionDirLock);
            if (writerDirMoved)
            {
                writerSessionDir = sessionDir;
                writerDirMoved = false;
            }
        }
        if (writerSessionDir != juce::File{} && ! sessionSaved.load())
            promoteToBackup (writerSessionDir);
        writerSessionDir = juce::File{};
        writerLayerNum = 0;
        sessionSaved.store (false);
        const juce::ScopedLock l (sessionDirLock);
        sessionDir = juce::File{};
    }

    void promoteToBackup (const juce::File& dir)
    {
        if (dir == juce::File{} || ! dir.isDirectory() || layersRoot == juce::File{})
            return;
        auto backup = layersRoot.getChildFile (kBackupFolderName);
        backup.deleteRecursively();
        if (! dir.moveFileTo (backup))
            dir.deleteRecursively(); // move failed (odd volume): don't leave strays
    }

    juce::AudioBuffer<float> buffer;
    juce::AudioBuffer<float> undoBuf; // what the last overdub pass added
    double sr = 0.0;
    int beatLen = 24000, clickLen = 1920;

    // audio-thread owned
    int aState = Idle, aPos = 0, aLen = 0, aCount = 0, aBeats = 4;
    bool aSessionOpen = false;
    int passStart = 0, passTouched = 0;   // the overdub pass in flight
    bool passActive = false;
    int dubStart = 0, dubCount = 0;       // the last completed pass (undoable)
    bool haveDub = false, dubRemoved = false;

    std::atomic<int> cmd { 0 };
    // Import handshake: the message thread stages importBuf/importLen, sets
    // importPending and posts cmd 7; the audio thread swaps buffers and clears
    // pending. Old loop storage returns via importBuf, freed on the NEXT import.
    juce::AudioBuffer<float> importBuf;
    std::atomic<int> importLen { 0 };
    std::atomic<bool> importPending { false }, importAdopted { false };
    std::atomic<int> state { Idle }, pos { 0 }, len { 0 };
    std::atomic<int> bpm { 120 }, beats { 4 };
    std::atomic<bool> playAfterRec { false };
    std::atomic<bool> countIn { true };
    std::atomic<bool> ringOut { false };
    std::atomic<float> playGainTarget { 1.0f };
    juce::SmoothedValue<float> playG { 1.0f };
    // the imported bed: its own buffer, gain and availability flag
    juce::AudioBuffer<float> baseBuf;
    juce::AudioBuffer<float> importDub;        // staging for the fresh dub buffer
    bool haveBase = false, eraseHadBase = false;   // audio thread
    std::atomic<float> baseGainTarget { 1.0f };
    juce::SmoothedValue<float> baseG { 1.0f };
    std::atomic<bool> baseAvail { false };
    juce::File importSrcFile;                  // message thread: the imported file's origin
    std::atomic<bool> sessionSaved { false };
    std::atomic<bool> undoAvail { false }, redoAvail { false }, uneraseAvail { false };
    int aErasedLen = 0; // audio thread: length of the last erased loop, 0 = none

    // layer streaming
    static constexpr int kFifoFrames = 1 << 18; // ~5.5 s at 48 k — plenty for disk hiccups
    juce::AbstractFifo fifo { kFifoFrames };
    std::vector<float> fifoL = std::vector<float> (kFifoFrames, 0.0f);
    std::vector<float> fifoR = std::vector<float> (kFifoFrames, 0.0f);
    juce::File layersRoot;
    std::atomic<double> writerRate { 48000.0 };

    // writer-thread owned
    std::unique_ptr<juce::AudioFormatWriter> layerWriter;
    juce::File writerSessionDir;
    int writerLayerNum = 0;

    // shared with the message thread (currentSessionDir / saveSession)
    mutable juce::CriticalSection sessionDirLock;
    juce::File sessionDir;
    bool writerDirMoved = false; // guarded by sessionDirLock
};
} // namespace ampcade
