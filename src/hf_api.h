//---------------------------------------------------------------------------
// hf_api.h  –  HuggingFace Hub API helpers (listing, tree, delete)
//---------------------------------------------------------------------------
#pragma once
#include "hf_http.h"
#include <vector>
#include <string>

namespace HfApi {

struct RepoEntry {
    std::wstring id;
    std::wstring name;
    std::wstring lastUpdated;
    bool         isPrivate = false;
};

struct FileEntry {
    std::wstring path;
    std::wstring name;
    std::wstring type;   // "file" or "directory"
    LONGLONG     size = 0;
    std::wstring lastCommitDate;
};

// ── Parse ISO 8601 date into FILETIME ────────────────────────────────────
inline FILETIME ParseIsoDate(const std::wstring& iso) {
    FILETIME ft = {};
    SYSTEMTIME st = {};
    if (iso.size() >= 19) {
        st.wYear   = (WORD)_wtoi(iso.substr(0,4).c_str());
        st.wMonth  = (WORD)_wtoi(iso.substr(5,2).c_str());
        st.wDay    = (WORD)_wtoi(iso.substr(8,2).c_str());
        st.wHour   = (WORD)_wtoi(iso.substr(11,2).c_str());
        st.wMinute = (WORD)_wtoi(iso.substr(14,2).c_str());
        st.wSecond = (WORD)_wtoi(iso.substr(17,2).c_str());
        SystemTimeToFileTime(&st, &ft);
    }
    return ft;
}

// ── Split JSON array into individual object strings ───────────────────────
inline std::vector<std::string> SplitJsonArray(const std::string& json) {
    std::vector<std::string> result;
    int depth = 0;
    size_t start = std::string::npos;
    for (size_t i = 0; i < json.size(); ++i) {
        if (json[i] == '{') { if (depth==0) start=i; ++depth; }
        else if (json[i] == '}') {
            --depth;
            if (depth==0 && start!=std::string::npos) {
                result.push_back(json.substr(start, i-start+1));
                start = std::string::npos;
            }
        }
    }
    return result;
}

// ── List repos for authenticated user / given author ─────────────────────
inline std::vector<RepoEntry> ListRepos(const std::wstring& apiToken,
                                        const std::string&  repoType,
                                        const std::string&  author = "")
{
    std::vector<RepoEntry> repos;
    std::string typePlural = (repoType == "model") ? "models" : "datasets";
    std::wstring path = L"/api/" + HfHttp::Utf8ToWide(typePlural) +
                        L"?limit=100&full=false";
    if (!author.empty())
        path += L"&author=" + HfHttp::Utf8ToWide(author);

    auto resp = HfHttp::DoApi(apiToken, "GET", path);
    if (!resp.ok()) return repos;

    for (auto& obj : SplitJsonArray(resp.body)) {
        RepoEntry e;
        std::string idStr   = HfHttp::JsonGet(obj, "id");
        std::string privStr = HfHttp::JsonGet(obj, "private");
        std::string lastUp  = HfHttp::JsonGet(obj, "lastModified");
        if (lastUp.empty()) lastUp = HfHttp::JsonGet(obj, "updatedAt");
        if (idStr.empty()) continue;
        e.id        = HfHttp::Utf8ToWide(idStr);
        e.isPrivate = (privStr == "true");
        e.lastUpdated = HfHttp::Utf8ToWide(lastUp);
        size_t slash = idStr.find('/');
        e.name = HfHttp::Utf8ToWide(slash != std::string::npos
                                    ? idStr.substr(slash+1) : idStr);
        repos.push_back(e);
    }
    return repos;
}

// ── List files/dirs inside a repo at a given subfolder ───────────────────
inline std::vector<FileEntry> ListTree(const std::wstring& apiToken,
                                       const std::string&  repoType,
                                       const std::string&  repoId,
                                       const std::string&  subfolder = "")
{
    std::vector<FileEntry> files;
    std::string typePlural = (repoType == "model") ? "models" : "datasets";
    std::wstring path = L"/api/" + HfHttp::Utf8ToWide(typePlural) +
                        L"/" + HfHttp::Utf8ToWide(repoId) +
                        L"/tree/main";
    if (!subfolder.empty())
        path += L"/" + HfHttp::UrlEncodePathW(HfHttp::Utf8ToWide(subfolder));

    auto resp = HfHttp::DoApi(apiToken, "GET", path);
    if (!resp.ok()) return files;

    for (auto& obj : SplitJsonArray(resp.body)) {
        FileEntry e;
        std::string pathStr = HfHttp::JsonGet(obj, "path");
        std::string typeStr = HfHttp::JsonGet(obj, "type");
        std::string sizeStr = HfHttp::JsonGet(obj, "size");
        std::string dateStr = HfHttp::JsonGet(obj, "lastCommit");
        if (dateStr.empty()) dateStr = HfHttp::JsonGet(obj, "updatedAt");
        if (pathStr.empty()) continue;
        e.path = HfHttp::Utf8ToWide(pathStr);
        e.type = HfHttp::Utf8ToWide(typeStr);
        e.size = sizeStr.empty() ? 0 : _atoi64(sizeStr.c_str());
        e.lastCommitDate = HfHttp::Utf8ToWide(dateStr);
        size_t slash = pathStr.rfind('/');
        e.name = HfHttp::Utf8ToWide(slash != std::string::npos
                                    ? pathStr.substr(slash+1) : pathStr);
        files.push_back(e);
    }
    return files;
}

// ── Delete a file via commit API ──────────────────────────────────────────
inline bool DeleteFile(const std::wstring& apiToken,
                       const std::string&  repoType,
                       const std::string&  repoId,
                       const std::string&  pathInRepo)
{
    std::string typePlural = (repoType == "model") ? "models" : "datasets";
    std::wstring apiPath = L"/api/" + HfHttp::Utf8ToWide(typePlural) +
                           L"/" + HfHttp::Utf8ToWide(repoId) + L"/commit/main";
    std::string body =
        "{\"summary\":\"Delete " + HfHttp::JsonEscape(pathInRepo) +
        " via HuggingFace VFS\","
        "\"deletedFiles\":[{\"path\":\"" + HfHttp::JsonEscape(pathInRepo) + "\"}]}";
    auto resp = HfHttp::DoApi(apiToken, "POST", apiPath, &body);
    return resp.ok();
}

// ── Search public repos by query string ──────────────────────────────────
inline std::vector<RepoEntry> SearchRepos(const std::wstring& apiToken,
                                          const std::string&  repoType,
                                          const std::string&  query,
                                          const std::string&  author = "",
                                          int                 limit  = 50)
{
    std::vector<RepoEntry> repos;
    std::string typePlural = (repoType == "model") ? "models" : "datasets";
    std::wstring path = L"/api/" + HfHttp::Utf8ToWide(typePlural) +
                        L"?limit=" + HfHttp::Utf8ToWide(std::to_string(limit)) +
                        L"&full=false";
    if (!query.empty())
        path += L"&search=" + HfHttp::UrlEncodePathW(HfHttp::Utf8ToWide(query));
    if (!author.empty())
        path += L"&author=" + HfHttp::Utf8ToWide(author);

    auto resp = HfHttp::DoApi(apiToken, "GET", path);
    if (!resp.ok()) return repos;

    for (auto& obj : SplitJsonArray(resp.body)) {
        RepoEntry e;
        std::string idStr  = HfHttp::JsonGet(obj, "id");
        std::string lastUp = HfHttp::JsonGet(obj, "lastModified");
        if (lastUp.empty()) lastUp = HfHttp::JsonGet(obj, "updatedAt");
        if (idStr.empty()) continue;
        e.id          = HfHttp::Utf8ToWide(idStr);
        e.lastUpdated = HfHttp::Utf8ToWide(lastUp);
        e.isPrivate   = (HfHttp::JsonGet(obj, "private") == "true");
        size_t slash = idStr.find('/');
        e.name = HfHttp::Utf8ToWide(slash != std::string::npos
                                    ? idStr.substr(slash+1) : idStr);
        repos.push_back(e);
    }
    return repos;
}
inline std::string GetWhoAmI(const std::wstring& apiToken) {
    auto resp = HfHttp::DoApi(apiToken, "GET", L"/api/whoami-v2");
    if (!resp.ok()) return "";
    return HfHttp::JsonGet(resp.body, "name");
}

} // namespace HfApi
