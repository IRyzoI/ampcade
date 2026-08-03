#pragma once

namespace ampcade
{
constexpr const char* kAppVersion = "0.1.0";

// TONE3000 API
constexpr const char* kT3kBase = "https://www.tone3000.com";
constexpr const char* kT3kApi = "https://www.tone3000.com/api/v1";

// Publishable key ("client id") for the TONE3000 OAuth app — a public
// identifier (like any OAuth client id), safe to ship in builds and commit.
// The matching SECRET key (t3k_cs_…) must never appear anywhere in this repo.
constexpr const char* kDefaultClientId = "t3k_pub_DyLBzrwu0V2Nf5gGFXx7AMueU2CM8BdV";

// Localhost redirect: never actually served — the auth webview intercepts the
// navigation in pageAboutToLoad() before any connection is attempted.
constexpr const char* kRedirectUri = "http://127.0.0.1:53682/callback";
} // namespace ampcade
