# HuggingFace VFS Plugin

A file system (WFX) plugin for **Total Commander**, **Double Commander** or any other file manager than loads wfx extensions,
that lets you browse, download, **upload**, delete, and rename files in your
HuggingFace Hub repositories — directly inside your file manager.

---

## Features

| Operation        | Models | Datasets |
|-----------------|--------|----------|
| Browse repos    | ✅     | ✅       |
| Browse folders  | ✅     | ✅       |
| **Upload files**| ✅     | ✅       |
| Download files  | ✅     | ✅       |
| Delete files    | ✅     | ✅       |
| Create folders  | ✅     | ✅       |
| Remove folders  | ✅     | ✅       |
| Rename / Move   | ✅     | ✅       |
| Background xfer | ✅     | ✅       |

---

## Download & Quick Install

1. **Download the latest release:**  
   Go to [**GitHub Releases**](https://github.com/mmoalem/HuggingFace_VFS/releases) and download the `HuggingFace_VFS.zip` file.
   
2. **Auto-Install (Total Commander):**  
   Simply open the downloaded `.zip` file inside Total Commander, and it will prompt you to install the plugin automatically.

3. **Manual Install:**  
   Extract `HuggingFace_VFS.wfx64` and `pluginst.inf` to a folder (e.g., `%COMMANDER_PATH%\Plugins\wfx\HuggingFace\`) and follow the [Installation](#installation) steps below.

---

## Virtual Path Structure

```
\HuggingFace\
  Models\
    owner~repo-name\        ← "owner/repo" uses ~ as separator
      README.md
      config.json
      model\
        pytorch_model.bin
  Datasets\
    owner~dataset-name\
      data\
        train.parquet
```

---

## Requirements

- **Windows 10/11 (x64)**
- A HuggingFace account with an **access token** (read+write)
  → https://huggingface.co/settings/tokens

*Note: While the WFX interface is supported by Double Commander on macOS/Linux, this binary is a Windows DLL and will not work there. Supporting other platforms would require replacing Windows-specific APIs (WinHTTP, BCrypt) and recompiling.*

---

## Installation

### Total Commander
1. Open Total Commander
2. Go to **Configuration → Options → Plugins**
3. Click **Configure** under **File system plugins (.WFX)**
4. Click **Add**, navigate to `HuggingFace_VFS.wfx64`, and click Open
5. Click OK

### Double Commander
1. Open Double Commander
2. Go to **Tools → Options → Plugins → File System plugins**
3. Click **Add**, navigate to `HuggingFace_VFS.wfx64`, and click Open
4. Click OK

---

## First Use — Setting your API Token

1. In the file manager, navigate to the **Network Neighborhood** (or VFS root)
2. Open the `HuggingFace` plugin entry
3. The plugin will prompt for your API token on first access
4. Paste your token (`hf_...`) and press OK
5. The token is saved to the plugin's INI file for future sessions

To update the token later, navigate into the plugin and open `<Settings>`.

**Generate a token:** https://huggingface.co/settings/tokens
- For read-only browsing: a **read** token is sufficient
- For upload/delete: you need a **write** token

---

## Uploading Files

Uploading works exactly like copying files in your file manager:

1. Navigate to the destination repo/folder on the **right panel** (HF plugin)
2. Select the file(s) you want to upload on the **left panel** (local disk)
3. Press **F5** (Copy) — the plugin streams the file to HF Hub via the commit API

Large files (>50 MB) are supported. The HuggingFace API automatically routes
large files through Git LFS.

---

## File Structure

```
HuggingFace_VFS/
├── HuggingFace_VFS.sln          Visual Studio solution
├── HuggingFace_VFS.vcxproj      MSVC project (x64, outputs .wfx64)
├── pluginst.inf                 Auto-install descriptor
└── src/
    ├── main.cpp                 All WFX exported functions
    ├── wfxplugin.h              WFX SDK interface (from ghisler/WFX-SDK)
    ├── hf_http.h                WinHTTP client: download, upload, base64
    ├── hf_api.h                 HF Hub API: list repos, tree, delete
    ├── hf_path.h                Virtual path parser (\ → owner/repo/file)
    ├── hf_settings.h            INI persistence, token management
    └── HuggingFace_VFS.def      DLL export definitions
```

---

## How the Upload Works

The plugin uses the **HuggingFace Hub commit API**:

```
POST https://huggingface.co/api/models/{owner}/{repo}/commit/main
Authorization: Bearer hf_...
Content-Type: application/json

{
  "summary": "Upload file.bin via HuggingFace VFS plugin",
  "files": [{
    "path": "subdir/file.bin",
    "content": "<base64-encoded file content>",
    "encoding": "base64"
  }]
}
```

This creates a proper git commit on the `main` branch, visible in the repo's
commit history on huggingface.co.

---

## Notes & Limitations

- **Repo creation** is not supported from the plugin (create repos on the website first)
- **Spaces** are not listed (models and datasets only, as requested)
- **Pagination**: the plugin fetches up to 100 repos per section; for accounts
  with more repos, open an issue and we can add a `<Load More>` virtual entry
- **Private repos** are accessible as long as your token has read access
- **Rename/Move** works by download + re-upload + delete (HF has no server-side rename)
- **Folder delete** recursively deletes all files within (HF has no empty directories)

---

## Building from Source (MSVC)

### Prerequisites
- Visual Studio 2019 or 2022 (any edition including Community — free)
- Windows SDK 10.0 (installed with VS)
- No external dependencies — uses only Windows built-in WinHTTP

### Steps

1. Open `HuggingFace_VFS.sln` in Visual Studio
2. Select **Release | x64** configuration
3. Build → Build Solution  (`Ctrl+Shift+B`)
4. Output: `bin\Release\HuggingFace_VFS.wfx64`

### Command-line build (Developer Command Prompt)
```
msbuild HuggingFace_VFS.sln /p:Configuration=Release /p:Platform=x64
```

---

## License

MIT License — do whatever you like with it.

---

## Support

If you find this project useful, consider supporting my work:

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-Donate-orange?style=for-the-badge&logo=buy-me-a-coffee&logoColor=white)](https://buymeacoffee.com/mmoalem)
