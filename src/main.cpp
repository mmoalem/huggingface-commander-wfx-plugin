//---------------------------------------------------------------------------
// main.cpp  –  HuggingFace VFS plugin for Free/Total/Double Commander
//
// Implements the full WFX file system plugin interface:
//   FsInitW, FsFindFirstW, FsFindNextW, FsFindClose
//   FsGetFileW (download), FsPutFileW (UPLOAD – new!)
//   FsDeleteFileW, FsMkDirW, FsRemoveDirW, FsRenMovFileW
//   FsExecuteFileW, FsGetBackgroundFlags, FsSetDefaultParams, FsStatusInfoW
//---------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <algorithm>

#include "wfxplugin.h"
#include "hf_http.h"
#include "hf_api.h"
#include "hf_path.h"
#include "hf_settings.h"

// ── Globals ───────────────────────────────────────────────────────────────
static int                g_pluginNr   = 0;
static tProgressProcW     g_progress   = nullptr;
static tLogProcW          g_log        = nullptr;
static tRequestProcW      g_request    = nullptr;

// ── Logging helper ────────────────────────────────────────────────────────
static void Log(int msgType, const std::wstring& msg) {
    if (g_log) g_log(g_pluginNr, msgType, msg.c_str());
}

// ── Progress helper (returns false = user aborted) ────────────────────────
static bool ReportProgress(LPCWSTR src, LPCWSTR dst, int pct) {
    if (!g_progress) return true;
    g_progress(g_pluginNr, src, dst, pct);
    return true; // TC/FC doesn't give us abort signal via ProgressProc
}

// ── Request helper (ask user for input) ──────────────────────────────────
static bool RequestInput(int type, LPCWSTR title, LPCWSTR prompt,
                         wchar_t* out, int maxLen) {
    if (!g_request) return false;
    return g_request(g_pluginNr, type, title, prompt, out, maxLen) != FALSE;
}

// ── Ensure we have an API token ───────────────────────────────────────────
static bool EnsureToken() {
    if (!HfSettings::g_apiToken.empty()) return true;
    wchar_t buf[512] = {};
    if (RequestInput(RT_Password,
                     L"HuggingFace API Token",
                     L"Enter your HuggingFace access token (hf_...):",
                     buf, 511))
    {
        HfSettings::g_apiToken = buf;
        // Resolve username
        std::string user = HfApi::GetWhoAmI(HfSettings::g_apiToken);
        if (!user.empty()) HfSettings::g_username = HfHttp::Utf8ToWide(user);
        HfSettings::Save();
        return true;
    }
    return false;
}

// ── FindFirst/Next state ──────────────────────────────────────────────────
struct FindState {
    std::vector<WIN32_FIND_DATAW_PLUGIN> entries;
    size_t index = 0;
};
static std::map<HANDLE, FindState*> g_finds;
static std::mutex g_findsMtx;
static LONG g_handleCounter = 100;

static HANDLE AllocFindHandle(FindState* st) {
    HANDLE h = (HANDLE)(LONG_PTR)InterlockedIncrement(&g_handleCounter);
    std::lock_guard<std::mutex> lk(g_findsMtx);
    g_finds[h] = st;
    return h;
}

// ── Helper: fill WIN32_FIND_DATAW_PLUGIN for a directory entry ────────────
static WIN32_FIND_DATAW_PLUGIN MakeDir(const std::wstring& name,
                                       const std::wstring& dateStr = {}) {
    WIN32_FIND_DATAW_PLUGIN d = {};
    d.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    wcsncpy_s(d.cFileName, name.c_str(), MAX_PATH - 1);
    if (!dateStr.empty()) {
        FILETIME ft = HfApi::ParseIsoDate(dateStr);
        d.ftLastWriteTime = ft;
        d.ftCreationTime  = ft;
        d.ftLastAccessTime = ft;
    } else {
        GetSystemTimeAsFileTime(&d.ftLastWriteTime);
        d.ftCreationTime   = d.ftLastWriteTime;
        d.ftLastAccessTime = d.ftLastWriteTime;
    }
    return d;
}

// ── Helper: fill WIN32_FIND_DATAW_PLUGIN for a file entry ─────────────────
static WIN32_FIND_DATAW_PLUGIN MakeFile(const std::wstring& name,
                                        LONGLONG size,
                                        const std::wstring& dateStr = {}) {
    WIN32_FIND_DATAW_PLUGIN d = {};
    d.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    wcsncpy_s(d.cFileName, name.c_str(), MAX_PATH - 1);
    d.nFileSizeHigh = (DWORD)(size >> 32);
    d.nFileSizeLow  = (DWORD)(size & 0xFFFFFFFF);
    if (!dateStr.empty()) {
        FILETIME ft = HfApi::ParseIsoDate(dateStr);
        d.ftLastWriteTime  = ft;
        d.ftCreationTime   = ft;
        d.ftLastAccessTime = ft;
    } else {
        GetSystemTimeAsFileTime(&d.ftLastWriteTime);
        d.ftCreationTime   = d.ftLastWriteTime;
        d.ftLastAccessTime = d.ftLastWriteTime;
    }
    return d;
}

//=============================================================================
// Exported WFX functions
//=============================================================================

extern "C" {

// ── FsInitW ───────────────────────────────────────────────────────────────
__declspec(dllexport) int __stdcall FsInitW(
    int PluginNr,
    tProgressProcW pProgress,
    tLogProcW      pLog,
    tRequestProcW  pRequest)
{
    g_pluginNr = PluginNr;
    g_progress = pProgress;
    g_log      = pLog;
    g_request  = pRequest;
    HfSettings::Load();
    Log(MSGTYPE_CONNECT, L"HuggingFace VFS plugin loaded.");
    return 0;
}

// ── FsSetDefaultParams ────────────────────────────────────────────────────
__declspec(dllexport) void __stdcall FsSetDefaultParams(FsDefaultParamStruct* dps) {
    if (dps) {
        HfSettings::SetIniPath(dps->DefaultIniName);
        HfSettings::Load();
    }
}

// ── FsGetDefRootName ──────────────────────────────────────────────────────
__declspec(dllexport) void __stdcall FsGetDefRootName(char* DefRootName, int maxlen) {
    strncpy_s(DefRootName, maxlen, "HuggingFace", maxlen - 1);
}

// ── FsGetBackgroundFlags ──────────────────────────────────────────────────
__declspec(dllexport) int __stdcall FsGetBackgroundFlags() {
    return BG_DOWNLOAD | BG_UPLOAD;
}

// ── FsStatusInfoW ─────────────────────────────────────────────────────────
__declspec(dllexport) void __stdcall FsStatusInfoW(
    LPCWSTR /*RemoteDir*/, int /*InfoStartEnd*/, int /*Operation*/) {
    // Optional – we don't need it
}

// ── Browse author cache (per session, per repo type) ─────────────────────
static std::map<std::string, std::wstring> g_browseAuthor;
static std::mutex g_browseMtx;

// ── Per-repo Xet cache ────────────────────────────────────────────────────
static std::set<std::string> g_xetRepos;
static std::mutex g_xetMtx;

// ── FsFindFirstW ─────────────────────────────────────────────────────────
__declspec(dllexport) HANDLE __stdcall FsFindFirstW(
    LPCWSTR Path, WIN32_FIND_DATAW_PLUGIN* FindData)
{
    if (!FindData) return INVALID_HANDLE_VALUE;

    auto parsed = HfPath::Parse(Path);
    auto* st = new FindState();

    // ── Root: show "Models" and "Datasets" folders ─────────────────────
    if (parsed.level == HfPath::Level::Root) {
        st->entries.push_back(MakeDir(L"Models"));
        st->entries.push_back(MakeDir(L"Datasets"));
    }
    // ── Section: show own repos + [Browse] virtual folder ─────────────
    else if (parsed.level == HfPath::Level::Section) {
        EnsureToken();
        // Special virtual folder to browse any author
        st->entries.push_back(MakeDir(HfPath::BROWSE_FOLDER));
        // Own repos
        std::string author = HfHttp::WideToUtf8(HfSettings::g_username);
        auto repos = HfApi::ListRepos(HfSettings::g_apiToken, parsed.repoType, author);
        for (auto& r : repos) {
            std::wstring slug = HfPath::IdToSlugW(r.id);
            st->entries.push_back(MakeDir(slug, r.lastUpdated));
        }
    }
    // ── Browse: list repos for cached or newly-entered author ────────────
    else if (parsed.level == HfPath::Level::Browse) {
        EnsureToken();

        // Get cached author for this repo type
        std::wstring cachedAuthor;
        {
            std::lock_guard<std::mutex> lk(g_browseMtx);
            auto it = g_browseAuthor.find(parsed.repoType);
            if (it != g_browseAuthor.end()) cachedAuthor = it->second;
        }

        // Prompt if no cached author yet
        if (cachedAuthor.empty()) {
            wchar_t authorBuf[256] = {};
            bool got = RequestInput(RT_Other,
                                    L"Browse HuggingFace Author / Org",
                                    L"Enter a HuggingFace username or organisation:",
                                    authorBuf, 255);
            if (got && authorBuf[0]) {
                cachedAuthor = authorBuf;
                std::lock_guard<std::mutex> lk(g_browseMtx);
                g_browseAuthor[parsed.repoType] = cachedAuthor;
            }
        }

        if (!cachedAuthor.empty()) {
            // Virtual entry to change the author
            st->entries.push_back(MakeDir(L"[Change Author]"));

            std::string author = HfHttp::WideToUtf8(cachedAuthor);
            auto repos = HfApi::ListRepos(HfSettings::g_apiToken,
                                          parsed.repoType, author);
            for (auto& r : repos) {
                std::wstring slug = HfPath::IdToSlugW(r.id);
                st->entries.push_back(MakeDir(slug, r.lastUpdated));
            }
        } else {
            // User cancelled — show a hint entry
            st->entries.push_back(MakeDir(L"[Enter Author to Browse]"));
        }
    }
    // ── Repo root or subdir: list files/folders ────────────────────────
    else if (parsed.level == HfPath::Level::Repo ||
             parsed.level == HfPath::Level::Subdir)
    {
        EnsureToken();
        auto files = HfApi::ListTree(
            HfSettings::g_apiToken,
            parsed.repoType,
            parsed.repoId,
            parsed.subPath);

        for (auto& f : files) {
            if (f.type == L"directory") {
                st->entries.push_back(MakeDir(f.name, f.lastCommitDate));
            } else {
                st->entries.push_back(MakeFile(f.name, f.size, f.lastCommitDate));
            }
        }
    }

    if (st->entries.empty()) {
        delete st;
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }

    *FindData = st->entries[0];
    st->index = 1;
    return AllocFindHandle(st);
}

// ── FsFindNextW ──────────────────────────────────────────────────────────
__declspec(dllexport) BOOL __stdcall FsFindNextW(
    HANDLE Hdl, WIN32_FIND_DATAW_PLUGIN* FindData)
{
    std::lock_guard<std::mutex> lk(g_findsMtx);
    auto it = g_finds.find(Hdl);
    if (it == g_finds.end()) return FALSE;
    FindState* st = it->second;
    if (st->index >= st->entries.size()) return FALSE;
    *FindData = st->entries[st->index++];
    return TRUE;
}

// ── FsFindClose ──────────────────────────────────────────────────────────
__declspec(dllexport) int __stdcall FsFindClose(HANDLE Hdl) {
    std::lock_guard<std::mutex> lk(g_findsMtx);
    auto it = g_finds.find(Hdl);
    if (it != g_finds.end()) {
        delete it->second;
        g_finds.erase(it);
    }
    return 0;
}

// ── FsGetFileW – DOWNLOAD ─────────────────────────────────────────────────
__declspec(dllexport) int __stdcall FsGetFileW(
    LPCWSTR RemoteName, LPWSTR LocalName,
    int CopyFlags, WIN32_FIND_DATAW_PLUGIN* /*RemoteInfo*/)
{
    auto parsed = HfPath::Parse(RemoteName);
    if (parsed.repoId.empty() || parsed.subPath.empty())
        return FS_FILE_NOTFOUND;

    // Check overwrite
    if (!(CopyFlags & FS_COPYFLAGS_OVERWRITE)) {
        if (GetFileAttributesW(LocalName) != INVALID_FILE_ATTRIBUTES)
            return FS_FILE_EXISTS;
    }

    EnsureToken();

    std::wstring urlPath = HfPath::ResolveUrlPath(
        parsed.repoType, parsed.repoId, parsed.subPath);

    Log(MSGTYPE_DETAILS,
        std::wstring(L"Downloading: ") + RemoteName);

    std::wstring remoteName = RemoteName;
    std::wstring localName  = LocalName;

    bool ok = HfHttp::DownloadFile(
        HfSettings::g_apiToken,
        urlPath,
        LocalName,
        [&](LONGLONG done, LONGLONG total) -> bool {
            int pct = (total > 0) ? (int)(done * 100 / total) : 50;
            ReportProgress(remoteName.c_str(), localName.c_str(), pct);
            return true;
        });

    if (ok) {
        ReportProgress(remoteName.c_str(), localName.c_str(), 100);
        Log(MSGTYPE_TRANSFERCOMPLETE,
            std::wstring(L"Downloaded: ") + RemoteName);
        return FS_FILE_OK;
    }
    return FS_FILE_READERROR;
}

// ── Upload helpers ────────────────────────────────────────────────────────

// Find hf.exe or huggingface-cli.exe on PATH or common install locations.
static std::wstring FindHfCli() {
    // Try plain PATH lookup first via SearchPath
    wchar_t buf[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"hf.exe", nullptr, MAX_PATH, buf, nullptr))
        return buf;
    if (SearchPathW(nullptr, L"huggingface-cli.exe", nullptr, MAX_PATH, buf, nullptr))
        return buf;

    // Common pip install locations
    static const wchar_t* candidates[] = {
        L"%APPDATA%\\Python\\Scripts\\hf.exe",
        L"%LOCALAPPDATA%\\Programs\\Python\\Python312\\Scripts\\hf.exe",
        L"%LOCALAPPDATA%\\Programs\\Python\\Python311\\Scripts\\hf.exe",
        L"%LOCALAPPDATA%\\Programs\\Python\\Python310\\Scripts\\hf.exe",
        L"%USERPROFILE%\\AppData\\Roaming\\Python\\Scripts\\hf.exe",
        L"%LOCALAPPDATA%\\Programs\\Python\\Python312\\Scripts\\huggingface-cli.exe",
        L"%LOCALAPPDATA%\\Programs\\Python\\Python311\\Scripts\\huggingface-cli.exe",
    };
    for (auto& c : candidates) {
        wchar_t expanded[MAX_PATH] = {};
        ExpandEnvironmentStringsW(c, expanded, MAX_PATH);
        if (GetFileAttributesW(expanded) != INVALID_FILE_ATTRIBUTES)
            return expanded;
    }
    return L"";
}

// ── Xet detection helpers ─────────────────────────────────────────────────

static bool IsKnownXetRepo(const std::string& repoId) {
    std::lock_guard<std::mutex> lk(g_xetMtx);
    return g_xetRepos.count(repoId) > 0;
}
static void MarkXetRepo(const std::string& repoId) {
    std::lock_guard<std::mutex> lk(g_xetMtx);
    g_xetRepos.insert(repoId);
    HfDebug::Log("Marked as Xet repo", repoId);
}

// Check if a 400 error body is a Xet rejection
static bool IsXetError(const std::string& errorBody) {
    return errorBody.find("xet") != std::string::npos ||
           errorBody.find("Xet") != std::string::npos ||
           errorBody.find("binary files") != std::string::npos;
}

// Check if a repo requires Xet by attempting a tiny sentinel commit and
// inspecting the error. We use a .gitattributes probe — a text file that
// never triggers Xet — to avoid false positives, then check repo metadata
// for the xet storage tag which HF places in the "tags" array.
static bool RepoRequiresXet(const std::wstring& apiToken,
                             const std::string& repoType,
                             const std::string& repoId)
{
    std::string typePlural = repoType + "s";
    std::wstring path = L"/api/" + HfHttp::Utf8ToWide(typePlural) +
                        L"/" + HfHttp::Utf8ToWide(repoId);
    auto resp = HfHttp::DoApi(apiToken, "GET", path);
    if (!resp.ok()) return false;

    HfDebug::Log("Repo metadata snippet", resp.body.substr(0, 600));

    // Check explicit xet fields
    for (auto& key : {"xet", "xetEnabled", "usesXet", "xet_enabled"}) {
        std::string v = HfHttp::JsonGet(resp.body, key);
        if (v == "true") { HfDebug::Log("Xet detected via field", key); return true; }
    }

    // Check tags array for "xet" tag (HF puts "xet" in tags for migrated repos)
    size_t tagsPos = resp.body.find("\"tags\"");
    if (tagsPos != std::string::npos) {
        size_t arrStart = resp.body.find('[', tagsPos);
        size_t arrEnd   = resp.body.find(']', arrStart);
        if (arrStart != std::string::npos && arrEnd != std::string::npos) {
            std::string tags = resp.body.substr(arrStart, arrEnd - arrStart);
            if (tags.find("\"xet\"") != std::string::npos) {
                HfDebug::Log("Xet detected via tags array", "");
                return true;
            }
        }
    }

    HfDebug::Log("Xet not detected in metadata", "");
    return false;
}

// Upload via the hf CLI subprocess (handles Xet, LFS, everything).
static int UploadViaCli(const std::wstring& hfCli,
                        const std::wstring& apiToken,
                        const std::string&  repoId,
                        const std::string&  repoType,
                        LPCWSTR             localPath,
                        const std::string&  pathInRepo,
                        LPCWSTR             remoteName)
{
    std::wstring repoTypeFlag = (repoType == "dataset") ? L"dataset" : L"model";
    std::wstring pathInRepoW  = HfHttp::Utf8ToWide(pathInRepo);

    auto Q = [](const std::wstring& s) -> std::wstring {
        std::wstring r = L"\"";
        for (wchar_t c : s) { if (c == L'"') r += L"\\\""; else r += c; }
        return r + L"\"";
    };

    std::wstring cmdLine =
        Q(hfCli) + L" upload " +
        HfHttp::Utf8ToWide(repoId) + L" " +
        Q(std::wstring(localPath)) + L" " +
        Q(pathInRepoW) +
        L" --repo-type " + repoTypeFlag;

    HfDebug::Log("CLI command", HfHttp::WideToUtf8(cmdLine));

    // Build environment block with HF_TOKEN injected
    std::wstring envBlock;
    {
        LPWCH env = GetEnvironmentStringsW();
        bool tokenSet = false;
        for (LPWCH p = env; *p; ) {
            std::wstring entry(p);
            if (entry.substr(0, 9) == L"HF_TOKEN=") {
                entry = L"HF_TOKEN=" + apiToken;
                tokenSet = true;
            }
            envBlock += entry; envBlock += L'\0';
            p += wcslen(p) + 1;
        }
        FreeEnvironmentStringsW(env);
        if (!tokenSet) { envBlock += L"HF_TOKEN=" + apiToken; envBlock += L'\0'; }
        envBlock += L'\0';
    }

    // Redirect stdout/stderr to NUL — the CLI emits ANSI progress bars which
    // flood a pipe buffer and cause the process to block/die with exit code 1.
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hNul = CreateFileW(L"NUL", GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hNul;
    si.hStdError  = hNul;

    PROCESS_INFORMATION pi = {};
    std::wstring cmdCopy = cmdLine;
    bool launched = CreateProcessW(
        nullptr, cmdCopy.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        envBlock.data(), nullptr, &si, &pi) != FALSE;

    CloseHandle(hNul);

    if (!launched) {
        HfDebug::Log("CreateProcess FAILED", (int)GetLastError());
        return FS_FILE_WRITEERROR;
    }

    // Poll process + report fake progress to keep the file manager responsive
    DWORD exitCode = 1;
    while (WaitForSingleObject(pi.hProcess, 500) == WAIT_TIMEOUT)
        ReportProgress(localPath, remoteName, 50);

    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    HfDebug::Log("CLI exit code", (int)exitCode);
    if (exitCode == 0) return FS_FILE_OK;

    // hf CLI sometimes exits 1 even on success (TTY/progress rendering issue).
    // Verify by checking if the file now exists in the repo tree.
    HfDebug::Log("CLI exit 1 — verifying file on HF", pathInRepo);
    std::string typePlural = repoType + "s";
    std::string parentDir  = pathInRepo;
    size_t slash = parentDir.rfind('/');
    std::string fileName = (slash != std::string::npos)
                           ? parentDir.substr(slash + 1) : parentDir;
    std::string dirPath  = (slash != std::string::npos)
                           ? parentDir.substr(0, slash) : "";

    std::wstring treePath = L"/api/" + HfHttp::Utf8ToWide(typePlural) +
                            L"/" + HfHttp::Utf8ToWide(repoId) + L"/tree/main";
    if (!dirPath.empty())
        treePath += L"/" + HfHttp::Utf8ToWide(dirPath);

    auto verifyResp = HfHttp::DoApi(apiToken, "GET", treePath);
    HfDebug::Log("Verify tree status", (int)verifyResp.statusCode);
    if (verifyResp.ok() &&
        (verifyResp.body.find("\"" + fileName + "\"") != std::string::npos ||
         verifyResp.body.find(pathInRepo) != std::string::npos))
    {
        HfDebug::Log("File confirmed on HF — treating CLI exit 1 as success", fileName);
        return FS_FILE_OK;
    }
    HfDebug::Log("File NOT confirmed on HF after CLI exit 1", fileName);
    return FS_FILE_WRITEERROR;
}

// ── FsPutFileW – UPLOAD ───────────────────────────────────────────────────
__declspec(dllexport) int __stdcall FsPutFileW(
    LPCWSTR LocalName, LPCWSTR RemoteName, int CopyFlags)
{
    auto parsed = HfPath::Parse(RemoteName);

    std::string pathInRepo;
    if (parsed.level == HfPath::Level::Repo   ||
        parsed.level == HfPath::Level::Subdir ||
        (parsed.isBrowse && !parsed.repoId.empty()))
    {
        if (!parsed.subPath.empty()) {
            pathInRepo = parsed.subPath;
        } else {
            std::wstring local = LocalName;
            size_t slash = local.find_last_of(L"\\/");
            std::wstring fname = (slash != std::wstring::npos)
                                 ? local.substr(slash + 1) : local;
            pathInRepo = HfHttp::WideToUtf8(fname);
        }
    } else {
        return FS_FILE_NOTIMPL;
    }

    if (parsed.repoId.empty()) return FS_FILE_NOTFOUND;

    EnsureToken();

    Log(MSGTYPE_DETAILS, std::wstring(L"Uploading: ") + LocalName + L" → " + RemoteName);

    std::wstring remoteName = RemoteName;
    std::wstring localName  = LocalName;

    ReportProgress(localName.c_str(), remoteName.c_str(), 0);

    // ── If this repo is known to require Xet, go straight to CLI ────────────
    bool knownXet = IsKnownXetRepo(parsed.repoId) ||
                    RepoRequiresXet(HfSettings::g_apiToken, parsed.repoType, parsed.repoId);
    HfDebug::Log("Xet repo", knownXet ? "YES" : "NO");

    if (knownXet) {
        MarkXetRepo(parsed.repoId);
        std::wstring hfCli = FindHfCli();
        if (hfCli.empty()) {
            if (g_request)
                g_request(g_pluginNr, RT_MsgOK,
                    L"HuggingFace VFS – Xet Storage Required",
                    L"This repository uses Xet storage.\n\n"
                    L"To upload files, install:\n"
                    L"    pip install huggingface_hub hf_xet\n\n"
                    L"Then ensure 'hf.exe' is on your PATH.",
                    nullptr, 0);
            Log(MSGTYPE_IMPORTANTERROR,
                L"Upload failed: Xet repo requires 'hf' CLI");
            return FS_FILE_WRITEERROR;
        }
        int result = UploadViaCli(hfCli, HfSettings::g_apiToken,
                                  parsed.repoId, parsed.repoType,
                                  LocalName, pathInRepo, RemoteName);
        if (result == FS_FILE_OK) {
            ReportProgress(localName.c_str(), remoteName.c_str(), 100);
            Log(MSGTYPE_TRANSFERCOMPLETE, std::wstring(L"Uploaded (Xet): ") + RemoteName);
        } else {
            Log(MSGTYPE_IMPORTANTERROR, std::wstring(L"Upload failed (Xet CLI): ") + RemoteName);
        }
        return result;
    }

    // ── Non-Xet repo: use direct WinHTTP upload (LFS / base64) ───────────
    HfHttp::LastErrorBody().clear();
    bool ok = HfHttp::UploadFile(
        HfSettings::g_apiToken,
        parsed.repoType, parsed.repoId, pathInRepo,
        LocalName,
        [&](LONGLONG done, LONGLONG total) -> bool {
            int pct = (total > 0) ? (int)(done * 100 / total) : 50;
            ReportProgress(localName.c_str(), remoteName.c_str(), pct);
            return true;
        });

    if (!ok) {
        bool wasXet = IsXetError(HfHttp::LastErrorBody());
        HfDebug::Log("Direct upload failed, wasXetError", wasXet ? "YES" : "NO");
        if (wasXet) MarkXetRepo(parsed.repoId);

        std::wstring hfCli = FindHfCli();
        if (!hfCli.empty()) {
            HfDebug::Log("Retrying via CLI");
            int result = UploadViaCli(hfCli, HfSettings::g_apiToken,
                                      parsed.repoId, parsed.repoType,
                                      LocalName, pathInRepo, RemoteName);
            if (result == FS_FILE_OK) {
                ReportProgress(localName.c_str(), remoteName.c_str(), 100);
                Log(MSGTYPE_TRANSFERCOMPLETE, std::wstring(L"Uploaded (CLI): ") + RemoteName);
                return FS_FILE_OK;
            }
        } else if (wasXet) {
            if (g_request)
                g_request(g_pluginNr, RT_MsgOK,
                    L"HuggingFace VFS – Xet Storage Required",
                    L"This repository uses Xet storage.\n\n"
                    L"To upload files, install:\n"
                    L"    pip install huggingface_hub hf_xet\n\n"
                    L"Then ensure 'hf.exe' is on your PATH.",
                    nullptr, 0);
        }
        Log(MSGTYPE_IMPORTANTERROR, std::wstring(L"Upload failed: ") + RemoteName);
        return FS_FILE_WRITEERROR;
    }

    ReportProgress(localName.c_str(), remoteName.c_str(), 100);
    Log(MSGTYPE_TRANSFERCOMPLETE, std::wstring(L"Uploaded: ") + RemoteName);
    return FS_FILE_OK;
}

// ── FsDeleteFileW ─────────────────────────────────────────────────────────
__declspec(dllexport) BOOL __stdcall FsDeleteFileW(LPCWSTR RemoteName) {
    auto parsed = HfPath::Parse(RemoteName);
    if (parsed.repoId.empty() || parsed.subPath.empty()) return FALSE;
    EnsureToken();
    Log(MSGTYPE_DETAILS, std::wstring(L"Deleting: ") + RemoteName);
    bool ok = HfApi::DeleteFile(
        HfSettings::g_apiToken,
        parsed.repoType, parsed.repoId, parsed.subPath);
    if (ok) Log(MSGTYPE_OPERATIONCOMPLETE, std::wstring(L"Deleted: ") + RemoteName);
    return ok ? TRUE : FALSE;
}

// ── FsMkDirW ──────────────────────────────────────────────────────────────
// HF repos don't support empty directories. We create a .gitkeep placeholder.
__declspec(dllexport) BOOL __stdcall FsMkDirW(LPCWSTR Path) {
    auto parsed = HfPath::Parse(Path);
    if (parsed.repoId.empty()) return FALSE;
    EnsureToken();

    // Determine the placeholder path
    std::string placeholder = parsed.subPath;
    if (!placeholder.empty() && placeholder.back() != '/')
        placeholder += '/';
    placeholder += ".gitkeep";

    // Upload an empty file as the placeholder
    // Create a temp empty file
    wchar_t tempPath[MAX_PATH], tempFile[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    GetTempFileNameW(tempPath, L"hfv", 0, tempFile);

    // Write empty content
    HANDLE hf = CreateFileW(tempFile, GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);

    bool ok = HfHttp::UploadFile(
        HfSettings::g_apiToken,
        parsed.repoType, parsed.repoId,
        placeholder, tempFile, nullptr);

    DeleteFileW(tempFile);
    return ok ? TRUE : FALSE;
}

// ── FsRemoveDirW ──────────────────────────────────────────────────────────
// Deletes all files within the directory (HF has no empty-dir concept)
__declspec(dllexport) BOOL __stdcall FsRemoveDirW(LPCWSTR RemoteName) {
    auto parsed = HfPath::Parse(RemoteName);
    if (parsed.repoId.empty() || parsed.subPath.empty()) return FALSE;
    EnsureToken();

    // List all files in this directory and delete them
    auto files = HfApi::ListTree(
        HfSettings::g_apiToken,
        parsed.repoType, parsed.repoId, parsed.subPath);

    bool anyFail = false;
    for (auto& f : files) {
        if (f.type != L"directory") {
            std::string fp = HfHttp::WideToUtf8(f.path);
            if (!HfApi::DeleteFile(HfSettings::g_apiToken,
                                   parsed.repoType, parsed.repoId, fp))
                anyFail = true;
        }
    }
    return anyFail ? FALSE : TRUE;
}

// ── FsRenMovFileW ─────────────────────────────────────────────────────────
// HF has no server-side rename. We download + re-upload + delete original.
__declspec(dllexport) int __stdcall FsRenMovFileW(
    LPCWSTR OldName, LPCWSTR NewName,
    BOOL Move, BOOL OverWrite,
    WIN32_FIND_DATAW_PLUGIN* /*RemoteInfo*/)
{
    auto parsedOld = HfPath::Parse(OldName);
    auto parsedNew = HfPath::Parse(NewName);

    if (parsedOld.repoId.empty() || parsedOld.subPath.empty()) return FS_FILE_NOTFOUND;
    if (parsedNew.repoId.empty() || parsedNew.subPath.empty()) return FS_FILE_NOTFOUND;

    EnsureToken();

    // 1. Download old to temp
    wchar_t tempPath[MAX_PATH], tempFile[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    GetTempFileNameW(tempPath, L"hfv", 0, tempFile);

    std::wstring downloadUrl = HfPath::ResolveUrlPath(
        parsedOld.repoType, parsedOld.repoId, parsedOld.subPath);

    if (!HfHttp::DownloadFile(HfSettings::g_apiToken, downloadUrl, tempFile, nullptr)) {
        DeleteFileW(tempFile);
        return FS_FILE_READERROR;
    }

    // 2. Upload to new path
    bool upOk = HfHttp::UploadFile(
        HfSettings::g_apiToken,
        parsedNew.repoType, parsedNew.repoId,
        parsedNew.subPath, tempFile, nullptr);
    DeleteFileW(tempFile);

    if (!upOk) return FS_FILE_WRITEERROR;

    // 3. If Move, delete original
    if (Move) {
        HfApi::DeleteFile(HfSettings::g_apiToken,
                          parsedOld.repoType, parsedOld.repoId, parsedOld.subPath);
    }
    return FS_FILE_OK;
}

// ── FsExecuteFileW ────────────────────────────────────────────────────────
// Handles special virtual items like <Settings>
__declspec(dllexport) int __stdcall FsExecuteFileW(
    HWND MainWin, LPWSTR RemoteName, LPCWSTR Verb)
{
    std::wstring name = RemoteName ? RemoteName : L"";
    std::wstring verb = Verb ? Verb : L"";

    // "[Change Author]" — re-prompt and refresh
    if (name.find(L"[Change Author]") != std::wstring::npos ||
        name.find(L"[Enter Author to Browse]") != std::wstring::npos)
    {
        auto parsed = HfPath::Parse(RemoteName);
        wchar_t authorBuf[256] = {};
        // Pre-fill with current cached author
        {
            std::lock_guard<std::mutex> lk(g_browseMtx);
            auto it = g_browseAuthor.find(parsed.repoType);
            if (it != g_browseAuthor.end())
                wcsncpy_s(authorBuf, it->second.c_str(), 255);
        }
        bool got = RequestInput(RT_Other,
                                L"Browse HuggingFace Author / Org",
                                L"Enter a HuggingFace username or organisation:",
                                authorBuf, 255);
        if (got && authorBuf[0]) {
            std::lock_guard<std::mutex> lk(g_browseMtx);
            g_browseAuthor[parsed.repoType] = authorBuf;
        }
        // Tell FC to refresh the current directory
        return FS_EXEC_YOURSELF;
    }

    // "<Settings>" virtual item
    if (name.find(L"<Settings>") != std::wstring::npos ||
        name.find(L"Settings")   != std::wstring::npos)
    {
        // Ask for token via the file manager's request dialog
        wchar_t buf[512] = {};
        wcsncpy_s(buf, HfSettings::g_apiToken.c_str(), 511);
        if (RequestInput(RT_Password,
                         L"HuggingFace API Token",
                         L"Enter your HuggingFace access token (hf_...):",
                         buf, 511))
        {
            HfSettings::g_apiToken = buf;
            std::string user = HfApi::GetWhoAmI(HfSettings::g_apiToken);
            if (!user.empty()) {
                HfSettings::g_username = HfHttp::Utf8ToWide(user);
                std::wstring msg = L"Connected as: " + HfSettings::g_username;
                if (g_request)
                    g_request(g_pluginNr, RT_MsgOK,
                              L"HuggingFace VFS", msg.c_str(), nullptr, 0);
            }
            HfSettings::Save();
        }
        return FS_EXEC_OK;
    }

    // For normal files, tell file manager to handle it itself
    return FS_EXEC_YOURSELF;
}

} // extern "C"

// ── DllMain ───────────────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        // nothing
    }
    return TRUE;
}
