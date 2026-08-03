#include "Tone3000Client.h"
#include "Library.h"
#include "Version.h"

#include <juce_cryptography/juce_cryptography.h>

namespace ampcade
{
juce::DynamicObject::Ptr makeObj() { return new juce::DynamicObject(); }

juce::var okResult()
{
    auto o = makeObj();
    o->setProperty ("ok", true);
    return juce::var (o.get());
}

juce::var errResult (const juce::String& message)
{
    auto o = makeObj();
    o->setProperty ("ok", false);
    o->setProperty ("error", message);
    return juce::var (o.get());
}

//==============================================================================
// "Is this URL actually TONE3000, over TLS?" — the gate on ever attaching the
// player's OAuth access token to a request. Deliberately strict: the host must
// BE tone3000.com or a real subdomain of it, and the scheme must be https.
static bool isTone3000Host (const juce::URL& url)
{
    if (! url.getScheme().equalsIgnoreCase ("https"))
        return false;

    const auto host = url.getDomain().toLowerCase();
    return host == "tone3000.com" || host.endsWith (".tone3000.com");
}

static juce::String base64Url (const void* data, size_t size)
{
    auto b64 = juce::Base64::toBase64 (data, size);
    return b64.replaceCharacter ('+', '-').replaceCharacter ('/', '_').removeCharacters ("=");
}

static juce::MemoryBlock randomBytes (size_t count)
{
    juce::MemoryBlock block (count);

    juce::File urandom ("/dev/urandom");
    if (auto stream = urandom.createInputStream(); stream != nullptr)
    {
        if (stream->read (block.getData(), (int) count) == (int) count)
            return block;
    }

    auto& rng = juce::Random::getSystemRandom();
    for (size_t i = 0; i < count; ++i)
        block[i] = (char) rng.nextInt (256);
    return block;
}

//==============================================================================
Tone3000Client::Tone3000Client()
{
    auto settings = Library::readJson (Library::settingsFile());
    clientId = settings.getProperty ("clientId", juce::String (kDefaultClientId)).toString();
    if (clientId.isEmpty())
        clientId = kDefaultClientId;
    loadTokens();
}

Tone3000Client::~Tone3000Client()
{
    pool.removeAllJobs (true, 5000);
}

bool Tone3000Client::isConnected() const
{
    const juce::ScopedLock l (tokenLock);
    return tokens.valid();
}

juce::String Tone3000Client::getUsername() const
{
    const juce::ScopedLock l (tokenLock);
    return tokens.username;
}

juce::String Tone3000Client::getClientId() const
{
    const juce::ScopedLock l (tokenLock);
    return clientId;
}

void Tone3000Client::setClientId (const juce::String& id)
{
    {
        const juce::ScopedLock l (tokenLock);
        clientId = id.trim();
    }
    auto settings = Library::readJson (Library::settingsFile());
    if (settings.getDynamicObject() == nullptr)
        settings = juce::var (makeObj().get());
    settings.getDynamicObject()->setProperty ("clientId", id.trim());
    Library::writeJson (Library::settingsFile(), settings);
}

//==============================================================================
void Tone3000Client::loadTokens()
{
    auto v = Library::readJson (Library::tokensFile());
    const juce::ScopedLock l (tokenLock);
    tokens.access = v.getProperty ("access", "").toString();
    tokens.refresh = v.getProperty ("refresh", "").toString();
    tokens.username = v.getProperty ("username", "").toString();
    tokens.expiresAt = (juce::int64) v.getProperty ("expiresAt", 0);
}

void Tone3000Client::saveTokens()
{
    juce::var v;
    {
        const juce::ScopedLock l (tokenLock);
        auto o = makeObj();
        o->setProperty ("access", tokens.access);
        o->setProperty ("refresh", tokens.refresh);
        o->setProperty ("username", tokens.username);
        o->setProperty ("expiresAt", tokens.expiresAt);
        v = juce::var (o.get());
    }
    Library::writeJson (Library::tokensFile(), v);
}

void Tone3000Client::disconnect()
{
    {
        const juce::ScopedLock l (tokenLock);
        tokens = {};
    }
    Library::tokensFile().deleteFile();
}

//==============================================================================
Tone3000Client::PendingAuth Tone3000Client::beginAuth()
{
    const auto verifierBytes = randomBytes (32);
    const auto verifier = base64Url (verifierBytes.getData(), verifierBytes.getSize());

    const auto hash = juce::SHA256 (verifier.toRawUTF8(), verifier.getNumBytesAsUTF8()).getRawData();
    const auto challenge = base64Url (hash.getData(), hash.getSize());

    const auto stateBytes = randomBytes (16);
    const auto state = base64Url (stateBytes.getData(), stateBytes.getSize());

    // Deliberately UNencoded: WebBrowserComponent::goToURL() on macOS
    // percent-encodes the whole string once (it expects raw input), so a
    // pre-encoded query gets double-encoded and TONE3000 then rejects
    // redirect_uri as "not a valid URL". Every piece below is URL-safe as-is:
    // the publishable key is alphanumeric, ':' and '/' are legal raw in a
    // query, and challenge/state are base64url.
    const auto url = juce::String (kT3kApi) + "/oauth/authorize"
                     + "?client_id=" + getClientId().trim()
                     + "&redirect_uri=" + kRedirectUri
                     + "&response_type=code"
                     + "&code_challenge=" + challenge
                     + "&code_challenge_method=S256"
                     + "&state=" + state;

    return { verifier, state, url };
}

juce::var Tone3000Client::postJson (const juce::String& url, const juce::var& body, int& statusOut)
{
    const auto payload = juce::JSON::toString (body, true);

    auto stream = juce::URL (url)
                      .withPOSTData (payload)
                      .createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                              .withHttpRequestCmd ("POST")
                                              .withExtraHeaders ("Content-Type: application/json\r\nAccept: application/json\r\n")
                                              .withConnectionTimeoutMs (20000)
                                              .withStatusCode (&statusOut));
    if (stream == nullptr)
        return {};
    return juce::JSON::parse (stream->readEntireStreamAsString());
}

bool Tone3000Client::refreshTokens()
{
    juce::String refresh, cid;
    {
        const juce::ScopedLock l (tokenLock);
        refresh = tokens.refresh;
        cid = clientId;
    }
    if (refresh.isEmpty())
        return false;

    auto body = makeObj();
    body->setProperty ("grant_type", "refresh_token");
    body->setProperty ("refresh_token", refresh);
    body->setProperty ("client_id", cid);

    int status = 0;
    auto res = postJson (juce::String (kT3kApi) + "/oauth/token", juce::var (body.get()), status);

    if (status != 200 || ! res.hasProperty ("access_token"))
        return false;

    {
        const juce::ScopedLock l (tokenLock);
        tokens.access = res["access_token"].toString();
        if (res.hasProperty ("refresh_token"))
            tokens.refresh = res["refresh_token"].toString();
        const auto expiresIn = (juce::int64) (double) res.getProperty ("expires_in", 3600.0);
        tokens.expiresAt = juce::Time::currentTimeMillis() + (expiresIn - 60) * 1000;
    }
    saveTokens();
    return true;
}

bool Tone3000Client::ensureFreshToken()
{
    {
        const juce::ScopedLock l (tokenLock);
        if (! tokens.valid())
            return false;
        if (juce::Time::currentTimeMillis() < tokens.expiresAt - 30000)
            return true;
    }
    return refreshTokens();
}

bool Tone3000Client::fetchUserIntoTokens()
{
    int status = 0;
    auto res = apiGet ("/user", {}, status);
    if (status != 200)
        return false;

    // Accept either a bare user object or { user: {...} }
    auto user = res.hasProperty ("username") ? res : res.getProperty ("user", juce::var());
    const auto name = user.getProperty ("username", "").toString();
    if (name.isEmpty())
        return false;

    const juce::ScopedLock l (tokenLock);
    tokens.username = name;
    return true;
}

void Tone3000Client::exchangeCode (const juce::String& code, const juce::String& verifier, Callback done)
{
    pool.addJob ([this, code, verifier, done]
    {
        auto body = makeObj();
        body->setProperty ("grant_type", "authorization_code");
        body->setProperty ("code", code);
        body->setProperty ("code_verifier", verifier);
        body->setProperty ("redirect_uri", kRedirectUri);
        body->setProperty ("client_id", getClientId());

        int status = 0;
        auto res = postJson (juce::String (kT3kApi) + "/oauth/token", juce::var (body.get()), status);

        if (status != 200 || ! res.hasProperty ("access_token"))
        {
            auto msg = res.getProperty ("error", "Login failed (HTTP " + juce::String (status) + ")").toString();
            onMessageThread (done, errResult (msg));
            return;
        }

        {
            const juce::ScopedLock l (tokenLock);
            tokens.access = res["access_token"].toString();
            tokens.refresh = res.getProperty ("refresh_token", "").toString();
            const auto expiresIn = (juce::int64) (double) res.getProperty ("expires_in", 3600.0);
            tokens.expiresAt = juce::Time::currentTimeMillis() + (expiresIn - 60) * 1000;
        }

        fetchUserIntoTokens();
        saveTokens();

        auto result = okResult();
        result.getDynamicObject()->setProperty ("username", getUsername());
        onMessageThread (done, result);
    });
}

//==============================================================================
juce::var Tone3000Client::apiGet (const juce::String& path, const juce::StringPairArray& params, int& statusOut)
{
    auto attempt = [&] (int& status) -> juce::var
    {
        juce::String bearer;
        {
            const juce::ScopedLock l (tokenLock);
            bearer = tokens.access;
        }

        auto url = juce::URL (juce::String (kT3kApi) + path);
        for (const auto& key : params.getAllKeys())
            url = url.withParameter (key, params[key]);

        auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                 .withHttpRequestCmd ("GET")
                                                 .withExtraHeaders ("Authorization: Bearer " + bearer + "\r\nAccept: application/json\r\n")
                                                 .withConnectionTimeoutMs (20000)
                                                 .withStatusCode (&status));
        if (stream == nullptr)
            return {};
        return juce::JSON::parse (stream->readEntireStreamAsString());
    };

    if (! ensureFreshToken())
    {
        statusOut = 401;
        return {};
    }

    auto res = attempt (statusOut);
    if (statusOut == 401 && refreshTokens())
        res = attempt (statusOut);
    return res;
}

void Tone3000Client::search (const juce::var& p, Callback done)
{
    pool.addJob ([this, p, done]
    {
        juce::StringPairArray q;
        const auto query = p.getProperty ("query", "").toString().trim();
        if (query.isNotEmpty())
            q.set ("query", query);

        if (auto* gears = p.getProperty ("gears", juce::var()).getArray())
        {
            juce::StringArray parts;
            for (const auto& g : *gears)
                parts.add (g.toString());
            if (! parts.isEmpty())
                q.set ("gears", parts.joinIntoString ("_"));
        }

        // Filter on format server-side (documented: `format` = nam | ir | …). This is
        // what stops IRs appearing in a pedal search only to be rejected on load,
        // and it is the only way to find an IR that its uploader filed under a gear
        // other than cab/space — e.g. a reverb pedal IR tagged gear=pedal.
        const auto format = p.getProperty ("format", "").toString();
        if (format.isNotEmpty())
            q.set ("format", format);

        q.set ("sort", p.getProperty ("sort", "trending").toString());
        q.set ("page", juce::String ((int) p.getProperty ("page", 1)));
        q.set ("page_size", juce::String ((int) p.getProperty ("page_size", 24)));

        int status = 0;
        auto res = apiGet ("/tones/search", q, status);

        if (status == 401)
        {
            onMessageThread (done, errResult ("Not connected to TONE3000"));
            return;
        }
        if (status != 200)
        {
            onMessageThread (done, errResult ("Search failed (HTTP " + juce::String (status) + ")"));
            return;
        }

        auto out = okResult();
        auto* obj = out.getDynamicObject();
        obj->setProperty ("page", res.getProperty ("page", 1));
        obj->setProperty ("total_pages", res.getProperty ("total_pages", 1));
        obj->setProperty ("total", res.getProperty ("total", 0));

        juce::Array<juce::var> tones;
        if (auto* data = res.getProperty ("data", juce::var()).getArray())
        {
            for (const auto& t : *data)
            {
                auto tone = makeObj();
                tone->setProperty ("id", t.getProperty ("id", 0));
                tone->setProperty ("title", t.getProperty ("title", ""));
                tone->setProperty ("gear", t.getProperty ("gear", ""));
                tone->setProperty ("description", t.getProperty ("description", ""));
                tone->setProperty ("downloads", t.getProperty ("downloads_count", 0));
                tone->setProperty ("favorites", t.getProperty ("favorites_count", 0));
                tone->setProperty ("models_count", t.getProperty ("models_count", 0));
                tone->setProperty ("author", t.getProperty ("user", juce::var()).getProperty ("username", ""));

                // Format matters to the user before they click: a "space" (reverb/delay)
                // tone exists as both an IR and a NAM capture, and only one of the two
                // fits the slot they are filling. The field is optional in search
                // responses, so an empty string here just means "unknown" and the UI
                // stays quiet rather than guessing.
                auto fmt = t.getProperty ("format", "").toString();
                if (fmt.isEmpty())
                    if (auto* formats = t.getProperty ("formats", juce::var()).getArray())
                    {
                        juce::StringArray fs;
                        for (const auto& f : *formats)
                            fs.addIfNotAlreadyThere (f.toString());
                        fmt = fs.joinIntoString (",");
                    }
                tone->setProperty ("format", fmt);

                juce::String image;
                if (auto* images = t.getProperty ("images", juce::var()).getArray())
                    if (! images->isEmpty())
                        image = images->getFirst().toString();
                tone->setProperty ("image", image);

                tones.add (juce::var (tone.get()));
            }
        }
        obj->setProperty ("tones", tones);
        onMessageThread (done, out);
    });
}

void Tone3000Client::tone (int toneId, Callback done)
{
    pool.addJob ([this, toneId, done]
    {
        int status = 0;
        auto res = apiGet ("/tones/" + juce::String (toneId), {}, status);

        if (status != 200)
        {
            onMessageThread (done, errResult ("Couldn't fetch tone (HTTP " + juce::String (status) + ")"));
            return;
        }

        // Accept either a bare tone object or { data: {...} } / { tone: {...} }
        auto t = res.hasProperty ("title") ? res
               : (res.hasProperty ("data") ? res["data"] : res.getProperty ("tone", juce::var()));

        auto out = okResult();
        auto* obj = out.getDynamicObject();
        obj->setProperty ("id", t.getProperty ("id", toneId));
        obj->setProperty ("title", t.getProperty ("title", ""));
        obj->setProperty ("gear", t.getProperty ("gear", ""));
        obj->setProperty ("format", t.getProperty ("format", ""));
        obj->setProperty ("author", t.getProperty ("user", juce::var()).getProperty ("username", ""));
        onMessageThread (done, out);
    });
}

void Tone3000Client::models (int toneId, Callback done)
{
    pool.addJob ([this, toneId, done]
    {
        juce::StringPairArray q;
        q.set ("tone_id", juce::String (toneId));
        q.set ("page_size", "100");

        int status = 0;
        auto res = apiGet ("/models", q, status);

        if (status != 200)
        {
            onMessageThread (done, errResult ("Couldn't fetch capture settings (HTTP " + juce::String (status) + ")"));
            return;
        }

        auto out = okResult();
        juce::Array<juce::var> models;
        if (auto* data = res.getProperty ("data", juce::var()).getArray())
        {
            for (const auto& m : *data)
            {
                auto model = makeObj();
                model->setProperty ("id", m.getProperty ("id", 0));
                model->setProperty ("name", m.getProperty ("name", ""));
                model->setProperty ("size", m.getProperty ("size", ""));
                model->setProperty ("architecture_version", m.getProperty ("architecture_version", ""));
                model->setProperty ("model_url", m.getProperty ("model_url", ""));
                models.add (juce::var (model.get()));
            }
        }
        out.getDynamicObject()->setProperty ("models", models);
        onMessageThread (done, out);
    });
}

void Tone3000Client::download (const juce::String& modelUrl, const juce::File& dest,
                               std::function<void (bool, juce::String)> done)
{
    pool.addJob ([this, modelUrl, dest, done]
    {
        auto finish = [done] (bool ok, juce::String err)
        {
            juce::MessageManager::callAsync ([done, ok, err] { done (ok, err); });
        };

        if (! ensureFreshToken())
        {
            finish (false, "Not connected to TONE3000");
            return;
        }

        juce::String current = modelUrl;
        if (current.startsWith ("/"))
            current = juce::String (kT3kBase) + current;

        for (int hop = 0; hop < 6; ++hop)
        {
            juce::URL url (current);
            // The player's live access token may only ride to TONE3000 itself,
            // over TLS. This used to be containsIgnoreCase("tone3000.com") — a
            // plain substring test on the host — so a redirect to
            // "tone3000.com.evil.example" was handed the token, and there was no
            // scheme check either, so a plain-http hop sent it in clear text.
            // juce::URL::getDomain() does not strip userinfo, so it can return
            // "tone3000.com@evil.example"; an exact/suffix match fails closed on
            // that too.
            const bool isT3k = isTone3000Host (url);

            juce::String headers = "Accept: */*\r\n";
            if (isT3k)
            {
                const juce::ScopedLock l (tokenLock);
                headers += "Authorization: Bearer " + tokens.access + "\r\n";
            }

            int status = 0;
            juce::StringPairArray responseHeaders;
            auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                     .withHttpRequestCmd ("GET")
                                                     .withExtraHeaders (headers)
                                                     .withConnectionTimeoutMs (30000)
                                                     .withNumRedirectsToFollow (0)
                                                     .withStatusCode (&status)
                                                     .withResponseHeaders (&responseHeaders));

            if (status >= 300 && status < 400)
            {
                auto location = responseHeaders.getValue ("Location", responseHeaders.getValue ("location", ""));
                if (location.isEmpty())
                {
                    finish (false, "Broken redirect from server");
                    return;
                }
                if (location.startsWith ("/"))
                    location = "https://" + url.getDomain() + location;
                current = location;
                continue;
            }

            if (stream == nullptr || status != 200)
            {
                finish (false, "Download failed (HTTP " + juce::String (status) + ")");
                return;
            }

            auto temp = dest.getSiblingFile (dest.getFileName() + ".part");
            temp.deleteFile();
            {
                juce::FileOutputStream fileOut (temp);
                if (! fileOut.openedOk())
                {
                    finish (false, "Couldn't write to library folder");
                    return;
                }
                fileOut.writeFromInputStream (*stream, -1);
            }

            if (temp.getSize() < 16)
            {
                temp.deleteFile();
                finish (false, "Server returned an empty file");
                return;
            }

            dest.deleteFile();
            if (! temp.moveFileTo (dest))
            {
                finish (false, "Couldn't move download into library");
                return;
            }

            finish (true, {});
            return;
        }

        finish (false, "Too many redirects");
    });
}

void Tone3000Client::onMessageThread (Callback cb, juce::var result)
{
    if (cb == nullptr)
        return;
    juce::MessageManager::callAsync ([cb, result] { cb (result); });
}
} // namespace ampcade
