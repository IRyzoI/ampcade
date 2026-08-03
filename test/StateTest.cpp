// Session-state round trips, headless. Usage: ampcade-state   (exit 0 = pass)
//
// The bug this exists for: a custom signal-chain order did not survive quitting
// and reopening the STANDALONE. Restoring the state put the order back, and
// then the standalone's "land on scene 1" launch step recalled scene 1's whole
// BOARD over the top of it — so the order (and the rack) silently reverted to
// whatever scene 1 happened to hold, on every launch. The saved pedalOrder was
// written at quit and thrown away at boot.
//
// Everything here drives the real AmpCadeProcessor through the same entry
// points the wrappers use: getStateInformation at quit, setStateInformation at
// launch, and a message-loop pump for the landing that setStateInformation
// queues.

#include <juce_audio_processors/juce_audio_processors.h>

#include "Library.h"
#include "PluginProcessor.h"

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

// A processor constructed the way a given wrapper would construct it —
// AudioProcessor::wrapperType is const and latched from this at construction.
static std::unique_ptr<AmpCadeProcessor> makeProcessor (juce::AudioProcessor::WrapperType type)
{
    juce::AudioProcessor::setTypeOfNextNewPlugin (type);
    auto p = std::make_unique<AmpCadeProcessor>();
    juce::AudioProcessor::setTypeOfNextNewPlugin (juce::AudioProcessor::wrapperType_Undefined);
    return p;
}

// setStateInformation defers both the slot restore and the launch landing onto
// the message thread, exactly as it does under a real wrapper.
static void pumpMessageLoop()
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
}

// The player's rearrangement: pull the chorus out and drop it after the cab.
// (Legal — every mono core block still precedes the delay, and the dly/air/rev
// tail keeps its relative order. It is the arrangement scene "Ambient" ships.)
static juce::StringArray chorusAfterCab (juce::StringArray order)
{
    order.removeString ("cho");
    order.insert (order.indexOf ("cab") + 1, "cho");
    return order;
}

// Knobs and stomps, in plain (scaled) units — the same route the editor's
// parameter attachments take.
static void setKnob (AmpCadeProcessor& p, const char* id, float plain)
{
    auto* rp = p.apvts.getParameter (id);
    jassert (rp != nullptr);
    rp->setValueNotifyingHost (rp->convertTo0to1 (plain));
}

static float knob (const AmpCadeProcessor& p, const char* id)
{
    return *p.apvts.getRawParameterValue (id);
}

// A quit: what the standalone writes to AmpCade.settings.
static juce::MemoryBlock quit (AmpCadeProcessor& p)
{
    juce::MemoryBlock state;
    p.getStateInformation (state);
    return state;
}

// A launch: what the wrapper hands back, landing included.
static std::unique_ptr<AmpCadeProcessor> relaunch (const juce::MemoryBlock& state,
                                                   juce::AudioProcessor::WrapperType type)
{
    auto p = makeProcessor (type);
    p->setStateInformation (state.getData(), (int) state.getSize());
    pumpMessageLoop();
    return p;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto standalone = juce::AudioProcessor::wrapperType_Standalone;
    const auto vst3       = juce::AudioProcessor::wrapperType_VST3;

    // ---- Set up a rig the way a player would, then quit -------------------
    juce::MemoryBlock saved;
    juce::StringArray customOrder;
    juce::StringArray customRack;
    {
        auto p = makeProcessor (standalone);
        check (p->wrapperType == standalone, "processor picks up the standalone wrapper type");

        // Scene 1 is saved first, holding the DEFAULT board — delay out front
        // and switched on.
        setKnob (*p, "amp_bass", 3.0f);
        setKnob (*p, "dly_on", 1.0f);
        check (! p->getHiddenSlots().contains ("dly"), "the delay starts out on the board");
        p->sceneSave (0);
        const auto sceneOrder = p->getChainOrderNames();

        // ...and only afterwards does the player rearrange the chain and rack
        // the delay. Neither is committed back into scene 1 — that is the whole
        // point: this is loose board state, and it still has to persist.
        customOrder = chorusAfterCab (sceneOrder);
        check (customOrder.joinIntoString (" ") != sceneOrder.joinIntoString (" "),
               "the rearrangement actually differs from scene 1's board");
        check (p->setChainOrderFromNames (customOrder), "the rearranged chain order is accepted");

        customRack = p->getHiddenSlots();
        customRack.addIfNotAlreadyThere ("dly");
        p->setHiddenSlots (customRack);
        setKnob (*p, "dly_on", 0.0f); // racking disengages, the way the stomp UI does it

        // A knob moved after the scene was saved, so we can tell the launch
        // landing apart from a plain verbatim restore.
        setKnob (*p, "amp_bass", 9.0f);

        saved = quit (*p);
        check (saved.getSize() > 0, "quitting writes a state blob");
    }

    // ---- Relaunching the standalone ---------------------------------------
    {
        auto p = relaunch (saved, standalone);

        // THE REGRESSION: the board the player left comes back untouched.
        check (p->getChainOrderNames().joinIntoString (" ") == customOrder.joinIntoString (" "),
               "custom chain order survives quitting and reopening the standalone");
        check (p->getHiddenSlots().contains ("dly"),
               "a racked block survives quitting and reopening the standalone");

        // ...while the launch still lands on scene 1's TONE, which is what the
        // landing was added for. A fix that simply deleted the landing would
        // pass the two checks above and fail this one.
        check (std::abs (knob (*p, "amp_bass") - 3.0f) < 1.0e-3f,
               "the standalone still lands on scene 1's tone at launch");

        // Racked means disengaged. Scene 1 was saved with the delay on the
        // board and running, so its snapshot says dly_on — and the board we
        // kept has the delay in the rack. It must not come back running where
        // nothing on the board can show it.
        check (knob (*p, "dly_on") < 0.5f,
               "a block racked on the live board stays disengaged after the landing");
    }

    // ---- The same state inside a DAW --------------------------------------
    {
        auto p = relaunch (saved, vst3);

        check (p->getChainOrderNames().joinIntoString (" ") == customOrder.joinIntoString (" "),
               "a DAW session restores the chain order verbatim");
        check (std::abs (knob (*p, "amp_bass") - 9.0f) < 1.0e-3f,
               "a DAW session restores the knob verbatim — no scene landing");
    }

    // ---- A launch with no usable scene 1 ----------------------------------
    // Nothing to land on: the restored board is all there is, and it stands.
    {
        juce::MemoryBlock noScenes;
        juce::StringArray order;
        {
            auto p = makeProcessor (standalone);
            order = chorusAfterCab (p->getChainOrderNames());
            check (p->setChainOrderFromNames (order), "chain order accepted (no-scene case)");
            noScenes = quit (*p); // scenes were never saved, so scene 1 is unused
        }
        auto p = relaunch (noScenes, standalone);
        check (p->getChainOrderNames().joinIntoString (" ") == order.joinIntoString (" "),
               "custom chain order survives a relaunch with no saved scene 1");
    }

    // ---- Gear paths carry no home directory, and cannot leave the folder ---
    {
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                              .getFullPathName();
        const auto inside = Library::modelsDir().getChildFile ("tone_1825/Bluesbreaker.nam")
                                .getFullPathName();

        const auto packed = Library::packGearPath (inside);
        check (! juce::File::isAbsolutePath (packed), "a stored gear path is relative");
        check (! packed.contains (home), "a stored gear path carries no home directory");
        check (Library::unpackGearPath (packed) == inside, "a stored gear path round trips");

        // Separators normalise, so a rig saved on Windows opens on a Mac.
        check (! packed.containsChar ('\\'), "a stored gear path uses '/' separators");

        // THE ATTACK: a preset that points a slot at the rest of the disk.
        check (Library::unpackGearPath ("../../../../etc/passwd").isEmpty(),
               "a gear path escaping the app folder is refused");
        check (Library::unpackGearPath ("models/../../.ssh/id_rsa").isEmpty(),
               "a gear path escaping mid-way is refused");

        // Gear from outside the folder stays absolute, which is exactly how
        // savePresetTo spots it and blanks it on the way into a preset.
        check (juce::File::isAbsolutePath (Library::packGearPath ("/tmp/elsewhere.nam")),
               "gear outside the app folder stays absolute in local state");
    }

    // ---- A preset file is untrusted input ----------------------------------
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("ampcade-state-test");
        dir.createDirectory();

        // An external-entity DOCTYPE would make the parser read a file of the
        // SENDER's choosing off the recipient's disk. It never gets parsed.
        auto hostile = dir.getChildFile ("hostile.ampcade");
        hostile.replaceWithText (
            "<?xml version=\"1.0\"?>\n"
            "<!DOCTYPE AMPCADE SYSTEM \"" + Library::settingsFile().getFullPathName() + "\">\n"
            "<AMPCADE pedalOrder=\"\"/>\n");
        check (Library::parsePresetXml (hostile) == nullptr,
               "a preset carrying a DOCTYPE is refused");

        auto bomb = dir.getChildFile ("bomb.ampcade");
        bomb.replaceWithText ("<?xml version=\"1.0\"?>\n"
                              "<!DOCTYPE AMPCADE [<!ENTITY a \"&a;\">]>\n"
                              "<AMPCADE pedalOrder=\"&a;\"/>\n");
        check (Library::parsePresetXml (bomb) == nullptr,
               "a preset carrying an entity bomb is refused");

        // juce::XmlDocument recurses once per nesting level and caps nothing, so
        // deep nesting is a stack overflow — in a plugin that kills the host DAW.
        // Refused before the parser ever sees it.
        {
            juce::String deep ("<AMPCADE>\n");
            const int levels = Library::kMaxPresetNesting + 500;
            for (int i = 0; i < levels; ++i)
                deep << "<A>";
            for (int i = 0; i < levels; ++i)
                deep << "</A>";
            deep << "\n</AMPCADE>\n";

            auto nested = dir.getChildFile ("nested.ampcade");
            nested.replaceWithText (deep);
            check (Library::parsePresetXml (nested) == nullptr,
                   "a preset nested past the depth ceiling is refused");
        }

        auto ok = dir.getChildFile ("ok.ampcade");
        ok.replaceWithText ("<AMPCADE pedalOrder=\"\"><SLOTS/></AMPCADE>\n");
        check (Library::parsePresetXml (ok) != nullptr, "an ordinary preset still parses");

        // A real preset nests SCENES > SCENE > SLOTS > SLOT and is nowhere near
        // the ceiling — proving the guard cannot reject an honest rig.
        {
            auto normal = dir.getChildFile ("normal-depth.ampcade");
            normal.replaceWithText (
                "<AMPCADE pedalOrder=\"\">\n"
                "  <SLOTS><SLOT id=\"p1\" loaded=\"0\"/></SLOTS>\n"
                "  <SCENES active=\"-1\">\n"
                "    <SCENE idx=\"0\" name=\"Clean\" used=\"1\">\n"
                "      <VALUES/>\n"
                "      <SLOTS><SLOT id=\"p1\" loaded=\"0\"/></SLOTS>\n"
                "    </SCENE>\n"
                "  </SCENES>\n"
                "</AMPCADE>\n");
            check (Library::parsePresetXml (normal) != nullptr,
                   "a normally-nested preset is unaffected by the depth ceiling");
        }

        // A preset aiming slots at the rest of the disk loads with those paths
        // dropped — not followed.
        // p3 is the compatibility case: an absolute path INSIDE the app folder
        // is a player's own pre-relativisation preset and must still resolve.
        const auto legacy = Library::modelsDir().getChildFile ("tone_9/legacy.nam").getFullPathName();
        auto sneaky = dir.getChildFile ("sneaky.ampcade");
        sneaky.replaceWithText (
            "<AMPCADE pedalOrder=\"\">\n"
            "  <SLOTS>\n"
            "    <SLOT id=\"p1\" loaded=\"1\" filePath=\"/etc/passwd\"/>\n"
            "    <SLOT id=\"p2\" loaded=\"1\" filePath=\"../../../../etc/passwd\"/>\n"
            "    <SLOT id=\"p3\" loaded=\"1\" filePath=\"" + legacy + "\"/>\n"
            "  </SLOTS>\n"
            // A scene keeps its OWN copy of the board. Sanitising only the
            // top-level SLOTS misses these entirely — which is how a preset
            // with a spotless SLOTS block still shipped 26 absolute paths.
            "  <SCENES active=\"-1\">\n"
            "    <SCENE idx=\"0\" name=\"Clean\" used=\"1\" hasBoard=\"1\" order=\"\" hidden=\"\">\n"
            "      <VALUES/>\n"
            "      <SLOTS><SLOT id=\"p4\" loaded=\"1\" filePath=\"/etc/passwd\"/></SLOTS>\n"
            "    </SCENE>\n"
            "  </SCENES>\n"
            "</AMPCADE>\n");

        auto p = makeProcessor (standalone);
        check (p->loadPresetFrom (sneaky), "the sneaky preset loads (rather than being followed)");
        pumpMessageLoop();

        // Read back what the slots actually ended up pointing at.
        juce::MemoryBlock after;
        p->getStateInformation (after);
        auto xml = juce::AudioProcessor::getXmlFromBinary (after.getData(), (int) after.getSize());
        check (xml != nullptr, "state round trips after the sneaky preset");
        if (xml != nullptr)
        {
            const auto root = juce::ValueTree::fromXml (*xml);
            int checked = 0;
            bool escaped = false;
            // Walks the rig's SLOTS *and* every scene's copy of the board.
            std::function<void (const juce::ValueTree&)> walk = [&] (const juce::ValueTree& t)
            {
                if (t.hasType ("SLOT"))
                {
                    const auto stored = t.getProperty ("filePath").toString();
                    if (stored.isNotEmpty())
                    {
                        ++checked;
                        const auto resolved = Library::unpackGearPath (stored);
                        if (resolved.isEmpty()
                            || ! juce::File (resolved).isAChildOf (Library::appDataDir()))
                            escaped = true;
                    }
                    return;
                }
                for (int i = 0; i < t.getNumChildren(); ++i)
                    walk (t.getChild (i));
            };
            walk (root);
            check (! escaped, "no slot points outside the app folder after a hostile preset");
            check (checked > 0, "the p3 legacy path survived, so the walk saw something");
        }

        // ---- A preset cannot write a non-finite value into a parameter ------
        // juce::NormalisableRange clamps with jlimit, and jlimit passes NaN
        // straight through: both "NaN < lo" and "hi < NaN" are false. JUCE also
        // parses the literal spelling "nan" into a real quiet_NaN (see
        // CharacterFunctions::readDoubleValue), so this is reachable from a
        // hand-written preset. One NaN latches in every recursive element in the
        // engine and the rig goes silent for good.
        {
            auto poison = dir.getChildFile ("poison.ampcade");
            poison.replaceWithText (
                "<AMPCADE pedalOrder=\"\">\n"
                "  <STATE>\n"
                "    <PARAM id=\"amp_bass\" value=\"nan\"/>\n"
                "    <PARAM id=\"amp_mid\" value=\"inf\"/>\n"
                "    <PARAM id=\"amp_treble\" value=\"-inf\"/>\n"
                "  </STATE>\n"
                "  <SLOTS/>\n"
                "</AMPCADE>\n");

            auto pp = makeProcessor (standalone);
            check (pp->loadPresetFrom (poison), "the poisoned preset loads");
            pumpMessageLoop();

            check (std::isfinite (knob (*pp, "amp_bass")), "a preset cannot leave amp_bass NaN");
            check (std::isfinite (knob (*pp, "amp_mid")), "a preset cannot leave amp_mid +Inf");
            check (std::isfinite (knob (*pp, "amp_treble")), "a preset cannot leave amp_treble -Inf");
        }

        // ---- A preset a player SENDS carries no home directory -------------
        // Local session state may legitimately hold absolute paths (gear
        // imported before adoptGearFile existed). Writing a preset from that
        // state must still produce a file safe to hand to a stranger — in the
        // rig's own SLOTS *and* in every scene's copy of the board, which is
        // the half that actually leaked.
        {
            auto p2 = makeProcessor (standalone);
            juce::MemoryBlock base;
            p2->getStateInformation (base);
            auto sx = juce::AudioProcessor::getXmlFromBinary (base.getData(), (int) base.getSize());
            check (sx != nullptr, "base state decodes");
            if (sx != nullptr)
            {
                auto root = juce::ValueTree::fromXml (*sx);
                const juce::String outside ("/Users/somebody/Desktop/secret-project/tone.nam");

                auto slots = root.getChildWithName ("SLOTS");
                check (slots.isValid() && slots.getNumChildren() > 0, "state has slots to poison");
                slots.getChild (0).setProperty ("loaded", true, nullptr);
                slots.getChild (0).setProperty ("filePath", outside, nullptr);

                // …and the same path buried in a scene's board snapshot.
                juce::ValueTree scenes ("SCENES"), scene ("SCENE"), sslots ("SLOTS"), sslot ("SLOT");
                sslot.setProperty ("id", "p2", nullptr);
                sslot.setProperty ("loaded", true, nullptr);
                sslot.setProperty ("filePath", outside, nullptr);
                sslots.appendChild (sslot, nullptr);
                scene.setProperty ("idx", 0, nullptr);
                scene.setProperty ("name", "Clean", nullptr);
                scene.setProperty ("used", true, nullptr);
                scene.setProperty ("hasBoard", true, nullptr);
                scene.setProperty ("order", "", nullptr);
                scene.setProperty ("hidden", "", nullptr);
                scene.appendChild (juce::ValueTree ("VALUES"), nullptr);
                scene.appendChild (sslots, nullptr);
                scenes.setProperty ("active", -1, nullptr);
                scenes.appendChild (scene, nullptr);
                if (auto oldScenes = root.getChildWithName ("SCENES"); oldScenes.isValid())
                    root.removeChild (oldScenes, nullptr);
                root.appendChild (scenes, nullptr);

                juce::MemoryBlock poisoned;
                if (auto out = root.createXml())
                    juce::AudioProcessor::copyXmlToBinary (*out, poisoned);

                // Restored as LOCAL state, so the paths are kept as-is…
                auto p3 = makeProcessor (standalone);
                p3->setStateInformation (poisoned.getData(), (int) poisoned.getSize());
                pumpMessageLoop();

                // …but a preset written from it must not carry them.
                auto shared = dir.getChildFile ("shared.ampcade");
                dir.createDirectory();
                check (p3->savePresetTo (shared, "{}"), "preset writes");
                const auto text = shared.loadFileAsString();
                check (! text.contains (outside), "a saved preset carries no absolute gear path");
                check (! text.contains ("/Users/"), "a saved preset carries no home directory");

                // The same for the scene auto-sync, which writes straight into
                // an existing preset file and bypassed savePresetTo entirely.
                check (p3->syncScenesToPresetFile (shared), "scene sync writes");
                const auto synced = shared.loadFileAsString();
                check (! synced.contains ("/Users/"),
                       "a scene auto-sync leaves no home directory in the preset");
            }
        }

        dir.deleteRecursively();
    }

    // ---- Pathless gear resolves from the download cache, offline -----------
    // Factory presets — and every preset anyone shares — identify gear by
    // TONE3000 id and ship no file paths. Resolving one used to require a live
    // connection, so an expired token made a rig whose files were already
    // downloaded come back as the built-in amp and cab: it reads as data loss
    // and is not. The cache is keyed by the very ids the preset is asking for.
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("ampcade-cache-test");
        dir.createDirectory();

        const int toneId = 999001, modelId = 888002;
        const juce::String modelName ("Test Cab IR");

        // A real, readable IR sitting where a download would have left it.
        auto cacheDir = Library::irsDir().getChildFile ("tone_" + juce::String (toneId));
        cacheDir.createDirectory();
        auto cached = cacheDir.getChildFile (Library::sanitizeFileName (modelName)
                                             + "_" + juce::String (modelId) + ".wav");
        {
            juce::AudioBuffer<float> ir (1, 2048);
            ir.clear();
            for (int i = 0; i < 2048; ++i)   // short, front-loaded: a speaker, not a space
                ir.setSample (0, i, std::exp (-(float) i / 120.0f) * (i == 0 ? 1.0f : 0.25f));

            juce::WavAudioFormat wav;
            cached.deleteFile();
            std::unique_ptr<juce::FileOutputStream> os (cached.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> w (
                wav.createWriterFor (os.get(), 48000.0, 1, 24, {}, 0));
            check (w != nullptr, "the cached IR writer opens");
            if (w != nullptr)
            {
                os.release();            // the writer owns the stream now
                w->writeFromAudioSampleBuffer (ir, 0, ir.getNumSamples());
            }
        }
        check (cached.existsAsFile(), "the cached IR exists on disk");

        auto pathless = dir.getChildFile ("pathless.ampcade");
        pathless.replaceWithText (
            "<AMPCADE pedalOrder=\"\">\n"
            "  <SLOTS>\n"
            "    <SLOT id=\"cab\" loaded=\"1\" title=\"Cached Cab\" author=\"t\" gear=\"cab\""
            " modelName=\"" + modelName + "\" filePath=\"\""
            " toneId=\"" + juce::String (toneId) + "\""
            " modelId=\"" + juce::String (modelId) + "\"/>\n"
            "  </SLOTS>\n"
            "</AMPCADE>\n");

        auto pc = makeProcessor (standalone);
        check (pc->loadPresetFrom (pathless), "the pathless preset loads");
        pumpMessageLoop();

        // Not connected to TONE3000 in this test — there is no token — so the
        // only way the slot can end up resolved is straight off the disk.
        juce::MemoryBlock after;
        pc->getStateInformation (after);
        auto xml = juce::AudioProcessor::getXmlFromBinary (after.getData(), (int) after.getSize());
        check (xml != nullptr, "state round trips after the pathless preset");
        if (xml != nullptr)
        {
            auto root = juce::ValueTree::fromXml (*xml);
            auto slots = root.getChildWithName ("SLOTS");
            auto cab = slots.getChildWithProperty ("id", "cab");
            check (cab.isValid(), "the cab slot survived the pathless preset");
            check (cab.getProperty ("filePath").toString().isNotEmpty(),
                   "pathless gear already in the cache resolves with no connection");
            check (! (bool) cab.getProperty ("missing", false),
                   "cached gear is not reported missing");
        }

        cacheDir.deleteRecursively();
        dir.deleteRecursively();
    }

    // ---- A preset's gear is shared by all of its scenes --------------------
    // A capture one scene uses has to stay reachable from every scene: on the
    // board where that scene puts it, in the RACK everywhere else. The merge
    // used to read the live rig only, so landing straight on a scene that did
    // not use the pedal left nothing to merge and the pedal was absent from
    // the preset entirely — not on the board, not in the rack.
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("ampcade-scene-share-test");
        dir.createDirectory();

        // Scene 1 does NOT use the pedal; scene 2 does. Order matters: a preset
        // load lands on scene 1, so the pedal is never in the live rig at any
        // point. Sourcing the merge from the live rig alone therefore has
        // nothing to offer, which is precisely the hole — the pedal is on no
        // board and in no rack, and the preset has silently lost it.
        auto shared = dir.getChildFile ("shared.ampcade");
        shared.replaceWithText (
            "<AMPCADE pedalOrder=\"\">\n"
            "  <SLOTS/>\n"
            "  <SCENES active=\"-1\">\n"
            // A scene with no VALUES is treated as unused (scenesFromTree), so
            // both carry a real knob.
            "    <SCENE idx=\"0\" name=\"Clean\" used=\"1\" hasBoard=\"1\" order=\"amp cab\" hidden=\"\">\n"
            "      <VALUES amp_bass=\"4.0\"/>\n"
            "      <SLOTS/>\n"
            "    </SCENE>\n"
            "    <SCENE idx=\"1\" name=\"Rhythm\" used=\"1\" hasBoard=\"1\" order=\"p1 amp cab\" hidden=\"\">\n"
            "      <VALUES amp_bass=\"6.0\"/>\n"
            "      <SLOTS><SLOT id=\"p1\" loaded=\"1\" title=\"TS808\" author=\"a\" gear=\"pedal\""
            " modelName=\"TS808\" filePath=\"\" toneId=\"30104\" modelId=\"137699\"/></SLOTS>\n"
            "    </SCENE>\n"
            "  </SCENES>\n"
            "</AMPCADE>\n");

        auto ps = makeProcessor (standalone);
        check (ps->loadPresetFrom (shared), "the shared-gear preset loads");
        pumpMessageLoop();   // lands on scene 1 — the one WITHOUT the pedal

        check (ps->getHiddenSlots().contains ("p1"),
               "a pedal another scene uses sits in this scene's rack");

        juce::MemoryBlock st;
        ps->getStateInformation (st);
        auto sx = juce::AudioProcessor::getXmlFromBinary (st.getData(), (int) st.getSize());
        check (sx != nullptr, "state round trips after the scene recall");
        if (sx != nullptr)
        {
            auto root = juce::ValueTree::fromXml (*sx);
            auto p1 = root.getChildWithName ("SLOTS").getChildWithProperty ("id", "p1");
            check (p1.isValid() && (int) p1.getProperty ("toneId", 0) == 30104,
                   "the racked pedal keeps its TONE3000 identity, so it can be reloaded");
        }

        // And the scene that DOES use it still puts it on the board.
        ps->sceneRecall (1);
        pumpMessageLoop();
        check (! ps->getHiddenSlots().contains ("p1"),
               "the scene that uses the pedal still has it on the board");

        dir.deleteRecursively();
    }

    std::cout << (failures == 0 ? "STATE PASS\n" : "STATE FAIL\n");
    return failures == 0 ? 0 : 1;
}
