#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

namespace ampcade
{
// REST client for the TONE3000 API (OAuth 2.0 + PKCE).
// Public async methods run on an internal pool; callbacks are marshalled to
// the message thread. Owned by the processor so downloads survive editor
// close/reopen.
class Tone3000Client
{
public:
    Tone3000Client();
    ~Tone3000Client();

    using Callback = std::function<void (juce::var)>; // message thread

    //================================================================== auth
    bool isConnected() const;
    juce::String getUsername() const;

    juce::String getClientId() const;
    void setClientId (const juce::String&);

    struct PendingAuth
    {
        juce::String verifier, state, url;
    };
    PendingAuth beginAuth();                                  // message thread
    void exchangeCode (const juce::String& code,
                       const juce::String& verifier, Callback done);
    void disconnect();

    //=================================================================== api
    // params: { query, gears:[..], sort, page, page_size } -> bridge-shaped result
    void search (const juce::var& params, Callback done);
    // single tone -> { ok, id, title, author, gear, format }
    void tone (int toneId, Callback done);
    // all capture variants of a tone -> { ok, models:[{id,name,size,architecture_version}] }
    void models (int toneId, Callback done);
    // GET model bytes (Bearer only on tone3000 hosts; redirects followed manually)
    void download (const juce::String& modelUrl, const juce::File& dest,
                   std::function<void (bool ok, juce::String error)> done);

private:
    struct Tokens
    {
        juce::String access, refresh, username;
        juce::int64 expiresAt = 0; // ms epoch
        bool valid() const { return access.isNotEmpty(); }
    };

    void loadTokens();
    void saveTokens();
    bool ensureFreshToken();  // pool thread
    bool refreshTokens();     // pool thread

    // Blocking helpers (pool thread)
    juce::var postJson (const juce::String& url, const juce::var& body, int& statusOut);
    juce::var apiGet (const juce::String& path, const juce::StringPairArray& params, int& statusOut);
    bool fetchUserIntoTokens();

    static void onMessageThread (Callback cb, juce::var result);

    mutable juce::CriticalSection tokenLock;
    Tokens tokens;
    juce::String clientId;

    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withNumberOfThreads (2) };

    JUCE_DECLARE_NON_COPYABLE (Tone3000Client)
};

// Small var-building helpers shared with the processor/editor.
juce::DynamicObject::Ptr makeObj();
juce::var okResult();
juce::var errResult (const juce::String& message);
} // namespace ampcade
