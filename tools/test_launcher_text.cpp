#include "launcher_text.hpp"
#include <cstdio>
#include <cstdlib>

using namespace launcher::text;

namespace {
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
}

// The launcher edits the player's live ERVR.ini while the game hot-reloads it: every byte it does not mean
// to change must survive, and it must read keys exactly as GetPrivateProfileString does.
int main() {
    const std::string ini =
        "\xEF\xBB\xBF; ERVR\r\n[General]\r\nLogLevel=info\r\n\r\n[VR]\r\n; comment = not a key\r\n"
        "StereoMode = full\r\nRenderScale=1.0\r\n; trailing comment\r\n\r\n[HUD]\r\nMode=quad\r\n[vr]\r\nWorldScale=9\r\n";
    Check(iniGet(ini, "General", "LogLevel") == "info", "plain key");
    Check(iniGet(ini, "vr", "stereomode") == "full", "section and key match without case, value trimmed");
    Check(!iniGet(ini, "VR", "comment").has_value(), "comment lines are no keys");
    Check(!iniGet(ini, "VR", "WorldScale").has_value(), "only the first section of a name counts");
    Check(!iniGet(ini, "Combat", "Enabled").has_value(), "missing section");

    std::string edited = iniSet(ini, "VR", "StereoMode", "cinema");
    Check(iniGet(edited, "VR", "StereoMode") == "cinema", "value replaced");
    Check(edited.find("StereoMode = cinema\r\n") != std::string::npos, "key spelling and spacing kept");
    Check(edited.size() == ini.size() + 2, "nothing else changes");
    Check(edited.substr(0, 3) == "\xEF\xBB\xBF", "byte order mark kept");

    // Kinetic: an update brings new [Physics] settings without touching the player's values.
    {
        const std::string shipped = "[VR]\nA=1\n[Physics]\n; physics layer\nEnabled=false\nSword=true\n\n[Experimental]\nX=1\n";
        const std::string old = "[VR]\r\nA=2\r\n";
        const std::string merged = iniAddMissing(old, shipped, "Physics");
        Check(merged.rfind("[VR]\r\nA=2\r\n", 0) == 0, "player's own lines kept");
        Check(merged.find("; physics layer\r\n") != std::string::npos, "missing section copied with comments, player's line ends");
        Check(iniGet(merged, "Physics", "Sword") == "true", "missing section's keys arrive");
        Check(merged.find("[Experimental]") == std::string::npos, "only that section");
        const std::string mine = "[Physics]\nEnabled=true\n";
        const std::string merged2 = iniAddMissing(mine, shipped, "Physics");
        Check(iniGet(merged2, "Physics", "Enabled") == "true", "player's value kept");
        Check(iniGet(merged2, "Physics", "Sword") == "true", "missing key added");
        Check(iniAddMissing(merged2, shipped, "Physics") == merged2, "nothing missing: unchanged");
        Check(iniAddMissing(old, "[VR]\nA=1\n", "Physics") == old, "section not shipped: unchanged");
    }

    edited = iniSet(ini, "VR", "WorldScale", "0.8");
    Check(edited.find("RenderScale=1.0\r\nWorldScale=0.8\r\n; trailing comment") != std::string::npos,
          "a missing key goes after its section's last key, with the file's line ends");
    Check(iniGet(edited, "VR", "WorldScale") == "0.8", "inserted key reads back");

    edited = iniSet(ini, "Combat", "Enabled", "false");
    Check(iniGet(edited, "Combat", "Enabled") == "false", "missing section appended");
    Check(edited.find("WorldScale=9\r\n\r\n[Combat]\r\nEnabled=false\r\n") != std::string::npos, "appended at the end");

    Check(iniGet(iniSet("[A]\nx=1", "A", "y", "2"), "A", "y") == "2", "last line without a line end");
    Check(iniSet("[A]\nx=1", "A", "y", "2") == "[A]\nx=1\ny=2\n", "the new key starts its own line");
    Check(iniSet("[A]\nx=\n", "A", "x", "3") == "[A]\nx=3\n", "empty value");

    // The game's GraphicsConfig.xml, in its own shape: every other setting the player chose must survive.
    const std::wstring graphics =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\" ?>\r\n<config><ScreenMode>WINDOW</ScreenMode>\r\n"
        L"<Auto-detectBestRenderingSettings>ON</Auto-detectBestRenderingSettings>\r\n<QualitySetting>MAX</QualitySetting>\r\n"
        L"<SSAO>MAX</SSAO>\r\n<DepthOfField>MAX</DepthOfField>\r\n<MotionBlur>DISABLE</MotionBlur>\r\n"
        L"<RaytracingQuality> HIGH </RaytracingQuality>\r\n</config>\r\n";
    Check(xmlGet(graphics, "DepthOfField") == "MAX" && xmlGet(graphics, "RaytracingQuality") == "HIGH", "xml value, trimmed");
    Check(!xmlGet(graphics, "Depth").has_value(), "a tag matches whole");
    std::wstring xml = graphics;
    std::vector<std::string> changed = turnEffectsOff(xml, allEffects());
    Check(changed == std::vector<std::string>{"depth of field (was MAX)", "ray tracing (was HIGH)"},
          "what changed; motion blur was off already");
    Check(xmlGet(xml, "DepthOfField") == "DISABLE" && xmlGet(xml, "MotionBlur") == "DISABLE" &&
              xmlGet(xml, "RaytracingQuality") == "DISABLE",
          "all three off");
    Check(xmlGet(xml, "QualitySetting") == "CUSTOM" && xmlGet(xml, "Auto-detectBestRenderingSettings") == "OFF",
          "preset and auto-detect can't put them back");
    Check(xml.find(L"<SSAO>MAX</SSAO>\r\n<DepthOfField>DISABLE</DepthOfField>") != std::wstring::npos &&
              xml.rfind(L"</config>\r\n") == xml.size() - 11,
          "nothing else changes");
    Check(turnEffectsOff(xml, allEffects()).empty(), "a second pass changes nothing");
    xml = graphics;
    Check(turnEffectsOff(xml, EffectsOff{false, true, false}).empty() && xml == graphics,
          "an effect left to the game stays, and nothing else moves");
    xml = L"<config><DepthOfField>LOW</DepthOfField></config>";
    Check(turnEffectsOff(xml, allEffects()).size() == 1 && xml == L"<config><DepthOfField>DISABLE</DepthOfField></config>",
          "missing tags stay missing");
    Check(listed({"a"}) == "a" && listed({"a", "b"}) == "a and b" && listed({"a", "b", "c"}) == "a, b and c", "lists");

    // ERVR.log's lines.
    LogLine line = parseLogLine("17:36:02:114 [ 1234] | INFO  | ERVR 0.4.0 starting (pid 99) in C:\\Game\\ERVR");
    Check(line.time == "17:36:02" && line.kind == LineKind::good, "start line is a milestone");
    Check(line.text.rfind("ERVR 0.4.0 starting", 0) == 0, "text after the level");
    Check(parseLogLine("17:36:02:114 [ 1234] | WARN  | [xr] x").kind == LineKind::warning, "warning");
    Check(parseLogLine("17:36:02:114 [ 1234] | ERROR | boom").kind == LineKind::error, "error");
    Check(parseLogLine("17:36:02:114 [ 1234] | DEBUG | hb").kind == LineKind::muted, "debug is muted");
    Check(parseLogLine("17:36:02:114 [ 1234] | INFO  | ERVR.ini reloaded").kind == LineKind::normal, "plain info");
    Check(parseLogLine("Starting Steam...").time.empty(), "launcher lines stay whole");

    Progress progress;
    Check(noteProgress("ERVR 0.4.0 starting (pid 99) in X", progress) && progress.loaded && progress.version == "0.4.0",
          "loaded");
    noteProgress("[xr] xrCreateInstance failed: XR_ERROR_RUNTIME_FAILURE (-2). The runtime ...", progress);
    Check(progress.waiting, "waiting for the runtime");
    noteProgress("[xr] runtime \"VirtualDesktopXR\" 1.0.9", progress);
    Check(!progress.waiting && progress.runtime == "VirtualDesktopXR", "runtime");
    noteProgress("[xr] system \"Meta Quest 3\": recommended eye 2688x2880, max 4096x4096", progress);
    Check(progress.headset == "Meta Quest 3" && progress.eye == "2688x2880", "headset and eye");
    noteProgress("[xr] session running", progress);
    Check(progress.vr, "in VR");

    // Steam.
    const std::string vdf = "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n\t}\n}";
    Check(vdfValues(vdf, "PATH") == std::vector<std::string>{"D:\\SteamLibrary"}, "vdf path, escapes decoded");

    // Versions and the shared address-map list.
    Check(parseVersion("2.7.2.0") == std::array<int, 4>{2, 7, 2, 0}, "version");
    Check(parseVersion("v0.4") == std::array<int, 4>{0, 4, 0, 0}, "short version");
    Check(verifiedPatch({2, 7, 2, 0}) == "1.17.2" && verifiedPatch({2, 6, 2, 0}) == "1.16.2", "verified builds");
    Check(verifiedPatch({2, 7, 3, 0}).empty(), "unknown build");

    // Runtimes.
    Check(runtimeLabel("C:\\Program Files\\Oculus\\Support\\oculus-runtime\\oculus_openxr_64.json") == "Meta Quest Link",
          "oculus");
    Check(jsonStrings("{\"runtime\" : [ \"C:\\\\Steam\\\\SteamVR\" ], \"config\": [\"x\"]}", "runtime") ==
              std::vector<std::string>{"C:\\Steam\\SteamVR"},
          "openvrpaths runtime list");
    Check(jsonStrings("{\"runtime\": {\"name\": \"Oculus\", \"library_path\": \"x\"}}", "name") ==
              std::vector<std::string>{"Oculus"},
          "manifest name");

    const ProbeResult probe = parseProbe("runtime: Oculus 1.106.0\r\nheadset: Meta Quest 3\neye: 2064x2208\n");
    Check(probe.runtime == "Oculus 1.106.0" && probe.headset == "Meta Quest 3" && probe.eye == "2064x2208", "probe");

    Check(quoteArgument(L"C:\\Program Files\\x\\") == L"\"C:\\Program Files\\x\\\\\"", "quoting");
    Check(withoutUserName("C:\\Users\\Jane Doe\\AppData\\x") == "~\\AppData\\x", "home folder written as ~");
    ReportFacts facts;
    facts.version = "0.4.0";
    facts.lastError = "line\nwith ``` fence";
    const std::string report = reportSummary(facts);
    Check(report.rfind("```\nERVR 0.4.0 report\n", 0) == 0 && report.find("with ''' fence") != std::string::npos,
          "report keeps to one fence");
}
