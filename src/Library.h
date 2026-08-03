#pragma once
#include <juce_core/juce_core.h>

namespace ampcade
{
// App-support directories and small JSON settings/token files.
class Library
{
public:
    static juce::File appDataDir();     // .../Application Support/AmpCade
    static juce::File modelsDir();      // downloaded .nam files
    static juce::File irsDir();         // downloaded/imported IR wavs
    static juce::File presetsDir();
    static juce::File settingsFile();   // settings.json  { clientId }
    static juce::File tokensFile();     // t3k-auth.json  { access, refresh, expiresAt }

    static juce::var readJson (const juce::File&);
    static bool writeJson (const juce::File&, const juce::var&);

    // "Marshall JCM800 / Gain 7" -> "Marshall_JCM800_Gain_7"
    static juce::String sanitizeFileName (const juce::String&);

    // AmpCade's own presets nest 4 deep. Anything past this is a stack-overflow
    // attempt, not a rig — juce::XmlDocument recurses once per nesting level.
    static constexpr int kMaxPresetNesting = 100;

    // Parses a .ampcade file. Presets are a SHARING format, so every file that
    // arrives this way is untrusted input — use this and never
    // juce::XmlDocument::parse(File). Returns nullptr on anything suspect.
    static std::unique_ptr<juce::XmlElement> parsePresetXml (const juce::File&);

    // Gear paths are written RELATIVE to appDataDir wherever the file lives
    // inside it, so nothing AmpCade saves carries the player's home directory —
    // a shared preset used to ship "/Users/<name>/Library/AmpCade/models/…".
    // Separators are normalised to '/' so a rig saved on Windows opens on a Mac.
    static juce::String packGearPath (const juce::String& absolutePath);
    // The inverse. A relative path can never escape appDataDir (getChildFile
    // resolves ".."), so a hostile preset cannot reach the rest of the disk;
    // an empty return means "refuse this path".
    static juce::String unpackGearPath (const juce::String& storedPath);

    // Copies the bundled factory presets into presetsDir exactly once (a
    // marker file remembers). Existing files are never overwritten, and a
    // player who deletes a factory preset stays rid of it.
    static void installFactoryPresets();
};
} // namespace ampcade
