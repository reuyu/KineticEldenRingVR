#pragma once
// The launcher's text work, with no Windows calls so tools/test_launcher_text.cpp checks it anywhere:
// ERVR.ini edits that keep every other byte of the file, ERVR.log's lines and what they say about the
// session, Steam's VDF files, version numbers, OpenXR runtime names and the summary for a bug report.
// The Steam, runtime and quoting helpers follow the Spidy launcher's (same author, same jobs).
#include "game_version.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace launcher::text {

inline bool sameText(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

inline bool startsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

// "a", "a and b", "a, b and c".
inline std::string listed(const std::vector<std::string>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i)
        out += (i == 0 ? "" : i + 1 == items.size() ? " and " : ", ") + items[i];
    return out;
}

// ---- ERVR.ini ----------------------------------------------------------------------------------------------
// ERVR reads it with GetPrivateProfileString: [Section] and Key without regard to case, the first section of
// that name and the first key in it, the value the rest of the line, trimmed.

namespace detail {
struct Line {
    size_t start{}, end{}, next{}; // `end` before the line end ("\r\n" or "\n"), `next` after it
};

inline bool nextLine(std::string_view text, size_t& at, Line& line) {
    if (at >= text.size())
        return false;
    const size_t newline = text.find('\n', at);
    line.start = at;
    line.next = newline == std::string_view::npos ? text.size() : newline + 1;
    line.end = newline == std::string_view::npos ? text.size() : newline;
    if (line.end > line.start && text[line.end - 1] == '\r')
        --line.end;
    at = line.next;
    return true;
}

// A UTF-8 byte order mark is no part of the first line.
inline size_t afterBom(std::string_view text) { return startsWith(text, "\xEF\xBB\xBF") ? 3 : 0; }

inline std::optional<std::string_view> section(std::string_view line) {
    line = trim(line);
    if (line.size() < 2 || line.front() != '[')
        return std::nullopt;
    const size_t close = line.find(']');
    if (close == std::string_view::npos)
        return std::nullopt;
    return trim(line.substr(1, close - 1));
}

struct Pair {
    std::string_view key, value;
    size_t valueStart{}; // in the line: the value's first character, or the line's end for an empty value
};

inline std::optional<Pair> pair(std::string_view line) {
    const std::string_view t = trim(line);
    if (t.empty() || t.front() == ';' || t.front() == '#' || t.front() == '[')
        return std::nullopt;
    const size_t equals = line.find('=');
    if (equals == std::string_view::npos)
        return std::nullopt;
    Pair p;
    p.key = trim(line.substr(0, equals));
    if (p.key.empty())
        return std::nullopt;
    p.valueStart = equals + 1;
    while (p.valueStart < line.size() && (line[p.valueStart] == ' ' || line[p.valueStart] == '\t'))
        ++p.valueStart;
    p.value = trim(line.substr(equals + 1));
    return p;
}
} // namespace detail

inline std::optional<std::string> iniGet(std::string_view text, std::string_view section, std::string_view key) {
    size_t at = detail::afterBom(text);
    detail::Line line;
    bool inside = false, seen = false;
    while (detail::nextLine(text, at, line)) {
        const std::string_view content = text.substr(line.start, line.end - line.start);
        if (auto name = detail::section(content)) {
            if (inside)
                break; // only the first section of that name counts
            inside = !seen && sameText(*name, section);
            seen |= inside;
            continue;
        }
        if (!inside)
            continue;
        if (auto p = detail::pair(content); p && sameText(p->key, key))
            return std::string(p->value);
    }
    return std::nullopt;
}

// The text with Key set to `value`, every other byte as it was. A missing key goes after the last key of its
// section; a missing section goes at the end of the file.
inline std::string iniSet(std::string_view text, std::string_view section, std::string_view key,
                          std::string_view value) {
    const std::string eol = text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
    size_t at = detail::afterBom(text);
    detail::Line line;
    bool inside = false, found = false;
    size_t insertAt = std::string_view::npos;
    while (detail::nextLine(text, at, line)) {
        const std::string_view content = text.substr(line.start, line.end - line.start);
        if (auto name = detail::section(content)) {
            if (inside)
                break;
            if (!found && sameText(*name, section)) {
                inside = found = true;
                insertAt = line.next;
            }
            continue;
        }
        if (!inside)
            continue;
        if (auto p = detail::pair(content)) {
            if (sameText(p->key, key)) {
                std::string out(text.substr(0, line.start + p->valueStart));
                out += value;
                out += text.substr(line.end);
                return out;
            }
            insertAt = line.next;
        }
    }
    std::string out(text);
    const std::string entry = std::string(key) + "=" + std::string(value) + eol;
    if (found) {
        // After a last line without its line end, the new key starts a line of its own.
        const bool needsBreak = insertAt == text.size() && !text.empty() && text.back() != '\n';
        out.insert(insertAt, needsBreak ? eol + entry : entry);
        return out;
    }
    if (!out.empty() && out.back() != '\n')
        out += eol;
    if (!out.empty())
        out += eol;
    out += "[" + std::string(section) + "]" + eol + entry;
    return out;
}

// Kinetic: carry a section's new settings into the player's ERVR.ini on install/update, never changing a value they
// have. A section the player's file lacks is copied whole from `from` (with its comments); otherwise each key of it
// that the player's file lacks is added with the shipped value. Returns the player's text unchanged when nothing is missing.
inline std::string iniAddMissing(std::string_view text, std::string_view from, std::string_view section) {
    // The section's lines in `from`: its header up to the next header (or the end).
    size_t at = detail::afterBom(from), blockStart = std::string_view::npos, blockEnd = from.size();
    detail::Line line;
    std::vector<std::pair<std::string, std::string>> keys;
    while (detail::nextLine(from, at, line)) {
        const std::string_view content = from.substr(line.start, line.end - line.start);
        if (auto name = detail::section(content)) {
            if (blockStart != std::string_view::npos) { blockEnd = line.start; break; }
            if (sameText(*name, section)) blockStart = line.start;
            continue;
        }
        if (blockStart == std::string_view::npos) continue;
        if (auto p = detail::pair(content)) keys.emplace_back(std::string(p->key), std::string(p->value));
    }
    if (blockStart == std::string_view::npos) return std::string(text);
    bool has = false;
    {
        size_t t = detail::afterBom(text);
        while (detail::nextLine(text, t, line))
            if (auto name = detail::section(text.substr(line.start, line.end - line.start)); name && sameText(*name, section)) { has = true; break; }
    }
    if (!has) {
        const std::string eol = text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
        std::string block(from.substr(blockStart, blockEnd - blockStart));
        while (!block.empty() && (block.back() == '\n' || block.back() == '\r')) block.pop_back();
        // The block in the player's line ends.
        std::string converted;
        for (size_t i = 0; i < block.size(); ++i) {
            if (block[i] == '\r') continue;
            if (block[i] == '\n') converted += eol; else converted += block[i];
        }
        std::string out(text);
        if (!out.empty() && out.back() != '\n') out += eol;
        if (!out.empty()) out += eol;
        out += converted + eol;
        return out;
    }
    std::string out(text);
    for (const auto& [key, value] : keys)
        if (!iniGet(out, section, key)) out = iniSet(out, section, key, value);
    return out;
}

// ---- ELDEN RING's GraphicsConfig.xml -----------------------------------------------------------------------
// The game's own graphics settings, %APPDATA%\EldenRing\GraphicsConfig.xml: UTF-16LE text (a byte order mark, if
// any, is part of it), one <Tag>VALUE</Tag> a setting. The game reads it as it starts and writes its settings back
// as it closes. Tags and values are ASCII.

// The effects START VR turns off: each blurs the picture or costs more in a headset than it gives.
struct GameEffect {
    std::string_view tag;
    const char* name; // for people: "depth of field"
};
inline constexpr std::array<GameEffect, 3> kVrEffects{{
    {"DepthOfField", "depth of field"},
    {"MotionBlur", "motion blur"},
    {"RaytracingQuality", "ray tracing"},
}};
// The game's word for an effect switched off, as its own menu writes it (LOW to MAX are the others).
inline constexpr std::string_view kEffectOff = "DISABLE";
// Which of kVrEffects to turn off, in its order.
using EffectsOff = std::array<bool, kVrEffects.size()>;
inline constexpr EffectsOff allEffects() {
    EffectsOff all{};
    for (bool& on : all)
        on = true;
    return all;
}

namespace detail {
inline std::wstring wide(std::string_view ascii) { return std::wstring(ascii.begin(), ascii.end()); }

// Where <tag>'s value starts in the text, and its length.
inline std::optional<std::pair<size_t, size_t>> xmlValue(std::wstring_view xml, std::string_view tag) {
    const std::wstring open = L"<" + wide(tag) + L">", close = L"</" + wide(tag) + L">";
    const size_t at = xml.find(open);
    if (at == std::wstring_view::npos)
        return std::nullopt;
    const size_t start = at + open.size();
    const size_t end = xml.find(close, start);
    if (end == std::wstring_view::npos)
        return std::nullopt;
    return std::pair{start, end - start};
}
} // namespace detail

// <tag>'s value, trimmed ("MAX"); none when the file has no such tag.
inline std::optional<std::string> xmlGet(std::wstring_view xml, std::string_view tag) {
    const auto value = detail::xmlValue(xml, tag);
    if (!value)
        return std::nullopt;
    std::string out;
    for (const wchar_t c : xml.substr(value->first, value->second))
        out += c < 0x80 ? static_cast<char>(c) : '?';
    return std::string(trim(out));
}

// <tag>'s value replaced, every other character as it was; false when the file has no such tag.
inline bool xmlSet(std::wstring& xml, std::string_view tag, std::string_view value) {
    const auto at = detail::xmlValue(xml, tag);
    if (!at)
        return false;
    xml.replace(at->first, at->second, detail::wide(value));
    return true;
}

// The effects `which` names switched off in the file's text. When one changes, the quality preset becomes CUSTOM
// and auto-detect OFF, as the game's own menu writes them when a single setting changes; a named preset or
// auto-detect could pick the effect again. A missing tag is left missing. Returns what changed, as
// "depth of field (was MAX)".
inline std::vector<std::string> turnEffectsOff(std::wstring& xml, const EffectsOff& which) {
    std::vector<std::string> changed;
    for (size_t i = 0; i < kVrEffects.size(); ++i) {
        const auto value = which[i] ? xmlGet(xml, kVrEffects[i].tag) : std::nullopt;
        if (!value || sameText(*value, kEffectOff))
            continue;
        xmlSet(xml, kVrEffects[i].tag, kEffectOff);
        changed.push_back(std::string(kVrEffects[i].name) + " (was " + (value->empty() ? "empty" : *value) + ")");
    }
    if (!changed.empty()) {
        xmlSet(xml, "QualitySetting", "CUSTOM");
        xmlSet(xml, "Auto-detectBestRenderingSettings", "OFF");
    }
    return changed;
}

// ---- ERVR.log ----------------------------------------------------------------------------------------------

enum class LineKind { normal, good, warning, error, muted };

struct LogLine {
    std::string time; // "17:36:02"; empty for a line of another shape
    LineKind kind = LineKind::normal;
    std::string text;
};

// What the session reached, from ERVR's own lines.
struct Progress {
    bool loaded{};          // "ERVR 0.4.0 starting": the game took the mod
    std::string version;    // ERVR's, from that line
    bool disabled{};        // [General] Enabled=false
    bool waiting{};         // the runtime refused a connection (VDXR before the headset streams); retrying
    std::string runtime;    // [xr] runtime "Oculus" 1.106.0
    std::string headset;    // [xr] system "Meta Quest 3": recommended eye 2064x2208, ...
    std::string eye;        // "2064x2208"
    bool addon{};           // "ReShade add-on live": ReShade took the add-on
    bool vr{};              // "[xr] session running": the headset shows the game
    std::string lastError;  // the last ERROR line
};

namespace detail {
// The text between the first pair of double quotes after `at`.
inline std::string quoted(std::string_view text, size_t at = 0) {
    const size_t open = text.find('"', at);
    const size_t close = open == std::string_view::npos ? open : text.find('"', open + 1);
    return close == std::string_view::npos ? std::string() : std::string(text.substr(open + 1, close - open - 1));
}
} // namespace detail

// What one of ERVR's messages says about the session; true when it changed `progress`.
inline bool noteProgress(std::string_view message, Progress& progress) {
    if (startsWith(message, "ERVR ") && message.find(" starting") != std::string_view::npos) {
        progress = Progress{};
        progress.loaded = true;
        const size_t end = message.find(' ', 5);
        progress.version = std::string(message.substr(5, end == std::string_view::npos ? 0 : end - 5));
        return true;
    }
    if (startsWith(message, "Disabled via ERVR.ini")) {
        progress.disabled = true;
        return true;
    }
    if (startsWith(message, "[xr] xrCreateInstance failed")) {
        progress.waiting = true;
        return true;
    }
    if (startsWith(message, "[xr] runtime \"")) {
        progress.runtime = detail::quoted(message);
        progress.waiting = false;
        return true;
    }
    if (startsWith(message, "[xr] system \"")) {
        progress.headset = detail::quoted(message);
        const size_t eye = message.find("recommended eye ");
        if (eye != std::string_view::npos) {
            const std::string_view rest = message.substr(eye + 16);
            progress.eye = std::string(rest.substr(0, rest.find(',')));
        }
        return true;
    }
    if (startsWith(message, "ReShade add-on live")) {
        progress.addon = true;
        return true;
    }
    if (startsWith(message, "[xr] session running")) {
        progress.vr = true;
        return true;
    }
    return false;
}

// "17:36:02:114 [ 1234] | INFO  | text": the time to the second, the level as the line's colour, the text.
// A line of another shape is kept whole.
inline LogLine parseLogLine(std::string_view line) {
    LogLine out;
    const size_t first = line.find(" | ");
    const size_t second = first == std::string_view::npos ? first : line.find(" | ", first + 3);
    if (line.size() < 12 || line[2] != ':' || line[5] != ':' || second == std::string_view::npos) {
        out.text = std::string(line);
        return out;
    }
    out.time = std::string(line.substr(0, 8));
    const std::string_view level = trim(line.substr(first + 3, second - first - 3));
    out.text = std::string(line.substr(second + 3));
    if (level == "DEBUG") {
        out.kind = LineKind::muted;
    } else if (level == "WARN") {
        out.kind = LineKind::warning;
    } else if (level == "ERROR") {
        out.kind = LineKind::error;
    } else {
        Progress probe;
        out.kind = noteProgress(out.text, probe) && !probe.waiting && !probe.disabled ? LineKind::good
                                                                                      : LineKind::normal;
    }
    return out;
}

// ---- Steam's VDF / ACF files -------------------------------------------------------------------------------

struct VdfToken {
    std::string text;
    bool quoted{};
};

// Quoted strings (escapes \\ \" \n \t decoded) and braces; // comments are skipped.
inline std::vector<VdfToken> vdfTokens(std::string_view text) {
    std::vector<VdfToken> tokens;
    for (size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n')
                ++i;
        } else if (c == '"') {
            std::string token;
            for (++i; i < text.size() && text[i] != '"'; ++i) {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    const char next = text[++i];
                    token += next == 'n' ? '\n' : next == 't' ? '\t' : next;
                } else {
                    token += text[i];
                }
            }
            ++i;
            tokens.push_back({std::move(token), true});
        } else if (c == '{' || c == '}') {
            tokens.push_back({std::string(1, c), false});
            ++i;
        } else {
            ++i;
        }
    }
    return tokens;
}

// Every value that follows `key`, in order, at any depth; keys match without regard to case, as Steam's do.
inline std::vector<std::string> vdfValues(std::string_view text, std::string_view key) {
    const auto tokens = vdfTokens(text);
    std::vector<std::string> values;
    for (size_t i = 0; i + 1 < tokens.size(); ++i)
        if (tokens[i].quoted && tokens[i + 1].quoted && sameText(tokens[i].text, key)) {
            values.push_back(tokens[i + 1].text);
            ++i;
        }
    return values;
}

// ---- Versions ----------------------------------------------------------------------------------------------

// "0.4.0", "2.7.2.0", "v1.2": four fields, missing ones 0.
inline std::array<int, 4> parseVersion(std::string_view text) {
    std::array<int, 4> fields{};
    size_t field = 0;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            fields[field] = fields[field] * 10 + (c - '0');
        } else if (c == '.') {
            if (++field == fields.size())
                break;
        } else if (c != 'v' && c != 'V') {
            break;
        }
    }
    return fields;
}

// The ELDEN RING patch an eldenring.exe version has a verified address map for ("1.17.2"); empty otherwise.
// game_version.h is the one list, shared with ERVR.dll.
inline std::string verifiedPatch(const std::array<int, 4>& exe) {
    const auto build = ervr::IdentifyGameBuild((static_cast<uint32_t>(exe[0]) << 16) | static_cast<uint32_t>(exe[1]),
                                               (static_cast<uint32_t>(exe[2]) << 16) | static_cast<uint32_t>(exe[3]));
    switch (build) {
    case ervr::GameBuild::V262: return "1.16.2";
    case ervr::GameBuild::V271: return "1.17.1";
    case ervr::GameBuild::V272: return "1.17.2";
    default: return {};
    }
}

// ---- OpenXR runtimes ---------------------------------------------------------------------------------------

// A friendly name for a runtime manifest's path; empty when the path says nothing.
inline std::string runtimeLabel(std::string_view manifest) {
    std::string lowered(manifest);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 8> known{{
        {"virtualdesktop", "Virtual Desktop"},
        {"steamxr", "SteamVR"},
        {"oculus", "Meta Quest Link"},
        {"mixedreality", "Windows Mixed Reality"},
        {"pimax", "Pimax"},
        {"varjo", "Varjo"},
        {"wivrn", "WiVRn"},
        {"vive", "VIVE"},
    }};
    for (const auto& [piece, label] : known)
        if (lowered.find(piece) != std::string::npos)
            return std::string(label);
    return {};
}

// The strings of the JSON array that follows "key" (openvrpaths.vrpath's "runtime": the SteamVR folders), or
// the one string that follows it (a runtime manifest's "name"); \\ \/ \" decoded.
inline std::vector<std::string> jsonStrings(std::string_view json, std::string_view key) {
    std::vector<std::string> out;
    const size_t at = json.find("\"" + std::string(key) + "\"");
    if (at == std::string_view::npos)
        return out;
    size_t i = json.find(':', at);
    if (i == std::string_view::npos)
        return out;
    ++i;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i])))
        ++i;
    const bool list = i < json.size() && json[i] == '[';
    for (; i < json.size(); ++i) {
        if (json[i] == ']' || (!list && !out.empty()))
            break;
        if (json[i] != '"')
            continue;
        std::string value;
        for (++i; i < json.size() && json[i] != '"'; ++i) {
            if (json[i] == '\\' && i + 1 < json.size())
                ++i;
            value += json[i];
        }
        out.push_back(std::move(value));
    }
    return out;
}

// ---- The headset check (the launcher's own --probe) --------------------------------------------------------

struct ProbeResult {
    std::string runtime, headset, eye, error;
};

// "runtime: Oculus 1.106.0", "headset: Meta Quest 3", "eye: 2064x2208", "error: ..." lines.
inline ProbeResult parseProbe(std::string_view output) {
    ProbeResult result;
    while (!output.empty()) {
        const size_t end = output.find('\n');
        const std::string_view line = trim(output.substr(0, end));
        output = end == std::string_view::npos ? std::string_view() : output.substr(end + 1);
        const auto take = [&](std::string_view prefix, std::string& into) {
            if (startsWith(line, prefix))
                into = std::string(trim(line.substr(prefix.size())));
        };
        take("runtime:", result.runtime);
        take("headset:", result.headset);
        take("eye:", result.eye);
        take("error:", result.error);
    }
    return result;
}

// ---- Command lines -----------------------------------------------------------------------------------------

// Quotes one argument so CommandLineToArgvW reads it back unchanged.
inline std::wstring quoteArgument(std::wstring_view argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        return std::wstring(argument);
    std::wstring quoted = L"\"";
    for (size_t i = 0;; ++i) {
        size_t slashes = 0;
        while (i < argument.size() && argument[i] == L'\\') {
            ++slashes;
            ++i;
        }
        if (i == argument.size()) {
            quoted.append(slashes * 2, L'\\');
            break;
        }
        quoted.append(argument[i] == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        quoted += argument[i];
    }
    return quoted + L'"';
}

// ---- The summary for a bug report --------------------------------------------------------------------------

// Paths under C:\Users\<name> written as ~, so that the summary can be posted without the Windows user name.
inline std::string withoutUserName(std::string_view text) {
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto slashes = [&](size_t& at) {
        const size_t from = at;
        while (at < text.size() && (text[at] == '\\' || text[at] == '/'))
            ++at;
        return at > from;
    };
    std::string out;
    for (size_t i = 0; i < text.size();) {
        size_t at = i + 2;
        if (lowered[i] >= 'a' && lowered[i] <= 'z' && i + 1 < text.size() && text[i + 1] == ':' && slashes(at) &&
            lowered.compare(at, 5, "users") == 0 && slashes(at += 5)) {
            size_t end = at;
            while (end < text.size() && static_cast<unsigned char>(text[end]) >= 0x20 &&
                   std::string_view("\\/\"<>|:*?").find(text[end]) == std::string_view::npos)
                ++end;
            if (end > at) {
                out += '~';
                i = end;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

struct ReportFacts {
    std::string version;  // the launcher's ERVR
    std::string game;     // eldenring.exe's version and where it came from
    std::string install;  // ERVR in the game folder
    std::string runtime;  // the runtime chosen, and the ones installed
    std::string headset;  // the check's line, or what the last session found
    std::string mode;     // offline or Seamless Co-op
    std::string others;   // flat mods / R.E.A.L. files in the game folder
    std::string gpu, ram;
    std::string lastLine, lastError; // the session's
};

// A fenced block for an issue or a Discord post: "ERVR <version> report", then one "Label: value" line
// each, values without line ends, code fences or the Windows user name.
inline std::string reportSummary(const ReportFacts& facts) {
    const auto value = [](std::string_view text, size_t limit) {
        std::string out = withoutUserName(text);
        for (char& c : out)
            if (static_cast<unsigned char>(c) < 0x20)
                c = ' ';
            else if (c == '`')
                c = '\'';
        out = std::string(trim(out));
        if (out.size() > limit) {
            size_t cut = limit - 3; // not inside a UTF-8 character
            while (cut && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
                --cut;
            out.resize(cut);
            out += "...";
        }
        return out.empty() ? std::string("unknown") : out;
    };
    std::string text = "```\nERVR " + value(facts.version, 20) + " report\n";
    text += "Game: " + value(facts.game, 200) + '\n';
    text += "ERVR in the game folder: " + value(facts.install, 200) + '\n';
    text += "VR runtime: " + value(facts.runtime, 200) + '\n';
    text += "Headset: " + value(facts.headset, 160) + '\n';
    text += "Start mode: " + value(facts.mode, 60) + '\n';
    text += "Other mods: " + value(facts.others, 200) + '\n';
    text += "GPU: " + value(facts.gpu, 100) + '\n';
    text += "RAM: " + value(facts.ram, 40) + '\n';
    if (!facts.lastLine.empty())
        text += "Last session: " + value(facts.lastLine, 300) + '\n';
    if (!facts.lastError.empty())
        text += "Last error: " + value(facts.lastError, 300) + '\n';
    return text + "```";
}

} // namespace launcher::text
