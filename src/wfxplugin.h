//---------------------------------------------------------------------------
// WFX Plugin Interface for Total Commander / Free Commander / Double Commander
// Based on official Ghisler WFX SDK documentation
//---------------------------------------------------------------------------
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Return values for FsGetFile / FsPutFile / FsRenMovFile
#define FS_FILE_OK            0
#define FS_FILE_EXISTS        1
#define FS_FILE_NOTFOUND      2
#define FS_FILE_READERROR     3
#define FS_FILE_WRITEERROR    4
#define FS_FILE_USERABORT     5
#define FS_FILE_NOTIMPL       6
#define FS_FILE_EXISTSRESUMEALLOWED 7
#define FS_FILE_SYMLINK       8

// Copy flags
#define FS_COPYFLAGS_OVERWRITE            0x01
#define FS_COPYFLAGS_RESUME               0x02
#define FS_COPYFLAGS_MOVE                 0x04
#define FS_COPYFLAGS_EXISTS_SAMECASE      0x08
#define FS_COPYFLAGS_EXISTS_DIFFERENTCASE 0x10

// Background transfer flags
#define BG_NONE     0
#define BG_DOWNLOAD 1
#define BG_UPLOAD   2
#define BG_ASK_USER 4

// Exec return values
#define FS_EXEC_OK       0
#define FS_EXEC_ERROR    1
#define FS_EXEC_YOURSELF -1
#define FS_EXEC_SYMLINK  -2

// Log severity
#define MSGTYPE_CONNECT           1
#define MSGTYPE_DISCONNECT        2
#define MSGTYPE_DETAILS           3
#define MSGTYPE_TRANSFERCOMPLETE  4
#define MSGTYPE_CONNECTCOMPLETE   5
#define MSGTYPE_IMPORTANTERROR    6
#define MSGTYPE_OPERATIONCOMPLETE 7

// Request types
#define RT_Other        0
#define RT_UserName     1
#define RT_Password     2
#define RT_Account      3
#define RT_TargetDir    6
#define RT_URL          7
#define RT_MsgOK        8
#define RT_MsgYesNo     9
#define RT_MsgOKCancel  10

#pragma pack(push,1)
typedef struct {
    DWORD SizeStructure;
    DWORD PluginInterfaceVersionLow;
    DWORD PluginInterfaceVersionHi;
    char  DefaultIniName[MAX_PATH];
} FsDefaultParamStruct;

typedef struct {
    DWORD  dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD    nFileSizeHigh;
    DWORD    nFileSizeLow;
    DWORD    dwReserved0;
    DWORD    dwReserved1;
    WCHAR    cFileName[MAX_PATH];
    WCHAR    cAlternateFileName[14];
} WIN32_FIND_DATAW_PLUGIN;
#pragma pack(pop)

typedef void (__stdcall *tProgressProcW)(int PluginNr, LPCWSTR SourceName, LPCWSTR TargetName, int PercentDone);
typedef void (__stdcall *tLogProcW)(int PluginNr, int MsgType, LPCWSTR LogString);
typedef BOOL (__stdcall *tRequestProcW)(int PluginNr, int RequestType, LPCWSTR CustomTitle, LPCWSTR CustomText, LPWSTR ReturnedText, int MaxLen);
