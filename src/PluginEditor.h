#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"

namespace ampcade
{
//==============================================================================
// In-app browser used only for the TONE3000 OAuth dance. Intercepts the
// localhost redirect before any connection attempt and hands back the code.
class AuthBrowser : public juce::WebBrowserComponent
{
public:
    AuthBrowser (juce::String redirectPrefix, juce::String expectedState,
                 std::function<void (juce::String code)> onCodeIn,
                 std::function<void (juce::String error)> onErrorIn)
        : WebBrowserComponent (Options{}
                                   .withBackend (Options::Backend::webview2)
                                   .withWinWebView2Options (Options::WinWebView2{}.withUserDataFolder (
                                       juce::File::getSpecialLocation (juce::File::tempDirectory)))),
          prefix (std::move (redirectPrefix)),
          state (std::move (expectedState)),
          onCode (std::move (onCodeIn)),
          onError (std::move (onErrorIn))
    {
    }

    bool pageAboutToLoad (const juce::String& newURL) override
    {
        if (! newURL.startsWith (prefix))
            return true;

        juce::URL url (newURL);
        juce::String code, gotState, error;
        const auto names = url.getParameterNames();
        const auto values = url.getParameterValues();
        for (int i = 0; i < names.size(); ++i)
        {
            if (names[i] == "code") code = values[i];
            if (names[i] == "state") gotState = values[i];
            if (names[i] == "error") error = values[i];
        }

        auto fail = [this] (juce::String msg)
        {
            juce::MessageManager::callAsync ([cb = onError, msg] { if (cb) cb (msg); });
        };

        if (error.isNotEmpty())
            fail (error == "access_denied" ? "Login cancelled" : error);
        else if (code.isEmpty() || gotState != state)
            fail (juce::String (juce::CharPointer_UTF8 ("Login failed — please try again")));
        else
            juce::MessageManager::callAsync ([cb = onCode, code] { if (cb) cb (code); });

        return false; // never actually navigate to the (unserved) localhost URI
    }

private:
    juce::String prefix, state;
    std::function<void (juce::String)> onCode;
    std::function<void (juce::String)> onError;
};

//==============================================================================
class AuthOverlay : public juce::Component
{
public:
    AuthOverlay (const juce::String& url, const juce::String& redirectPrefix, const juce::String& expectedState,
                 std::function<void (juce::String code)> onCode,
                 std::function<void (juce::String error)> onError,
                 std::function<void()> onCancel)
        : browser (redirectPrefix, expectedState, std::move (onCode), std::move (onError))
    {
        title.setText ("Connect your TONE3000 account", juce::dontSendNotification);
        title.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.9f));
        title.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        addAndMakeVisible (title);

        close.setButtonText ("Cancel");
        close.onClick = std::move (onCancel);
        addAndMakeVisible (close);

        addAndMakeVisible (browser);
        browser.goToURL (url);
    }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff17181d)); }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromTop (40).reduced (10, 6);
        close.setBounds (bar.removeFromRight (80));
        title.setBounds (bar);
        browser.setBounds (r);
    }

private:
    juce::Label title;
    juce::TextButton close;
    AuthBrowser browser;
};

//==============================================================================
class AmpCadeEditor : public juce::AudioProcessorEditor,
                      private juce::Timer,
                      private juce::ChangeListener
{
public:
    explicit AmpCadeEditor (AmpCadeProcessor&);
    ~AmpCadeEditor() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void parentHierarchyChanged() override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    juce::WebBrowserComponent::Options makeOptions();
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);

    void pushState();
    // Standalone shell polish, applied once the window peer exists (polled from
    // the timer): hides the shell's redundant "Options" pill and, on macOS,
    // lets the green traffic light do native full screen.
    void configureStandaloneWindow();
    void startConnect (std::function<void (const juce::var&)> complete);
    void finishConnect (const juce::var& result);
    void chooseFileFor (const juce::String& slot, std::function<void (const juce::var&)> complete);

    AmpCadeProcessor& proc;

    // NOTE: declaration order matters — relays must outlive/precede the web
    // component, attachments come last.
    std::vector<std::unique_ptr<juce::WebSliderRelay>> sliderRelays;
    std::vector<std::unique_ptr<juce::WebToggleButtonRelay>> toggleRelays;

    std::unique_ptr<juce::WebBrowserComponent> web;

    std::vector<std::unique_ptr<juce::WebSliderParameterAttachment>> sliderAttachments;
    std::vector<std::unique_ptr<juce::WebToggleButtonParameterAttachment>> toggleAttachments;

    std::unique_ptr<AuthOverlay> authOverlay;
    juce::String pendingVerifier;
    std::function<void (const juce::var&)> pendingConnectDone;

    std::unique_ptr<juce::FileChooser> fileChooser;
    bool standaloneWindowConfigured = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmpCadeEditor)
};
} // namespace ampcade
