//---------------------------------------------------------------------------
// hf_settings.h  –  Plugin configuration (API token, username, prefs)
//---------------------------------------------------------------------------
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include "hf_http.h"
#include "hf_api.h"

namespace HfSettings {

static wchar_t g_iniPath[MAX_PATH] = {};
static std::wstring g_apiToken;
static std::wstring g_username;   // resolved via whoami

inline void SetIniPath(const char* path) {
    MultiByteToWideChar(CP_ACP, 0, path, -1, g_iniPath, MAX_PATH);
}

inline void Load() {
    if (!g_iniPath[0]) return;
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(L"HuggingFace", L"ApiToken", L"", buf, 512, g_iniPath);
    g_apiToken = buf;
    GetPrivateProfileStringW(L"HuggingFace", L"Username", L"", buf, 512, g_iniPath);
    g_username = buf;
}

inline void Save() {
    if (!g_iniPath[0]) return;
    WritePrivateProfileStringW(L"HuggingFace", L"ApiToken", g_apiToken.c_str(), g_iniPath);
    WritePrivateProfileStringW(L"HuggingFace", L"Username", g_username.c_str(), g_iniPath);
}

// ── Settings dialog resource IDs (embedded dialog) ───────────────────────
#define IDD_SETTINGS   100
#define IDC_TOKEN      101
#define IDC_USERNAME   102
#define IDC_TESTSTATUS 103
#define IDC_TESTBTN    104

// ── Simple dialog proc ────────────────────────────────────────────────────
inline INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM /*lParam*/) {
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(hDlg, IDC_TOKEN,    g_apiToken.c_str());
        SetDlgItemTextW(hDlg, IDC_USERNAME, g_username.c_str());
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            wchar_t buf[512] = {};
            GetDlgItemTextW(hDlg, IDC_TOKEN, buf, 512);
            g_apiToken = buf;
            GetDlgItemTextW(hDlg, IDC_USERNAME, buf, 512);
            g_username = buf;
            Save();
            EndDialog(hDlg, IDOK);
        } else if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
        } else if (LOWORD(wParam) == IDC_TESTBTN) {
            wchar_t buf[512] = {};
            GetDlgItemTextW(hDlg, IDC_TOKEN, buf, 512);
            std::wstring tok = buf;
            std::string user = HfApi::GetWhoAmI(tok);
            if (!user.empty()) {
                std::wstring msg = L"Connected as: " + HfHttp::Utf8ToWide(user);
                SetDlgItemTextW(hDlg, IDC_TESTSTATUS, msg.c_str());
                SetDlgItemTextW(hDlg, IDC_USERNAME, HfHttp::Utf8ToWide(user).c_str());
            } else {
                SetDlgItemTextW(hDlg, IDC_TESTSTATUS, L"Connection failed – check your token.");
            }
        }
        return TRUE;
    }
    return FALSE;
}

// ── Show settings dialog (built from code, no .rc required) ──────────────
inline void ShowSettingsDialog(HWND parent) {
    // Build dialog template in memory
    struct DlgTemplate {
        DLGTEMPLATE tmpl;
        WORD menu, cls, title[16];
        WORD fontSize; WCHAR fontName[16];
    };

    // We use a simple MessageBox-style approach via WinAPI dialogs in code.
    // For a production build you would add a proper .rc resource.
    // Here we show an InputBox-style prompt using the request callback if available,
    // or fall back to a simple dialog.
    wchar_t token[512] = {};
    wcsncpy_s(token, g_apiToken.c_str(), 511);

    // Simple input via standard dialog
    // (In the real plugin this is invoked via RequestProc callback to the file manager)
    // For standalone settings, open a proper dialog box.
    // Since we cannot embed a full dialog resource here without an .rc file,
    // we expose this via the FsExecuteFile("<Settings>") path handled in main.cpp.
    (void)parent;
}

} // namespace HfSettings
