//---------------------------------------------------------------------------
// hf_path.h  –  Maps the plugin's virtual path hierarchy to HF concepts
//
//  Virtual path structure:
//    \                              → root
//    \Models\                       → section (My Models + special folders)
//    \Datasets\                     → section
//    \Models\[Browse]\              → lists repos for a chosen author
//    \Models\[Browse]\author~repo\  → files in that repo
//    \Models\owner~repo\            → files in own repo (unchanged)
//    \Models\owner~repo\subdir\     → subdir
//
//  Special virtual folder names (bracketed, can't clash with real slugs):
//    [Browse]   → triggers author input prompt
//---------------------------------------------------------------------------
#pragma once
#include <string>
#include <windows.h>

namespace HfPath {

enum class Level {
    Root,       // "\"
    Section,    // "\Models\" or "\Datasets\"
    Browse,     // "\Models\[Browse]\"  — virtual author-browser folder
    Repo,       // "\Models\owner~repo\"  or "\Models\[Browse]\owner~repo\"
    Subdir,     // "\Models\owner~repo\subdir[...]\"
    File        // not a directory
};

static const wchar_t* BROWSE_FOLDER = L"[Browse]";

struct ParsedPath {
    Level       level       = Level::Root;
    std::string repoType;   // "model" or "dataset"
    std::string repoId;     // "owner/repo"
    std::string subPath;    // path within repo
    bool        isBrowse    = false; // path goes through [Browse]
};

// Convert "owner~repo" ↔ "owner/repo"
inline std::string SlugToId(const std::string& slug) {
    std::string r = slug;
    for (auto& c : r) if (c == '~') c = '/';
    return r;
}
inline std::wstring SlugToIdW(const std::wstring& slug) {
    std::wstring r = slug;
    for (auto& c : r) if (c == L'~') c = L'/';
    return r;
}
inline std::string IdToSlug(const std::string& id) {
    std::string r = id;
    for (auto& c : r) if (c == '/') c = '~';
    return r;
}
inline std::wstring IdToSlugW(const std::wstring& id) {
    std::wstring r = id;
    for (auto& c : r) if (c == L'/') c = L'~';
    return r;
}

inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string r(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        r.data(), n, nullptr, nullptr);
    return r;
}
inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring r(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n);
    return r;
}

// Split wide path on backslash, removing empty components
inline std::vector<std::wstring> Split(const std::wstring& path) {
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t c : path) {
        if (c == L'\\' || c == L'/') {
            if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

inline ParsedPath Parse(LPCWSTR remotePath) {
    ParsedPath r;
    if (!remotePath) return r;

    auto parts = Split(std::wstring(remotePath));

    if (parts.empty()) { r.level = Level::Root; return r; }

    // parts[0] = "Models" or "Datasets"
    std::wstring section = parts[0];
    if      (section == L"Models"   || section == L"models")   r.repoType = "model";
    else if (section == L"Datasets" || section == L"datasets")  r.repoType = "dataset";
    else { r.level = Level::Root; return r; }

    if (parts.size() == 1) { r.level = Level::Section; return r; }

    // parts[1] = "[Browse]" or "owner~repo"
    if (parts[1] == BROWSE_FOLDER) {
        r.isBrowse = true;
        if (parts.size() == 2) { r.level = Level::Browse; return r; }
        // parts[2] = "owner~repo"
        r.repoId = SlugToId(WideToUtf8(parts[2]));
        if (parts.size() == 3) { r.level = Level::Repo; return r; }
        r.level = Level::Subdir;
        for (size_t i = 3; i < parts.size(); ++i) {
            if (!r.subPath.empty()) r.subPath += '/';
            r.subPath += WideToUtf8(parts[i]);
        }
        return r;
    }

    // Normal: parts[1] = "owner~repo"
    r.repoId = SlugToId(WideToUtf8(parts[1]));
    if (parts.size() == 2) { r.level = Level::Repo; return r; }

    r.level = Level::Subdir;
    for (size_t i = 2; i < parts.size(); ++i) {
        if (!r.subPath.empty()) r.subPath += '/';
        r.subPath += WideToUtf8(parts[i]);
    }
    return r;
}

// Build a virtual path from components
inline std::wstring Build(const std::string& repoType,
                           const std::wstring& repoSlug = {},
                           const std::wstring& subPath  = {})
{
    std::wstring p = L"\\";
    p += (repoType == "model") ? L"Models" : L"Datasets";
    if (!repoSlug.empty()) { p += L"\\"; p += repoSlug; }
    if (!subPath.empty())  { p += L"\\"; p += subPath;  }
    return p;
}

// Convert a full HF file path ("owner/repo/subdir/file.bin") to a virtual path
inline std::wstring FilePathToVirtual(const std::string& repoType,
                                       const std::string& repoId,
                                       const std::string& pathInRepo)
{
    std::wstring slug = IdToSlugW(Utf8ToWide(repoId));
    std::wstring sub  = Utf8ToWide(pathInRepo);
    for (auto& c : sub) if (c == L'/') c = L'\\';
    return Build(repoType, slug, sub);
}

// Extract just the pathInRepo from a virtual path (everything after owner~repo)
inline std::string GetPathInRepo(LPCWSTR remotePath) {
    auto p = Parse(remotePath);
    return p.subPath;
}

// Build the HF resolve URL path for downloading
// e.g. /owner/repo/resolve/main/subdir/file.bin
inline std::wstring ResolveUrlPath(const std::string& repoType,
                                    const std::string& repoId,
                                    const std::string& pathInRepo)
{
    std::string typeSuffix;
    if (repoType == "dataset") typeSuffix = "/datasets";
    // models have no prefix in the resolve URL

    std::wstring p = Utf8ToWide(typeSuffix)
                   + L"/" + Utf8ToWide(repoId)
                   + L"/resolve/main/"
                   + Utf8ToWide(pathInRepo);
    return p;
}

} // namespace HfPath
