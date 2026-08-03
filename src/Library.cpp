#include "Library.h"

#ifndef AMPCADE_SMOKE
#include "BinaryData.h" // factory presets ride in with the UI assets
#endif

namespace ampcade
{
static juce::File ensured (juce::File f)
{
    if (! f.exists())
        f.createDirectory();
    return f;
}

juce::File Library::appDataDir()
{
    return ensured (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                        .getChildFile ("AmpCade"));
}

juce::File Library::modelsDir()  { return ensured (appDataDir().getChildFile ("models")); }
juce::File Library::irsDir()     { return ensured (appDataDir().getChildFile ("irs")); }
juce::File Library::presetsDir() { return ensured (appDataDir().getChildFile ("presets")); }
juce::File Library::settingsFile() { return appDataDir().getChildFile ("settings.json"); }
juce::File Library::tokensFile()   { return appDataDir().getChildFile ("t3k-auth.json"); }

juce::var Library::readJson (const juce::File& f)
{
    if (! f.existsAsFile())
        return {};
    return juce::JSON::parse (f.loadFileAsString());
}

bool Library::writeJson (const juce::File& f, const juce::var& v)
{
    return f.replaceWithText (juce::JSON::toString (v));
}

// juce::XmlDocument parses recursively and caps nothing: readNextElement calls
// readChildElements calls readNextElement, one stack frame per nesting level.
// A preset with tens of thousands of nested elements therefore overflows the
// stack — in a plugin that takes the whole host DAW down with it, losing
// whatever the player had not saved. The scan below is deliberately crude and
// over-counts (it does not skip comments or attribute values); it does not need
// to be exact, because AmpCade's own presets nest 4 deep and the ceiling is 100.
static bool nestingWithinLimit (const juce::String& text, int maxDepth)
{
    int depth = 0;
    auto p = text.getCharPointer();

    while (! p.isEmpty())
    {
        if (p.getAndAdvance() != '<')
            continue;

        const auto next = *p;
        if (next == '/')      // closing tag
        {
            --depth;
            continue;
        }
        if (next == '!' || next == '?')   // comment, CDATA, declaration
            continue;

        // Opening tag: it only nests if it is not self-closing ("<FOO/>").
        bool selfClosing = false;
        juce::juce_wchar lastNonSpace = 0;
        while (! p.isEmpty())
        {
            const auto c = p.getAndAdvance();
            if (c == '>')
            {
                selfClosing = (lastNonSpace == '/');
                break;
            }
            if (! juce::CharacterFunctions::isWhitespace (c))
                lastNonSpace = c;
        }

        if (! selfClosing && ++depth > maxDepth)
            return false;
    }
    return true;
}

std::unique_ptr<juce::XmlElement> Library::parsePresetXml (const juce::File& f)
{
    const auto text = f.loadFileAsString();
    // Refuse a DOCTYPE outright. juce::XmlDocument expands external entities,
    // and a "<!DOCTYPE … SYSTEM '…'>" in a preset someone sent you makes the
    // parser read a file of THEIR choosing off YOUR disk (FileInputSource ->
    // getSiblingFile, which resolves ".." and absolute paths). Refusing it also
    // rules out entity-expansion bombs: "<!ENTITY a '&a;'>" recurses until the
    // stack gives out. Nothing AmpCade writes contains either.
    if (text.containsIgnoreCase ("<!DOCTYPE") || text.containsIgnoreCase ("<!ENTITY"))
        return nullptr;
    if (! nestingWithinLimit (text, kMaxPresetNesting))
        return nullptr;
    // Parsed from the STRING, not the File: that leaves the document with no
    // input source at all, so external entities cannot load even if the check
    // above is ever outgrown.
    return juce::XmlDocument (text).getDocumentElement();
}

juce::String Library::packGearPath (const juce::String& absolutePath)
{
    if (absolutePath.isEmpty())
        return {};
    const juce::File f (absolutePath);
    const auto root = appDataDir();
    if (! f.isAChildOf (root))
        return absolutePath; // gear from outside our folder — presets blank it
    return f.getRelativePathFrom (root).replaceCharacter ('\\', '/');
}

juce::String Library::unpackGearPath (const juce::String& storedPath)
{
    if (storedPath.isEmpty())
        return {};
    if (juce::File::isAbsolutePath (storedPath))
        return storedPath; // legacy rigs, and only ever from local state
    const auto root = appDataDir();
    const auto f = root.getChildFile (storedPath.replaceCharacter ('\\', '/'));
    return f.isAChildOf (root) ? f.getFullPathName() : juce::String();
}

void Library::installFactoryPresets()
{
#ifndef AMPCADE_SMOKE
    // The marker means "the factory set has been offered once" — respecting a
    // player who deleted Clean.ampcade matters more than restoring it.
    const auto marker = presetsDir().getChildFile (".factory-installed");
    if (marker.existsAsFile())
        return;

    struct { const char* name; const char* data; int size; } presets[] = {
        { "Clean.ampcade", BinaryData::Clean_ampcade, BinaryData::Clean_ampcadeSize },
        { "Lonestar.ampcade", BinaryData::Lonestar_ampcade, BinaryData::Lonestar_ampcadeSize },
    };
    for (const auto& p : presets)
    {
        const auto f = presetsDir().getChildFile (p.name);
        if (! f.existsAsFile()) // never clobber a player's own preset of the same name
            f.replaceWithData (p.data, (size_t) p.size);
    }
    marker.replaceWithText ("");
#endif
}

juce::String Library::sanitizeFileName (const juce::String& in)
{
    auto s = in.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.");
    s = s.trim().replaceCharacter (' ', '_');
    return s.isEmpty() ? "untitled" : s.substring (0, 80);
}
} // namespace ampcade
