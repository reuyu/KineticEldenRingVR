#include "system.hpp"
#include <dxgi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace launcher {
namespace {

// What a play package holds: tools/package_release.ps1's layout, minus the documents.
constexpr std::array kPayloadFiles{L"dxgi.dll", L"ReShade.ini", L"dinput8.dll", L"ERVR\\ERVR.dll", L"ERVR\\ERVR.ini"};
// Documents that travel with an install when the package has them.
constexpr std::array kPayloadDocuments{L"LICENSE-ReShade.txt", L"ERVR\\README.md", L"ERVR\\CHANGELOG.md",
                                       L"ERVR\\LICENSE.txt", L"ERVR\\THIRD_PARTY.md"};
// bHaptics' library beside ERVR.dll (body haptics) and the notices that travel with it: installed with ERVR.dll
// when the package has them, removed with it. Without it ERVR only goes without body haptics.
constexpr std::array kPayloadBhaptics{L"ERVR\\bhaptics_library.dll", L"ERVR\\LICENSE-bHaptics.txt",
                                      L"ERVR\\bHaptics-THIRD_PARTY_NOTICES.txt"};
// R.E.A.L. VR's files: it and ReShade both want to be dxgi.dll (install.ps1's leftovers).
constexpr std::array kRealVrFiles{L"RealVR64.dll", L"openvr_api.dll", L"cudart64_12.dll", L"RealVR.ini"};
// The processes that are the game: Steam's start_protected_game.exe is a copy of eldenring.exe on installs
// whose EAC launcher was replaced (tools/sync_game_launcher.ps1).
constexpr std::array kGameProcesses{L"eldenring.exe", L"start_protected_game.exe"};

struct Handle {
    HANDLE value{};
    Handle() = default;
    explicit Handle(HANDLE h) : value(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (value)
            CloseHandle(value);
    }
    explicit operator bool() const { return value != nullptr; }
};

std::wstring registryString(HKEY root, const wchar_t* key, const wchar_t* name, DWORD flags = 0) {
    wchar_t buffer[2048];
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | flags, nullptr, buffer, &size) !=
        ERROR_SUCCESS)
        return {};
    return buffer;
}

DWORD registryDword(HKEY root, const wchar_t* key, const wchar_t* name) {
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS ? value : 0;
}

std::wstring environment(const wchar_t* name) {
    wchar_t buffer[4096];
    const DWORD size = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    return size && size < std::size(buffer) ? std::wstring(buffer, size) : std::wstring();
}

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR path{};
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)))
        result = path;
    CoTaskMemFree(path);
    return result;
}

std::string readText(const fs::path& path, size_t limit = 4 << 20) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::string content(limit, '\0');
    file.read(content.data(), static_cast<std::streamsize>(limit));
    content.resize(static_cast<size_t>(file.gcount()));
    return content;
}

bool isFile(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}

// The path as Windows spells it (Steam's registry keeps it in lower case).
std::wstring truePath(const std::wstring& path) {
    Handle file(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    wchar_t buffer[MAX_PATH * 4];
    const DWORD length = file ? GetFinalPathNameByHandleW(file.value, buffer, static_cast<DWORD>(std::size(buffer)),
                                                          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)
                              : 0;
    if (!length || length >= std::size(buffer))
        return path;
    std::wstring result(buffer, length);
    if (result.rfind(L"\\\\?\\UNC\\", 0) == 0)
        return L"\\\\" + result.substr(8);
    return result.rfind(L"\\\\?\\", 0) == 0 ? result.substr(4) : result;
}

bool sameFile(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool sameFolder(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && !b.empty() && sameFile(truePath(a), truePath(b));
}

// Byte for byte.
bool sameContent(const fs::path& a, const fs::path& b) {
    std::error_code error;
    const auto sizeA = fs::file_size(a, error);
    if (error)
        return false;
    const auto sizeB = fs::file_size(b, error);
    if (error || sizeA != sizeB)
        return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb)
        return false;
    std::vector<char> ba(1 << 20), bb(1 << 20);
    while (fa && fb) {
        fa.read(ba.data(), static_cast<std::streamsize>(ba.size()));
        fb.read(bb.data(), static_cast<std::streamsize>(bb.size()));
        if (fa.gcount() != fb.gcount() || std::memcmp(ba.data(), bb.data(), static_cast<size_t>(fa.gcount())) != 0)
            return false;
    }
    return true;
}

std::array<int, 4> fileVersion(const fs::path& path) {
    DWORD ignored{};
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    std::vector<char> data(size);
    VS_FIXEDFILEINFO* info{};
    UINT length{};
    if (!size || !GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info)
        return {};
    return {static_cast<int>(HIWORD(info->dwFileVersionMS)), static_cast<int>(LOWORD(info->dwFileVersionMS)),
            static_cast<int>(HIWORD(info->dwFileVersionLS)), static_cast<int>(LOWORD(info->dwFileVersionLS))};
}

std::string versionText(const std::array<int, 4>& v, int fields) {
    std::string out;
    for (int i = 0; i < fields; ++i)
        out += (i ? "." : "") + std::to_string(v[i]);
    return out;
}

// A string of the file's version resource ("ProductName"); empty without one.
std::string versionString(const fs::path& path, const wchar_t* name) {
    DWORD ignored{};
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    std::vector<char> data(size);
    if (!size || !GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return {};
    struct Translation {
        WORD language, codePage;
    }* translations{};
    UINT length{};
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length) ||
        length < sizeof(Translation))
        return {};
    wchar_t query[128];
    swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\%s", translations[0].language, translations[0].codePage, name);
    wchar_t* value{};
    if (!VerQueryValueW(data.data(), query, reinterpret_cast<void**>(&value), &length) || !value)
        return {};
    return narrow(value);
}

bool isErvrModule(const fs::path& path) { return versionString(path, L"ProductName").rfind("ERVR", 0) == 0; }

bool processRunning(const wchar_t* name) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry{sizeof(entry)};
    if (!snapshot || !Process32FirstW(snapshot.value, &entry))
        return false;
    do {
        if (_wcsicmp(entry.szExeFile, name) == 0)
            return true;
    } while (Process32NextW(snapshot.value, &entry));
    return false;
}

// The game's process when one runs from `gameDir`: its id and a handle to wait on (none when Windows keeps
// it from this launcher).
struct GameProcess {
    DWORD pid{};
    HANDLE handle{};
};

std::optional<GameProcess> findGame(const std::wstring& gameDir) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry{sizeof(entry)};
    if (!snapshot || !Process32FirstW(snapshot.value, &entry))
        return std::nullopt;
    do {
        if (std::none_of(kGameProcesses.begin(), kGameProcesses.end(),
                         [&](const wchar_t* name) { return _wcsicmp(entry.szExeFile, name) == 0; }))
            continue;
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process)
            return GameProcess{entry.th32ProcessID, nullptr};
        wchar_t image[MAX_PATH * 4];
        DWORD size = static_cast<DWORD>(std::size(image));
        if (QueryFullProcessImageNameW(process, 0, image, &size) &&
            sameFolder(fs::path(image).parent_path().wstring(), gameDir))
            return GameProcess{entry.th32ProcessID, process};
        CloseHandle(process);
    } while (Process32NextW(snapshot.value, &entry));
    return std::nullopt;
}

// Environment block with `name` set to `value`, or removed when `value` is empty.
std::wstring environmentWith(std::vector<std::pair<std::wstring, std::wstring>> changes) {
    std::wstring block;
    if (wchar_t* strings = GetEnvironmentStringsW()) {
        for (const wchar_t* entry = strings; *entry; entry += wcslen(entry) + 1) {
            const bool changed = std::any_of(changes.begin(), changes.end(), [&](const auto& c) {
                return _wcsnicmp(entry, c.first.c_str(), c.first.size()) == 0 && entry[c.first.size()] == L'=';
            });
            if (!changed) {
                block += entry;
                block += L'\0';
            }
        }
        FreeEnvironmentStringsW(strings);
    }
    for (const auto& [name, value] : changes)
        if (!value.empty())
            block += name + L'=' + value + L'\0';
    block += L'\0';
    return block;
}

std::wstring exePath() {
    wchar_t exe[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    return length && length < std::size(exe) ? std::wstring(exe, length) : std::wstring();
}

std::string timeNow() {
    SYSTEMTIME now;
    GetLocalTime(&now);
    char text[16];
    snprintf(text, sizeof(text), "%02u:%02u:%02u", now.wHour, now.wMinute, now.wSecond);
    return text;
}

std::wstring stampNow() {
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t text[32];
    swprintf_s(text, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    return text;
}

// --- Steam and the game ----------------------------------------------------------------------------------

std::vector<std::wstring> steamRoots() {
    std::vector<std::wstring> roots;
    for (auto path : {registryString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"),
                      registryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"),
                      registryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath"),
                      environment(L"ProgramFiles(x86)") + L"\\Steam"}) {
        std::replace(path.begin(), path.end(), L'/', L'\\');
        if (!path.empty() && isFile(fs::path(path) / L"steam.exe") &&
            std::none_of(roots.begin(), roots.end(), [&](const auto& r) { return sameFile(r, path); }))
            roots.push_back(path);
    }
    return roots;
}

std::wstring steamGame(const std::vector<std::wstring>& roots) {
    std::vector<std::wstring> libraries;
    for (const auto& root : roots) {
        libraries.push_back(root);
        for (const auto* file : {L"steamapps\\libraryfolders.vdf", L"config\\libraryfolders.vdf"})
            for (const auto& path : text::vdfValues(readText(fs::path(root) / file), "path"))
                libraries.push_back(widen(path));
    }
    const std::wstring manifest = std::wstring(L"steamapps\\appmanifest_") + kSteamAppId + L".acf";
    for (const auto& library : libraries) {
        const fs::path acf = fs::path(library) / manifest;
        if (!isFile(acf))
            continue;
        const auto folders = text::vdfValues(readText(acf), "installdir");
        const std::wstring folder = folders.empty() ? L"ELDEN RING" : widen(folders.front());
        const fs::path exe = fs::path(library) / L"steamapps\\common" / folder / L"Game\\eldenring.exe";
        if (isFile(exe))
            return exe.wstring();
    }
    // Steam also records the folder for Windows' installed-apps list.
    const std::wstring uninstall = registryString(
        HKEY_LOCAL_MACHINE,
        (std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App ") + kSteamAppId).c_str(),
        L"InstallLocation", RRF_SUBKEY_WOW6464KEY);
    if (!uninstall.empty() && isFile(fs::path(uninstall) / L"Game\\eldenring.exe"))
        return (fs::path(uninstall) / L"Game\\eldenring.exe").wstring();
    return {};
}

bool steamSignedIn() {
    return registryDword(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser") != 0 &&
           processRunning(L"steam.exe");
}

// --- ERVR's files ------------------------------------------------------------------------------------------

// The folder with the files to install: beside this launcher (the zip), else build\dist when the launcher
// runs from a build folder of a checkout.
std::wstring findPayload() {
    const fs::path exe = exePath();
    fs::path folder = exe.parent_path();
    const auto holds = [](const fs::path& f) { return isFile(f / L"ERVR\\ERVR.dll"); };
    if (holds(folder))
        return folder.wstring();
    for (int i = 0; i < 3 && !folder.empty() && folder != folder.parent_path(); ++i, folder = folder.parent_path())
        if (holds(folder / L"dist"))
            return (folder / L"dist").wstring();
    return {};
}

std::wstring resolveGamePath(const std::wstring& gameDir, const std::string& value) {
    const fs::path p(widen(value));
    return (p.is_absolute() ? p : fs::path(gameDir) / p).lexically_normal().wstring();
}

// A ReShade preset that runs effects: one that names techniques (ReShade saves an empty one by itself).
bool presetRunsEffects(const fs::path& preset) {
    std::istringstream lines(readText(preset, 1 << 20));
    std::string line;
    while (std::getline(lines, line)) {
        const size_t equals = line.find('=');
        if (equals != std::string::npos && text::sameText(text::trim(line.substr(0, equals)), "Techniques"))
            return !text::trim(line.substr(equals + 1)).empty();
    }
    return false;
}

// game_mode.ps1's list: DLSS Enabler / OptiScaler DLLs, ReShade's add-ons and its effects preset, plus
// R.E.A.L. VR's files. Each live or parked.
std::vector<ModFile> flatMods(const std::wstring& gameDir) {
    std::vector<ModFile> mods;
    const auto add = [&](const fs::path& live, std::string label) {
        const bool here = isFile(live), parked = isFile(live.wstring() + kParked);
        if ((here || parked) && std::none_of(mods.begin(), mods.end(), [&](const ModFile& m) {
                return sameFile(m.path, live.wstring());
            }))
            mods.push_back({live.wstring(), std::move(label), !here && parked});
    };
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(gameDir, error)) {
        std::wstring name = entry.path().filename().wstring();
        std::wstring live = entry.path().wstring();
        if (name.size() > wcslen(kParked) && _wcsicmp(name.c_str() + name.size() - wcslen(kParked), kParked) == 0) {
            live.resize(live.size() - wcslen(kParked));
            name.resize(name.size() - wcslen(kParked));
        }
        if (name.size() < 4 || _wcsicmp(name.c_str() + name.size() - 4, L".dll") != 0 || !entry.is_regular_file(error))
            continue;
        const std::string product = versionString(entry.path(), L"ProductName");
        if (product.find("DLSS Enabler") != std::string::npos || product.find("OptiScaler") != std::string::npos)
            add(live, product + " (" + narrow(name) + ")");
    }
    const fs::path reshadeIni = fs::path(gameDir) / L"ReShade.ini";
    const std::string ini = readText(reshadeIni, 1 << 20);
    const auto setting = [&](const char* key) -> std::string {
        // AddonPath and PresetPath sit in different sections across ReShade versions: any section.
        std::istringstream lines(ini);
        std::string line;
        while (std::getline(lines, line)) {
            const size_t equals = line.find('=');
            if (equals != std::string::npos && text::sameText(text::trim(line.substr(0, equals)), key))
                return std::string(text::trim(line.substr(equals + 1)));
        }
        return {};
    };
    const std::string addonPath = setting("AddonPath");
    const std::wstring addons = addonPath.empty() ? gameDir : resolveGamePath(gameDir, addonPath);
    for (const auto& entry : fs::directory_iterator(addons, error)) {
        std::wstring name = entry.path().filename().wstring();
        std::wstring live = entry.path().wstring();
        if (name.size() > wcslen(kParked) && _wcsicmp(name.c_str() + name.size() - wcslen(kParked), kParked) == 0) {
            live.resize(live.size() - wcslen(kParked));
            name.resize(name.size() - wcslen(kParked));
        }
        const auto ends = [&](const wchar_t* suffix) {
            const size_t n = wcslen(suffix);
            return name.size() > n && _wcsicmp(name.c_str() + name.size() - n, suffix) == 0;
        };
        if (ends(L".addon") || ends(L".addon64"))
            add(live, "ReShade add-on " + narrow(name));
    }
    if (const std::string preset = setting("PresetPath"); !preset.empty()) {
        const std::wstring live = resolveGamePath(gameDir, preset);
        if (presetRunsEffects(isFile(live) ? live : live + kParked))
            add(live, "ReShade effects (" + narrow(fs::path(live).filename().wstring()) + ")");
    }
    for (const auto* name : kRealVrFiles)
        add(fs::path(gameDir) / name, "R.E.A.L. VR (" + narrow(name) + ")");
    return mods;
}

bool isRealVr(const ModFile& mod) { return mod.label.rfind("R.E.A.L.", 0) == 0; }

Install scanInstall(const std::wstring& gameDir, const std::wstring& payload, const std::string& payloadVersion) {
    Install in;
    const fs::path game(gameDir), ervr = game / L"ERVR";
    in.dll = isFile(ervr / L"ERVR.dll");
    in.ini = isFile(ervr / L"ERVR.ini");
    if (in.dll)
        in.version = versionText(fileVersion(ervr / L"ERVR.dll"), 3);
    const fs::path proxy = game / L"dinput8.dll";
    if (isFile(proxy)) {
        in.proxy = isErvrModule(proxy);
        in.foreignProxy = !in.proxy;
    }
    in.proxyParked = isFile(proxy.wstring() + kParked) && isErvrModule(proxy.wstring() + kParked);
    const fs::path dxgi = game / L"dxgi.dll";
    if (isFile(dxgi)) {
        const std::string product = versionString(dxgi, L"ProductName");
        in.reshade = !payload.empty() ? sameContent(dxgi, fs::path(payload) / L"dxgi.dll") : product == "ReShade";
        if (!in.reshade)
            in.dxgi = product == "ReShade"    ? "ReShade " + versionText(fileVersion(dxgi), 3)
                      : product.empty() ? "not ReShade"
                                        : product;
    }
    in.flat = flatMods(gameDir);
    // Parked for flat play (game_mode.ps1 -Mode flat); without its DLL or its loader, not installed.
    if (in.proxyParked && !in.proxy && !in.foreignProxy) {
        in.state = InstallState::parked;
        return in;
    }
    if (!in.dll || (!in.proxy && !in.foreignProxy)) {
        in.state = InstallState::missing;
        return in;
    }
    if (payload.empty()) {
        in.state = InstallState::current;
        return in;
    }
    const auto have = text::parseVersion(in.version), offer = text::parseVersion(payloadVersion);
    if (have < offer)
        in.state = InstallState::older;
    else if (have > offer)
        in.state = InstallState::newer;
    else
        in.state = sameContent(ervr / L"ERVR.dll", fs::path(payload) / L"ERVR\\ERVR.dll") &&
                           (!in.proxy || sameContent(proxy, fs::path(payload) / L"dinput8.dll"))
                       ? InstallState::current
                       : InstallState::different;
    return in;
}

bool folderWritable(const fs::path& folder) {
    Handle probe(CreateFileW((folder / L".ervr-write-check").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    return static_cast<bool>(probe);
}

// --- VR ------------------------------------------------------------------------------------------------------

std::string manifestName(const std::wstring& manifest) {
    if (auto label = text::runtimeLabel(narrow(manifest)); !label.empty())
        return label;
    const auto names = text::jsonStrings(readText(fs::path(manifest), 1 << 16), "name");
    return names.empty() ? narrow(fs::path(manifest).stem().wstring()) : names.front();
}

std::vector<Runtime> openXrRuntimes() {
    const std::wstring virtualDesktop =
        environment(L"ProgramFiles") + L"\\Virtual Desktop Streamer\\OpenXR\\virtualdesktop-openxr.json";
    const std::wstring active =
        registryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", RRF_SUBKEY_WOW6464KEY);
    std::vector<std::wstring> manifests{active, virtualDesktop};
    HKEY key{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        wchar_t name[2048];
        for (DWORD i = 0;; ++i) {
            DWORD length = static_cast<DWORD>(std::size(name));
            if (RegEnumValueW(key, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;
            manifests.emplace_back(name, length);
        }
        RegCloseKey(key);
    }
    // The registry names SteamVR only while it is Windows' active runtime; OpenVR's path file says where it is.
    const std::wstring localAppData = knownFolder(FOLDERID_LocalAppData);
    if (!localAppData.empty())
        for (const auto& folder :
             text::jsonStrings(readText(fs::path(localAppData) / L"openvr" / L"openvrpaths.vrpath"), "runtime"))
            manifests.push_back(truePath((fs::path(widen(folder)) / L"steamxr_win64.json").wstring()));
    std::vector<Runtime> runtimes;
    for (const auto& manifest : manifests) {
        if (manifest.empty() || !isFile(manifest) ||
            std::any_of(runtimes.begin(), runtimes.end(), [&](const Runtime& r) { return sameFile(r.manifest, manifest); }))
            continue;
        runtimes.push_back({manifest, manifestName(manifest), sameFile(manifest, active)});
    }
    return runtimes;
}

std::wstring settingsPath() {
    const std::wstring appData = knownFolder(FOLDERID_RoamingAppData);
    return appData.empty() ? std::wstring() : (fs::path(appData) / L"ERVR" / L"launcher.ini").wstring();
}

// --- Changes to the game folder -------------------------------------------------------------------------------

struct Changer {
    Change change;
    const std::wstring& gameDir;

    std::string name(const std::wstring& path) const {
        const fs::path p(path);
        std::error_code error;
        const fs::path relative = fs::relative(p, gameDir, error);
        return narrow((error || relative.empty() || *relative.begin() == L".." ? p.filename() : relative).wstring());
    }
    bool fail(const std::wstring& path, const char* what) {
        const DWORD code = GetLastError();
        change.ok = false;
        change.accessDenied |= code == ERROR_ACCESS_DENIED;
        change.lines.push_back(std::string("Could not ") + what + " " + name(path) +
                               (code == ERROR_ACCESS_DENIED       ? ": Windows denied access."
                                : code == ERROR_SHARING_VIOLATION ? ": the file is in use (close the game)."
                                                                  : " (Windows error " + std::to_string(code) + ")."));
        return false;
    }
    bool copy(const fs::path& from, const fs::path& to) {
        std::error_code error;
        fs::create_directories(to.parent_path(), error);
        return CopyFileW(from.c_str(), to.c_str(), FALSE) || fail(to.wstring(), "write");
    }
    bool move(const std::wstring& from, const std::wstring& to) {
        return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) || fail(from, "rename");
    }
    bool remove(const std::wstring& path) { return DeleteFileW(path.c_str()) || fail(path, "remove"); }
    // game_mode.ps1's Park / Unpark: a file of the other state already there is kept under a dated name.
    void park(const std::wstring& live) {
        if (!isFile(live))
            return;
        if (isFile(live + kParked)) {
            const std::wstring keep = live + L".ervr-new-" + stampNow();
            if (move(live, keep))
                change.lines.push_back("Kept the newer " + name(live) + " as " + name(keep) + ".");
            return;
        }
        if (move(live, live + kParked))
            change.lines.push_back("Parked " + name(live) + ".");
    }
    void unpark(const std::wstring& live) {
        if (!isFile(live + kParked))
            return;
        if (isFile(live)) {
            const std::wstring keep = live + L".ervr-new-" + stampNow();
            if (!move(live, keep))
                return;
            change.lines.push_back("Kept the file made while parked as " + name(keep) + ".");
        }
        if (move(live + kParked, live))
            change.lines.push_back("Restored " + name(live) + ".");
    }
};

std::optional<Change> refuseWhileRunning(const Scan& scan) {
    if (scan.gameDir.empty())
        return Change{false, false, {"ELDEN RING was not found."}};
    if (gameRunning(scan.gameDir))
        return Change{false, false, {"Close ELDEN RING first: its files are locked while it runs."}};
    return std::nullopt;
}

// `bytes` written whole beside `path`, then put in its place, so that no reader sees half a file.
bool replaceWhole(const std::wstring& path, std::string_view bytes) {
    const std::wstring written = path + L".new";
    {
        std::ofstream file(fs::path(written), std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            file.close();
            DeleteFileW(written.c_str());
            return false;
        }
    }
    if (!MoveFileExW(written.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(written.c_str());
        return false;
    }
    return true;
}

// --- ELDEN RING's graphics settings ---------------------------------------------------------------------------

// launcher.ini's key for one of text::kVrEffects: "turn_off_depth_of_field".
std::string effectKey(const text::GameEffect& effect) {
    std::string key = std::string("turn_off_") + effect.name;
    std::replace(key.begin(), key.end(), ' ', '_');
    return key;
}

std::vector<std::wstring> graphicsConfigFiles() {
    std::vector<std::wstring> files;
    const std::wstring appData = knownFolder(FOLDERID_RoamingAppData);
    if (appData.empty())
        return files;
    const fs::path root = fs::path(appData) / L"EldenRing";
    if (isFile(root / L"GraphicsConfig.xml"))
        files.push_back((root / L"GraphicsConfig.xml").wstring());
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(root, error))
        if (entry.is_directory(error) && isFile(entry.path() / L"GraphicsConfig.xml"))
            files.push_back((entry.path() / L"GraphicsConfig.xml").wstring());
    return files;
}

// "GraphicsConfig.xml", or "<Steam account>\GraphicsConfig.xml" for one in an account's folder.
std::string graphicsConfigName(const fs::path& file) {
    const fs::path folder = file.parent_path().filename();
    return narrow((_wcsicmp(folder.c_str(), L"EldenRing") == 0 ? file.filename() : folder / file.filename()).wstring());
}

// UTF-16LE. The game writes it without a byte order mark (ERVR.dll's edits add one); a mark there stays in the
// text, so that the file is written back as it was but for the values changed.
std::optional<std::wstring> readUtf16(const fs::path& path) {
    const std::string bytes = readText(path, 1 << 20);
    if (bytes.size() < 2)
        return std::nullopt;
    std::wstring text(bytes.size() / sizeof(wchar_t), L'\0');
    std::memcpy(text.data(), bytes.data(), text.size() * sizeof(wchar_t));
    return text;
}

bool writeUtf16(const std::wstring& path, const std::wstring& text) {
    return replaceWhole(path, std::string_view(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t)));
}

} // namespace

// --- Text ------------------------------------------------------------------------------------------------------

std::string narrow(std::wstring_view value) {
    if (value.empty())
        return {};
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring widen(std::string_view value) {
    if (value.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

// --- Settings ----------------------------------------------------------------------------------------------------

Settings loadSettings() {
    Settings settings;
    std::istringstream lines(readText(settingsPath(), 1 << 16));
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = line.substr(0, equals), value = line.substr(equals + 1);
        if (key == "game") settings.gameExe = widen(value);
        else if (key == "xr_runtime") settings.runtime = value.empty() ? kAutoRuntime : widen(value);
        else if (key == "mode") settings.mode = value == "coop" ? StartMode::coop : StartMode::offline;
        else if (key == "shortcuts_asked") settings.shortcutsAsked = value == "1";
        for (size_t i = 0; i < text::kVrEffects.size(); ++i)
            if (key == effectKey(text::kVrEffects[i]))
                settings.effectsOff[i] = value != "0";
    }
    return settings;
}

void saveSettings(const Settings& settings) {
    const fs::path path = settingsPath();
    if (path.empty())
        return;
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << "game=" << narrow(settings.gameExe) << "\nxr_runtime=" << narrow(settings.runtime)
         << "\nmode=" << (settings.mode == StartMode::coop ? "coop" : "offline")
         << "\nshortcuts_asked=" << (settings.shortcutsAsked ? 1 : 0) << "\n";
    for (size_t i = 0; i < text::kVrEffects.size(); ++i)
        file << effectKey(text::kVrEffects[i]) << "=" << (settings.effectsOff[i] ? 1 : 0) << "\n";
}

// --- Scanner -------------------------------------------------------------------------------------------------------

Scanner::~Scanner() {
    if (worker_.joinable())
        worker_.join();
}

void Scanner::start(const Settings& settings) {
    {
        std::lock_guard lock(mutex_);
        if (busy_)
            return;
        busy_ = true;
    }
    if (worker_.joinable())
        worker_.join();
    worker_ = std::thread([this, settings] { run(settings); });
}

Scan Scanner::snapshot() {
    std::lock_guard lock(mutex_);
    return scan_;
}

bool Scanner::busy() {
    std::lock_guard lock(mutex_);
    return busy_;
}

void Scanner::run(Settings settings) {
    Scan scan;
    scan.payload = findPayload();
    if (!scan.payload.empty()) {
        for (const auto* file : kPayloadFiles)
            if (!isFile(fs::path(scan.payload) / file))
                scan.payloadMissing.push_back(narrow(file));
        scan.payloadVersion = versionText(fileVersion(fs::path(scan.payload) / L"ERVR\\ERVR.dll"), 3);
    }
    const auto roots = steamRoots();
    if (!roots.empty())
        scan.steam = (fs::path(roots.front()) / L"steam.exe").wstring();
    scan.steamRunning = processRunning(L"steam.exe");
    const fs::path beside = fs::path(exePath()).parent_path() / L"eldenring.exe";
    if (!settings.gameExe.empty() && isFile(settings.gameExe)) {
        scan.gameExe = settings.gameExe;
        scan.gameSource = "chosen by you";
    } else if (isFile(beside)) {
        scan.gameExe = beside.wstring();
        scan.gameSource = "the launcher's folder";
    } else if (auto steam = steamGame(roots); !steam.empty()) {
        scan.gameExe = steam;
        scan.gameSource = "Steam";
    }
    if (!scan.gameExe.empty()) {
        scan.gameExe = truePath(scan.gameExe);
        scan.gameDir = fs::path(scan.gameExe).parent_path().wstring();
        const auto version = fileVersion(scan.gameExe);
        if (version != std::array<int, 4>{})
            scan.gameVersion = versionText(version, 4);
        scan.gamePatch = text::verifiedPatch(version);
        scan.gameRunning = gameRunning(scan.gameDir);
        scan.coop = isFile(fs::path(scan.gameDir) / L"ersc_launcher.exe");
        scan.writable = folderWritable(scan.gameDir);
        scan.install = scanInstall(scan.gameDir, scan.payload, scan.payloadVersion);
        scan.payloadInGame = sameFolder(scan.payload, scan.gameDir);
    }
    scan.graphics = readGameGraphics();
    scan.runtimes = openXrRuntimes();
    scan.virtualDesktop = processRunning(L"VirtualDesktop.Streamer.exe");
    scan.steamVr = processRunning(L"vrserver.exe");
    scan.done = true;
    std::lock_guard lock(mutex_);
    scan_ = std::move(scan);
    busy_ = false;
}

void refreshInstall(Scan& scan) {
    if (!scan.gameDir.empty())
        scan.install = scanInstall(scan.gameDir, scan.payload, scan.payloadVersion);
}

const Runtime* automaticRuntime(const Scan& scan) {
    const auto find = [&](const char* name) -> const Runtime* {
        for (const auto& r : scan.runtimes)
            if (r.name == name)
                return &r;
        return nullptr;
    };
    if (scan.virtualDesktop)
        if (const Runtime* r = find("Virtual Desktop"))
            return r;
    if (scan.steamVr)
        if (const Runtime* r = find("SteamVR"))
            return r;
    return nullptr;
}

// --- Changes -------------------------------------------------------------------------------------------------------

Change installErvr(const Scan& scan, bool replaceBuild) {
    if (auto refused = refuseWhileRunning(scan))
        return *refused;
    if (scan.payload.empty() || !scan.payloadMissing.empty())
        return {false, false, {"This launcher's folder lacks ERVR's files: extract the whole zip again."}};
    if (sameFolder(scan.payload, scan.gameDir))
        return {true, false, {"ERVR's files are already in the game folder (the launcher runs from it)."}};
    Changer c{{}, scan.gameDir};
    const fs::path from(scan.payload), game(scan.gameDir);
    const Install& in = scan.install;
    // A newer ERVR, or another build of this version (a player's own), stays unless asked for.
    const bool takeBuild = replaceBuild || in.state == InstallState::missing || in.state == InstallState::older ||
                           in.state == InstallState::parked;
    const auto copyErvr = [&] {
        if (takeBuild || !in.dll) {
            if (!c.copy(from / L"ERVR\\ERVR.dll", game / L"ERVR\\ERVR.dll"))
                return false;
            c.change.lines.push_back("Installed ERVR " + scan.payloadVersion + " (ERVR\\ERVR.dll).");
            for (const auto* document : kPayloadDocuments)
                if (isFile(from / document))
                    CopyFileW((from / document).c_str(), (game / document).c_str(), FALSE);
        }
        // With the build taken, or wherever one is missing: an install from before body haptics keeps its ERVR.dll.
        for (const auto* file : kPayloadBhaptics)
            if (isFile(from / file) && (takeBuild || !in.dll || !isFile(game / file)))
                CopyFileW((from / file).c_str(), (game / file).c_str(), FALSE);
        if (!in.ini && c.copy(from / L"ERVR\\ERVR.ini", game / L"ERVR\\ERVR.ini"))
            c.change.lines.push_back("Added ERVR\\ERVR.ini (its settings; edit them in Options).");
        // Kinetic: an existing ERVR.ini keeps every value, but gains the [Physics] settings it lacks.
        if (in.ini) {
            const fs::path mine = game / L"ERVR\\ERVR.ini";
            const std::string have = readText(mine, 1 << 22);
            const std::string merged = text::iniAddMissing(have, readText(from / L"ERVR\\ERVR.ini", 1 << 22), "Physics");
            if (merged != have && replaceWhole(mine.wstring(), merged))
                c.change.lines.push_back("Added the new [Physics] settings to ERVR\\ERVR.ini (your values stay).");
        }
        return true;
    };
    const fs::path dxgi = game / L"dxgi.dll";
    if (!in.reshade) {
        if (isFile(dxgi)) {
            std::wstring backup = dxgi.wstring() + L".ervr-bak";
            if (isFile(backup))
                backup += L"-" + stampNow();
            if (!c.move(dxgi.wstring(), backup))
                return c.change;
            c.change.lines.push_back("Kept the old dxgi.dll (" + in.dxgi + ") as " + c.name(backup) + ".");
        }
        if (!c.copy(from / L"dxgi.dll", dxgi))
            return c.change;
        c.change.lines.push_back("Installed ReShade " + versionText(fileVersion(dxgi), 3) +
                                 " with add-on support as dxgi.dll.");
    }
    if (!isFile(game / L"ReShade.ini"))
        c.copy(from / L"ReShade.ini", game / L"ReShade.ini");
    // Never in place of another mod loader: that loader has to load ERVR\ERVR.dll.
    if (in.foreignProxy) {
        copyErvr();
        c.change.lines.push_back("Another dinput8.dll mod loader is in the game folder, so ERVR's was not installed: "
                                 "that loader has to load ERVR\\ERVR.dll (put it in its mods folder).");
        return c.change;
    }
    if (takeBuild || !in.proxy) {
        if (!c.copy(from / L"dinput8.dll", game / L"dinput8.dll"))
            return c.change;
        if (in.proxyParked)
            DeleteFileW((game / L"dinput8.dll").wstring().append(kParked).c_str());
    }
    copyErvr();
    return c.change;
}

Change switchToVr(const Scan& scan) {
    if (auto refused = refuseWhileRunning(scan))
        return *refused;
    Changer c{{}, scan.gameDir};
    for (const auto& mod : scan.install.flat)
        if (!mod.parked)
            c.park(mod.path);
    if (scan.install.proxyParked)
        c.unpark((fs::path(scan.gameDir) / L"dinput8.dll").wstring());
    return c.change;
}

Change switchToFlat(const Scan& scan) {
    if (auto refused = refuseWhileRunning(scan))
        return *refused;
    Changer c{{}, scan.gameDir};
    if (scan.install.proxy)
        c.park((fs::path(scan.gameDir) / L"dinput8.dll").wstring());
    for (const auto& mod : scan.install.flat)
        if (mod.parked && !isRealVr(mod))
            c.unpark(mod.path);
    if (c.change.lines.empty())
        c.change.lines.push_back("Nothing to change.");
    return c.change;
}

Change uninstallErvr(const Scan& scan) {
    if (auto refused = refuseWhileRunning(scan))
        return *refused;
    Changer c{{}, scan.gameDir};
    const fs::path game(scan.gameDir);
    const std::wstring proxy = (game / L"dinput8.dll").wstring();
    if (scan.install.proxy && c.remove(proxy))
        c.change.lines.push_back("Removed dinput8.dll.");
    if (scan.install.proxyParked && c.remove(proxy + kParked))
        c.change.lines.push_back("Removed dinput8.dll.ervr-off.");
    if (scan.install.dll && c.remove((game / L"ERVR\\ERVR.dll").wstring()))
        c.change.lines.push_back("Removed ERVR\\ERVR.dll.");
    if (isFile(game / kPayloadBhaptics[0]) && c.remove((game / kPayloadBhaptics[0]).wstring()))
        c.change.lines.push_back("Removed ERVR\\bhaptics_library.dll.");
    const fs::path dxgi = game / L"dxgi.dll";
    if (isFile(dxgi) && versionString(dxgi, L"ProductName") == "ReShade" && c.remove(dxgi.wstring())) {
        c.change.lines.push_back("Removed dxgi.dll (ReShade).");
        if (isFile(dxgi.wstring() + L".ervr-bak") && c.move(dxgi.wstring() + L".ervr-bak", dxgi.wstring()))
            c.change.lines.push_back("Restored the dxgi.dll ERVR had replaced.");
    }
    c.change.lines.push_back("Kept ERVR\\ERVR.ini, the logs and ReShade.ini.");
    return c.change;
}

Change restoreDefaultIni(const Scan& scan) {
    if (scan.payload.empty() || !isFile(fs::path(scan.payload) / L"ERVR\\ERVR.ini"))
        return {false, false, {"This launcher's folder has no ERVR\\ERVR.ini to restore from."}};
    if (scan.gameDir.empty())
        return {false, false, {"ELDEN RING was not found."}};
    const fs::path live = fs::path(scan.gameDir) / L"ERVR\\ERVR.ini";
    if (sameFolder(scan.payload, scan.gameDir))
        return {false, false, {"The launcher's ERVR.ini is the game's: there is no other copy to restore from."}};
    Changer c{{}, scan.gameDir};
    if (isFile(live)) {
        const std::wstring keep = live.wstring() + L"." + stampNow() + L".bak";
        if (!c.copy(live, keep))
            return c.change;
        c.change.lines.push_back("Kept your settings as " + c.name(keep) + ".");
    }
    if (c.copy(fs::path(scan.payload) / L"ERVR\\ERVR.ini", live))
        c.change.lines.push_back("ERVR.ini is back to ERVR " + scan.payloadVersion + "'s defaults.");
    return c.change;
}

// --- ELDEN RING's graphics settings ---------------------------------------------------------------------------

GameGraphics readGameGraphics() {
    GameGraphics graphics;
    graphics.files = graphicsConfigFiles();
    if (!graphics.files.empty())
        if (const auto xml = readUtf16(graphics.files.front()))
            for (size_t i = 0; i < text::kVrEffects.size(); ++i)
                graphics.values[i] = text::xmlGet(*xml, text::kVrEffects[i].tag).value_or("");
    return graphics;
}

Change turnOffGameEffects(const Scan& scan, const text::EffectsOff& which) {
    // Any ELDEN RING: every install shares these files.
    if (processRunning(L"eldenring.exe") || (!scan.gameDir.empty() && gameRunning(scan.gameDir)))
        return {false, false, {"ELDEN RING is running: it writes its graphics settings back as it closes, so they "
                               "change at the next START VR."}};
    Change change;
    for (const auto& path : graphicsConfigFiles()) {
        auto xml = readUtf16(path);
        if (!xml) {
            change.lines.push_back("Could not read " + graphicsConfigName(path) + ": the game's graphics stay as they are.");
            continue;
        }
        const auto changed = text::turnEffectsOff(*xml, which);
        if (changed.empty())
            continue;
        const std::wstring backup = path.substr(0, path.size() - 4) + L"_ervr_backup.xml";
        if (!isFile(backup))
            CopyFileW(path.c_str(), backup.c_str(), TRUE);
        if (!writeUtf16(path, *xml)) {
            change.ok = false;
            change.lines.push_back("Windows refused " + graphicsConfigName(path) + ": the game's graphics stay as they are.");
            continue;
        }
        change.lines.push_back(graphicsConfigName(path) + ": turned off " + text::listed(changed) + ".");
    }
    return change;
}

// --- ERVR.ini -------------------------------------------------------------------------------------------------------

uint64_t writeTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
        return 0;
    return (uint64_t(attributes.ftLastWriteTime.dwHighDateTime) << 32) | attributes.ftLastWriteTime.dwLowDateTime;
}

std::optional<IniFile> readIni(const std::wstring& path) {
    if (path.empty() || !isFile(path))
        return std::nullopt;
    IniFile ini{path, readText(path, 1 << 22), writeTime(path)};
    return ini;
}

bool writeIni(IniFile& ini) {
    if (!replaceWhole(ini.path, ini.text))
        return false;
    ini.stamp = writeTime(ini.path);
    return true;
}

std::wstring iniPath(const Scan& scan) {
    if (!scan.gameDir.empty() && isFile(fs::path(scan.gameDir) / L"ERVR\\ERVR.ini"))
        return (fs::path(scan.gameDir) / L"ERVR\\ERVR.ini").wstring();
    if (!scan.payload.empty() && isFile(fs::path(scan.payload) / L"ERVR\\ERVR.ini"))
        return (fs::path(scan.payload) / L"ERVR\\ERVR.ini").wstring();
    return {};
}

// --- Headset check ---------------------------------------------------------------------------------------------------

namespace {
// Runs a hidden command to completion (or `timeoutMs`, or `stop`), collecting what it prints.
std::optional<DWORD> capture(std::wstring commandLine, std::wstring* environmentBlock, DWORD timeoutMs,
                             std::string& output, const std::atomic<bool>& stop) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE readEnd{}, writeEnd{};
    if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0))
        return std::nullopt;
    Handle reader(readEnd), writer(writeEnd);
    SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0);
    // Only the pipe is inherited, so parallel work never holds it open.
    SIZE_T size{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> storage(size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    HANDLE handles[] = {writer.value};
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size) ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr,
                                   nullptr))
        return std::nullopt;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = writer.value;
    startup.StartupInfo.hStdError = writer.value;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION info{};
    const BOOL ok = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                                   environmentBlock ? environmentBlock->data() : nullptr, nullptr,
                                   &startup.StartupInfo, &info);
    DeleteProcThreadAttributeList(attributes);
    if (!ok) {
        output = "error: could not start the check (Windows error " + std::to_string(GetLastError()) + ")";
        return std::nullopt;
    }
    CloseHandle(info.hThread);
    Handle process(info.hProcess);
    CloseHandle(writer.value);
    writer.value = nullptr;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    char buffer[4096];
    for (;;) {
        DWORD available{};
        if (PeekNamedPipe(reader.value, nullptr, 0, nullptr, &available, nullptr) && available) {
            DWORD got{};
            if (ReadFile(reader.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr))
                output.append(buffer, got);
            continue;
        }
        if (WaitForSingleObject(process.value, 20) == WAIT_OBJECT_0) {
            while (PeekNamedPipe(reader.value, nullptr, 0, nullptr, &available, nullptr) && available) {
                DWORD got{};
                if (!ReadFile(reader.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr))
                    break;
                output.append(buffer, got);
            }
            DWORD code{};
            GetExitCodeProcess(process.value, &code);
            return code;
        }
        if (stop || GetTickCount64() > deadline) {
            TerminateProcess(process.value, 1);
            output += "\nerror: the runtime did not answer in time";
            return std::nullopt;
        }
    }
}

std::atomic<bool> g_closing{false};
} // namespace

HeadsetCheck::~HeadsetCheck() {
    g_closing = true;
    if (worker_.joinable())
        worker_.join();
}

void HeadsetCheck::start(const std::wstring& manifest) {
    {
        std::lock_guard lock(mutex_);
        if (state_ == Outcome::running)
            return;
        state_ = Outcome::running;
        result_ = {};
    }
    if (worker_.joinable())
        worker_.join();
    worker_ = std::thread([this, manifest] {
        std::wstring block = environmentWith({{L"XR_RUNTIME_JSON", manifest}});
        std::string output;
        // SteamVR starts itself when asked, which takes up to half a minute.
        const auto code = capture(text::quoteArgument(exePath()) + L" --probe", &block, 60000, output, g_closing);
        auto result = text::parseProbe(output);
        if (!code && result.error.empty())
            result.error = "the check did not finish";
        std::lock_guard lock(mutex_);
        state_ = code == 0u ? Outcome::ok : Outcome::error;
        result_ = std::move(result);
    });
}

Outcome HeadsetCheck::state() {
    std::lock_guard lock(mutex_);
    return state_;
}

text::ProbeResult HeadsetCheck::result() {
    std::lock_guard lock(mutex_);
    return result_;
}

void HeadsetCheck::reset() {
    std::lock_guard lock(mutex_);
    if (state_ != Outcome::running) {
        state_ = Outcome::idle;
        result_ = {};
    }
}

// --- Session ---------------------------------------------------------------------------------------------------------

namespace {
// ERVR.log, read as it grows. ERVR renames the last session's log to ERVR.log.prev as it starts and writes a
// new one: the file the session started with is skipped (by its file id, as Windows gives a file made under a
// just-renamed name the old one's creation time), and a newer one is read from its start.
class LogTail {
public:
    LogTail(fs::path path, bool current) : path_(std::move(path)) {
        if (!current)
            skip_ = id();
    }

    // Calls `line` for each complete line written since; `all` reads a last unfinished one too.
    template <class Line> void read(Line&& line, bool all = false) {
        Handle file(CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, 0, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!file || !GetFileInformationByHandle(file.value, &info))
            return;
        const uint64_t id = (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        if (id == skip_)
            return;
        const uint64_t size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (id != id_ || size < offset_) {
            id_ = id;
            offset_ = 0;
            pending_.clear();
        }
        std::vector<char> buffer(1 << 18);
        while (offset_ < size) {
            LARGE_INTEGER at;
            at.QuadPart = static_cast<LONGLONG>(offset_);
            DWORD got = 0;
            if (!SetFilePointerEx(file.value, at, nullptr, FILE_BEGIN) ||
                !ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) || !got)
                break;
            offset_ += got;
            pending_.append(buffer.data(), got);
            size_t newline;
            while ((newline = pending_.find('\n')) != std::string::npos) {
                std::string text = pending_.substr(0, newline);
                pending_.erase(0, newline + 1);
                if (!text.empty() && text.back() == '\r')
                    text.pop_back();
                line(text);
            }
        }
        if (all && !pending_.empty()) {
            line(pending_);
            pending_.clear();
        }
    }

private:
    uint64_t id() const {
        Handle file(CreateFileW(path_.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, 0, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        return file && GetFileInformationByHandle(file.value, &info)
                   ? (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow
                   : 0;
    }
    fs::path path_;
    uint64_t skip_{}, id_{}, offset_{};
    std::string pending_;
};

std::atomic<bool> g_sessionClosing{false};

// The process's top-level windows asked to close, as Alt+F4 does; false when it has none.
bool closeWindows(DWORD pid) {
    struct Search {
        DWORD pid;
        int closed;
    } search{pid, 0};
    EnumWindows(
        [](HWND window, LPARAM data) -> BOOL {
            auto* s = reinterpret_cast<Search*>(data);
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner == s->pid && IsWindowVisible(window) && !GetWindow(window, GW_OWNER)) {
                PostMessageW(window, WM_CLOSE, 0, 0);
                ++s->closed;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.closed > 0;
}

std::string exitDescription(DWORD code) {
    switch (code) {
    case 0xC0000005: return "an access violation";
    case 0xC0000409: return "a stack buffer overrun";
    case 0xC00000FD: return "a stack overflow";
    case 0x80000003: return "a breakpoint";
    case 0xE06D7363: return "an unhandled C++ exception";
    default: return {};
    }
}
} // namespace

Session::~Session() {
    g_sessionClosing = true;
    if (worker_.joinable())
        worker_.join();
}

void Session::add(LogLine line, bool ervr) {
    std::lock_guard lock(mutex_);
    if (ervr)
        text::noteProgress(line.text, progress_);
    if (ervr && line.kind == LineKind::error)
        progress_.lastError = line.text;
    if (lines_.size() >= 8000)
        lines_.erase(lines_.begin(), lines_.begin() + 2000);
    lines_.push_back(std::move(line));
}

void Session::say(LineKind kind, std::string text) { add({timeNow(), kind, std::move(text)}, false); }

bool Session::start(LaunchPlan plan, std::string& error) {
    if (running()) {
        error = "A session is already running.";
        return false;
    }
    if (worker_.joinable())
        worker_.join();
    {
        std::lock_guard lock(mutex_);
        lines_.clear();
        progress_ = {};
        running_ = true;
        quitRequested_ = false;
        phase_ = Phase::idle;
        gamePid_ = 0;
        exit_.reset();
    }
    started_ = std::chrono::steady_clock::now();
    worker_ = std::thread([this, plan = std::move(plan)] { run(plan); });
    return true;
}

void Session::run(LaunchPlan plan) {
    const auto finish = [this](DWORD code) {
        std::lock_guard lock(mutex_);
        exit_ = code;
        running_ = false;
        phase_ = Phase::idle;
    };
    const auto setPhase = [this](Phase phase) {
        std::lock_guard lock(mutex_);
        phase_ = phase;
    };
    const auto quitting = [this] {
        std::lock_guard lock(mutex_);
        return quitRequested_ || g_sessionClosing;
    };
    for (auto& line : plan.before)
        say(LineKind::normal, line);
    LogTail tail(fs::path(plan.gameDir) / L"ERVR" / L"ERVR.log", plan.attach);
    const auto readLog = [&](bool all) {
        tail.read([&](const std::string& text) { add(text::parseLogLine(text), true); }, all);
    };
    std::optional<GameProcess> game;
    if (plan.attach) {
        game = findGame(plan.gameDir);
        say(LineKind::normal, "Following the ELDEN RING that is already running.");
    } else {
        if (!steamSignedIn()) {
            setPhase(Phase::steam);
            say(LineKind::normal, "Starting Steam: the game needs it running and signed in...");
            if (plan.steamExe.empty() ||
                reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", plan.steamExe.c_str(), nullptr, nullptr,
                                                        SW_SHOWNORMAL)) <= 32) {
                say(LineKind::error, "Steam did not start. Start it yourself, sign in, then press START VR again.");
                return finish(2);
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
            while (!steamSignedIn()) {
                if (quitting()) {
                    say(LineKind::normal, "Cancelled.");
                    return finish(130);
                }
                if (std::chrono::steady_clock::now() > deadline) {
                    say(LineKind::error, "Steam did not finish signing in. Sign in, then press START VR again.");
                    return finish(2);
                }
                Sleep(500);
            }
            say(LineKind::good, "Steam is running.");
            Sleep(1500); // its client interface settles a moment after sign-in
        }
        std::vector<std::pair<std::wstring, std::wstring>> environmentChanges{{L"XR_RUNTIME_JSON", plan.runtimeManifest}};
        std::wstring exe;
        if (plan.mode == StartMode::coop) {
            exe = (fs::path(plan.gameDir) / L"ersc_launcher.exe").wstring();
            say(LineKind::normal, "Starting Seamless Co-op (ersc_launcher.exe starts the game with EAC off)...");
        } else {
            // The practice tool's EAC bypass: with steam_appid.txt beside it, eldenring.exe runs as it is
            // started instead of starting again through Steam's EAC launcher.
            const fs::path appId = fs::path(plan.gameDir) / L"steam_appid.txt";
            if (text::trim(readText(appId, 64)) != narrow(kSteamAppId)) {
                std::ofstream file(appId, std::ios::binary | std::ios::trunc);
                file << narrow(kSteamAppId);
                file.close();
                say(file ? LineKind::normal : LineKind::warning,
                    file ? "Wrote steam_appid.txt: the game starts without EAC, offline from FromSoftware's servers."
                         : "Could not write steam_appid.txt; the game may start through EAC.");
            }
            environmentChanges.push_back({L"SteamAppId", kSteamAppId});
            environmentChanges.push_back({L"SteamGameId", kSteamAppId});
            exe = plan.gameExe;
            say(LineKind::normal, "Starting ELDEN RING with EAC off...");
        }
        say(LineKind::normal, plan.runtimeManifest.empty()
                                  ? "VR runtime: Windows' active OpenXR runtime."
                                  : "VR runtime for this session: " + plan.runtimeName + ".");
        setPhase(Phase::starting);
        std::wstring block = environmentWith(environmentChanges);
        std::wstring command = text::quoteArgument(exe);
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                            block.data(), plan.gameDir.c_str(), &startup, &info)) {
            const DWORD code = GetLastError();
            say(LineKind::error, "Windows did not start " + narrow(fs::path(exe).filename().wstring()) +
                                     (code == ERROR_ELEVATION_REQUIRED
                                          ? ": it asks for administrator rights. Restart the launcher as administrator."
                                          : " (Windows error " + std::to_string(code) + ")."));
            return finish(2);
        }
        CloseHandle(info.hThread);
        Handle child(info.hProcess);
        // The game's process: the one just started, or the one Seamless Co-op's launcher starts.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (!(game = findGame(plan.gameDir))) {
            readLog(false);
            if (quitting()) {
                say(LineKind::normal, "Cancelled.");
                return finish(130);
            }
            if (std::chrono::steady_clock::now() > deadline) {
                DWORD code = 0;
                GetExitCodeProcess(child.value, &code);
                say(LineKind::error, "ELDEN RING did not start" +
                                         (code != STILL_ACTIVE ? " (the launcher stopped with code " +
                                                                     std::to_string(code) + ")."
                                                               : "."));
                return finish(2);
            }
            Sleep(250);
        }
    }
    if (!game) {
        say(LineKind::error, "ELDEN RING is not running.");
        return finish(2);
    }
    {
        std::lock_guard lock(mutex_);
        phase_ = Phase::game;
        gamePid_ = game->pid;
    }
    say(LineKind::good, "ELDEN RING is running (process " + std::to_string(game->pid) + "). ERVR's log follows.");
    DWORD exitCode = 0;
    auto gone = std::chrono::steady_clock::time_point{};
    for (;;) {
        readLog(false);
        if (g_sessionClosing)
            break;
        if (game) {
            const bool exited = game->handle ? WaitForSingleObject(game->handle, 250) == WAIT_OBJECT_0
                                             : (Sleep(250), !gameRunning(plan.gameDir));
            if (!exited)
                continue;
            if (game->handle) {
                GetExitCodeProcess(game->handle, &exitCode);
                CloseHandle(game->handle);
            }
            game.reset();
            gone = std::chrono::steady_clock::now();
            continue;
        }
        // The game may start again under another process (through Steam): follow it a few seconds.
        if ((game = findGame(plan.gameDir))) {
            std::lock_guard lock(mutex_);
            gamePid_ = game->pid;
            continue;
        }
        if (std::chrono::steady_clock::now() - gone > std::chrono::seconds(6))
            break;
        Sleep(250);
    }
    if (game && game->handle)
        CloseHandle(game->handle);
    readLog(true);
    if (g_sessionClosing)
        return finish(0);
    if (exitCode == 0 || quitting()) {
        say(LineKind::normal, "ELDEN RING closed.");
    } else {
        char code[16];
        snprintf(code, sizeof(code), "0x%08lX", exitCode);
        const std::string what = exitDescription(exitCode);
        say(LineKind::error, std::string("ELDEN RING stopped with code ") + code + (what.empty() ? "" : " (" + what + ")") +
                                 ". ERVR\\ERVR.log and crash dumps tell why.");
    }
    finish(quitting() ? 0 : exitCode);
}

void Session::requestQuit() {
    DWORD pid;
    {
        std::lock_guard lock(mutex_);
        if (!running_ || quitRequested_)
            return;
        quitRequested_ = true;
        quitAt_ = std::chrono::steady_clock::now();
        pid = gamePid_;
    }
    if (!pid)
        return;
    say(LineKind::normal, closeWindows(pid) ? "Asked ELDEN RING to close." : "ELDEN RING has no window to close yet.");
}

void Session::forceQuit() {
    DWORD pid;
    {
        std::lock_guard lock(mutex_);
        pid = running_ ? gamePid_ : 0;
    }
    if (!pid)
        return;
    if (HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
        TerminateProcess(process, 1);
        CloseHandle(process);
        say(LineKind::warning, "Ended ELDEN RING's process.");
    } else {
        say(LineKind::error, "Windows did not let the launcher end ELDEN RING (it runs as administrator?).");
    }
}

bool Session::running() {
    std::lock_guard lock(mutex_);
    return running_;
}

bool Session::quitRequested() {
    std::lock_guard lock(mutex_);
    return quitRequested_;
}

Session::Phase Session::phase() {
    std::lock_guard lock(mutex_);
    return phase_;
}

std::optional<DWORD> Session::exitCode() {
    std::lock_guard lock(mutex_);
    return exit_;
}

size_t Session::lines(size_t from, std::vector<LogLine>& out) {
    std::lock_guard lock(mutex_);
    // After the oldest lines were dropped, `from` may be past the end: the caller starts over.
    if (from > lines_.size())
        from = 0;
    for (size_t i = from; i < lines_.size(); ++i)
        out.push_back(lines_[i]);
    return lines_.size();
}

Progress Session::progress() {
    std::lock_guard lock(mutex_);
    return progress_;
}

// --- This PC -------------------------------------------------------------------------------------------------------

Hardware hardware() {
    Hardware pc;
    MEMORYSTATUSEX memory{sizeof(memory)};
    if (GlobalMemoryStatusEx(&memory))
        pc.ramMb = memory.ullTotalPhys >> 20;
    IDXGIFactory1* factory{};
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        IDXGIAdapter1* adapter{};
        for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i) {
            DXGI_ADAPTER_DESC1 description{};
            // Not Windows' software renderer, which every PC lists.
            if (SUCCEEDED(adapter->GetDesc1(&description)) && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                (pc.gpu.empty() || description.DedicatedVideoMemory >> 20 > pc.gpuMemoryMb)) {
                pc.gpu = narrow(description.Description);
                pc.gpuMemoryMb = description.DedicatedVideoMemory >> 20;
            }
            adapter->Release();
        }
        factory->Release();
    }
    return pc;
}

std::vector<LogLine> lastLog(const std::wstring& gameDir) {
    std::vector<LogLine> lines;
    if (gameDir.empty())
        return lines;
    LogTail tail(fs::path(gameDir) / L"ERVR" / L"ERVR.log", true);
    tail.read([&](const std::string& text) { lines.push_back(text::parseLogLine(text)); }, true);
    return lines;
}

bool gameRunning(const std::wstring& gameDir) {
    auto game = findGame(gameDir);
    if (game && game->handle)
        CloseHandle(game->handle);
    return game.has_value();
}

bool askGameToClose(const std::wstring& gameDir) {
    auto game = findGame(gameDir);
    if (!game)
        return false;
    if (game->handle)
        CloseHandle(game->handle);
    return closeWindows(game->pid);
}

// --- Shell -----------------------------------------------------------------------------------------------------------

void openUrl(const char* url) { ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void openPath(const std::wstring& path) {
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::optional<std::wstring> browseForGame(HWND owner, const std::wstring& current) {
    IFileOpenDialog* dialog{};
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return std::nullopt;
    const COMDLG_FILTERSPEC filters[] = {{L"eldenring.exe", L"eldenring.exe"}, {L"Programs", L"*.exe"}};
    dialog->SetFileTypes(2, filters);
    dialog->SetTitle(L"Find ELDEN RING (Game\\eldenring.exe)");
    if (!current.empty()) {
        IShellItem* folder{};
        if (SUCCEEDED(SHCreateItemFromParsingName(fs::path(current).parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    std::optional<std::wstring> chosen;
    IShellItem* item{};
    if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR path{};
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            chosen = path;
        CoTaskMemFree(path);
        item->Release();
    }
    dialog->Release();
    return chosen;
}

bool createShortcuts(std::string& error) {
    const std::wstring exe = exePath();
    int made = 0;
    for (const auto& folder : {knownFolder(FOLDERID_Desktop), knownFolder(FOLDERID_Programs)}) {
        if (folder.empty())
            continue;
        IShellLinkW* link{};
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
            continue;
        link->SetPath(exe.c_str());
        link->SetWorkingDirectory(fs::path(exe).parent_path().c_str());
        link->SetDescription(L"ERVR - native VR for ELDEN RING");
        link->SetIconLocation(exe.c_str(), 0);
        IPersistFile* file{};
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
            if (SUCCEEDED(file->Save((fs::path(folder) / L"ELDEN RING VR (ERVR).lnk").c_str(), TRUE)))
                ++made;
            file->Release();
        }
        link->Release();
    }
    if (!made)
        error = "Windows did not create the shortcuts.";
    return made > 0;
}

bool restartAsAdministrator(std::string& error) {
    const std::wstring exe = exePath();
    DWORD code = ERROR_FILE_NOT_FOUND;
    if (!exe.empty()) {
        // main.cpp's --wait-for: the one-at-a-time lock is this launcher's until it has closed.
        const std::wstring arguments = L"--wait-for " + std::to_wstring(GetCurrentProcessId());
        const std::wstring folder = fs::path(exe).parent_path().wstring();
        SHELLEXECUTEINFOW run{sizeof(run)};
        run.lpVerb = L"runas";
        run.lpFile = exe.c_str();
        run.lpParameters = arguments.c_str();
        run.lpDirectory = folder.c_str();
        run.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&run))
            return true;
        code = GetLastError();
    }
    error = code == ERROR_CANCELLED ? std::string()
                                    : "Windows did not start the launcher as administrator (error " +
                                          std::to_string(code) + "). Right-click it and choose Run as administrator.";
    return false;
}

} // namespace launcher
