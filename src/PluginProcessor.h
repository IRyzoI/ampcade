#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include <array>

#include "BuiltinGear.h"
#include "FxPedals.h"
#include "IrLoader.h"
#include "Looper.h"
#include "NamSlot.h"
#include "Tuner.h"
#include "TiltEq.h"
#include "ToneStack.h"
#include "Tone3000Client.h"

namespace ampcade
{
class AmpCadeProcessor : public juce::AudioProcessor,
                         private juce::Timer,
                         private juce::AudioProcessorValueTreeState::Listener
{
public:
    AmpCadeProcessor();
    ~AmpCadeProcessor() override;

    //================================================================ engine
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    //=============================================================== plumbing
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "AmpCade"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; } // delay + reverb trails
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //================================================================= slots
    // Capture pedal slots. Empty ones cost a branch (NamSlot::process returns
    // false straight away) and stay invisible on the board until something is
    // loaded, so this is a ceiling, not a budget — raise it freely. The only
    // real prices are one automatable param set per slot (host parameter lists
    // are fixed at load, so they must all be declared up front) and the
    // resampler latency each LOADED capture adds.
    static constexpr int kNumPedals = 16;
    static const juce::StringArray& slotNames(); // p1..pN amp cab air

    struct SlotMeta
    {
        // `loaded` means the ENGINE actually holds this gear — slotsFromTree
        // clears it and the load path sets it. `wanted` is what the state we
        // are restoring asked for, which is a different question and the only
        // one worth asking before the file exists: a factory or shared preset
        // names gear by TONE3000 id and ships no path, so "should this slot
        // have something in it?" cannot be answered from `loaded`.
        bool loaded = false, wanted = false, missing = false;
        juce::String title, author, gear, modelName, filePath;
        int toneId = 0, modelId = 0;
    };

    // All of these run on the message thread (called by the editor bridge).
    void requestT3kLoad (const juce::String& slot, int toneId, int modelId,
                         std::function<void (juce::var)> done);
    // Copies the file into AmpCade's own folder first (see adoptGearFile), so
    // the rig never depends on a path outside it and presets stay pathless.
    void importLocalFile (const juce::String& slot, const juce::File& chosen);
    void clearSlotByName (const juce::String& slot);
    // Override the automatic cab-vs-space routing: move the IR currently in the
    // OTHER slot into `dest` ("cab" or "air"), no questions asked.
    bool moveIrTo (const juce::String& dest);
    juce::var getUiState();

    bool savePresetTo (const juce::File&, const juce::String& designsJson = {});
    bool loadPresetFrom (const juce::File&);
    // gear designs carried by the last loaded preset file (message thread;
    // the UI merges them into its own prefs — see getUiState)
    juce::String presetDesigns;
    // Rewrite ONLY the SCENES tree inside an existing preset file: scene saves
    // must stick without committing the rest of the current (possibly messy)
    // rig into the preset.
    bool syncScenesToPresetFile (const juce::File&);

    // Signal-chain order for every block: one id per BYTE, read through a
    // seqlock (see snapshotChainOrder). This used to be a uint64 with one id
    // per nibble, which capped the rig at 16 blocks total and was exactly
    // full — the byte array is what lets kNumPedals move. The mono core
    // (p1..pN, drv, cho, amp, cab) rearranges freely BEFORE the stereo tail;
    // dly/air/rev keep their relative order but the shape EQ and the three
    // clean boosts are "floaters" — they may sit absolutely anywhere,
    // including between or after the tail effects, where they process both
    // channels. setChainOrderFromNames enforces those invariants. "drv" is
    // the built-in Screamer drive, "cho" the built-in chorus (modulation
    // cannot be a NAM capture, so it has to live here).
    //
    // Pedals take ids 0..kNumPedals-1 and the built-ins follow, so these move
    // with kNumPedals. Nothing persists a raw id — presets and scenes store
    // the NAMES — so renumbering is invisible to saved rigs.
    static constexpr int kNumBuiltinBlocks = 12;
    static constexpr int kNumChainBlocks = kNumPedals + kNumBuiltinBlocks;
    static constexpr int kBlockAmp = kNumPedals + 0, kBlockCab = kNumPedals + 1,
                         kBlockDrive = kNumPedals + 2, kBlockChorus = kNumPedals + 3,
                         kBlockEq = kNumPedals + 4, kBlockBoost1 = kNumPedals + 5,
                         kBlockBoost2 = kNumPedals + 6, kBlockBoost3 = kNumPedals + 7,
                         kBlockDelay = kNumPedals + 8, kBlockAir = kNumPedals + 9,
                         kBlockReverb = kNumPedals + 10, kBlockComp = kNumPedals + 11;
    bool setChainOrderFromNames (const juce::StringArray&);
    juce::StringArray getChainOrderNames() const;
    // cmp b1 b2 b3 p1..pN drv cho amp cab eq dly air rev — squeeze + boosts at
    // the front where they classically live, shape EQ trimming the finished
    // rig, tail last. The "New preset" chain, and what a short saved order is
    // padded out to.
    static juce::StringArray defaultChainOrderNames();

    // Blocks the player tucked away in the backup rack: off the board, still
    // loaded, bypassed by their own toggles. Message thread only; persisted.
    juce::StringArray getHiddenSlots() const { return hiddenSlots; }
    void setHiddenSlots (const juce::StringArray&);

    //================================================================= scenes
    // One-tap snapshots of every parameter (knobs + stomps) AND the board
    // itself — gear in slots, rack contents, chain order. Recalling a scene
    // brings its rig back even if blocks were removed or racked meanwhile.
    // A dynamic list: players add/rename/delete their own, up to kMaxScenes.
    static constexpr int kMaxScenes = 8;
    struct Scene
    {
        juce::String name;
        bool used = false;
        std::map<juce::String, float> values; // paramID -> plain (scaled) value
        // Board snapshot. hasBoard=false on scenes saved before 0.9 — those
        // recall knobs only and leave the board alone.
        bool hasBoard = false;
        juce::String order;                   // chain order, space-joined
        juce::StringArray hidden;             // rack contents
        juce::ValueTree slots;                // SLOTS tree (what's loaded where)
    };
    void sceneSave (int idx);
    // withBoard=false recalls the scene's TONE only and leaves the live board
    // (order, rack, slots) alone — the standalone's launch landing, which must
    // not throw away the board the player left. See setStateInformation.
    void sceneRecall (int idx, bool withBoard = true);
    void sceneClear (int idx);
    void sceneDelete (int idx);
    void sceneAdd (const juce::String& name); // snapshots current settings
    void sceneRename (int idx, const juce::String& name);
    void sceneMove (int from, int to);        // drag-to-reorder in the topbar

    //================================================================= looper
    Looper looper;
    bool saveLoopTo (const juce::File& f) const { return looper.writeWav (f); }

    //=================================================================== undo
    // Whole-rig snapshot undo (knobs, stomps, gear, order, rack, scenes).
    // Param changes coalesce on a quiet-time timer; structural ops push
    // explicitly through beginUserAction() from the editor's native fns.
    void beginUserAction();
    bool undo();
    bool redo();

    // Factory-fresh rig: default params, empty slots, default board + scenes.
    // The "New preset" starting point.
    void resetRigToDefault();

    //================================================================== tuner
    Tuner tuner;

    //=========================================================== editor hooks
    juce::ChangeBroadcaster stateBroadcaster; // fire -> editor pushes stateChanged
    std::function<void (juce::String slot, bool busy, juce::String label)> onBusy;
    std::function<void (juce::String msg, juce::String kind)> onToast;

    // Linear. meterIn/meterOut hold a peak across the editor's whole polling
    // interval (the editor exchanges them for 0 when it reads), so a quiet
    // steady signal is visible instead of only whatever the last block caught.
    // meterInRms is the smoothed level the gate itself detects on.
    // meterOutPre is the peak BEFORE the soft ceiling — the readout shows how
    // much the clipper is actually shaving so the player can gain-stage to it.
    std::atomic<float> meterIn { 0.0f }, meterInRms { 0.0f }, meterOut { 0.0f },
                       meterOutPre { 0.0f };
    // Slow loudness memory of the player's own (pre-looper) output while they
    // actually play — imported loops level-match to this, so they sit against
    // the live rig and overdubs on top balance naturally. 0 = never played.
    std::atomic<float> liveLoudRms { 0.0f };

    // Do the current knobs differ from what the active scene has saved?
    // Cheap (~40 params); the editor polls it alongside the meters so the UI
    // can show "unsaved changes" on the active scene chip.
    bool isActiveSceneDirty() const;

    // Heartbeat stamped by every processBlock. 0 or stale = the audio device is
    // not running — the editor surfaces that instead of leaving a silent rig
    // that looks alive (which is how "the looper is broken" got reported when
    // the standalone's device had failed to start).
    std::atomic<juce::uint32> lastBlockMs { 0 };

    juce::AudioProcessorValueTreeState apvts;
    Tone3000Client t3k;

private:
    void timerCallback() override;
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    NamSlot* namSlotFor (const juce::String& slot); // nullptr for "cab"/"air"
    SlotMeta* metaFor (const juce::String& slot);
    // resetPedalKnobs: a NEW capture landing in a pedal slot starts at noon
    // (drive/level 0, tone 5, stomp on) instead of inheriting whatever the
    // previous pedal left behind. Only user-initiated loads pass true.
    void finishLoadIntoSlot (const juce::String& slot, const juce::File& file, SlotMeta newMeta,
                             bool resetPedalKnobs = false);
    void resetPedalParams (const juce::String& slot);
    // userInitiated=false on state/preset restore: the saved MIX is the player's
    // decision and must not be second-guessed by the auto-blend rules below.
    // `force` skips the cab-vs-space triage: the player said where it goes.
    void loadIrIntoCab (const juce::File& file, SlotMeta newMeta, bool userInitiated = true, bool force = false);
    // The stereo tail convolution for reverb/delay IRs — its own slot, so a
    // room and a speaker cab can run together. Each loader routes by the IR's
    // energy decay, so a reverb "loaded into the cab" lands here (and vice
    // versa) — unless forced.
    void loadIrIntoAir (const juce::File& file, SlotMeta newMeta, bool userInitiated = true, bool force = false);
    void clearCabIr();
    void clearAirIr();
    void setCabMix (float mix);
    void setParamPlain (const char* id, float plainValue);
    int blocksForSeconds (double seconds) const;
    void restoreSlotsFromState();
    // Where requestT3kLoad would have parked this gear:
    // <models|irs>/tone_<toneId>/<sanitised name>_<modelId>.<nam|wav>.
    // A preset that ships no file paths — every factory preset, and every
    // preset anyone shares — is resolvable offline whenever the player already
    // downloaded that capture once. Returns a non-existent File when it is
    // genuinely not on this machine.
    static juce::File cachedGearFile (const juce::String& slot, const SlotMeta& sm);
    void updateLatency();
    void notifyState();
    void beginStateBatch();
    void endStateBatch();
    void busy (const juce::String& slot, bool b, const juce::String& label = {});
    void toast (const juce::String& msg, const juce::String& kind = "error");

    juce::ValueTree slotsToTree() const;
    void slotsFromTree (const juce::ValueTree&);
    juce::ValueTree scenesToTree() const;
    void scenesFromTree (const juce::ValueTree&);
    void initScenes();
    void recallSceneBoard (const Scene&);

    // One rig = params + order + rack + slots + scenes + looper config. Shared
    // by session state, presets and undo snapshots. (Non-const only because
    // juce's copyState() isn't marked const.)
    juce::ValueTree rigToTree();
    // deferSlotLoad: hosts call setStateInformation in places where model
    // loading must not happen inline — everything else applies synchronously.
    void applyRigTree (const juce::ValueTree& root, bool deferSlotLoad);
    // Replaces non-finite parameter values with the parameter's own default.
    // JUCE clamps a restored value with jlimit, and jlimit passes NaN straight
    // through (both "NaN < lo" and "hi < NaN" are false), so a preset carrying
    // value="nan" — juce::CharacterFunctions::readDoubleValue parses that
    // spelling into a real quiet_NaN — reaches the DSP unclamped. From there it
    // latches in every recursive element and the rig is silent for good.
    void scrubNonFiniteParams (juce::ValueTree& params) const;

    void parameterChanged (const juce::String&, float) override; // undo dirt tracking
    void commitParamDirt();
    void pushUndo (juce::ValueTree);

    //================================================================== state
    std::map<juce::String, SlotMeta> meta; // keyed by slot name
    mutable juce::CriticalSection metaLock;
    // What each slot held before the last slotsFromTree — restoreSlotsFromState
    // skips reloading a file the engine already has (undo/preset/scene recalls
    // would otherwise re-download-and-reload identical gear).
    std::map<juce::String, juce::String> prevEnginePaths;

    // ---- undo ----
    static constexpr int kMaxUndo = 60;
    std::vector<juce::ValueTree> undoStack, redoStack;
    juce::ValueTree undoBaseline;              // rig as of the last commit point
    std::atomic<bool> paramDirty { false };
    std::atomic<juce::uint32> lastParamChangeMs { 0 };
    std::atomic<bool> restoringState { false }; // suppress dirt while we restore
    int notifySuspend = 0;       // message thread: >0 = batch state pushes
    bool notifyPending = false;
    // Fresh installs park the chorus, tuner, EQ, boosts + compressor in the
    // rack — the board starts simple.
    juce::StringArray hiddenSlots { "cho", "tun", "eq", "b1", "b2", "b3", "cmp" };
    std::vector<Scene> scenes;
    int activeScene = -1; // last recalled scene, or -1
    std::atomic<bool> cabLoaded { false };
    // juce::dsp::Convolution only swaps in a queued IR from inside process(), so
    // a slot that stops processing keeps convolving whatever it last had. After
    // clearing we run the convolution with its output discarded until the inert
    // IR has actually taken (see clearCabIr).
    std::atomic<int> cabFlushBlocks { 0 };
    std::atomic<bool> airLoaded { false };
    std::atomic<int> airFlushBlocks { 0 };

    //================================================================= engine
    NamSlot pedals[kNumPedals];
    NamSlot amp;
    juce::dsp::Convolution cab;
    juce::dsp::Convolution air; // reverb/delay IRs, stereo, in the tail
    ToneStack toneStack;
    ToneStack eqPedal;   // the built-in shape EQ chain block (left/mono...)
    ToneStack eqPedalR;  // ...and its right half when placed after the fan-out
    BoostPedal boosts[3];
    CompPedal compPedal;
    AirEq airEq;         // wet-only bass/treble on the AIR convolution
    TiltEq pedalTone[kNumPedals];
    juce::dsp::NoiseGate<float> gate;
    StereoDelay fxDelay;
    ReverbPedal fxReverb;
    ReverbPedal loopVerb; // the ring-out reverb, post-everything (see loopPlay)
    // Built-in gear: the drive pedal is its own chain block; the amp and cab
    // sims step in whenever their slot has no capture/IR loaded.
    ScreamerDrive drvPedal;
    ChorusPedal choPedal;
    TweedClean builtinAmp;
    TweedCab builtinCab;
    // ---- chain order (seqlock) ----
    // chainSlots[k] = block id at chain position k. Writers are message-thread
    // and rare (a drag, a preset/scene recall); the audio thread copies the
    // whole array once per block and retries only if a write landed mid-copy.
    // On a torn read it keeps chainLocal — last block's order runs for one
    // more block, which is inaudible and costs no lock and no allocation.
    // Odd chainSeq = a write in progress.
    std::atomic<juce::uint32> chainSeq { 0 };
    std::array<std::atomic<juce::uint8>, (size_t) kNumChainBlocks> chainSlots;
    juce::uint8 chainLocal[kNumChainBlocks] {}; // audio thread only
    void publishChainOrder (const juce::uint8* ids);   // message thread
    void snapshotChainOrder();                         // audio thread -> chainLocal

    juce::AudioBuffer<float> mono;
    juce::AudioBuffer<float> stereo;
    juce::AudioBuffer<float> cabDry; // dry copy around the convolution, for cab_mix
    juce::AudioBuffer<float> airDry; // stereo dry copy around the air convolution
    // Ring-out: the loop's playback goes through its OWN reverb (mirroring the
    // Space Verb's knobs), entirely AFTER the chain/out-gain/ceiling — the
    // loop's level belongs to loop_vol alone, scene and preset gains can never
    // touch it, and stopping leaves this reverb's tail ringing.
    juce::AudioBuffer<float> loopPlay;
    juce::SmoothedValue<float> inGain { 1.0f }, outGain { 1.0f };
    float dcPrevIn = 0.0f, dcPrevOut = 0.0f;
    // Which input channel carries the guitar (see processBlock): smoothed
    // per-channel level plus hysteresis, so it never flips mid-riff.
    float inLevelL = 0.0f, inLevelR = 0.0f;
    int inChannel = 0;
    float inRmsEnv = 0.0f; // what the gate's detector sees, for the input meter

    // cached raw params
    std::atomic<float>*p_in {}, *p_out {}, *p_gate {};
    struct PedalParams { std::atomic<float>*on {}, *drive {}, *level {}, *tone {}; } p_pedals[kNumPedals];
    std::atomic<float>*p_drvOn {}, *p_drvGain {}, *p_drvTone {}, *p_drvLevel {};
    std::atomic<float>*p_choOn {}, *p_choRate {}, *p_choDepth {}, *p_choMix {};
    std::atomic<float>*p_eqOn {}, *p_eqBass {}, *p_eqMid {}, *p_eqTreble {};
    struct BoostParams { std::atomic<float>*on {}, *gain {}; } p_boosts[3];
    std::atomic<float>*p_cmpOn {}, *p_cmpComp {}, *p_cmpAtt {}, *p_cmpLevel {};
    std::atomic<float>*p_tunOn {}, *p_tunMute {};
    juce::SmoothedValue<float> tunMuteGain { 1.0f };
    std::atomic<float>*p_ampOn {}, *p_ampGain {}, *p_ampBass {}, *p_ampMid {}, *p_ampTreble {}, *p_ampVol {};
    std::atomic<float>*p_cabOn {}, *p_cabMix {};
    std::atomic<float>*p_airOn {}, *p_airMix {}, *p_airLevel {}, *p_airBass {}, *p_airTreble {};
    std::atomic<float>*p_dlyOn {}, *p_dlyTime {}, *p_dlyFb {}, *p_dlyMix {}, *p_dlyBpm {}, *p_dlySync {};
    std::atomic<float>*p_revOn {}, *p_revSize {}, *p_revMix {}, *p_loopVol {}, *p_loopImp {};

    juce::ThreadPool loaderPool { juce::ThreadPoolOptions{}.withNumberOfThreads (1) };
    double currentSampleRate = 48000.0;
    // Every DSP object whose state is allocated in prepare() is used
    // unconditionally in processBlock, so a host (or a validator) that calls
    // processBlock outside the documented order dereferenced unallocated filter
    // state and took the host process down. Silence is the correct answer.
    std::atomic<bool> prepared { false };
    int currentBlockSize = 512;

public:
    JUCE_DECLARE_WEAK_REFERENCEABLE (AmpCadeProcessor)

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmpCadeProcessor)
};
} // namespace ampcade
