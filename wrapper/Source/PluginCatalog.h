#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

/** Process-wide list of installed plug-ins (shared by all wrapper instances). */
struct PluginCatalog
{
    PluginCatalog()
    {
        formats.addDefaultFormats();
        dataDir().createDirectory();
        if (auto xml = juce::XmlDocument::parse (listFile()))
            list.recreateFromXml (*xml);
    }

    static juce::File dataDir()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Application Support").getChildFile ("NightCap");
    }
    static juce::File listFile()       { return dataDir().getChildFile ("plugins.xml"); }
    static juce::File deadMansPedal()  { return dataDir().getChildFile ("scanning.tmp"); }
    static juce::File mappingDir()     { return dataDir().getChildFile ("Mappings"); }

    void save()
    {
        // never offer ourselves as something to wrap
        for (auto& t : list.getTypes())
            if (t.name.startsWithIgnoreCase ("kNocTurn NightCap"))
                list.removeType (t);
        if (auto xml = list.createXml())
            xml->writeTo (listFile());
    }

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList list;
};
