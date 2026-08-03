#include "PluginEditor.h"
#include "Library.h"
#include "Version.h"
#include "BinaryData.h"

// For the standalone only: reach the app shell to (a) unmute the audio input —
// JUCE's feedback-protection default mutes it, which for a guitar amp reads as
// "the plugin is broken" — and (b) open the audio settings dialog from the UI's
// "audio isn't running" banner. getInstance() is null inside VST3/AU hosts.
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

#if JUCE_MAC
 #include <objc/message.h>
#endif

namespace ampcade
{
#if JUCE_MAC
// Depth-first search for the WKWebView inside our peer's NSView tree.
static void* findWkWebView (void* nsView)
{
    using ObjcId = void*;
    auto getObj  = (ObjcId (*) (ObjcId, SEL)) objc_msgSend;
    auto getBool = (signed char (*) (ObjcId, SEL, ObjcId)) objc_msgSend;
    auto getUL   = (unsigned long (*) (ObjcId, SEL)) objc_msgSend;
    auto getAt   = (ObjcId (*) (ObjcId, SEL, unsigned long)) objc_msgSend;

    if (getBool ((ObjcId) nsView, sel_registerName ("isKindOfClass:"), (ObjcId) objc_getClass ("WKWebView")))
        return nsView;

    ObjcId subviews = getObj ((ObjcId) nsView, sel_registerName ("subviews"));
    const auto n = subviews != nullptr ? getUL (subviews, sel_registerName ("count")) : 0;
    for (unsigned long i = 0; i < n; ++i)
        if (auto* hit = findWkWebView (getAt (subviews, sel_registerName ("objectAtIndex:"), i)))
            return hit;
    return nullptr;
}
#endif
// Must stay in sync with PARAM_DEFS in ui/index.html — a name missing here gets
// no relay, so its knob silently never reaches the parameter.
// The per-pedal names are generated from kNumPedals rather than spelled out, so
// raising the pedal count cannot leave a slot with a dead knob here.
static const juce::StringArray& sliderNames()
{
    static const juce::StringArray names = []
    {
        juce::StringArray n {
            "in_gain", "out_gain", "gate",
            "drv_gain", "drv_tone", "drv_level",
            "cho_rate", "cho_depth", "cho_mix"
        };
        for (int i = 1; i <= AmpCadeProcessor::kNumPedals; ++i)
            for (auto* k : { "_drive", "_tone", "_level" })
                n.add ("p" + juce::String (i) + k);
        n.addArray ({
            "amp_gain", "amp_bass", "amp_mid", "amp_treble", "amp_vol", "cab_mix", "air_mix", "air_level",
            "air_bass", "air_treble", "eq_bass", "eq_mid", "eq_treble",
            "b1_gain", "b2_gain", "b3_gain", "cmp_comp", "cmp_att", "cmp_level",
            "dly_time", "dly_fb", "dly_mix", "dly_bpm", "dly_sync", "rev_size", "rev_mix", "loop_vol", "loop_imp"
        });
        return n;
    }();
    return names;
}

static const juce::StringArray& toggleNames()
{
    static const juce::StringArray names = []
    {
        juce::StringArray n { "drv_on", "cho_on", "tun_on", "tun_mute", "eq_on",
                              "b1_on", "b2_on", "b3_on", "cmp_on" };
        for (int i = 1; i <= AmpCadeProcessor::kNumPedals; ++i)
            n.add ("p" + juce::String (i) + "_on");
        n.addArray ({ "amp_on", "cab_on", "air_on", "dly_on", "rev_on" });
        return n;
    }();
    return names;
}

static juce::var parseArg (const juce::Array<juce::var>& args, int index = 0)
{
    if (index >= args.size())
        return {};
    const auto& a = args.getReference (index);
    if (a.isString())
    {
        auto parsed = juce::JSON::parse (a.toString());
        return parsed.isVoid() ? a : parsed;
    }
    return a;
}

static juce::String jsonify (const juce::var& v) { return juce::JSON::toString (v, true); }

// juce::String's plain char* ctor reads bytes as Latin-1 — UTF-8 literals
// (em dashes, ellipses) must come through here or they render as mojibake.
static juce::String u8 (const char* text)
{
    return juce::String (juce::CharPointer_UTF8 (text));
}

//==============================================================================
AmpCadeEditor::AmpCadeEditor (AmpCadeProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    for (const auto& name : sliderNames())
        sliderRelays.push_back (std::make_unique<juce::WebSliderRelay> (name));
    for (const auto& name : toggleNames())
        toggleRelays.push_back (std::make_unique<juce::WebToggleButtonRelay> (name));

    web = std::make_unique<juce::WebBrowserComponent> (makeOptions());
    addAndMakeVisible (*web);

    int i = 0;
    for (const auto& name : sliderNames())
    {
        if (auto* param = proc.apvts.getParameter (name))
            sliderAttachments.push_back (std::make_unique<juce::WebSliderParameterAttachment> (*param, *sliderRelays[(size_t) i], nullptr));
        ++i;
    }
    i = 0;
    for (const auto& name : toggleNames())
    {
        if (auto* param = proc.apvts.getParameter (name))
            toggleAttachments.push_back (std::make_unique<juce::WebToggleButtonParameterAttachment> (*param, *toggleRelays[(size_t) i], nullptr));
        ++i;
    }

    proc.stateBroadcaster.addChangeListener (this);

    proc.onBusy = [this] (juce::String slot, bool busy, juce::String label)
    {
        auto o = makeObj();
        o->setProperty ("slot", slot);
        o->setProperty ("busy", busy);
        o->setProperty ("label", label);
        web->emitEventIfBrowserIsVisible ("busy", juce::var (o.get()));
    };

    proc.onToast = [this] (juce::String msg, juce::String kind)
    {
        auto o = makeObj();
        o->setProperty ("msg", msg);
        o->setProperty ("kind", kind);
        web->emitEventIfBrowserIsVisible ("toast", juce::var (o.get()));
    };

    web->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

    // Standalone: a guitar rig with a muted input is just a silent app. The
    // shell's own "mute input" toggle still works — this only flips the default.
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
        holder->getMuteInputValue().setValue (false);

    setResizable (true, true);
    setResizeLimits (920, 560, 4000, 2600);
    setSize (1220, 700);

    startTimerHz (24);
}

AmpCadeEditor::~AmpCadeEditor()
{
    proc.stateBroadcaster.removeChangeListener (this);
    proc.onBusy = nullptr;
    proc.onToast = nullptr;
}

void AmpCadeEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff15161b));
}

void AmpCadeEditor::resized()
{
    web->setBounds (getLocalBounds());
    if (authOverlay != nullptr)
        authOverlay->setBounds (getLocalBounds());
}

void AmpCadeEditor::parentHierarchyChanged()
{
    // Standalone: decide the title-bar style BEFORE the window is first shown.
    // This fires from the shell's setContentOwned; the window is already
    // peered (DocumentWindow's ctor adds it to the desktop) but still hidden,
    // so the recreation setUsingNativeTitleBar triggers happens off-screen.
    // Flipping it later (the old first-timer-tick path) destroyed and re-keyed
    // a just-shown NSWindow mid app-activation on every launch, which is what
    // left the app half-focused until a couple of cmd-tabs.
    if (juce::StandalonePluginHolder::getInstance() == nullptr)
        return;

    if (auto* dw = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent()))
        if (! dw->isShowing() && ! dw->isUsingNativeTitleBar())
        {
            dw->setTitleBarButtonsRequired (juce::DocumentWindow::allButtons, false);
            dw->setUsingNativeTitleBar (true);
        }
}

void AmpCadeEditor::configureStandaloneWindow()
{
    if (standaloneWindowConfigured || juce::StandalonePluginHolder::getInstance() == nullptr)
        return;

    auto* top = getTopLevelComponent();
    if (top == nullptr || top == this || top->getPeer() == nullptr)
        return;

    standaloneWindowConfigured = true;

    // The shell's "Options" pill duplicated the app's own menu ("why are there
    // an Options AND a Settings?"): audio settings now live in the app menu,
    // presets in the topbar dropdown. Hide the pill.
    for (int i = 0; i < top->getNumChildComponents(); ++i)
        if (auto* b = dynamic_cast<juce::TextButton*> (top->getChildComponent (i)))
            if (b->getButtonText().startsWith ("Options"))
                b->setVisible (false);

    // Idempotent fallback: parentHierarchyChanged normally set this before the
    // window ever reached the desktop (recreating a live peer here re-keys a
    // brand-new NSWindow mid-activation — the old first-launch focus bug).
    if (auto* dw = dynamic_cast<juce::DocumentWindow*> (top))
    {
        dw->setTitleBarButtonsRequired (juce::DocumentWindow::allButtons, false);
        dw->setUsingNativeTitleBar (true);
    }

   #if JUCE_MAC
    // Opt the window into native full screen (green traffic light). JUCE
    // windows default to zoom-only; NSWindowCollectionBehaviorFullScreenPrimary
    // is 1 << 7. Re-fetch the peer: the title-bar switch above recreated it.
    if (top->getPeer() != nullptr)
    if (void* nsView = top->getPeer()->getNativeHandle())
    {
        using ObjcId = void*;
        auto getObj = (ObjcId (*) (ObjcId, SEL)) objc_msgSend;
        auto getUL = (unsigned long (*) (ObjcId, SEL)) objc_msgSend;
        auto setUL = (void (*) (ObjcId, SEL, unsigned long)) objc_msgSend;

        if (ObjcId window = getObj ((ObjcId) nsView, sel_registerName ("window")))
        {
            const auto behavior = getUL (window, sel_registerName ("collectionBehavior"));
            setUL (window, sel_registerName ("setCollectionBehavior:"), behavior | (1UL << 7));
        }
    }
   #endif
}

void AmpCadeEditor::timerCallback()
{
    configureStandaloneWindow();

   #if JUCE_MAC
    // macOS: every window activation makes JUCE's content NSView first
    // responder (NSViewComponentPeer::becomeKeyWindow -> grabFocus), which
    // dead-ends every keystroke in NSBeep — the page's shortcuts never fire
    // until the user clicks the web content. Detect exactly that state
    // (first responder == the peer view) and hand first responder straight to
    // the WKWebView. Deliberately AppKit-level: Component::grabKeyboardFocus
    // always re-firsts the PEER view before JUCE hands over, so the WebView
    // resigns, JUCE's direction flag poisons, and the two fight forever —
    // that was the "every key beeps" regression. Standalone only; never while
    // the OAuth overlay owns the window, never when another window is key
    // (file choosers), never when a page text field holds focus (the first
    // responder is then a WKContentView, not the peer view).
    if (juce::StandalonePluginHolder::getInstance() != nullptr && authOverlay == nullptr)
        if (auto* peer = getPeer(); peer != nullptr && peer->isFocused())
            if (void* nsView = peer->getNativeHandle())
            {
                using ObjcId = void*;
                auto getObj  = (ObjcId (*) (ObjcId, SEL)) objc_msgSend;
                auto setResp = (signed char (*) (ObjcId, SEL, ObjcId)) objc_msgSend;
                if (ObjcId window = getObj ((ObjcId) nsView, sel_registerName ("window")))
                    if (getObj (window, sel_registerName ("firstResponder")) == nsView)
                        if (void* wk = findWkWebView (nsView))
                            setResp (window, sel_registerName ("makeFirstResponder:"), (ObjcId) wk);
            }
   #endif

    // Peaks are exchanged for zero: the processor holds the loudest sample since
    // the last read, so nothing falls between polls. The floor is below the
    // meter's own floor so a quiet input still lands on the scale.
    const auto db = [] (float gain) { return juce::Decibels::gainToDecibels (gain, -96.0f); };

    auto o = makeObj();
    o->setProperty ("in", db (proc.meterInRms.load()));
    o->setProperty ("inPeak", db (proc.meterIn.exchange (0.0f)));
    o->setProperty ("out", db (proc.meterOut.exchange (0.0f)));
    // Peak before the soft ceiling: out < outPre means the clipper is working.
    o->setProperty ("outPre", db (proc.meterOutPre.exchange (0.0f)));
    // Rides along at 24 Hz so the active scene chip can show unsaved changes
    // as soon as a knob moves (knob moves alone never push a full state).
    o->setProperty ("sceneDirty", proc.isActiveSceneDirty());

    // Looper status rides along with the meters (same 24 Hz cadence the UI
    // already listens on, no extra event plumbing).
    auto lp = makeObj();
    lp->setProperty ("st", proc.looper.getState());
    lp->setProperty ("len", proc.looper.lengthSeconds());
    lp->setProperty ("pos", proc.looper.positionSeconds());
    lp->setProperty ("undo", proc.looper.canUndoLayer());
    lp->setProperty ("redo", proc.looper.canRedoLayer());
    lp->setProperty ("unerase", proc.looper.canUnerase());
    lp->setProperty ("imp", proc.looper.hasImportedBase());
    o->setProperty ("loop", juce::var (lp.get()));

    o->setProperty ("tunerHz", proc.tuner.freq.load());

    // Is the audio engine actually calling us? 0 = never has.
    const auto lastBlock = proc.lastBlockMs.load();
    o->setProperty ("audioDead", lastBlock == 0
                                     || juce::Time::getMillisecondCounter() - lastBlock > 700);

    web->emitEventIfBrowserIsVisible ("meters", juce::var (o.get()));
}

void AmpCadeEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    pushState();
}

void AmpCadeEditor::pushState()
{
    web->emitEventIfBrowserIsVisible ("stateChanged", proc.getUiState());
}

//==============================================================================
std::optional<juce::WebBrowserComponent::Resource> AmpCadeEditor::getResource (const juce::String& url)
{
    const auto make = [] (const char* data, int size, const char* mime)
    {
        std::vector<std::byte> bytes ((size_t) size);
        std::memcpy (bytes.data(), data, (size_t) size);
        return juce::WebBrowserComponent::Resource { std::move (bytes), juce::String (mime) };
    };

    const auto path = url == "/" ? juce::String ("/index.html") : url;

    // The charset is mandatory: WKWebView decodes a custom-scheme response with an
    // unqualified "text/html" as Latin-1 and ignores the document's own <meta charset>,
    // so every non-ASCII character (em dashes, ellipses, arrows) rendered as mojibake.
    if (path == "/index.html")
        return make (BinaryData::index_html, BinaryData::index_htmlSize, "text/html; charset=utf-8");
    if (path == "/juce/index.js")
        return make (BinaryData::index_js, BinaryData::index_jsSize, "text/javascript; charset=utf-8");

    return std::nullopt;
}

juce::WebBrowserComponent::Options AmpCadeEditor::makeOptions()
{
    using Options = juce::WebBrowserComponent::Options;

    auto safeComplete = [this] (auto complete) // only touch the webview if the editor is alive
    {
        return [sp = juce::Component::SafePointer<AmpCadeEditor> (this), complete] (const juce::var& v)
        {
            if (sp != nullptr)
                complete (jsonify (v));
        };
    };

    auto options = Options{}
        .withBackend (Options::Backend::webview2)
        .withWinWebView2Options (Options::WinWebView2{}.withUserDataFolder (
            juce::File::getSpecialLocation (juce::File::tempDirectory)))
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withResourceProvider ([this] (const auto& url) { return getResource (url); });

    for (auto& r : sliderRelays)
        options = options.withOptionsFrom (*r);
    for (auto& r : toggleRelays)
        options = options.withOptionsFrom (*r);

    return options
        .withNativeFunction ("getState", [this] (auto&, auto complete)
        {
            complete (jsonify (proc.getUiState()));
        })
        .withNativeFunction ("t3kSearch", [this, safeComplete] (auto& args, auto complete)
        {
            proc.t3k.search (parseArg (args), safeComplete (complete));
        })
        .withNativeFunction ("t3kModels", [this, safeComplete] (auto& args, auto complete)
        {
            proc.t3k.models ((int) parseArg (args), safeComplete (complete));
        })
        .withNativeFunction ("t3kLoad", [this, safeComplete] (auto& args, auto complete)
        {
            proc.beginUserAction();
            auto v = parseArg (args);
            proc.requestT3kLoad (v.getProperty ("slot", "").toString(),
                                 (int) v.getProperty ("toneId", 0),
                                 (int) v.getProperty ("modelId", 0),
                                 safeComplete (complete));
        })
        .withNativeFunction ("clearSlot", [this] (auto& args, auto complete)
        {
            proc.beginUserAction();
            proc.clearSlotByName (parseArg (args).toString());
            complete (jsonify (okResult()));
        })
        .withNativeFunction ("importFile", [this, safeComplete] (auto& args, auto complete)
        {
            chooseFileFor (parseArg (args).toString(), safeComplete (complete));
        })
        .withNativeFunction ("t3kConnect", [this, safeComplete] (auto&, auto complete)
        {
            startConnect (safeComplete (complete));
        })
        .withNativeFunction ("t3kDisconnect", [this] (auto&, auto complete)
        {
            proc.t3k.disconnect();
            pushState();
            complete (jsonify (okResult()));
        })
        .withNativeFunction ("getSettings", [this] (auto&, auto complete)
        {
            auto o = makeObj();
            o->setProperty ("ok", true);
            o->setProperty ("clientId", proc.t3k.getClientId());
            o->setProperty ("clientIdSet", proc.t3k.getClientId().isNotEmpty());
            o->setProperty ("libraryDir", Library::appDataDir().getFullPathName());
            o->setProperty ("version", kAppVersion);
            complete (jsonify (juce::var (o.get())));
        })
        .withNativeFunction ("setPedalOrder", [this] (auto& args, auto complete)
        {
            // The var must outlive getArray(): the pointer belongs to it, and a
            // temporary here dies at the end of the condition, before the loop runs.
            // That dangled and reorders silently failed with "couldn't apply".
            const auto arg = parseArg (args);
            juce::StringArray names;
            if (auto* arr = arg.getArray())
                for (const auto& v : *arr)
                    names.add (v.toString());

            proc.beginUserAction();
            complete (jsonify (proc.setChainOrderFromNames (names)
                                   ? okResult()
                                   : errResult ("Couldn't apply that chain order")));
        })
        .withNativeFunction ("setHidden", [this] (auto& args, auto complete)
        {
            const auto arg = parseArg (args); // named local — see setPedalOrder
            juce::StringArray names;
            if (auto* arr = arg.getArray())
                for (const auto& v : *arr)
                    names.add (v.toString());
            proc.beginUserAction();
            proc.setHiddenSlots (names);
            complete (jsonify (okResult()));
        })
        .withNativeFunction ("sceneSave", [this] (auto& args, auto complete)
        {
            proc.beginUserAction();
            proc.sceneSave ((int) parseArg (args).getProperty ("idx", -1));
            complete (jsonify (okResult()));
        })
        .withNativeFunction ("sceneAdd", [this] (auto& args, auto complete)
        {
            proc.beginUserAction();
            proc.sceneAdd (parseArg (args).getProperty ("name", "").toString());
            complete (jsonify (okResult()));
        })
        .withNativeFunction ("sceneDelete", [this] (auto& args, auto complete)
        {
            proc.beginUserAction();
            proc.sceneDelete ((int) parseArg (args).getProperty ("idx", -1));
            complete (jsonify (okResult()));
        })
        // Scene edits auto-persist into the active preset (UI calls this after
        // every scene mutation when a named preset is loaded).
        .withNativeFunction ("syncScenesToPreset", [this] (auto& args, auto complete)
        {
            const auto name = Library::sanitizeFileName (
                parseArg (args).getProperty ("name", "").toString());
            const auto f = Library::presetsDir().getChildFile (name + ".ampcade");
            complete (jsonify (f.existsAsFile() && proc.syncScenesToPresetFile (f)
                                   ? okResult()
                                   : errResult ("Couldn't update the preset's scenes")));
        })
        .withNativeFunction ("sceneMove", [this] (auto& args, auto complete)
        {
            const auto v = parseArg (args);
            proc.beginUserAction();
            proc.sceneMove ((int) v.getProperty ("from", -1), (int) v.getProperty ("to", -1));
            complete (jsonify (okResult()));
        })
        .withNativeFunction ("looperConfig", [this] (auto& args, auto complete)
        {
            const auto v = parseArg (args);
            proc.looper.setConfig ((int) v.getProperty ("bpm", proc.looper.getBpm()),
                                   (int) v.getProperty ("beats", proc.looper.getBeats()),
                                   (bool) v.getProperty ("playAfterRec", proc.looper.getPlayAfterRec()),
                                   (bool) v.getProperty ("countIn", proc.looper.getCountIn()),
                                   (bool) v.getProperty ("ringOut", proc.looper.getRingOut()));
            complete (jsonify (okResult()));
        })
        .withNativeFunction ("sceneRecall", [this] (auto& args, auto complete)
        {
            proc.beginUserAction();
            proc.sceneRecall ((int) parseArg (args).getProperty ("idx", -1));
            complete (jsonify (okResult()));
        })
        .withNativeFunction ("sceneClear", [this] (auto& args, auto complete)
        {
            proc.beginUserAction();
            proc.sceneClear ((int) parseArg (args).getProperty ("idx", -1));
            complete (jsonify (okResult()));
        })
        .withNativeFunction ("sceneRename", [this] (auto& args, auto complete)
        {
            const auto v = parseArg (args);
            proc.beginUserAction();
            proc.sceneRename ((int) v.getProperty ("idx", -1), v.getProperty ("name", "").toString());
            complete (jsonify (okResult()));
        })
        .withNativeFunction ("openAudioSettings", [] (auto&, auto complete)
        {
            if (auto* holder = juce::StandalonePluginHolder::getInstance())
            {
                holder->showAudioSettingsDialog();
                complete (jsonify (okResult()));
            }
            else
            {
                complete (jsonify (errResult (u8 ("Audio is driven by your host (DAW) — check its audio device settings."))));
            }
        })
        .withNativeFunction ("looperTap", [this] (auto&, auto complete)
        {
            proc.looper.tap();
            complete (jsonify (okResult()));
        })
        .withNativeFunction ("looperStop", [this] (auto&, auto complete)
        {
            proc.looper.stopLoop();
            complete (jsonify (okResult()));
        })
        .withNativeFunction ("looperErase", [this] (auto&, auto complete)
        {
            proc.looper.erase();
            complete (jsonify (okResult()));
        })
        .withNativeFunction ("looperUnerase", [this] (auto&, auto complete)
        {
            proc.looper.unerase();
            complete (jsonify (okResult()));
        })
        .withNativeFunction ("looperUndo", [this] (auto&, auto complete)
        {
            proc.looper.undoLayer();
            complete (jsonify (okResult()));
        })
        .withNativeFunction ("looperRedo", [this] (auto&, auto complete)
        {
            proc.looper.redoLayer();
            complete (jsonify (okResult()));
        })
        // One click keeps the WHOLE session: the mixed loop plus every layer
        // stem, moved out of the throwaway Unsaved area in one go. No chooser —
        // most loops are noodles; the ones worth keeping deserve zero friction.
        .withNativeFunction ("looperSave", [this] (auto&, auto complete)
        {
            const auto dir = proc.looper.saveSession();
            if (dir == juce::File{})
            {
                complete (jsonify (errResult ("No loop to save yet")));
                return;
            }
            auto o = makeObj();
            o->setProperty ("ok", true);
            o->setProperty ("folder", dir.getFileName());
            complete (jsonify (juce::var (o.get())));
        })
        .withNativeFunction ("undo", [this] (auto&, auto complete)
        {
            auto o = makeObj();
            o->setProperty ("ok", true);
            o->setProperty ("did", proc.undo());
            complete (jsonify (juce::var (o.get())));
        })
        .withNativeFunction ("redo", [this] (auto&, auto complete)
        {
            auto o = makeObj();
            o->setProperty ("ok", true);
            o->setProperty ("did", proc.redo());
            complete (jsonify (juce::var (o.get())));
        })
        .withNativeFunction ("newPreset", [this] (auto&, auto complete)
        {
            proc.beginUserAction();     // the old rig stays one Cmd+Z away
            proc.resetRigToDefault();
            complete (jsonify (okResult()));
        })
        .withNativeFunction ("moveIr", [this] (auto& args, auto complete)
        {
            const auto dest = parseArg (args).getProperty ("to", "").toString();
            proc.beginUserAction();
            complete (jsonify (proc.moveIrTo (dest)
                                   ? okResult()
                                   : errResult ("Nothing to move there")));
        })
        // ---- named presets: the topbar dropdown works the presets folder
        // directly, no file-chooser round trip. importPresets copies outside
        // files into that folder; revealPresets opens it for sharing out.
        .withNativeFunction ("listPresets", [] (auto&, auto complete)
        {
            juce::Array<juce::File> files;
            Library::presetsDir().findChildFiles (files, juce::File::findFiles, false, "*.ampcade");
            files.sort();
            juce::Array<juce::var> names;
            for (const auto& f : files)
                names.add (f.getFileNameWithoutExtension());
            auto o = makeObj();
            o->setProperty ("ok", true);
            o->setProperty ("presets", names);
            complete (jsonify (juce::var (o.get())));
        })
        .withNativeFunction ("savePresetAs", [this] (auto& args, auto complete)
        {
            const auto v = parseArg (args);
            const auto name = Library::sanitizeFileName (v.getProperty ("name", "").toString());
            if (name.isEmpty() || name == "untitled")
            {
                complete (jsonify (errResult ("Give the preset a name first")));
                return;
            }
            const auto f = Library::presetsDir().getChildFile (name + ".ampcade");
            const auto designs = v.getProperty ("designs", "").toString();
            complete (jsonify (proc.savePresetTo (f, designs) ? okResult()
                                                              : errResult ("Couldn't save the preset")));
        })
        .withNativeFunction ("loadPresetNamed", [this] (auto& args, auto complete)
        {
            const auto name = Library::sanitizeFileName (
                parseArg (args).getProperty ("name", "").toString());
            const auto f = Library::presetsDir().getChildFile (name + ".ampcade");
            if (f.existsAsFile())
                proc.beginUserAction();
            complete (jsonify (f.existsAsFile() && proc.loadPresetFrom (f)
                                   ? okResult()
                                   : errResult ("Couldn't read that preset")));
        })
        .withNativeFunction ("deletePresetNamed", [] (auto& args, auto complete)
        {
            // Soft delete: the file moves into .trash so Cmd/Ctrl+Z can bring
            // it back (restorePreset). listPresets never sees the subfolder.
            const auto name = Library::sanitizeFileName (
                parseArg (args).getProperty ("name", "").toString());
            const auto f = Library::presetsDir().getChildFile (name + ".ampcade");
            if (! f.existsAsFile())
            {
                complete (jsonify (errResult ("Couldn't delete that preset")));
                return;
            }
            auto trashDir = Library::presetsDir().getChildFile (".trash");
            trashDir.createDirectory();
            auto dest = trashDir.getChildFile (f.getFileName());
            if (dest.existsAsFile())
                dest = dest.getNonexistentSibling();
            if (! f.moveFileTo (dest))
            {
                complete (jsonify (errResult ("Couldn't delete that preset")));
                return;
            }
            auto o = makeObj();
            o->setProperty ("ok", true);
            o->setProperty ("trashed", dest.getFileName());
            complete (jsonify (juce::var (o.get())));
        })
        .withNativeFunction ("restorePreset", [] (auto& args, auto complete)
        {
            const auto v = parseArg (args);
            const auto trashed = v.getProperty ("trashed", "").toString();
            const auto name = Library::sanitizeFileName (v.getProperty ("name", "").toString());
            const auto trashDir = Library::presetsDir().getChildFile (".trash");
            const auto src = trashDir.getChildFile (trashed);
            // `trashed` must be a bare filename inside .trash — never a path.
            // (getChildFile resolves ".." and absolute paths, which would let
            // this fn move arbitrary files; sanitizeFileName can't be used
            // here because it strips the "(2)" that uniquified trash names.)
            if (trashed.isEmpty() || name.isEmpty()
                || src.getFileName() != trashed || ! src.isAChildOf (trashDir)
                || ! src.existsAsFile())
            {
                complete (jsonify (errResult ("That preset is gone for good")));
                return;
            }
            auto dest = Library::presetsDir().getChildFile (name + ".ampcade");
            if (dest.existsAsFile())            // a new preset took the name meanwhile
                dest = dest.getNonexistentSibling();
            if (! src.moveFileTo (dest))
            {
                complete (jsonify (errResult ("Couldn't restore that preset")));
                return;
            }
            auto o = makeObj();
            o->setProperty ("ok", true);
            o->setProperty ("name", dest.getFileNameWithoutExtension());
            complete (jsonify (juce::var (o.get())));
        })
        .withNativeFunction ("looperImport", [this, safeComplete] (auto&, auto complete)
        {
            auto done = safeComplete (complete);
            fileChooser = std::make_unique<juce::FileChooser> ("Import loop",
                juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a");
            fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                      [this, done] (const juce::FileChooser& fc)
                                      {
                                          auto f = fc.getResult();
                                          if (f == juce::File{})
                                          {
                                              done (errResult ("cancelled"));
                                              return;
                                          }
                                          juce::String err;
                                          done (proc.looper.importLoopFile (f, err, proc.liveLoudRms.load())
                                                    ? okResult()
                                                    : errResult (err));
                                      });
        })
        .withNativeFunction ("looperFolder", [this] (auto&, auto complete)
        {
            // The current session's stems if there are any, else the Loops root.
            auto dir = proc.looper.currentSessionDir();
            if (dir == juce::File{} || ! dir.isDirectory())
                dir = proc.looper.getLayersRoot();
            dir.createDirectory();
            dir.revealToUser();
            complete (jsonify (okResult()));
        })
        // Small UI preference store (theme, per-tone cosmetic styles). Lives in
        // its own JSON file so it survives sessions and is shared by instances.
        .withNativeFunction ("getPrefs", [] (auto&, auto complete)
        {
            auto o = makeObj();
            o->setProperty ("ok", true);
            o->setProperty ("prefs", Library::readJson (Library::appDataDir().getChildFile ("ui-prefs.json")));
            complete (jsonify (juce::var (o.get())));
        })
        .withNativeFunction ("setPrefs", [] (auto& args, auto complete)
        {
            const auto v = parseArg (args);
            const bool ok = Library::writeJson (Library::appDataDir().getChildFile ("ui-prefs.json"), v);
            complete (jsonify (ok ? okResult() : errResult ("Couldn't save preferences")));
        })
        .withNativeFunction ("debugLog", [] (auto& args, auto complete)
        {
            auto f = Library::appDataDir().getChildFile ("ui.log");
            if (f.getSize() > 200 * 1024)
                f.replaceWithText ({});
            const auto raw = args.size() > 0 ? args.getReference (0).toString() : juce::String();
            f.appendText (juce::Time::getCurrentTime().toISO8601 (true) + "  "
                              + raw.substring (0, 500) + "\n");
            complete (jsonify (okResult()));
        })
        .withNativeFunction ("openExternal", [] (auto& args, auto complete)
        {
            const auto raw = parseArg (args).toString();
            if (raw.startsWith ("https://") || raw.startsWith ("http://"))
                juce::URL (raw).launchInDefaultBrowser();
            complete (jsonify (okResult()));
        })
        .withNativeFunction ("importPresets", [this, safeComplete] (auto&, auto complete)
        {
            auto done = safeComplete (complete);
            fileChooser = std::make_unique<juce::FileChooser> ("Import presets",
                juce::File::getSpecialLocation (juce::File::userHomeDirectory), "*.ampcade");
            fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                          | juce::FileBrowserComponent::canSelectFiles
                                          | juce::FileBrowserComponent::canSelectMultipleItems,
                                      [this, done] (const juce::FileChooser& fc)
                                      {
                                          const auto files = fc.getResults();
                                          if (files.isEmpty())
                                          {
                                              done (errResult ("cancelled"));
                                              return;
                                          }
                                          juce::Array<juce::var> imported;
                                          juce::File lastDest;
                                          for (const auto& f : files)
                                          {
                                              auto xml = Library::parsePresetXml (f);
                                              if (xml == nullptr || ! juce::ValueTree::fromXml (*xml).hasType ("AMPCADE"))
                                                  continue; // not an AmpCade preset
                                              const auto name = Library::sanitizeFileName (f.getFileNameWithoutExtension());
                                              auto dest = Library::presetsDir().getChildFile (name + ".ampcade");
                                              // same name, different rig -> uniquify; identical file -> reuse
                                              for (int n = 2; dest.existsAsFile()
                                                              && dest.loadFileAsString() != f.loadFileAsString(); ++n)
                                                  dest = Library::presetsDir().getChildFile (name + "_" + juce::String (n) + ".ampcade");
                                              if (f == dest || f.copyFileTo (dest))
                                              {
                                                  imported.add (dest.getFileNameWithoutExtension());
                                                  lastDest = dest;
                                              }
                                          }
                                          auto o = makeObj();
                                          o->setProperty ("ok", ! imported.isEmpty());
                                          if (imported.isEmpty())
                                              o->setProperty ("error", "No AmpCade presets in that selection");
                                          o->setProperty ("imported", imported);
                                          if (imported.size() == 1 && lastDest.existsAsFile())
                                          {
                                              proc.beginUserAction();
                                              if (proc.loadPresetFrom (lastDest))
                                                  o->setProperty ("loaded", lastDest.getFileNameWithoutExtension());
                                          }
                                          done (juce::var (o.get()));
                                      });
        })
        .withNativeFunction ("revealPresets", [] (auto&, auto complete)
        {
            Library::presetsDir().revealToUser(); // drag out of Finder = export
            complete (jsonify (okResult()));
        });
}

//==============================================================================
void AmpCadeEditor::chooseFileFor (const juce::String& slot, std::function<void (const juce::var&)> complete)
{
    const bool isCab = slot == "cab" || slot == "air";
    const auto patterns = isCab ? juce::String ("*.wav;*.aif;*.aiff;*.flac") : juce::String ("*.nam");
    const auto title = isCab ? juce::String ("Choose an impulse response") : juce::String ("Choose a NAM model");

    fileChooser = std::make_unique<juce::FileChooser> (title,
                                                       juce::File::getSpecialLocation (juce::File::userHomeDirectory),
                                                       patterns);
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this, slot, complete] (const juce::FileChooser& fc)
                              {
                                  auto f = fc.getResult();
                                  if (f == juce::File{})
                                  {
                                      complete (errResult ("cancelled"));
                                      return;
                                  }
                                  proc.beginUserAction();
                                  proc.importLocalFile (slot, f);
                                  complete (okResult());
                              });
}

void AmpCadeEditor::startConnect (std::function<void (const juce::var&)> complete)
{
    if (proc.t3k.getClientId().isEmpty())
    {
        complete (errResult ("Add your TONE3000 publishable key in Settings first"));
        return;
    }

    if (authOverlay != nullptr)
    {
        complete (errResult ("Login already in progress"));
        return;
    }

    auto auth = proc.t3k.beginAuth();
    pendingVerifier = auth.verifier;
    pendingConnectDone = std::move (complete);

    // Breadcrumb for remote debugging of login problems (see NOTES.md)
    Library::appDataDir().getChildFile ("ui.log")
        .appendText (juce::Time::getCurrentTime().toISO8601 (true) + "  oauth-begin " + auth.url + "\n");

    const juce::String redirectPrefix (kRedirectUri);

    auto sp = juce::Component::SafePointer<AmpCadeEditor> (this);

    authOverlay = std::make_unique<AuthOverlay> (
        auth.url, redirectPrefix, auth.state,
        [sp] (juce::String code)
        {
            if (sp == nullptr) return;
            sp->proc.t3k.exchangeCode (code, sp->pendingVerifier, [sp] (juce::var result)
            {
                if (sp == nullptr) return;
                sp->finishConnect (result);
            });
        },
        [sp] (juce::String error)
        {
            if (sp != nullptr)
                sp->finishConnect (errResult (error));
        },
        [sp]
        {
            if (sp != nullptr)
                sp->finishConnect (errResult ("cancelled"));
        });

    addAndMakeVisible (*authOverlay);
    authOverlay->setBounds (getLocalBounds());
}

void AmpCadeEditor::finishConnect (const juce::var& result)
{
    authOverlay = nullptr;
    pendingVerifier.clear();

    if (pendingConnectDone)
    {
        auto done = std::move (pendingConnectDone);
        pendingConnectDone = nullptr;
        done (result);
    }
    pushState();
}
} // namespace ampcade
