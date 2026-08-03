#include "PluginProcessor.h"
#ifndef AMPCADE_SMOKE
#include "PluginEditor.h" // the offline console tests link no UI (see CMakeLists)
#endif
#include "Library.h"
#include "Version.h"

namespace ampcade
{
static juce::String getProp (const juce::var& v, const char* key, const juce::String& fallback = {})
{
    return v.getProperty (key, fallback).toString();
}

// For string literals containing non-ASCII (…, —, ·): juce::String's plain
// char* constructor reads bytes as Latin-1, so a UTF-8 ellipsis rendered as
// "â€¦" in every busy label and toast. Route them through CharPointer_UTF8.
static juce::String u8 (const char* text)
{
    return juce::String (juce::CharPointer_UTF8 (text));
}

const juce::StringArray& AmpCadeProcessor::slotNames()
{
    static const juce::StringArray names = []
    {
        juce::StringArray n;
        for (int i = 1; i <= kNumPedals; ++i)
            n.add ("p" + juce::String (i));
        n.addArray ({ "amp", "cab", "air" });
        return n;
    }();
    return names;
}

juce::StringArray AmpCadeProcessor::defaultChainOrderNames()
{
    juce::StringArray n { "cmp", "b1", "b2", "b3" };
    for (int i = 1; i <= kNumPedals; ++i)
        n.add ("p" + juce::String (i));
    n.addArray ({ "drv", "cho", "amp", "cab", "eq", "dly", "air", "rev" });
    return n;
}

// Chain block id for a saved block name, or -1 if it names nothing we have.
// Ids are positional, never persisted — see the header.
static int blockIdForName (const juce::String& name)
{
    using P = ampcade::AmpCadeProcessor;
    if (name == "amp") return P::kBlockAmp;
    if (name == "cab") return P::kBlockCab;
    if (name == "drv") return P::kBlockDrive;
    if (name == "cho") return P::kBlockChorus;
    if (name == "eq")  return P::kBlockEq;
    if (name == "b1")  return P::kBlockBoost1;
    if (name == "b2")  return P::kBlockBoost2;
    if (name == "b3")  return P::kBlockBoost3;
    if (name == "cmp") return P::kBlockComp;
    if (name == "dly") return P::kBlockDelay;
    if (name == "air") return P::kBlockAir;
    if (name == "rev") return P::kBlockReverb;
    if (name.startsWith ("p"))
    {
        const int idx = name.getTrailingIntValue() - 1;
        if (idx >= 0 && idx < P::kNumPedals)
            return idx;
    }
    return -1;
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout AmpCadeProcessor::createLayout()
{
    using FloatParam = juce::AudioParameterFloat;
    using BoolParam = juce::AudioParameterBool;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    auto db = [] (float lo, float hi) { return juce::NormalisableRange<float> (lo, hi, 0.1f); };

    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "in_gain", 1 }, "Input", db (-24, 36), 0.0f,
                                              juce::AudioParameterFloatAttributes{}.withLabel ("dB")));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "out_gain", 1 }, "Output", db (-24, 24), 0.0f,
                                              juce::AudioParameterFloatAttributes{}.withLabel ("dB")));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "gate", 1 }, "Gate",
                                              juce::NormalisableRange<float> (-101.0f, -20.0f, 0.5f), -101.0f,
                                              juce::AudioParameterFloatAttributes{}.withLabel ("dB")));

    // Built-in Screamer drive: on the board from first launch, bypassed and
    // ready to kick on. Knobs mirror the capture-pedal set (drive/tone/level).
    layout.add (std::make_unique<BoolParam> (juce::ParameterID { "drv_on", 1 }, "Drive On", false));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "drv_gain", 1 }, "Drive Gain",
                                              juce::NormalisableRange<float> (0.0f, 10.0f, 0.1f), 5.0f));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "drv_tone", 1 }, "Drive Tone",
                                              juce::NormalisableRange<float> (0.0f, 10.0f, 0.1f), 5.0f));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "drv_level", 1 }, "Drive Level", db (-24, 24), 0.0f));

    // Built-in chorus: modulation cannot exist as a NAM capture (a capture is a
    // static waveshaper), so this is the only way the rig gets one. Starts in
    // the backup rack, bypassed.
    layout.add (std::make_unique<BoolParam> (juce::ParameterID { "cho_on", 1 }, "Chorus On", false));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "cho_rate", 1 }, "Chorus Rate",
                                              juce::NormalisableRange<float> (0.1f, 8.0f, 0.05f), 0.6f,
                                              juce::AudioParameterFloatAttributes{}.withLabel ("Hz")));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "cho_depth", 1 }, "Chorus Depth",
                                              juce::NormalisableRange<float> (0.0f, 10.0f, 0.1f), 5.0f));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "cho_mix", 1 }, "Chorus Mix",
                                              juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.5f));

    // Built-in tuner: taps the input, optionally mutes the output while you
    // tune (default on — that's what a tuner pedal is for). Starts in the rack.
    layout.add (std::make_unique<BoolParam> (juce::ParameterID { "tun_on", 1 }, "Tuner On", false));
    layout.add (std::make_unique<BoolParam> (juce::ParameterID { "tun_mute", 1 }, "Tuner Mute", true));

    for (int i = 1; i <= kNumPedals; ++i)
    {
        const auto p = "p" + juce::String (i);
        layout.add (std::make_unique<BoolParam> (juce::ParameterID { p + "_on", 1 }, "Pedal " + juce::String (i) + " On", true));
        layout.add (std::make_unique<FloatParam> (juce::ParameterID { p + "_drive", 1 }, "Pedal " + juce::String (i) + " Drive", db (-24, 24), 0.0f));
        layout.add (std::make_unique<FloatParam> (juce::ParameterID { p + "_tone", 1 }, "Pedal " + juce::String (i) + " Tone",
                                                 juce::NormalisableRange<float> (0.0f, 10.0f, 0.1f), 5.0f));
        layout.add (std::make_unique<FloatParam> (juce::ParameterID { p + "_level", 1 }, "Pedal " + juce::String (i) + " Level", db (-24, 24), 0.0f));
    }

    layout.add (std::make_unique<BoolParam> (juce::ParameterID { "amp_on", 1 }, "Amp On", true));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "amp_gain", 1 }, "Amp Gain", db (-24, 24), 0.0f));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "amp_bass", 1 }, "Bass", juce::NormalisableRange<float> (0.0f, 10.0f, 0.1f), 5.0f));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "amp_mid", 1 }, "Mid", juce::NormalisableRange<float> (0.0f, 10.0f, 0.1f), 5.0f));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "amp_treble", 1 }, "Treble", juce::NormalisableRange<float> (0.0f, 10.0f, 0.1f), 5.0f));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "amp_vol", 1 }, "Amp Volume", db (-24, 24), 0.0f));

    layout.add (std::make_unique<BoolParam> (juce::ParameterID { "cab_on", 1 }, "Cab On", true));
    // A cab IR is the speaker, so it runs 100% wet. A reverb/delay IR in the same
    // slot is an effect and needs a dry path or it is nothing but tail — see
    // loadIrIntoCab, which sets this for you based on how long the IR is.
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "cab_mix", 1 }, "Cab Mix",
                                              juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 1.0f));

    // The AIR slot: a second convolution for reverb/delay IRs, so a room and a
    // speaker cab can run at the same time (loading one used to evict the other).
    // Stereo, in the tail with the time effects. 30% default mix — a space IR at
    // 100% is nothing but tail.
    layout.add (std::make_unique<BoolParam> (juce::ParameterID { "air_on", 1 }, "Air On", true));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "air_mix", 1 }, "Air Mix",
                                              juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.3f));
    // Wet-only trim. 0 dB is "as loud as the dry rig" — the IR buffer is
    // normalised to unit energy on load (see IrLoader), so this knob starts at a
    // sane loudness for any IR instead of following whatever level the file
    // happened to be rendered at.
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "air_level", 1 }, "Air Level",
                                              db (-24, 12), 0.0f));
    // Wet-only bass/treble (see AirEq): tames IRs whose tail is all low-end
    // bloom without touching the dry rig. 5 = flat, like the amp's stack.
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "air_bass", 1 }, "Air Bass",
                                              juce::NormalisableRange<float> (0.0f, 10.0f, 0.1f), 5.0f));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "air_treble", 1 }, "Air Treble",
                                              juce::NormalisableRange<float> (0.0f, 10.0f, 0.1f), 5.0f));

    layout.add (std::make_unique<BoolParam> (juce::ParameterID { "dly_on", 1 }, "Delay On", false));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "dly_time", 1 }, "Delay Time",
                                              juce::NormalisableRange<float> (60.0f, 2000.0f, 1.0f), 380.0f,
                                              juce::AudioParameterFloatAttributes{}.withLabel ("ms")));
    // Tempo sync: SYNC picks a note division (0 = free, TIME in ms rules) and
    // BPM sets the grid. In a DAW the host's tempo wins over the BPM knob.
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "dly_bpm", 1 }, "Delay BPM",
                                              juce::NormalisableRange<float> (40.0f, 240.0f, 1.0f), 120.0f));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "dly_sync", 1 }, "Delay Sync",
                                              juce::NormalisableRange<float> (0.0f, 7.0f, 1.0f), 0.0f));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "dly_fb", 1 }, "Delay Feedback",
                                              juce::NormalisableRange<float> (0.0f, 0.9f, 0.01f), 0.35f));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "dly_mix", 1 }, "Delay Mix",
                                              juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.25f));

    // Three built-in clean boosts: one knob of level each, placeable anywhere
    // in the chain. Racked (and off) until the player pulls one out. The range
    // dips below zero so a "boost" can also duck — a volume pedal anywhere in
    // the chain (old 0..24 saves land unchanged inside the wider range).
    for (int i = 1; i <= 3; ++i)
    {
        const auto b = "b" + juce::String (i);
        layout.add (std::make_unique<BoolParam> (juce::ParameterID { b + "_on", 1 }, "Boost " + juce::String (i) + " On", false));
        layout.add (std::make_unique<FloatParam> (juce::ParameterID { b + "_gain", 1 }, "Boost " + juce::String (i) + " Gain",
                                                  juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 6.0f,
                                                  juce::AudioParameterFloatAttributes{}.withLabel ("dB")));
    }

    // The built-in compressor: COMP squeezes and makes up in one move,
    // ATTACK decides how much pick gets through, LEVEL trims. Racked + off.
    layout.add (std::make_unique<BoolParam> (juce::ParameterID { "cmp_on", 1 }, "Compressor On", false));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "cmp_comp", 1 }, "Compressor Amount",
                                              juce::NormalisableRange<float> (0.0f, 10.0f, 0.1f), 5.0f));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "cmp_att", 1 }, "Compressor Attack",
                                              juce::NormalisableRange<float> (0.0f, 10.0f, 0.1f), 3.0f));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "cmp_level", 1 }, "Compressor Level",
                                              db (-12, 12), 0.0f));

    // The built-in shape EQ: a rack tool for final tone shaping, off the board
    // by default. Same 3-band stack as the amp; 5 = flat on every knob.
    layout.add (std::make_unique<BoolParam> (juce::ParameterID { "eq_on", 1 }, "EQ On", true));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "eq_bass", 1 }, "EQ Bass",
                                              juce::NormalisableRange<float> (0.0f, 10.0f, 0.1f), 5.0f));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "eq_mid", 1 }, "EQ Mid",
                                              juce::NormalisableRange<float> (0.0f, 10.0f, 0.1f), 5.0f));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "eq_treble", 1 }, "EQ Treble",
                                              juce::NormalisableRange<float> (0.0f, 10.0f, 0.1f), 5.0f));

    // Loop playback volume: sits the loop under (or over) the live rig.
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "loop_vol", 1 }, "Loop Volume",
                                              db (-24, 12), 0.0f,
                                              juce::AudioParameterFloatAttributes{}.withLabel ("dB")));
    // Imported-bed volume: balances an imported loop against the player's own
    // overdub layers (which loop_vol governs). Only shown while a bed exists.
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "loop_imp", 1 }, "Loop Import Volume",
                                              db (-24, 12), 0.0f,
                                              juce::AudioParameterFloatAttributes{}.withLabel ("dB")));

    // A touch of reverb on by default: part of the plug-and-play clean rig.
    layout.add (std::make_unique<BoolParam> (juce::ParameterID { "rev_on", 1 }, "Reverb On", true));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "rev_size", 1 }, "Reverb Size",
                                              juce::NormalisableRange<float> (0.0f, 10.0f, 0.1f), 5.0f));
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { "rev_mix", 1 }, "Reverb Mix",
                                              juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.25f));

    return layout;
}

//==============================================================================
AmpCadeProcessor::AmpCadeProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    for (const auto& name : slotNames())
        meta[name] = {};

    p_in = apvts.getRawParameterValue ("in_gain");
    p_out = apvts.getRawParameterValue ("out_gain");
    p_gate = apvts.getRawParameterValue ("gate");
    for (int i = 0; i < kNumPedals; ++i)
    {
        const auto p = "p" + juce::String (i + 1);
        p_pedals[i].on = apvts.getRawParameterValue (p + "_on");
        p_pedals[i].drive = apvts.getRawParameterValue (p + "_drive");
        p_pedals[i].level = apvts.getRawParameterValue (p + "_level");
        p_pedals[i].tone = apvts.getRawParameterValue (p + "_tone");
    }
    p_drvOn = apvts.getRawParameterValue ("drv_on");
    p_drvGain = apvts.getRawParameterValue ("drv_gain");
    p_drvTone = apvts.getRawParameterValue ("drv_tone");
    p_drvLevel = apvts.getRawParameterValue ("drv_level");
    p_choOn = apvts.getRawParameterValue ("cho_on");
    p_choRate = apvts.getRawParameterValue ("cho_rate");
    p_choDepth = apvts.getRawParameterValue ("cho_depth");
    p_choMix = apvts.getRawParameterValue ("cho_mix");
    p_tunOn = apvts.getRawParameterValue ("tun_on");
    p_tunMute = apvts.getRawParameterValue ("tun_mute");
    p_ampOn = apvts.getRawParameterValue ("amp_on");
    p_ampGain = apvts.getRawParameterValue ("amp_gain");
    p_ampBass = apvts.getRawParameterValue ("amp_bass");
    p_ampMid = apvts.getRawParameterValue ("amp_mid");
    p_ampTreble = apvts.getRawParameterValue ("amp_treble");
    p_ampVol = apvts.getRawParameterValue ("amp_vol");
    p_cabOn = apvts.getRawParameterValue ("cab_on");
    p_cabMix = apvts.getRawParameterValue ("cab_mix");
    p_airOn = apvts.getRawParameterValue ("air_on");
    p_airMix = apvts.getRawParameterValue ("air_mix");
    p_airLevel = apvts.getRawParameterValue ("air_level");
    p_airBass = apvts.getRawParameterValue ("air_bass");
    p_airTreble = apvts.getRawParameterValue ("air_treble");
    p_eqOn = apvts.getRawParameterValue ("eq_on");
    p_eqBass = apvts.getRawParameterValue ("eq_bass");
    p_eqMid = apvts.getRawParameterValue ("eq_mid");
    p_eqTreble = apvts.getRawParameterValue ("eq_treble");
    for (int i = 0; i < 3; ++i)
    {
        const auto b = "b" + juce::String (i + 1);
        p_boosts[i].on = apvts.getRawParameterValue (b + "_on");
        p_boosts[i].gain = apvts.getRawParameterValue (b + "_gain");
    }
    p_cmpOn = apvts.getRawParameterValue ("cmp_on");
    p_cmpComp = apvts.getRawParameterValue ("cmp_comp");
    p_cmpAtt = apvts.getRawParameterValue ("cmp_att");
    p_cmpLevel = apvts.getRawParameterValue ("cmp_level");
    p_dlyOn = apvts.getRawParameterValue ("dly_on");
    p_dlyTime = apvts.getRawParameterValue ("dly_time");
    p_dlyFb = apvts.getRawParameterValue ("dly_fb");
    p_dlyMix = apvts.getRawParameterValue ("dly_mix");
    p_dlyBpm = apvts.getRawParameterValue ("dly_bpm");
    p_dlySync = apvts.getRawParameterValue ("dly_sync");
    p_revOn = apvts.getRawParameterValue ("rev_on");
    p_revSize = apvts.getRawParameterValue ("rev_size");
    p_revMix = apvts.getRawParameterValue ("rev_mix");
    p_loopVol = apvts.getRawParameterValue ("loop_vol");
    p_loopImp = apvts.getRawParameterValue ("loop_imp");

    // Seed the chain before anything can read it: chainSlots starts zeroed,
    // which would read as every position holding p1. Published directly rather
    // than through setChainOrderFromNames because that notifies state, and the
    // rig (scenes especially) is not built yet.
    {
        const auto names = defaultChainOrderNames();
        jassert (names.size() == kNumChainBlocks);
        juce::uint8 ids[kNumChainBlocks] = {};
        for (int k = 0; k < juce::jmin (names.size(), kNumChainBlocks); ++k)
            ids[k] = (juce::uint8) juce::jmax (0, blockIdForName (names[k]));
        publishChainOrder (ids);
        std::memcpy (chainLocal, ids, sizeof (chainLocal));
    }

    // The two factory presets (references to TONE3000 gear, no bundled
    // captures) land in the presets folder on first run.
    Library::installFactoryPresets();

    // Loop sessions are throwaways until saved — but never silently lost: the
    // newest leftover in Unsaved (a crash, a force-quit) is promoted to the
    // rolling "Last loop (auto-backup)" folder; older strays are deleted.
    // Age-guarded so a second running instance's live session survives.
    {
        auto loops = Library::appDataDir().getChildFile ("Loops");
        juce::Array<juce::File> stale;
        loops.getChildFile ("Unsaved").findChildFiles (stale, juce::File::findDirectories, false);
        juce::File newest;
        for (const auto& d : stale)
        {
            if (juce::Time::getCurrentTime() - d.getLastModificationTime() < juce::RelativeTime::hours (6))
                continue; // possibly another instance's live session
            if (newest == juce::File{})
            {
                newest = d;
            }
            else if (d.getLastModificationTime() > newest.getLastModificationTime())
            {
                newest.deleteRecursively();
                newest = d;
            }
            else
            {
                d.deleteRecursively();
            }
        }
        if (newest != juce::File{})
        {
            auto backup = loops.getChildFile (Looper::kBackupFolderName);
            if (! backup.exists() || newest.getLastModificationTime() > backup.getLastModificationTime())
            {
                backup.deleteRecursively();
                newest.moveFileTo (backup);
            }
            else
            {
                newest.deleteRecursively(); // the existing backup is newer
            }
        }
    }
    looper.setLayersRoot (Library::appDataDir().getChildFile ("Loops"));

    initScenes();

    // Undo: knob turns mark dirt here and coalesce on quiet time in
    // timerCallback; structural edits push explicitly via beginUserAction.
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            apvts.addParameterListener (rp->paramID, this);
    undoBaseline = rigToTree();

    startTimer (500);
}

AmpCadeProcessor::~AmpCadeProcessor()
{
    stopTimer();
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            apvts.removeParameterListener (rp->paramID, this);
    loaderPool.removeAllJobs (true, 5000);
}

void AmpCadeProcessor::timerCallback()
{
    for (auto& p : pedals)
        p.purgeGraveyard();
    amp.purgeGraveyard();

    // A burst of knob moves becomes ONE undo step once the knobs go quiet.
    if (paramDirty.load()
        && juce::Time::getMillisecondCounter() - lastParamChangeMs.load() > 900)
        commitParamDirt();
}

//==============================================================================
void AmpCadeProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize = samplesPerBlock;

    mono.setSize (1, samplesPerBlock, false, true);
    stereo.setSize (2, samplesPerBlock, false, true);
    cabDry.setSize (1, samplesPerBlock, false, true);
    airDry.setSize (2, samplesPerBlock, false, true);
    loopPlay.setSize (2, samplesPerBlock, false, true);

    for (auto& p : pedals)
        p.prepare (sampleRate, samplesPerBlock);
    amp.prepare (sampleRate, samplesPerBlock);

    fxDelay.prepare (sampleRate, samplesPerBlock);
    fxReverb.prepare (sampleRate, samplesPerBlock);
    loopVerb.prepare (sampleRate, samplesPerBlock);

    drvPedal.prepare (sampleRate, samplesPerBlock);
    choPedal.prepare (sampleRate, samplesPerBlock);
    builtinAmp.prepare (sampleRate, samplesPerBlock);
    builtinCab.prepare (sampleRate, samplesPerBlock);
    looper.prepare (sampleRate, samplesPerBlock);
    tuner.prepare (sampleRate, samplesPerBlock);
    tunMuteGain.reset (sampleRate, 0.03);
    tunMuteGain.setCurrentAndTargetValue (1.0f);

    toneStack.prepare (sampleRate, samplesPerBlock);
    eqPedal.prepare (sampleRate, samplesPerBlock);
    eqPedalR.prepare (sampleRate, samplesPerBlock);
    for (auto& b : boosts)
        b.prepare (sampleRate, samplesPerBlock);
    compPedal.prepare (sampleRate, samplesPerBlock);
    airEq.prepare (sampleRate, samplesPerBlock);
    for (auto& t : pedalTone)
        t.prepare (sampleRate, samplesPerBlock);

    const juce::dsp::ProcessSpec monoSpec { sampleRate, (juce::uint32) samplesPerBlock, 1 };
    gate.prepare (monoSpec);
    gate.setRatio (10.0f);
    gate.setAttack (1.5f);
    // The gate detects on an RMS envelope, so a decaying note sits well below its
    // own peak by the time it fades: closing in 120 ms clipped the tail off. This
    // is slow enough to ride a note out and still shut up between phrases.
    gate.setRelease (280.0f);

    cab.prepare (monoSpec);
    air.prepare (juce::dsp::ProcessSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 });

    inGain.reset (sampleRate, 0.02);
    outGain.reset (sampleRate, 0.02);
    dcPrevIn = dcPrevOut = 0.0f;
    inLevelL = inLevelR = 0.0f;
    inRmsEnv = 0.0f;

    updateLatency();
    prepared.store (true);
}

bool AmpCadeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo())
        return false;
    return true;
}

void AmpCadeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    lastBlockMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);

    const int n = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();

    if (n == 0 || numOut == 0)
        return;

    if (! prepared.load())
    {
        buffer.clear();
        return;
    }

    if (n > mono.getNumSamples())
        mono.setSize (1, n, false, true); // defensive; hosts shouldn't exceed prepare size

    auto* m = mono.getWritePointer (0);

    if (numIn >= 2)
    {
        // A guitar goes into ONE input of a 2-in interface, so averaging L+R threw
        // away 6 dB before the signal ever reached a capture — and 6 dB into a
        // nonlinear model is not just quieter, it is thinner and cleaner, which is
        // why a boost pedal in front felt mandatory. Follow whichever channel
        // carries signal instead. Slow smoothing plus 3 dB of hysteresis means it
        // settles once and never flips mid-riff; a dual-mono feed lands on either
        // channel at correct level, and a true stereo source picks one side (this
        // is a mono guitar rig, the captures are mono by construction).
        const auto* l = buffer.getReadPointer (0);
        const auto* r = buffer.getReadPointer (1);

        double sqL = 0.0, sqR = 0.0;
        for (int i = 0; i < n; ++i)
        {
            sqL += (double) l[i] * l[i];
            sqR += (double) r[i] * r[i];
        }

        const auto blockRms = [n] (double sq) { return (float) std::sqrt (sq / (double) juce::jmax (1, n)); };
        const float smooth = 0.15f;
        inLevelL += smooth * (blockRms (sqL) - inLevelL);
        inLevelR += smooth * (blockRms (sqR) - inLevelR);

        const float hysteresis = 1.413f; // 3 dB
        if (inChannel == 0 && inLevelR > inLevelL * hysteresis)
            inChannel = 1;
        else if (inChannel == 1 && inLevelL > inLevelR * hysteresis)
            inChannel = 0;

        juce::FloatVectorOperations::copy (m, inChannel == 1 ? r : l, n);
    }
    else if (numIn == 1)
    {
        juce::FloatVectorOperations::copy (m, buffer.getReadPointer (0), n);
    }
    else
    {
        juce::FloatVectorOperations::clear (m, n);
    }

    // Scrub what ARRIVES, not just what leaves. The output scrub at the end of
    // this function sits downstream of every recursive element — the DC blocker,
    // the delay's feedback, the reverb tail, the IIR tone stack — so it cleans
    // the outgoing block while the engine quietly keeps the NaN forever. One
    // non-finite sample from an upstream plugin (the ordinary "a plugin blew up"
    // event every other plugin shrugs off) silenced AmpCade for the rest of the
    // session, curable only by re-inserting it. Cleaning at the door is what
    // makes that recoverable: the bad block is lost, the next one plays.
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (m[i]))
            m[i] = 0.0f;

    inGain.setTargetValue (juce::Decibels::decibelsToGain (p_in->load()));
    inGain.applyGain (m, n);

    // Tuner taps here — post input gain, pre gate (the gate would eat the
    // quiet tail of the string you're trying to tune).
    if (p_tunOn->load() > 0.5f)
        tuner.push (m, n);
    else
        tuner.freq.store (0.0f);

    // Input metering. The peak is held until the editor reads it (a block peak
    // sampled a few times a second missed almost everything quiet), and the RMS
    // envelope is what the input tower draws, because that is the quantity the
    // gate compares its threshold against — the line and the bar now agree.
    {
        const float blockPeak = mono.getMagnitude (0, 0, n);
        float held = meterIn.load();
        while (blockPeak > held && ! meterIn.compare_exchange_weak (held, blockPeak)) {}

        double sq = 0.0;
        for (int i = 0; i < n; ++i)
            sq += (double) m[i] * m[i];
        const float rms = (float) std::sqrt (sq / (double) juce::jmax (1, n));
        inRmsEnv += (rms > inRmsEnv ? 0.5f : 0.06f) * (rms - inRmsEnv);
        meterInRms.store (inRmsEnv);
    }

    const float gateDb = p_gate->load();
    if (gateDb > -100.0f)
    {
        gate.setThreshold (gateDb);
        float* chans[1] = { m };
        auto block = juce::dsp::AudioBlock<float> (chans, 1, (size_t) n);
        gate.process (juce::dsp::ProcessContextReplacing<float> (block));
    }

    // One unified chain: mono core blocks in any order, then the stereo tail —
    // with the EQ and boosts free to sit anywhere, even between or after the
    // tail effects (they run on both channels once the chain has fanned out).
    // setChainOrderFromNames guarantees every mono core precedes the tail.
    if (n > stereo.getNumSamples())
        stereo.setSize (2, n, false, true);
    auto* sL = stereo.getWritePointer (0);
    auto* sR = stereo.getWritePointer (1);
    bool wide = false; // has the mono chain fanned out to stereo yet?
    const auto goWide = [&]
    {
        if (wide)
            return;
        wide = true;
        // Last gate before the stereo tail, and the one that matters: everything
        // downstream (delay feedback, reverb tail) remembers what it is given, so
        // a NaN out of a corrupt capture would latch there permanently. The whole
        // mono segment — every NAM slot and pedal — is upstream of this line.
        for (int i = 0; i < n; ++i)
            if (! std::isfinite (m[i]))
                m[i] = 0.0f;
        if (! std::isfinite (dcPrevIn) || ! std::isfinite (dcPrevOut))
            dcPrevIn = dcPrevOut = 0.0f;

        // DC blocker (~10 Hz one-pole high-pass) — some captures drift.
        // It runs exactly once, at the end of the mono segment.
        const float R = 1.0f - (float) (juce::MathConstants<double>::twoPi * 10.0 / currentSampleRate);
        float xm1 = dcPrevIn, ym1 = dcPrevOut;
        for (int i = 0; i < n; ++i)
        {
            const float x = m[i];
            const float y = x - xm1 + R * ym1;
            xm1 = x;
            ym1 = y;
            m[i] = y;
        }
        dcPrevIn = xm1;
        dcPrevOut = ym1;
        juce::FloatVectorOperations::copy (sL, m, n);
        juce::FloatVectorOperations::copy (sR, m, n);
    };

    snapshotChainOrder();
    for (int k = 0; k < kNumChainBlocks; ++k)
    {
        const int id = (int) chainLocal[k];

        if (id < kNumPedals)
        {
            if (p_pedals[id].on->load() > 0.5f)
            {
                // A pedal shapes tone without changing your level (measured: raw
                // captures ate 15-22 dB each, so stacking them buried the rig).
                if (pedals[id].process (m, n, p_pedals[id].drive->load(), p_pedals[id].level->load(),
                                        NamSlot::Makeup::unity))
                    pedalTone[id].process (m, n, p_pedals[id].tone->load());
            }
        }
        else if (id == kBlockDrive)
        {
            // Self-bypassing (short crossfade), so kicking it on never clicks.
            drvPedal.process (m, n, p_drvGain->load(), p_drvTone->load(), p_drvLevel->load(),
                              p_drvOn->load() > 0.5f);
        }
        else if (id == kBlockChorus)
        {
            choPedal.process (m, n, p_choRate->load(), p_choDepth->load(), p_choMix->load(),
                              p_choOn->load() > 0.5f);
        }
        else if (id == kBlockEq)
        {
            // Linear and instant — no crossfade needed; at flat knobs the
            // stack is numerically a straight wire. After the fan-out the
            // right channel gets its own filter bank.
            if (p_eqOn->load() > 0.5f)
            {
                if (! wide)
                {
                    eqPedal.process (m, n, p_eqBass->load(), p_eqMid->load(), p_eqTreble->load());
                }
                else
                {
                    eqPedal.process (sL, n, p_eqBass->load(), p_eqMid->load(), p_eqTreble->load());
                    eqPedalR.process (sR, n, p_eqBass->load(), p_eqMid->load(), p_eqTreble->load());
                }
            }
        }
        else if (id == kBlockComp)
        {
            if (! wide)
                compPedal.process (m, n, p_cmpComp->load(), p_cmpAtt->load(),
                                   p_cmpLevel->load(), p_cmpOn->load() > 0.5f);
            else
                compPedal.processStereo (sL, sR, n, p_cmpComp->load(), p_cmpAtt->load(),
                                         p_cmpLevel->load(), p_cmpOn->load() > 0.5f);
        }
        else if (id >= kBlockBoost1 && id <= kBlockBoost3)
        {
            auto& bp = p_boosts[id - kBlockBoost1];
            auto& b = boosts[id - kBlockBoost1];
            if (! wide)
                b.process (m, n, bp.gain->load(), bp.on->load() > 0.5f);
            else
                b.processStereo (sL, sR, n, bp.gain->load(), bp.on->load() > 0.5f);
        }
        else if (id == kBlockDelay)
        {
            goWide();
            // Delay time: free ms, or synced to a note division. The BPM knob
            // sets the grid; when a host supplies a tempo, the host wins.
            float dlyMs = p_dlyTime->load();
            if (const int div = (int) std::lround (p_dlySync->load()); div > 0)
            {
                double bpm = (double) p_dlyBpm->load();
                if (auto* playhead = getPlayHead())
                    if (auto pos = playhead->getPosition())
                        if (pos->getBpm().hasValue() && *pos->getBpm() > 20.0)
                            bpm = *pos->getBpm();
                // divisions 1..7: 1/2, dotted 1/4, 1/4, dotted 1/8, 1/8, 1/8 triplet, 1/16
                static constexpr double kBeats[] = { 1.0, 2.0, 1.5, 1.0, 0.75, 0.5, 1.0 / 3.0, 0.25 };
                dlyMs = (float) juce::jlimit (60.0, 2000.0, 60000.0 / bpm * kBeats[juce::jlimit (0, 7, div)]);
            }
            fxDelay.process (sL, sR, n, dlyMs, p_dlyFb->load(), p_dlyMix->load(),
                             p_dlyOn->load() > 0.5f);
        }
        else if (id == kBlockAir)
        {
            goWide();
            // AIR: the reverb/delay IR slot. Runs like the cab convolution
            // (including the drain-after-clear trick) but stereo, blended by
            // air_mix with wet-only LEVEL and BASS/TREBLE.
            const bool wet = p_airOn->load() > 0.5f && airLoaded.load() && air.getCurrentIRSize() > 0;
            const int flush = airFlushBlocks.load (std::memory_order_relaxed);

            if (wet || flush > 0)
            {
                if (n > airDry.getNumSamples())
                    airDry.setSize (2, n, false, false, true);

                auto* dryL = airDry.getWritePointer (0);
                auto* dryR = airDry.getWritePointer (1);
                juce::FloatVectorOperations::copy (dryL, sL, n);
                juce::FloatVectorOperations::copy (dryR, sR, n);

                float* chans[2] = { sL, sR };
                auto block = juce::dsp::AudioBlock<float> (chans, 2, (size_t) n);
                air.process (juce::dsp::ProcessContextReplacing<float> (block));

                if (wet)
                {
                    airEq.process (sL, sR, n, p_airBass->load(), p_airTreble->load());

                    const float mix = juce::jlimit (0.0f, 1.0f, p_airMix->load());
                    const float wetGain = mix * juce::Decibels::decibelsToGain (p_airLevel->load());
                    for (int i = 0; i < n; ++i)
                    {
                        sL[i] = wetGain * sL[i] + (1.0f - mix) * dryL[i];
                        sR[i] = wetGain * sR[i] + (1.0f - mix) * dryR[i];
                    }
                }
                else
                {
                    juce::FloatVectorOperations::copy (sL, dryL, n); // draining only
                    juce::FloatVectorOperations::copy (sR, dryR, n);
                }

                if (flush > 0)
                    airFlushBlocks.store (air.getCurrentIRSize() <= 1 ? 0 : flush - 1,
                                          std::memory_order_relaxed);
            }
        }
        else if (id == kBlockReverb)
        {
            goWide();
            fxReverb.process (sL, sR, n, p_revSize->load(), p_revMix->load(),
                              p_revOn->load() > 0.5f);
        }
        else if (id == kBlockAmp)
        {
            if (p_ampOn->load() > 0.5f)
            {
                // The amp is what sets the rig's level, so it keeps NAM's own
                // loudness normalization: swapping amps stays an A/B, not a surprise.
                // With nothing loaded the built-in tweed clean steps in, so the rig
                // makes a proper sound before anything has been downloaded.
                if (! amp.process (m, n, p_ampGain->load(), p_ampVol->load(), NamSlot::Makeup::loudness))
                    builtinAmp.process (m, n, p_ampGain->load(), p_ampVol->load());
                toneStack.process (m, n, p_ampBass->load(), p_ampMid->load(), p_ampTreble->load());
            }
        }
        else if (id == kBlockCab)
        {
            const bool wet = p_cabOn->load() > 0.5f && cabLoaded.load() && cab.getCurrentIRSize() > 0;
            const int flush = cabFlushBlocks.load (std::memory_order_relaxed);

            if (wet || flush > 0)
            {
                if (n > cabDry.getNumSamples())
                    cabDry.setSize (1, n, false, false, true);

                auto* dry = cabDry.getWritePointer (0);
                juce::FloatVectorOperations::copy (dry, m, n);

                float* chans[1] = { m };
                auto block = juce::dsp::AudioBlock<float> (chans, 1, (size_t) n);
                cab.process (juce::dsp::ProcessContextReplacing<float> (block));

                if (wet)
                {
                    // A cab sits at mix 1 and this is a straight wire; a reverb IR
                    // needs the dry path back or all you hear is the tail.
                    const float mix = juce::jlimit (0.0f, 1.0f, p_cabMix->load());
                    if (mix < 0.999f)
                        for (int i = 0; i < n; ++i)
                            m[i] = mix * m[i] + (1.0f - mix) * dry[i];
                }
                else
                {
                    juce::FloatVectorOperations::copy (m, dry, n); // draining only: discard
                }

                if (flush > 0)
                    cabFlushBlocks.store (cab.getCurrentIRSize() <= 1 ? 0 : flush - 1,
                                          std::memory_order_relaxed);
            }
            else if (p_cabOn->load() > 0.5f)
            {
                // No IR loaded: built-in 1x12 voicing, honouring cab_mix the same way.
                if (n > cabDry.getNumSamples())
                    cabDry.setSize (1, n, false, false, true);

                auto* dry = cabDry.getWritePointer (0);
                juce::FloatVectorOperations::copy (dry, m, n);

                builtinCab.process (m, n);

                const float mix = juce::jlimit (0.0f, 1.0f, p_cabMix->load());
                if (mix < 0.999f)
                    for (int i = 0; i < n; ++i)
                        m[i] = mix * m[i] + (1.0f - mix) * dry[i];
            }
        }
    }

    goWide(); // defensive — the migrated order always contains the tail

    outGain.setTargetValue (juce::Decibels::decibelsToGain (p_out->load()));
    outGain.applyGain (stereo, n);

    // Soft ceiling: with the wider gain ranges a hot capture can reach 0 dBFS,
    // and raw digital clipping sounds broken in a way players blame on the
    // plugin. Below the knee this is a straight wire; above it the curve
    // rounds off and asymptotes just under full scale.
    const auto softClip = [] (float x, float knee)
    {
        const float a = std::abs (x);
        if (a <= knee)
            return x;
        const float over = (a - knee) / (1.0f - knee);
        const float shaped = knee + (1.0f - knee) * std::tanh (over);
        return x < 0.0f ? -shaped : shaped;
    };

    // The -6 dBFS ceiling runs on the LIVE rig, BEFORE the looper: the loop
    // records this exact signal and must replay it at this exact level
    // forever. When the ceiling sat after the looper it compressed the
    // live+loop SUM, so the current scene's out_gain modulated how hard the
    // loop was being squashed — switching scenes audibly pumped the loop.
    float peakPre = 0.0f;
    double liveSum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        if (! std::isfinite (sL[i])) sL[i] = 0.0f;
        if (! std::isfinite (sR[i])) sR[i] = 0.0f;
        peakPre = juce::jmax (peakPre, std::abs (sL[i]), std::abs (sR[i]));
        liveSum += (double) sL[i] * sL[i] + (double) sR[i] * sR[i];
        sL[i] = softClip (sL[i], 0.5f);   // -6 dBFS knee
        sR[i] = softClip (sR[i], 0.5f);
    }
    float heldPre = meterOutPre.load();
    while (peakPre > heldPre && ! meterOutPre.compare_exchange_weak (heldPre, peakPre)) {}

    // Loudness memory (see PluginProcessor.h): active blocks only, so pauses
    // between phrases don't drag the estimate down.
    {
        const float blockRms = std::sqrt ((float) (liveSum / juce::jmax (1, n * 2)));
        if (blockRms > 1.0e-3f)   // ~-60 dBFS: the player is actually playing
        {
            const float cur = liveLoudRms.load (std::memory_order_relaxed);
            liveLoudRms.store (cur <= 0.0f ? blockRms : cur * 0.99f + blockRms * 0.01f,
                               std::memory_order_relaxed);
        }
    }

    looper.setPlayGain (juce::Decibels::decibelsToGain (p_loopVol->load()));
    looper.setBaseGain (juce::Decibels::decibelsToGain (p_loopImp->load()));

    // Ring-out mode, LEVEL-NEUTRAL by design: while the loop plays, its
    // playback goes out bit-direct (exactly the recorded signal — it already
    // contains the rig's reverb) and the side reverb is fed SILENTLY: wet
    // muted, tail accumulating. On stop the wet ramps in and the tail rings
    // out. Adding audible wet during play double-reverbed the loop and drove
    // the output overload guard into saturation — the "crunchy loop" bug.
    // Record/overdub stay on the direct path as always.
    if (looper.getRingOut())
    {
        if (n > loopPlay.getNumSamples())
            loopPlay.setSize (2, n, false, false, true);
        auto* lpL = loopPlay.getWritePointer (0);
        auto* lpR = loopPlay.getWritePointer (1);
        juce::FloatVectorOperations::clear (lpL, n);
        juce::FloatVectorOperations::clear (lpR, n);

        if (looper.getState() == Looper::Play)
        {
            looper.process (lpL, lpR, n);                    // pure playback
            juce::FloatVectorOperations::add (sL, lpL, n);   // direct — level-neutral
            juce::FloatVectorOperations::add (sR, lpR, n);
            // feed the verb, keep its wet muted; output discarded
            loopVerb.process (lpL, lpR, n, p_revSize->load(), p_revMix->load(), false);
        }
        else
        {
            looper.process (sL, sR, n);   // record/dub/idle: direct, as always
            // silence in, wet on: whatever tail the verb holds rings out
            loopVerb.process (lpL, lpR, n, p_revSize->load(), p_revMix->load(),
                              p_revOn->load() > 0.5f);
            juce::FloatVectorOperations::add (sL, lpL, n);
            juce::FloatVectorOperations::add (sR, lpR, n);
        }
    }
    else
    {
        looper.process (sL, sR, n);
    }

    // Tuner mute: silences everything (live and loop) with a short ramp while
    // you tune — exactly what a tuner pedal's mute does.
    {
        const bool muted = p_tunOn->load() > 0.5f && p_tunMute->load() > 0.5f;
        tunMuteGain.setTargetValue (muted ? 0.0f : 1.0f);
        if (muted || tunMuteGain.getCurrentValue() < 0.999f)
            for (int i = 0; i < n; ++i)
            {
                const float g = tunMuteGain.getNextValue();
                sL[i] *= g;
                sR[i] *= g;
            }
        else
            tunMuteGain.skip (n);
    }

    // Overload guard on the summed output: transparent until live + stacked
    // loop layers genuinely overflow (knee at -1 dBFS), so the loop's level
    // stays untouched in normal use.
    float peak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        sL[i] = softClip (sL[i], 0.891f);
        sR[i] = softClip (sR[i], 0.891f);
        peak = juce::jmax (peak, std::abs (sL[i]), std::abs (sR[i]));
    }
    float heldOut = meterOut.load(); // held until the editor reads it, like the input
    while (peak > heldOut && ! meterOut.compare_exchange_weak (heldOut, peak)) {}

    if (numOut == 1)
    {
        auto* out = buffer.getWritePointer (0);
        for (int i = 0; i < n; ++i)
            out[i] = 0.5f * (sL[i] + sR[i]);
    }
    else
    {
        juce::FloatVectorOperations::copy (buffer.getWritePointer (0), sL, n);
        juce::FloatVectorOperations::copy (buffer.getWritePointer (1), sR, n);
        for (int ch = 2; ch < numOut; ++ch)
            buffer.clear (ch, 0, n);
    }
}

//==============================================================================
NamSlot* AmpCadeProcessor::namSlotFor (const juce::String& slot)
{
    if (slot == "amp") return &amp;
    if (slot.startsWith ("p"))
    {
        const int idx = slot.getTrailingIntValue() - 1;
        if (idx >= 0 && idx < kNumPedals)
            return &pedals[idx];
    }
    return nullptr;
}

AmpCadeProcessor::SlotMeta* AmpCadeProcessor::metaFor (const juce::String& slot)
{
    auto it = meta.find (slot);
    return it != meta.end() ? &it->second : nullptr;
}

void AmpCadeProcessor::busy (const juce::String& slot, bool b, const juce::String& label)
{
    if (onBusy)
        onBusy (slot, b, label);
}

void AmpCadeProcessor::toast (const juce::String& msg, const juce::String& kind)
{
    if (onToast)
        onToast (msg, kind);
}

void AmpCadeProcessor::notifyState()
{
    // Every structural change lands here (message thread), which makes it the
    // natural place to refresh the undo baseline: whatever just happened was
    // either pushed by beginUserAction or is the tail end of an async load.
    if (! restoringState.load())
    {
        undoBaseline = rigToTree();
        paramDirty.store (false);
    }
    // Compound operations (scene recall, preset load) mutate order, rack and
    // slots in sequence; pushing each intermediate state made the board
    // re-render mid-operation — blocks visibly jumped around. Batched: one
    // render when the operation ends.
    if (notifySuspend > 0)
    {
        notifyPending = true;
        return;
    }
    stateBroadcaster.sendChangeMessage();
}

// Message thread. Suspends state pushes for the duration of a compound edit.
void AmpCadeProcessor::beginStateBatch()
{
    ++notifySuspend;
}

void AmpCadeProcessor::endStateBatch()
{
    if (--notifySuspend <= 0)
    {
        notifySuspend = 0;
        if (notifyPending)
        {
            notifyPending = false;
            notifyState();
        }
    }
}

void AmpCadeProcessor::updateLatency()
{
    int total = 0;
    for (auto& p : pedals)
        total += p.getLatency();
    total += amp.getLatency();
    setLatencySamples (total);
}

//==============================================================================
void AmpCadeProcessor::requestT3kLoad (const juce::String& slot, int toneId, int modelId,
                                       std::function<void (juce::var)> done)
{
    if (metaFor (slot) == nullptr)
    {
        done (errResult ("Unknown slot"));
        return;
    }

    // A DIFFERENT capture landing in a pedal or amp slot starts at defaults —
    // it must not inherit the outgoing gear's knobs. Swapping settings of the
    // same tone keeps the knobs (you're dialing that gear in, not replacing it).
    bool resetKnobs = false;
    if (slot.startsWith ("p") || slot == "amp")
    {
        const juce::ScopedLock l (metaLock);
        if (auto* mp = metaFor (slot))
            resetKnobs = ! mp->loaded || mp->toneId != toneId;
    }

    busy (slot, true, u8 ("Fetching…"));

    juce::WeakReference<AmpCadeProcessor> safe (this);

    t3k.tone (toneId, [safe, slot, toneId, modelId, done, resetKnobs] (juce::var tr)
    {
        auto* self = safe.get();
        if (self == nullptr)
            return;

        if (! (bool) tr.getProperty ("ok", false))
        {
            self->busy (slot, false);
            self->toast (getProp (tr, "error", "Couldn't fetch tone info"));
            done (tr);
            return;
        }

        const auto title = getProp (tr, "title");
        const auto author = getProp (tr, "author");
        const auto gear = getProp (tr, "gear");
        const auto format = getProp (tr, "format");

        // Classify on FORMAT alone, never on gear. A "space" tone (reverb/delay gear,
        // e.g. a Hall of Fame) exists as both an IR and a NAM capture: the IR belongs
        // in the convolution slot and the NAM capture belongs in a pedal slot. Keying
        // off gear sent every space capture to the Cab slot, where the cab search
        // filter (gears=cab) then could not find it at all.
        // BOTH convolution slots take IRs — "air" was missing here, which rejected
        // every reverb/delay IR with "load it into the Cab · IR slot".
        const bool wantIr = (slot == "cab" || slot == "air");
        const bool gotIr = (format == "ir");
        if (wantIr != gotIr)
        {
            self->busy (slot, false);
            const auto msg = wantIr ? u8 ("That's a NAM capture — load it into a pedal or amp slot")
                                    : u8 ("That's an IR — load it into the Cab · IR or Air · IR slot");
            self->toast (msg);
            done (errResult (msg));
            return;
        }

        self->t3k.models (toneId, [safe, slot, toneId, modelId, title, author, gear, done, resetKnobs] (juce::var mr)
        {
            auto* self = safe.get();
            if (self == nullptr)
                return;

            if (! (bool) mr.getProperty ("ok", false))
            {
                self->busy (slot, false);
                self->toast (getProp (mr, "error", "Couldn't fetch capture settings"));
                done (mr);
                return;
            }

            juce::var chosen;
            if (auto* models = mr.getProperty ("models", juce::var()).getArray())
            {
                for (const auto& mv : *models)
                    if ((int) mv.getProperty ("id", 0) == modelId)
                        chosen = mv;
                if (chosen.isVoid() && ! models->isEmpty())
                    chosen = models->getFirst();
            }

            if (chosen.isVoid())
            {
                self->busy (slot, false);
                self->toast ("This tone has no downloadable files");
                done (errResult ("No models on tone"));
                return;
            }

            const auto modelUrl = getProp (chosen, "model_url");
            const auto modelName = getProp (chosen, "name", "capture");
            const int chosenId = (int) chosen.getProperty ("id", modelId);

            const bool isIr = (slot == "cab" || slot == "air");
            const auto ext = isIr ? ".wav" : ".nam";
            auto destDir = (isIr ? Library::irsDir() : Library::modelsDir()).getChildFile ("tone_" + juce::String (toneId));
            destDir.createDirectory();
            auto dest = destDir.getChildFile (Library::sanitizeFileName (modelName) + "_" + juce::String (chosenId) + ext);

            SlotMeta nm;
            nm.loaded = true;
            nm.title = title;
            nm.author = author;
            nm.gear = gear;
            nm.modelName = modelName;
            nm.filePath = dest.getFullPathName();
            nm.toneId = toneId;
            nm.modelId = chosenId;

            auto proceed = [safe, slot, dest, nm, done, resetKnobs]
            {
                auto* self = safe.get();
                if (self == nullptr)
                    return;

                if (slot == "cab")
                    self->loadIrIntoCab (dest, nm);
                else if (slot == "air")
                    self->loadIrIntoAir (dest, nm);
                else
                    self->finishLoadIntoSlot (slot, dest, nm, resetKnobs);
                done (okResult());
            };

            if (dest.existsAsFile() && dest.getSize() > 16)
            {
                proceed();
                return;
            }

            self->busy (slot, true, u8 ("Downloading…"));
            self->t3k.download (modelUrl, dest, [safe, slot, proceed, done] (bool ok, juce::String err)
            {
                auto* self = safe.get();
                if (self == nullptr)
                    return;

                if (! ok)
                {
                    self->busy (slot, false);
                    self->toast (err.isNotEmpty() ? err : "Download failed");
                    done (errResult (err));
                    return;
                }
                proceed();
            });
        });
    });
}

void AmpCadeProcessor::finishLoadIntoSlot (const juce::String& slot, const juce::File& file, SlotMeta newMeta,
                                           bool resetPedalKnobs)
{
    auto* target = namSlotFor (slot);
    if (target == nullptr)
        return;

    busy (slot, true, u8 ("Loading…"));

    juce::WeakReference<AmpCadeProcessor> safe (this);

    loaderPool.addJob ([target, safe, slot, file, newMeta, resetPedalKnobs]
    {
        // loaderPool is joined in ~AmpCadeProcessor, so `target` stays valid here
        const auto err = target->load (file);

        juce::MessageManager::callAsync ([safe, slot, newMeta, err, resetPedalKnobs]
        {
            auto* self = safe.get();
            if (self == nullptr)
                return;

            self->busy (slot, false);

            if (err.isNotEmpty())
            {
                self->toast (err);
                self->notifyState();
                return;
            }

            {
                const juce::ScopedLock l (self->metaLock);
                if (auto* mp = self->metaFor (slot))
                    *mp = newMeta;
            }
            if (resetPedalKnobs)
                self->resetPedalParams (slot);
            self->updateLatency();
            self->notifyState();
        });
    });
}

// Freshly-picked gear starts at its own defaults, never the outgoing gear's
// knobs: a pedal at noon and switched on; a NEW amp at flat EQ and unity —
// the boosted volume you dialed for the old Fender must not ride in on the
// Marshall that replaces it.
void AmpCadeProcessor::resetPedalParams (const juce::String& slot)
{
    if (slot == "amp")
    {
        setParamPlain ("amp_gain", 0.0f);
        setParamPlain ("amp_bass", 5.0f);
        setParamPlain ("amp_mid", 5.0f);
        setParamPlain ("amp_treble", 5.0f);
        setParamPlain ("amp_vol", 0.0f);
        setParamPlain ("amp_on", 1.0f);
        return;
    }
    if (! slot.startsWith ("p"))
        return;
    setParamPlain ((slot + "_drive").toRawUTF8(), 0.0f);
    setParamPlain ((slot + "_tone").toRawUTF8(), 5.0f);
    setParamPlain ((slot + "_level").toRawUTF8(), 0.0f);
    setParamPlain ((slot + "_on").toRawUTF8(), 1.0f);
}

int AmpCadeProcessor::blocksForSeconds (double seconds) const
{
    const double blocks = seconds * currentSampleRate / (double) juce::jmax (1, currentBlockSize);
    return juce::jlimit (8, 4000, (int) std::ceil (blocks));
}

void AmpCadeProcessor::setParamPlain (const char* id, float plainValue)
{
    if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (id)))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
        p->endChangeGesture();
    }
}

void AmpCadeProcessor::setCabMix (float mix)
{
    setParamPlain ("cab_mix", juce::jlimit (0.0f, 1.0f, mix));
}

// Installs an inert one-sample IR and keeps the convolution running (output
// thrown away) until it has taken. juce::dsp::Convolution only swaps a queued IR
// from inside process(), so simply flipping cabLoaded off left the old IR in the
// engine: it came back the moment anything ran the block again, and a preset that
// had no cab never displaced it. Draining it here means "removed" means removed.
void AmpCadeProcessor::clearCabIr()
{
    juce::AudioBuffer<float> unit (1, 1);
    unit.setSample (0, 0, 1.0f);

    cab.loadImpulseResponse (std::move (unit), currentSampleRate,
                             juce::dsp::Convolution::Stereo::no,
                             juce::dsp::Convolution::Trim::no,
                             juce::dsp::Convolution::Normalise::no);
    cabLoaded.store (false);
    cabFlushBlocks.store (blocksForSeconds (2.0));
}

// Same drain trick as clearCabIr, for the air convolution.
void AmpCadeProcessor::clearAirIr()
{
    juce::AudioBuffer<float> unit (1, 1);
    unit.setSample (0, 0, 1.0f);

    air.loadImpulseResponse (std::move (unit), currentSampleRate,
                             juce::dsp::Convolution::Stereo::no,
                             juce::dsp::Convolution::Trim::no,
                             juce::dsp::Convolution::Normalise::no);
    airLoaded.store (false);
    airFlushBlocks.store (blocksForSeconds (4.0)); // a space tail rings longer than a cab
}

void AmpCadeProcessor::loadIrIntoCab (const juce::File& file, SlotMeta newMeta, bool userInitiated, bool force)
{
    // A reverb/delay IR is an effect, not a speaker — it gets its own slot now,
    // so it no longer evicts the cab. This branch also migrates old sessions
    // that stored a long IR in the cab slot. Routing is by energy decay, not
    // file length (padded cab IRs are long files with all their energy up
    // front), and `force` skips it entirely — the player's word is final.
    // Always analyse, even when `force` skips the routing decision: this is also
    // where a file that would poison the convolution gets refused.
    const auto info = ir::analyze (file);
    if (! info.valid)
    {
        busy ("cab", false);
        toast ("That IR file couldn't be read — it may be corrupt or an unsupported format.");
        return;
    }
    if (! force && ir::isSpace (info))
    {
        {
            const juce::ScopedLock l (metaLock);
            if (auto* mp = metaFor ("cab"))
                if (mp->filePath == newMeta.filePath)
                    *mp = {}; // moving out of the cab slot, not duplicating into both
        }
        // An old session blended the space IR with cab_mix; that intent moves to
        // air_mix and the cab goes back to being a speaker at 100%.
        if (! userInitiated && p_cabMix != nullptr && p_cabMix->load() < 0.99f)
        {
            setParamPlain ("air_mix", p_cabMix->load());
            setCabMix (1.0f);
        }
        busy ("cab", false);
        if (userInitiated)
            toast (u8 ("Long reverb-style IR — loaded into the AIR slot. "
                       "Use \"Move to CAB\" in its panel if it’s meant as a speaker."), "ok");
        loadIrIntoAir (file, std::move (newMeta), userInitiated);
        return;
    }

    // Cab-length IR: energy normalisation is exactly right here, and keeps
    // swapping speakers from being a volume change.
    cab.loadImpulseResponse (file,
                             juce::dsp::Convolution::Stereo::no,
                             juce::dsp::Convolution::Trim::yes,
                             0,
                             juce::dsp::Convolution::Normalise::yes);
    cabLoaded.store (true);
    cabFlushBlocks.store (0);

    // Only when the player just picked this IR — restoring a session must leave
    // their saved MIX exactly where they left it.
    if (userInitiated && p_cabMix != nullptr && p_cabMix->load() < 0.99f)
        setCabMix (1.0f); // a speaker IR blended with dry is a comb filter, never a feature

    {
        const juce::ScopedLock l (metaLock);
        if (auto* mp = metaFor ("cab"))
            *mp = newMeta;
    }
    busy ("cab", false);

    // Front-loaded spring/plate IRs can classify as speakers (99% of a surf
    // spring's energy fits in ~130 ms). When the tail is long enough to be
    // suspicious, point at the override instead of guessing wrong silently.
    if (userInitiated && ! force && info.t99 > 0.05)
        toast ("Loaded into the CAB slot. Use \"Move to AIR\" in its panel if it's a reverb.", "ok");

    notifyState();
}

void AmpCadeProcessor::loadIrIntoAir (const juce::File& file, SlotMeta newMeta, bool userInitiated, bool force)
{
    const auto airInfo = ir::analyze (file);
    if (! airInfo.valid)
    {
        busy ("air", false);
        toast ("That IR file couldn't be read — it may be corrupt or an unsupported format.");
        return;
    }

    // Symmetric routing: an IR whose energy decays like a speaker is a cab.
    if (! force && ! ir::isSpace (airInfo))
    {
        busy ("air", false);
        if (userInitiated)
            toast (u8 ("Speaker-length IR — loaded into the CAB slot. "
                       "Use \"Move to AIR\" in its panel if it’s meant as a space."), "ok");
        loadIrIntoCab (file, std::move (newMeta), userInitiated);
        return;
    }

    juce::AudioBuffer<float> spaceIr;
    double spaceSr = currentSampleRate;

    if (ir::readSpace (file, spaceIr, spaceSr, ir::kMaxSpaceSeconds, true))
    {
        const bool stereo = spaceIr.getNumChannels() > 1;
        air.loadImpulseResponse (std::move (spaceIr), spaceSr,
                                 stereo ? juce::dsp::Convolution::Stereo::yes
                                        : juce::dsp::Convolution::Stereo::no,
                                 juce::dsp::Convolution::Trim::no,
                                 juce::dsp::Convolution::Normalise::no);
    }
    else
    {
        // Fallback for files our reader can't open. Normalise::yes is JUCE's
        // energy normalisation (−12 dB of ours) — quieter than ideal, but a raw
        // un-normalised reverb tail is +20..30 dB hot, which is the speaker-
        // blowing failure this slot exists to avoid.
        air.loadImpulseResponse (file,
                                 juce::dsp::Convolution::Stereo::yes,
                                 juce::dsp::Convolution::Trim::no,
                                 0,
                                 juce::dsp::Convolution::Normalise::yes);
    }
    airLoaded.store (true);
    airFlushBlocks.store (0);

    // A DIFFERENT space starts at default knobs — the bass cut and level you
    // dialed to tame the last boomy IR must not colour the new one. Re-picking
    // the same IR (or a setting of it) keeps everything; session/preset
    // restores never touch the saved knobs.
    bool freshIr = false;
    {
        const juce::ScopedLock l (metaLock);
        if (auto* mp = metaFor ("air"))
            freshIr = ! mp->loaded || mp->filePath != newMeta.filePath;
    }
    if (userInitiated && freshIr)
    {
        setParamPlain ("air_mix", 0.3f);
        setParamPlain ("air_level", 0.0f);
        setParamPlain ("air_bass", 5.0f);
        setParamPlain ("air_treble", 5.0f);
    }
    // 100% wet space is nothing but tail; nudge even a same-IR re-pick to a
    // sane blend, but never second-guess a saved session's mix.
    else if (userInitiated && p_airMix != nullptr && p_airMix->load() > 0.99f)
        setParamPlain ("air_mix", 0.3f);

    {
        const juce::ScopedLock l (metaLock);
        if (auto* mp = metaFor ("air"))
            *mp = newMeta;
    }
    busy ("air", false);
    notifyState();
}

// Gear picked from anywhere on disk is copied into AmpCade's own folder before
// anything else happens. Two reasons: a preset can then store a RELATIVE path,
// so sharing one never ships the player's home directory, and the rig stops
// depending on a file they may later move, rename or delete. A file already
// inside the folder (re-picking a downloaded capture) is used where it sits.
static juce::File adoptGearFile (const juce::String& slot, const juce::File& src)
{
    if (src.isAChildOf (Library::appDataDir()))
        return src;

    const bool isIr = slot == "cab" || slot == "air";
    const auto dir = (isIr ? Library::irsDir() : Library::modelsDir()).getChildFile ("imported");
    dir.createDirectory();

    const auto stem = Library::sanitizeFileName (src.getFileNameWithoutExtension());
    const auto ext = src.getFileExtension();
    auto dest = dir.getChildFile (stem + ext);
    // Same name and same bytes: reuse it. Same name, different capture: keep both.
    for (int n = 2; dest.existsAsFile() && ! dest.hasIdenticalContentTo (src); ++n)
        dest = dir.getChildFile (stem + "_" + juce::String (n) + ext);

    if (dest.existsAsFile() || src.copyFileTo (dest))
        return dest;
    return src; // out of disk or read-only: play it where it lies rather than fail
}

void AmpCadeProcessor::importLocalFile (const juce::String& slot, const juce::File& chosen)
{
    const auto file = adoptGearFile (slot, chosen);

    SlotMeta nm;
    nm.loaded = true;
    nm.title = chosen.getFileNameWithoutExtension();
    nm.author = "Imported";
    nm.gear = slot == "cab" ? "cab" : (slot == "air" ? "space" : (slot == "amp" ? "amp" : "pedal"));
    nm.modelName = chosen.getFileName();
    nm.filePath = file.getFullPathName();

    bool resetKnobs = false;
    if (slot.startsWith ("p") || slot == "amp")
    {
        const juce::ScopedLock l (metaLock);
        if (auto* mp = metaFor (slot))
            resetKnobs = ! mp->loaded || mp->filePath != nm.filePath;
    }

    if (slot == "cab")
        loadIrIntoCab (file, nm);
    else if (slot == "air")
        loadIrIntoAir (file, nm);
    else
        finishLoadIntoSlot (slot, file, nm, resetKnobs);
}

bool AmpCadeProcessor::moveIrTo (const juce::String& dest)
{
    if (dest != "cab" && dest != "air")
        return false;
    const auto src = dest == "cab" ? juce::String ("air") : juce::String ("cab");

    SlotMeta sm;
    {
        const juce::ScopedLock l (metaLock);
        if (auto* mp = metaFor (src))
            sm = *mp;
    }
    if (! sm.loaded || sm.filePath.isEmpty() || ! juce::File (sm.filePath).existsAsFile())
        return false;

    // Empty the source (engine + meta) first, then force-load the destination.
    clearSlotByName (src);
    if (dest == "cab")
        loadIrIntoCab (juce::File (sm.filePath), sm, true, true);
    else
        loadIrIntoAir (juce::File (sm.filePath), sm, true, true);
    return true;
}

void AmpCadeProcessor::clearSlotByName (const juce::String& slot)
{
    if (slot == "cab")
    {
        clearCabIr();
    }
    else if (slot == "air")
    {
        clearAirIr();
    }
    else if (auto* s = namSlotFor (slot))
    {
        s->clear();
    }

    {
        const juce::ScopedLock l (metaLock);
        if (auto* mp = metaFor (slot))
            *mp = {};
    }
    updateLatency();
    notifyState();
}

//==============================================================================
// Accepts a permutation of every block name in any arrangement. Older lists are
// upgraded: a four-name pre-0.3 pedals-only order gains amp and cab, the
// built-in drive (pre-0.7) and chorus (pre-0.8) join in their default spot just
// before the amp, and an order saved when the rig had fewer pedal slots gains
// the new ones — so old presets and sessions restore.
bool AmpCadeProcessor::setChainOrderFromNames (const juce::StringArray& namesIn)
{
    auto names = namesIn;
    // A pre-0.3 order was four pedal names and nothing else. Tested on content,
    // not on size: once kNumPedals passed 4, a plain size check started
    // misreading current-shape orders as ancient ones.
    if (names.size() == 4 && ! names.contains ("amp") && ! names.contains ("cab"))
    {
        names.add ("amp");
        names.add ("cab");
    }
    // a six-name pre-0.7 list lacks drv, a seven-name pre-0.8 list lacks cho
    for (auto* builtin : { "drv", "cho" })
        if (names.size() < kNumChainBlocks && ! names.contains (builtin))
        {
            const int ampAt = names.indexOf ("amp");
            names.insert (ampAt >= 0 ? ampAt : names.size(), builtin);
        }
    // an order saved before the shape EQ existed lacks it — it goes to the
    // end of the mono segment, the "final tone shaping" position it ships in
    if (! names.contains ("eq"))
        names.add ("eq");
    // the clean boosts + compressor land at the chain front (their classic
    // spot; racked and off by default, so a migrated rig sounds identical)
    for (auto* b : { "b3", "b2", "b1", "cmp" })
        if (! names.contains (b))
            names.insert (0, b);
    // the stereo tail joined the orderable chain in this shape; orders saved
    // before that lack the three names — they append in their fixed order
    for (auto* t : { "dly", "air", "rev" })
        if (! names.contains (t))
            names.add (t);
    // Pedal slots added since this order was saved fall in right behind the
    // last pedal that IS in it — the classic run of pedals stays contiguous,
    // and because the new slots are empty the migrated rig sounds identical.
    for (int i = 1; i <= kNumPedals; ++i)
    {
        const auto p = "p" + juce::String (i);
        if (names.contains (p))
            continue;
        int after = -1;
        for (int k = 0; k < names.size(); ++k)
            if (names[k].startsWith ("p") && names[k].getTrailingIntValue() > 0)
                after = k;
        const int ampAt = names.indexOf ("amp");
        names.insert (after >= 0 ? after + 1 : (ampAt >= 0 ? ampAt : names.size()), p);
    }

    if (names.size() != kNumChainBlocks)
        return false;

    const auto idOf = [] (const juce::String& name) { return blockIdForName (name); };

    juce::uint8 ids[kNumChainBlocks] = {};
    bool seen[kNumChainBlocks] = {};
    int pos[kNumChainBlocks] = {};

    for (int k = 0; k < kNumChainBlocks; ++k)
    {
        const int id = idOf (names[k]);
        if (id < 0 || seen[id])
            return false;
        seen[id] = true;
        pos[id] = k;
        ids[k] = (juce::uint8) id;
    }

    // Invariants the engine depends on: the tail keeps its relative order,
    // and every mono core block precedes it (only the EQ and the boosts may
    // float into the stereo region).
    if (! (pos[kBlockDelay] < pos[kBlockAir] && pos[kBlockAir] < pos[kBlockReverb]))
        return false;
    for (int p = 0; p < kNumPedals; ++p)
        if (pos[p] > pos[kBlockDelay])
            return false;
    for (int core : { kBlockAmp, kBlockCab, kBlockDrive, kBlockChorus })
        if (pos[core] > pos[kBlockDelay])
            return false;

    publishChainOrder (ids);
    notifyState();
    return true;
}

// Message thread. Seqlock writer: bump to odd, write, bump to even. The audio
// thread only ever accepts a copy taken between two equal even reads.
void AmpCadeProcessor::publishChainOrder (const juce::uint8* ids)
{
    const auto seq = chainSeq.load (std::memory_order_relaxed);
    chainSeq.store (seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_release);
    for (int k = 0; k < kNumChainBlocks; ++k)
        chainSlots[(size_t) k].store (ids[k], std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_release);
    chainSeq.store (seq + 2, std::memory_order_release);
}

// Audio thread. Refreshes chainLocal, or leaves it alone when a write is in
// flight — one extra block on the previous order is inaudible, and it keeps
// this wait-free.
void AmpCadeProcessor::snapshotChainOrder()
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const auto before = chainSeq.load (std::memory_order_acquire);
        if ((before & 1u) != 0u)
            continue; // mid-write
        juce::uint8 tmp[kNumChainBlocks];
        for (int k = 0; k < kNumChainBlocks; ++k)
            tmp[k] = chainSlots[(size_t) k].load (std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_acquire);
        if (chainSeq.load (std::memory_order_relaxed) == before)
        {
            std::memcpy (chainLocal, tmp, sizeof (chainLocal));
            return;
        }
    }
}

juce::StringArray AmpCadeProcessor::getChainOrderNames() const
{
    juce::StringArray names;
    for (int k = 0; k < kNumChainBlocks; ++k)
    {
        const int id = (int) chainSlots[(size_t) k].load (std::memory_order_relaxed);
        names.add (id == kBlockAmp    ? juce::String ("amp")
                 : id == kBlockCab    ? juce::String ("cab")
                 : id == kBlockDrive  ? juce::String ("drv")
                 : id == kBlockChorus ? juce::String ("cho")
                 : id == kBlockEq     ? juce::String ("eq")
                 : id == kBlockBoost1 ? juce::String ("b1")
                 : id == kBlockBoost2 ? juce::String ("b2")
                 : id == kBlockBoost3 ? juce::String ("b3")
                 : id == kBlockComp   ? juce::String ("cmp")
                 : id == kBlockDelay  ? juce::String ("dly")
                 : id == kBlockAir    ? juce::String ("air")
                 : id == kBlockReverb ? juce::String ("rev")
                                      : "p" + juce::String (id + 1));
    }
    return names;
}

//==============================================================================
void AmpCadeProcessor::setHiddenSlots (const juce::StringArray& names)
{
    // Everything is hideable — the player owns the board. Hiding only parks the
    // block in the rack; bypass is its own toggle, which the UI stomps off.
    // "tun" is the tuner and "loop" the looper — UI-side pseudo-slots.
    static const juce::StringArray hideable = []
    {
        juce::StringArray h;
        for (int i = 1; i <= kNumPedals; ++i)
            h.add ("p" + juce::String (i));
        h.addArray ({ "drv", "cho", "eq", "b1", "b2", "b3",
                      "cmp", "amp", "cab", "air", "dly", "rev", "tun", "loop" });
        return h;
    }();
    juce::StringArray clean;
    for (const auto& n : names)
        if (hideable.contains (n) && ! clean.contains (n))
            clean.add (n);
    hiddenSlots = clean;
    notifyState();
}

//==============================================================================
void AmpCadeProcessor::initScenes()
{
    scenes.clear();
    for (auto* name : { "Clean", "Rhythm", "Lead", "Solo" })
    {
        Scene sc;
        sc.name = name;
        scenes.push_back (std::move (sc));
    }
    activeScene = -1;
}

void AmpCadeProcessor::sceneSave (int idx)
{
    if (idx < 0 || idx >= (int) scenes.size())
        return;

    auto& sc = scenes[(size_t) idx];
    sc.values.clear();
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            // The loop's volumes are LIVE controls, not part of a rig:
            // a scene flip mid-performance must never move a playing loop.
            if (rp->paramID == "loop_vol" || rp->paramID == "loop_imp")
                continue;
            sc.values[rp->paramID] = rp->convertFrom0to1 (rp->getValue());
        }
    // The board is part of the scene: gear in slots, rack contents, chain
    // order. A pedal removed while noodling in scene 1 comes back when the
    // scene that owns it is recalled.
    sc.hasBoard = true;
    sc.order = getChainOrderNames().joinIntoString (" ");
    sc.hidden = hiddenSlots;
    sc.slots = slotsToTree();
    sc.used = true;
    activeScene = idx;
    notifyState();
}

void AmpCadeProcessor::sceneAdd (const juce::String& name)
{
    if ((int) scenes.size() >= kMaxScenes)
        return;

    Scene sc;
    sc.name = name.trim().isNotEmpty() ? name.trim().substring (0, 18)
                                       : "Scene " + juce::String ((int) scenes.size() + 1);
    scenes.push_back (std::move (sc));
    sceneSave ((int) scenes.size() - 1); // a new scene IS the current settings
}

void AmpCadeProcessor::sceneDelete (int idx)
{
    if (idx < 0 || idx >= (int) scenes.size())
        return;
    scenes.erase (scenes.begin() + idx);
    if (activeScene == idx) activeScene = -1;
    else if (activeScene > idx) --activeScene;
    notifyState();
}

void AmpCadeProcessor::sceneRecall (int idx, bool withBoard)
{
    if (idx < 0 || idx >= (int) scenes.size() || ! scenes[(size_t) idx].used)
        return;

    beginStateBatch(); // order + rack + slots land as ONE board render

    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            if (rp->paramID == "loop_vol" || rp->paramID == "loop_imp") // live controls — scenes keep out (see sceneSave)
                continue;
            const auto it = scenes[(size_t) idx].values.find (rp->paramID);
            if (it == scenes[(size_t) idx].values.end())
                continue;
            const float norm = rp->convertTo0to1 (it->second);
            if (std::abs (norm - rp->getValue()) > 1.0e-6f)
            {
                rp->beginChangeGesture();
                rp->setValueNotifyingHost (norm);
                rp->endChangeGesture();
            }
        }
    if (withBoard)
    {
        recallSceneBoard (scenes[(size_t) idx]);
    }
    else
    {
        // Tone only — the live board stays. That board can rack a block the
        // scene had out front, and the scene's own snapshot would switch it
        // back on: racked means DISENGAGED (the invariant recallSceneBoard
        // keeps for the scene's rack), so re-assert it against the rack we are
        // actually keeping. Otherwise a relaunch leaves an invisible delay
        // running in the chain.
        for (const auto& s : hiddenSlots)
            if (s != "loop")
                setParamPlain ((s + "_on").toRawUTF8(), 0.0f);
    }
    activeScene = idx;
    endStateBatch();
    notifyState();
}

// Bring back the scene's board: order, rack, and what's loaded where. Gear the
// engine already holds is not reloaded (see restoreSlotsFromState), so flipping
// between scenes that share a rig only moves knobs. Pre-0.9 scenes have no
// board snapshot and leave the board alone.
//
// PEDAL captures are preset-wide, not per-scene: an overdrive added while the
// Rhythm scene was active is not in the Clean scene's snapshot — recalling
// Clean must not silently delete it. Any live pedal the scene doesn't place on
// its board stays loaded and lands in the rack, ready to pull out.
void AmpCadeProcessor::recallSceneBoard (const Scene& sc)
{
    if (! sc.hasBoard)
        return;
    if (sc.order.isNotEmpty())
        setChainOrderFromNames (juce::StringArray::fromTokens (sc.order, " ", ""));

    auto merged = sc.slots.isValid() ? sc.slots.createCopy() : juce::ValueTree ("SLOTS");
    auto hidden = sc.hidden;
    // Scenes saved before the shape EQ / boosts existed park them in the rack
    // (same migration as applyRigTree).
    if (! sc.order.contains ("eq") && ! hidden.contains ("eq"))
        hidden.add ("eq");
    if (! sc.order.contains ("b1"))
        for (auto* b : { "b1", "b2", "b3" })
            if (! hidden.contains (b))
                hidden.add (b);
    if (! sc.order.contains ("cmp") && ! hidden.contains ("cmp"))
        hidden.add ("cmp");
    // Gear is shared across a preset's scenes: a capture any scene uses stays
    // reachable from every scene — on the board where that scene puts it, in
    // the rack everywhere else. Sourcing that from the LIVE rig alone left
    // holes. Recall a scene that does not use the TS808 and it is parked in
    // the rack, fine; but land on such a scene at launch and `meta` never had
    // it, so there was nothing to merge and the pedal was simply absent from
    // the preset until you happened to hit a scene that still carried it. The
    // union of every scene's own snapshot is what a player means by "the gear
    // in this preset", so that is what gets merged.
    //
    // "Definition wins" order is live rig first, then scenes by index, so the
    // copy you are actually playing is the one that survives when two scenes
    // put different captures in the same slot (only one can occupy p<n>).
    std::map<juce::String, juce::ValueTree> shared;
    const auto usable = [] (const juce::ValueTree& s)
    {
        // A pathless slot is still real: factory and shared presets ship gear
        // identified only by TONE3000 id, and restoreSlotsFromState resolves
        // those from the download cache.
        return (bool) s.getProperty ("loaded", false)
               && (s.getProperty ("filePath").toString().isNotEmpty()
                   || ((int) s.getProperty ("toneId", 0) > 0
                       && (int) s.getProperty ("modelId", 0) > 0));
    };

    {
        const juce::ScopedLock l (metaLock);
        for (const auto& [name, sm] : meta)
        {
            if (! name.startsWith ("p") || ! sm.loaded)
                continue;
            if (sm.filePath.isEmpty() && (sm.toneId <= 0 || sm.modelId <= 0))
                continue;

            juce::ValueTree s ("SLOT");
            s.setProperty ("id", name, nullptr);
            s.setProperty ("loaded", true, nullptr);
            s.setProperty ("title", sm.title, nullptr);
            s.setProperty ("author", sm.author, nullptr);
            s.setProperty ("gear", sm.gear, nullptr);
            s.setProperty ("modelName", sm.modelName, nullptr);
            s.setProperty ("filePath", sm.filePath, nullptr);
            s.setProperty ("toneId", sm.toneId, nullptr);
            s.setProperty ("modelId", sm.modelId, nullptr);
            shared.emplace (name, s);
        }
    }

    for (const auto& other : scenes)
    {
        if (! other.used || ! other.slots.isValid())
            continue;
        for (int i = 0; i < other.slots.getNumChildren(); ++i)
        {
            const auto s = other.slots.getChild (i);
            const auto id = s.getProperty ("id").toString();
            if (id.startsWith ("p") && usable (s))
                shared.emplace (id, s.createCopy()); // first definition wins
        }
    }

    for (const auto& [name, def] : shared)
    {
        bool sceneHasIt = false;
        for (int i = merged.getNumChildren(); --i >= 0;)
        {
            auto s = merged.getChild (i);
            if (s.getProperty ("id").toString() != name)
                continue;
            sceneHasIt = usable (s);
            if (! sceneHasIt)
                merged.removeChild (i, nullptr); // empty placeholder — replaced below
            break;
        }
        if (sceneHasIt)
            continue;                            // this scene puts it on the board

        merged.appendChild (def.createCopy(), nullptr);
        if (! hidden.contains (name))
            hidden.add (name);                   // present, but parked in the rack
    }

    setHiddenSlots (hidden);

    // Racked means DISENGAGED — the same invariant the stomp UI keeps (hiding
    // a block turns it off). The scene's own param snapshot can't know about a
    // pedal that was merged into its rack after it was saved, so its stale
    // "_on = true" would leave an invisible overdrive running in the chain.
    for (const auto& s : hidden)
        if (s != "loop")
            setParamPlain ((s + "_on").toRawUTF8(), 0.0f);

    slotsFromTree (merged);
    restoreSlotsFromState();
}

void AmpCadeProcessor::sceneClear (int idx)
{
    if (idx < 0 || idx >= (int) scenes.size())
        return;
    scenes[(size_t) idx].used = false;
    scenes[(size_t) idx].values.clear();
    if (activeScene == idx)
        activeScene = -1;
    notifyState();
}

void AmpCadeProcessor::sceneRename (int idx, const juce::String& name)
{
    if (idx < 0 || idx >= (int) scenes.size() || name.trim().isEmpty())
        return;
    scenes[(size_t) idx].name = name.trim().substring (0, 18);
    notifyState();
}

void AmpCadeProcessor::sceneMove (int from, int to)
{
    const int n = (int) scenes.size();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to)
        return;
    auto sc = std::move (scenes[(size_t) from]);
    scenes.erase (scenes.begin() + from);
    scenes.insert (scenes.begin() + to, std::move (sc));
    // the active marker follows its scene
    if (activeScene == from)                            activeScene = to;
    else if (activeScene > from && activeScene <= to)   --activeScene;
    else if (activeScene < from && activeScene >= to)   ++activeScene;
    notifyState();
}

bool AmpCadeProcessor::isActiveSceneDirty() const
{
    if (activeScene < 0 || activeScene >= (int) scenes.size())
        return false;
    const auto& sc = scenes[(size_t) activeScene];
    if (! sc.used)
        return false;

    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            const auto it = sc.values.find (rp->paramID);
            if (it == sc.values.end())
                continue;
            // Compare in normalized space so a 0.1 dB knob and a 0..1 mix knob
            // get the same tolerance.
            if (std::abs (rp->convertTo0to1 (it->second) - rp->getValue()) > 2.0e-3f)
                return true;
        }
    return false;
}

juce::ValueTree AmpCadeProcessor::scenesToTree() const
{
    juce::ValueTree tree ("SCENES");
    tree.setProperty ("active", activeScene, nullptr);
    for (int i = 0; i < (int) scenes.size(); ++i)
    {
        const auto& sc = scenes[(size_t) i];
        juce::ValueTree s ("SCENE");
        s.setProperty ("idx", i, nullptr);
        s.setProperty ("name", sc.name, nullptr);
        s.setProperty ("used", sc.used, nullptr);
        if (sc.used)
        {
            juce::ValueTree v ("VALUES");
            for (const auto& [id, val] : sc.values)
                v.setProperty (id, val, nullptr);
            s.appendChild (v, nullptr);
            if (sc.hasBoard)
            {
                s.setProperty ("hasBoard", true, nullptr);
                s.setProperty ("order", sc.order, nullptr);
                s.setProperty ("hidden", sc.hidden.joinIntoString (" "), nullptr);
                if (sc.slots.isValid())
                    s.appendChild (sc.slots.createCopy(), nullptr);
            }
        }
        tree.appendChild (s, nullptr);
    }
    return tree;
}

void AmpCadeProcessor::scenesFromTree (const juce::ValueTree& tree)
{
    if (! tree.isValid())
    {
        initScenes();
        return;
    }

    // The list is ordered; the stored idx is legacy metadata from the fixed-4
    // era and is ignored beyond ordering (children are already in order).
    scenes.clear();
    for (int i = 0; i < tree.getNumChildren() && (int) scenes.size() < kMaxScenes; ++i)
    {
        auto s = tree.getChild (i);
        if (! s.hasType ("SCENE"))
            continue;
        Scene sc;
        sc.name = s.getProperty ("name", "Scene " + juce::String ((int) scenes.size() + 1)).toString();
        sc.used = (bool) s.getProperty ("used", false);
        auto v = s.getChildWithName ("VALUES");
        if (sc.used && v.isValid())
            for (int k = 0; k < v.getNumProperties(); ++k)
            {
                const auto id = v.getPropertyName (k);
                sc.values[id.toString()] = (float) v.getProperty (id);
            }
        if (sc.used && sc.values.empty())
            sc.used = false;
        if (sc.used && (bool) s.getProperty ("hasBoard", false))
        {
            sc.hasBoard = true;
            sc.order = s.getProperty ("order", "").toString();
            sc.hidden = juce::StringArray::fromTokens (s.getProperty ("hidden", "").toString(), " ", "");
            auto sl = s.getChildWithName ("SLOTS");
            if (sl.isValid())
                sc.slots = sl.createCopy();
        }
        scenes.push_back (std::move (sc));
    }
    if (scenes.empty())
        initScenes();
    else
        activeScene = juce::jlimit (-1, (int) scenes.size() - 1, (int) tree.getProperty ("active", -1));
}

//==============================================================================
juce::var AmpCadeProcessor::getUiState()
{
    auto root = makeObj();
    root->setProperty ("connected", t3k.isConnected());
    root->setProperty ("username", t3k.getUsername());
    root->setProperty ("clientIdSet", t3k.getClientId().isNotEmpty());
    // The UI's key-swallowing (beep suppression, Tab = rack) must not eat a
    // DAW host's keystrokes — only the standalone shell beeps on unclaimed keys.
    root->setProperty ("standalone", wrapperType == wrapperType_Standalone);
    root->setProperty ("presetDesigns", presetDesigns);

    juce::Array<juce::var> orderArr;
    for (const auto& name : getChainOrderNames())
        orderArr.add (name);
    root->setProperty ("order", orderArr);

    juce::Array<juce::var> hiddenArr;
    for (const auto& name : hiddenSlots)
        hiddenArr.add (name);
    root->setProperty ("hidden", hiddenArr);

    juce::Array<juce::var> sceneArr;
    for (const auto& sc : scenes)
    {
        auto s = makeObj();
        s->setProperty ("name", sc.name);
        s->setProperty ("used", sc.used);
        sceneArr.add (juce::var (s.get()));
    }
    root->setProperty ("scenes", sceneArr);
    root->setProperty ("activeScene", activeScene);
    root->setProperty ("maxScenes", kMaxScenes);

    auto lp = makeObj();
    lp->setProperty ("bpm", looper.getBpm());
    lp->setProperty ("beats", looper.getBeats());
    lp->setProperty ("playAfterRec", looper.getPlayAfterRec());
    lp->setProperty ("countIn", looper.getCountIn());
    lp->setProperty ("ringOut", looper.getRingOut());
    root->setProperty ("looper", juce::var (lp.get()));

    auto slots = makeObj();
    {
        const juce::ScopedLock l (metaLock);
        for (const auto& name : slotNames())
        {
            const auto& sm = meta[name];
            auto s = makeObj();
            s->setProperty ("loaded", sm.loaded);
            if (sm.loaded || sm.missing)
            {
                s->setProperty ("title", sm.title);
                s->setProperty ("author", sm.author);
                s->setProperty ("gear", sm.gear);
                s->setProperty ("modelName", sm.modelName);
                // Only real TONE3000 gear carries an id. Locally imported gear
                // keeps the default 0, and the UI's "is this from TONE3000?"
                // test is `ss.toneId != null` — which 0 passes. That gave every
                // imported capture a tone link to /tones/0 (a 404) and a
                // settings dropdown that could only ever fail. Omitting the
                // property makes it undefined, which that test reads correctly.
                if (sm.toneId > 0)
                    s->setProperty ("toneId", sm.toneId);
                if (sm.modelId > 0)
                    s->setProperty ("modelId", sm.modelId);
                s->setProperty ("missing", sm.missing);
            }
            slots->setProperty (name, juce::var (s.get()));
        }
    }
    root->setProperty ("slots", juce::var (slots.get()));
    return juce::var (root.get());
}

//==============================================================================
juce::ValueTree AmpCadeProcessor::slotsToTree() const
{
    juce::ValueTree tree ("SLOTS");
    const juce::ScopedLock l (metaLock);
    for (const auto& [name, sm] : meta)
    {
        juce::ValueTree s ("SLOT");
        s.setProperty ("id", name, nullptr);
        s.setProperty ("loaded", sm.loaded || sm.missing, nullptr);
        s.setProperty ("title", sm.title, nullptr);
        s.setProperty ("author", sm.author, nullptr);
        s.setProperty ("gear", sm.gear, nullptr);
        s.setProperty ("modelName", sm.modelName, nullptr);
        // Relative to the app folder wherever it lives inside it — see
        // Library::packGearPath. Paths stay absolute in memory; only what we
        // write to disk is de-personalised.
        s.setProperty ("filePath", Library::packGearPath (sm.filePath), nullptr);
        s.setProperty ("toneId", sm.toneId, nullptr);
        s.setProperty ("modelId", sm.modelId, nullptr);
        tree.appendChild (s, nullptr);
    }
    return tree;
}

void AmpCadeProcessor::slotsFromTree (const juce::ValueTree& tree)
{
    const juce::ScopedLock l (metaLock);
    // Remember what the engine currently holds, so the restore that follows
    // can skip reloading identical gear (scene flips, undo, presets).
    prevEnginePaths.clear();
    for (const auto& [name, sm] : meta)
        if (sm.loaded && sm.filePath.isNotEmpty())
            prevEnginePaths[name] = sm.filePath;

    for (const auto& name : slotNames())
        meta[name] = {};

    for (int i = 0; i < tree.getNumChildren(); ++i)
    {
        auto s = tree.getChild (i);
        const auto id = s.getProperty ("id").toString();
        if (auto* mp = metaFor (id))
        {
            mp->loaded = false; // becomes true once the file actually loads
            mp->missing = false;
            mp->title = s.getProperty ("title").toString();
            mp->author = s.getProperty ("author").toString();
            mp->gear = s.getProperty ("gear").toString();
            mp->modelName = s.getProperty ("modelName").toString();
            mp->filePath = Library::unpackGearPath (s.getProperty ("filePath").toString());
            mp->toneId = (int) s.getProperty ("toneId", 0);
            mp->modelId = (int) s.getProperty ("modelId", 0);
            mp->wanted = (bool) s.getProperty ("loaded", false);
            if (! mp->wanted)
                mp->filePath.clear();
        }
    }
}

juce::File AmpCadeProcessor::cachedGearFile (const juce::String& slot, const SlotMeta& sm)
{
    if (sm.toneId <= 0 || sm.modelId <= 0)
        return {};

    const bool isIr = (slot == "cab" || slot == "air");
    const auto dir = (isIr ? Library::irsDir() : Library::modelsDir())
                         .getChildFile ("tone_" + juce::String (sm.toneId));
    if (! dir.isDirectory())
        return {};

    const auto ext = juce::String (isIr ? ".wav" : ".nam");
    const auto exact = dir.getChildFile (Library::sanitizeFileName (sm.modelName)
                                         + "_" + juce::String (sm.modelId) + ext);
    if (exact.existsAsFile())
        return exact;

    // Renamed on TONE3000 since the download: the model id still pins the file.
    juce::Array<juce::File> hits;
    dir.findChildFiles (hits, juce::File::findFiles, false,
                        "*_" + juce::String (sm.modelId) + ext);
    return hits.isEmpty() ? juce::File() : hits.getReference (0);
}

void AmpCadeProcessor::restoreSlotsFromState()
{
    juce::StringArray toLoad, toClear, toFetch;
    {
        const juce::ScopedLock l (metaLock);
        for (auto& [name, sm] : meta)
        {
            // A slot whose file is gone (or was never on this machine — a
            // factory or shared preset ships no paths) but that knows its
            // TONE3000 identity can simply fetch itself back through the
            // player's own account. No connection = the missing badge and its
            // re-download button, as before.
            bool fileGone = sm.filePath.isEmpty() || ! juce::File (sm.filePath).existsAsFile();
            // `wanted`, not `loaded`: slotsFromTree has just cleared `loaded`
            // on every slot (it means "the engine holds it", which nothing can
            // yet), so testing it here made this whole branch unreachable —
            // pathless gear was never fetched and never resolved, it was
            // simply dropped, which is why loading a factory preset without a
            // live connection came back as the built-in amp and cab.
            const bool fetchable = sm.wanted && sm.toneId > 0 && sm.modelId > 0;

            // ...but look in the download cache BEFORE deciding it needs the
            // network at all. Asking "is filePath empty? then fetch it" made a
            // pathless preset depend on being logged in even when the exact
            // file was already sitting in the cache under the very ids the
            // preset was asking for — so an expired token read as "my amp and
            // cab reverted to the built-ins", which looks like data loss and is
            // not. This also means gear a friend's shared preset names loads
            // instantly if you already own it, with no download at all.
            if (fileGone && fetchable)
            {
                const auto cached = cachedGearFile (name, sm);
                if (cached.existsAsFile())
                {
                    sm.filePath = cached.getFullPathName();
                    sm.missing = false;
                    fileGone = false;
                }
            }

            if (fileGone && fetchable && t3k.isConnected())
            {
                toFetch.add (name);
                toClear.add (name); // silence the stale engine slot while it downloads
                continue;
            }
            if (sm.filePath.isEmpty() && ! fetchable)
            {
                toClear.add (name);
                continue;
            }
            if (fileGone)
            {
                sm.missing = true;
                sm.loaded = false;
                toClear.add (name);
                continue;
            }
            toLoad.add (name);
        }
    }

    // Anything the new state does not fill has to be emptied in the engine too.
    // Without this a preset with no cab left the previous IR convolving away with
    // nothing on the board to explain it — and no way to get rid of it. (A slot
    // the engine never held can skip the engine clear: draining an already-empty
    // convolution just wastes blocks.)
    bool cleared = false;
    for (const auto& name : toClear)
    {
        if (prevEnginePaths.find (name) == prevEnginePaths.end())
            continue;
        cleared = true;
        if (name == "cab")
            clearCabIr();
        else if (name == "air")
            clearAirIr();
        else if (auto* s = namSlotFor (name))
            s->clear();
    }
    if (cleared)
        updateLatency();

    for (const auto& name : toLoad)
    {
        SlotMeta sm;
        {
            const juce::ScopedLock l (metaLock);
            sm = *metaFor (name);
        }
        sm.loaded = true;
        sm.missing = false;

        // The engine already runs this exact file — a scene flip or undo that
        // keeps the gear only needs the meta flag back, not a reload.
        const auto prev = prevEnginePaths.find (name);
        if (prev != prevEnginePaths.end() && prev->second == sm.filePath)
        {
            const juce::ScopedLock l (metaLock);
            if (auto* mp = metaFor (name))
                *mp = sm;
            continue;
        }

        if (name == "cab")
            loadIrIntoCab (juce::File (sm.filePath), sm, false);
        else if (name == "air")
            loadIrIntoAir (juce::File (sm.filePath), sm, false);
        else
            finishLoadIntoSlot (name, juce::File (sm.filePath), sm);
    }

    for (const auto& name : toFetch)
    {
        SlotMeta sm;
        {
            const juce::ScopedLock l (metaLock);
            sm = *metaFor (name);
        }
        requestT3kLoad (name, sm.toneId, sm.modelId, [] (juce::var) {});
    }

    notifyState();
}

// One rig, one tree: session state, presets and undo snapshots all use this.
juce::ValueTree AmpCadeProcessor::rigToTree()
{
    juce::ValueTree root ("AMPCADE");
    root.setProperty ("version", kAppVersion, nullptr);
    root.setProperty ("pedalOrder", getChainOrderNames().joinIntoString (" "), nullptr);
    root.setProperty ("hiddenSlots", hiddenSlots.joinIntoString (" "), nullptr);
    root.setProperty ("looperBpm", looper.getBpm(), nullptr);
    root.setProperty ("looperBeats", looper.getBeats(), nullptr);
    root.setProperty ("looperPlayAfterRec", looper.getPlayAfterRec(), nullptr);
    root.setProperty ("looperCountIn", looper.getCountIn(), nullptr);
    root.setProperty ("looperRingOut", looper.getRingOut(), nullptr);
    root.appendChild (apvts.copyState(), nullptr);
    root.appendChild (slotsToTree(), nullptr);
    root.appendChild (scenesToTree(), nullptr);
    return root;
}

void AmpCadeProcessor::scrubNonFiniteParams (juce::ValueTree& params) const
{
    for (auto child : params)
    {
        if (! child.hasType ("PARAM"))
            continue;

        const double v = child.getProperty ("value");
        if (std::isfinite (v))
            continue;

        float fallback = 0.0f;
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (
                apvts.getParameter (child.getProperty ("id").toString())))
            fallback = rp->convertFrom0to1 (rp->getDefaultValue());

        child.setProperty ("value", fallback, nullptr);
    }
}

void AmpCadeProcessor::applyRigTree (const juce::ValueTree& root, bool deferSlotLoad)
{
    restoringState.store (true);
    beginStateBatch();

    // The loop volume rides through preset/undo/state swaps untouched while a
    // loop exists — the loop keeps playing, so its level must not jump. The
    // held value is patched INTO the incoming tree before it applies: setting
    // it back afterwards let the smoother chase the stored value for a few
    // milliseconds first, which was audible as a down-then-up swell. With the
    // looper empty (fresh app start), the saved value restores normally.
    auto params = root.getChildWithName (apvts.state.getType());
    if (params.isValid())
    {
        auto copy = params.createCopy(); // never alias an undo snapshot with live state
        scrubNonFiniteParams (copy);     // presets and host state are untrusted input
        if (looper.getState() != Looper::Idle)
        {
            // both loop volumes are LIVE controls: a preset/undo swap while a
            // loop plays must never move them (matches the scene exclusions)
            const std::pair<const char*, std::atomic<float>*> live[] = {
                { "loop_vol", p_loopVol }, { "loop_imp", p_loopImp } };
            for (const auto& [id, ptr] : live)
            {
                if (ptr == nullptr)
                    continue;
                auto lv = copy.getChildWithProperty ("id", id);
                if (! lv.isValid())
                {
                    lv = juce::ValueTree ("PARAM");
                    lv.setProperty ("id", id, nullptr);
                    copy.appendChild (lv, nullptr);
                }
                lv.setProperty ("value", ptr->load(), nullptr);
            }
        }
        apvts.replaceState (copy);
    }

    const auto orderStr = root.getProperty ("pedalOrder", "").toString();
    if (orderStr.isNotEmpty())
        setChainOrderFromNames (juce::StringArray::fromTokens (orderStr, " ", ""));

    // A state without a rack list (ancient dev builds) gets the fresh-install
    // default; an explicit empty list means the player pulled everything onto
    // the board. (The old version-gated tuner migration is gone: the public
    // renumbering to 0.1 made "is this older than 0.8.1" meaningless.)
    {
        juce::StringArray hidden;
        if (root.hasProperty ("hiddenSlots"))
            hidden = juce::StringArray::fromTokens (root.getProperty ("hiddenSlots").toString(), " ", "");
        else
            hidden = juce::StringArray { "cho", "tun", "eq", "b1", "b2", "b3", "cmp" };
        // A state saved before the shape EQ / boosts existed knows nothing
        // about them — they must land in the rack, not appear unasked on a
        // board the player already arranged. (An order that DOES name them is
        // newer and its rack list is the player's word.)
        if (! orderStr.contains ("eq") && ! hidden.contains ("eq"))
            hidden.add ("eq");
        if (! orderStr.contains ("b1"))
            for (auto* b : { "b1", "b2", "b3" })
                if (! hidden.contains (b))
                    hidden.add (b);
        if (! orderStr.contains ("cmp") && ! hidden.contains ("cmp"))
            hidden.add ("cmp");
        setHiddenSlots (hidden);
    }

    looper.setConfig ((int) root.getProperty ("looperBpm", 120),
                      (int) root.getProperty ("looperBeats", 4),
                      (bool) root.getProperty ("looperPlayAfterRec", false),
                      (bool) root.getProperty ("looperCountIn", true),
                      (bool) root.getProperty ("looperRingOut", false));

    scenesFromTree (root.getChildWithName ("SCENES"));

    auto slots = root.getChildWithName ("SLOTS");
    if (slots.isValid())
    {
        slotsFromTree (slots);
        if (deferSlotLoad)
        {
            // Model loading must happen off the host's state-restore path
            juce::MessageManager::callAsync ([safe = juce::WeakReference<AmpCadeProcessor> (this)]
            {
                if (auto* self = safe.get())
                    self->restoreSlotsFromState();
            });
        }
        else
        {
            restoreSlotsFromState();
        }
    }

    restoringState.store (false);
    undoBaseline = rigToTree();
    paramDirty.store (false);
    endStateBatch();
    notifyState();
}

void AmpCadeProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = rigToTree().createXml())
        copyXmlToBinary (*xml, destData);
}

void AmpCadeProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr)
        return;

    auto root = juce::ValueTree::fromXml (*xml);
    if (! root.hasType ("AMPCADE"))
        return;

    applyRigTree (root, true);

    // A DAW session restores verbatim — reopening a project must sound exactly
    // as it was left, mid-scene and all. The standalone relaunching is a fresh
    // sit-down instead: land on scene 1 the way a preset load does, not on
    // whichever scene happened to be active at quit (the Clean rig arriving on
    // its Solo scene read as "the preset loads the wrong tone"). Queued so it
    // runs after the deferred slot restore above.
    //
    // TONE ONLY (withBoard=false). Landing on scene 1's *board* would undo the
    // board we just restored, which is the one the player actually left: the
    // chain order and rack above would be overwritten by whatever scene 1 was
    // saved with, every single launch. That made a custom signal-chain order
    // impossible to keep across a restart — the top-level pedalOrder/
    // hiddenSlots were written at quit and then thrown away at boot. The
    // wrong-tone complaint this landing fixes was about knobs, not layout.
    if (wrapperType == wrapperType_Standalone)
        juce::MessageManager::callAsync ([safe = juce::WeakReference<AmpCadeProcessor> (this)]
        {
            if (auto* self = safe.get())
                if (! self->scenes.empty() && self->scenes[0].used)
                    self->sceneRecall (0, false);
        });

    // A host restore is a new world — history from the old one makes no sense.
    undoStack.clear();
    redoStack.clear();
}

// Every SLOT in a rig tree: the board's own, AND the copy each scene keeps of
// the board it was saved with. The scene copies are the ones easily forgotten —
// a preset whose top-level SLOTS block looked spotless still carried 26
// absolute paths inside its SCENES.
static void forEachSlot (juce::ValueTree tree, const std::function<void (juce::ValueTree&)>& fn)
{
    if (tree.hasType ("SLOT"))
    {
        fn (tree);
        return;
    }
    for (int i = 0; i < tree.getNumChildren(); ++i)
    {
        auto child = tree.getChild (i);
        forEachSlot (child, fn);
    }
}

bool AmpCadeProcessor::savePresetTo (const juce::File& file, const juce::String& designsJson)
{
    auto tree = rigToTree();
    // The rig's cosmetic designs (names, colours, knobs — normally ui-prefs
    // only) ride along in the file so a shared preset looks the same on the
    // receiving machine. The engine never reads this; the UI applies it.
    if (designsJson.isNotEmpty())
        tree.setProperty ("designs", designsJson, nullptr);

    // A preset is a file players SEND EACH OTHER, so it must never carry an
    // absolute path — that would ship the sender's home directory (and with it
    // their account name) to everyone who opens it. slotsToTree already made
    // every path inside the app folder relative; anything still absolute is
    // gear from outside it, and travels as "not on this machine" — the
    // receiving rig re-fetches by toneId exactly like a factory preset does.
    forEachSlot (tree, [] (juce::ValueTree& s)
    {
        if (juce::File::isAbsolutePath (s.getProperty ("filePath").toString()))
            s.setProperty ("filePath", "", nullptr);
    });

    if (auto xml = tree.createXml())
        return file.replaceWithText (xml->toString (juce::XmlElement::TextFormat()));
    return false;
}

bool AmpCadeProcessor::loadPresetFrom (const juce::File& file)
{
    auto xml = Library::parsePresetXml (file);
    if (xml == nullptr)
        return false;

    auto root = juce::ValueTree::fromXml (*xml);
    if (! root.hasType ("AMPCADE"))
        return false;

    // Everything below this line came from a file someone else may have
    // written, so no slot may end up pointing outside AmpCade's own folder.
    // Relative paths already cannot escape it (Library::unpackGearPath clamps
    // them); an absolute one is either a rig saved before paths were
    // relativised — kept, so a player's own older presets still find their
    // gear — or an attempt to aim a slot at the rest of the disk, which is
    // dropped. A dropped path is not a dead slot: the rig re-fetches by toneId
    // exactly as a factory preset does.
    forEachSlot (root, [] (juce::ValueTree& s)
    {
        const auto stored = s.getProperty ("filePath").toString();
        if (juce::File::isAbsolutePath (stored)
            && ! juce::File (stored).isAChildOf (Library::appDataDir()))
            s.setProperty ("filePath", "", nullptr);
    });

    presetDesigns = root.getProperty ("designs", juce::String()).toString();
    beginStateBatch();
    applyRigTree (root, false);
    // Land on scene 1 every time: switching presets should always arrive at
    // the same, predictable starting point.
    if (! scenes.empty() && scenes[0].used)
        sceneRecall (0);
    else
        activeScene = -1; // scene 1 is empty: nothing to land on
    endStateBatch();
    notifyState();
    return true;
}

// Scene edits must stick even when the player never re-saves the preset —
// but ONLY the scenes: the preset's own saved rig (its top-level knobs and
// gear) stays exactly as the player last saved it.
bool AmpCadeProcessor::syncScenesToPresetFile (const juce::File& file)
{
    auto xml = Library::parsePresetXml (file);
    if (xml == nullptr)
        return false;

    auto root = juce::ValueTree::fromXml (*xml);
    if (! root.hasType ("AMPCADE"))
        return false;

    auto old = root.getChildWithName ("SCENES");
    if (old.isValid())
        root.removeChild (old, nullptr);
    root.appendChild (scenesToTree(), nullptr);

    // This writes into a preset FILE, so it needs the same scrub savePresetTo
    // does — and it is the one that actually leaked. Scene edits auto-persist
    // here after every save/rename/move, and each scene carries its own board
    // snapshot, so absolute paths accumulated inside SCENES while the preset's
    // own SLOTS block stayed clean and looked fine.
    forEachSlot (root, [] (juce::ValueTree& s)
    {
        if (juce::File::isAbsolutePath (s.getProperty ("filePath").toString()))
            s.setProperty ("filePath", "", nullptr);
    });

    if (auto out = root.createXml())
        return file.replaceWithText (out->toString (juce::XmlElement::TextFormat()));
    return false;
}

//==============================================================================
// Undo: whole-rig ValueTree snapshots. Knob turns coalesce into one step per
// quiet-time burst (parameterChanged → timerCallback); everything structural
// pushes explicitly through beginUserAction() before it mutates anything.
void AmpCadeProcessor::parameterChanged (const juce::String&, float)
{
    if (restoringState.load())
        return;
    paramDirty.store (true);
    lastParamChangeMs.store (juce::Time::getMillisecondCounter());
}

void AmpCadeProcessor::pushUndo (juce::ValueTree t)
{
    undoStack.push_back (std::move (t));
    if ((int) undoStack.size() > kMaxUndo)
        undoStack.erase (undoStack.begin());
}

void AmpCadeProcessor::commitParamDirt()
{
    paramDirty.store (false);
    pushUndo (undoBaseline.isValid() ? undoBaseline : rigToTree());
    undoBaseline = rigToTree();
    redoStack.clear();
}

void AmpCadeProcessor::beginUserAction()
{
    if (restoringState.load())
        return;
    if (paramDirty.load())
        commitParamDirt();
    pushUndo (rigToTree());
    redoStack.clear();
}

bool AmpCadeProcessor::undo()
{
    if (paramDirty.load())
        commitParamDirt();  // knob moves since the last commit become the top entry
    if (undoStack.empty())
        return false;
    auto tree = undoStack.back();
    undoStack.pop_back();
    redoStack.push_back (rigToTree());
    applyRigTree (tree, false);
    return true;
}

bool AmpCadeProcessor::redo()
{
    if (paramDirty.load())
        commitParamDirt();  // fresh edits invalidate the redo branch (this clears it)
    if (redoStack.empty())
        return false;
    auto tree = redoStack.back();
    redoStack.pop_back();
    pushUndo (rigToTree());
    applyRigTree (tree, false);
    return true;
}

// Factory-fresh rig for "New preset": default knobs, empty slots (built-in amp
// + cab step in), default order, chorus + tuner racked, fresh unused scenes.
void AmpCadeProcessor::resetRigToDefault()
{
    restoringState.store (true);
    beginStateBatch();
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            if (std::abs (rp->getValue() - rp->getDefaultValue()) > 1.0e-6f)
            {
                rp->beginChangeGesture();
                rp->setValueNotifyingHost (rp->getDefaultValue());
                rp->endChangeGesture();
            }
    for (const auto& name : slotNames())
        clearSlotByName (name);
    setChainOrderFromNames (defaultChainOrderNames());
    hiddenSlots = juce::StringArray { "cho", "tun", "eq", "b1", "b2", "b3", "cmp" };
    initScenes();
    looper.setConfig (120, 4, false, true);
    restoringState.store (false);
    undoBaseline = rigToTree();
    paramDirty.store (false);
    endStateBatch();
    notifyState();
}

juce::AudioProcessorEditor* AmpCadeProcessor::createEditor()
{
#ifdef AMPCADE_SMOKE
    return nullptr; // headless: the test build has no editor to hand back
#else
    return new AmpCadeEditor (*this);
#endif
}
} // namespace ampcade

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ampcade::AmpCadeProcessor();
}
