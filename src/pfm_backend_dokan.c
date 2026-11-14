// The Dokan backend of pipe-file-mount: serves the core's tree on a Dokan volume (Dokany 2.x). The kernel
// driver does not enforce per-file permissions, so the create callback checks the caller's token against
// the volume's security descriptor itself.
//
// MIT licensed. Uses Dokan (Dokany), https://github.com/dokan-dev/dokany: dokan2.dll and the driver are
// LGPL licensed and are loaded dynamically (delay-loaded); they are not part of this program.
#include <stdio.h>
#include <stdlib.h>
#include "pfm_backend.h"
#include "dokan.h"
#include "dokan_access.h"

#define MAX_PATHW 1024
#define DOKAN_REQUEST_TIMEOUT_MS (5 * 60 * 1000)   // the driver unmounts the volume when one request exceeds this; 5 minutes is its maximum

typedef struct { void *file; } Handle;

static DOKAN_HANDLE g_inst;
static WCHAR g_mount[MAX_PATHW];
static BOOL g_folder_mount, g_folder_existed;

static void file_time(UINT64 t, FILETIME *ft) { ft->dwLowDateTime = (DWORD)t; ft->dwHighDateTime = (DWORD)(t >> 32); }

static NTSTATUS DOKAN_CALLBACK ZwCreate(LPCWSTR name, PDOKAN_IO_SECURITY_CONTEXT sc, ACCESS_MASK access, ULONG attrs,
    ULONG share, ULONG disp, ULONG opts, PDOKAN_FILE_INFO fi)
{
    (void)sc; (void)attrs; (void)share;
    BOOL dir = FALSE;
    NTSTATUS st = pfm_stat(name, &dir, NULL, NULL);
    if (!NT_SUCCESS(st)) {
        // A name that does not exist: opening it fails, creating it is refused (the volume is read-only).
        if (st == STATUS_OBJECT_NAME_NOT_FOUND && disp != FILE_OPEN && disp != FILE_OPEN_IF) {
            pfm_log("create refused %ls", name);
            return STATUS_MEDIA_WRITE_PROTECTED;
        }
        return st;
    }
    if (disp == FILE_CREATE) return STATUS_OBJECT_NAME_COLLISION;
    if (disp != FILE_OPEN && disp != FILE_OPEN_IF) return STATUS_MEDIA_WRITE_PROTECTED;   // supersede / overwrite
    if (dir && (opts & FILE_NON_DIRECTORY_FILE)) return STATUS_FILE_IS_A_DIRECTORY;
    if (!dir && (opts & FILE_DIRECTORY_FILE)) return STATUS_NOT_A_DIRECTORY;
    st = dokan_check_access(name, access, fi, pfm_stat);
    if (!NT_SUCCESS(st)) return st;

    Handle *h = (Handle *)calloc(1, sizeof *h);
    PfmInfo info;
    // The caller is the process that issued this open (the same pid is set in every callback of the request).
    st = pfm_open(name, access, fi->ProcessId, &h->file, &info);
    if (!NT_SUCCESS(st)) { free(h); return st; }
    fi->IsDirectory = info.dir;
    fi->Context = (ULONG64)(ULONG_PTR)h;
    return STATUS_SUCCESS;
}

static void DOKAN_CALLBACK Cleanup(LPCWSTR name, PDOKAN_FILE_INFO fi) { (void)name; (void)fi; }

static void DOKAN_CALLBACK CloseFile(LPCWSTR name, PDOKAN_FILE_INFO fi)
{
    (void)name;
    Handle *h = (Handle *)(ULONG_PTR)fi->Context;
    if (!h) return;
    fi->Context = 0;
    pfm_close(h->file);
    free(h);
}

static NTSTATUS DOKAN_CALLBACK ReadFile_(LPCWSTR name, LPVOID buf, DWORD len, LPDWORD done, LONGLONG off, PDOKAN_FILE_INFO fi)
{
    (void)name;
    Handle *h = (Handle *)(ULONG_PTR)fi->Context;
    if (!h || off < 0) return STATUS_INVALID_PARAMETER;
    // The first read waits for the data server (which may be waiting for the user to approve the reader).
    DokanResetTimeout(DOKAN_REQUEST_TIMEOUT_MS, fi);
    ULONG n = 0;
    NTSTATUS st = pfm_read(h->file, (UINT64)off, len, buf, &n);
    if (st == STATUS_END_OF_FILE) { *done = 0; return STATUS_SUCCESS; }
    if (NT_SUCCESS(st)) *done = n;
    return st;
}

static NTSTATUS DOKAN_CALLBACK GetInfo(LPCWSTR name, LPBY_HANDLE_FILE_INFORMATION b, PDOKAN_FILE_INFO fi)
{
    (void)name;
    Handle *h = (Handle *)(ULONG_PTR)fi->Context;
    if (!h) return STATUS_INVALID_PARAMETER;
    PfmInfo info;
    NTSTATUS st = pfm_get_info(h->file, &info);
    if (!NT_SUCCESS(st)) return st;
    memset(b, 0, sizeof *b);
    b->dwFileAttributes = info.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
    file_time(info.time, &b->ftCreationTime); b->ftLastAccessTime = b->ftLastWriteTime = b->ftCreationTime;
    b->dwVolumeSerialNumber = 0x50464d54;
    b->nFileSizeHigh = (DWORD)(info.size >> 32); b->nFileSizeLow = (DWORD)info.size;
    b->nNumberOfLinks = 1;
    b->nFileIndexHigh = (DWORD)(info.id >> 32); b->nFileIndexLow = (DWORD)info.id;
    return STATUS_SUCCESS;
}

typedef struct { PFillFindData fill; PDOKAN_FILE_INFO fi; } Fill;

static BOOL add_entry(void *ctx, const WCHAR *leaf, BOOL dir)
{
    Fill *f = (Fill *)ctx;
    WIN32_FIND_DATAW d; memset(&d, 0, sizeof d);
    wcscpy_s(d.cFileName, MAX_PATH, leaf);
    d.dwFileAttributes = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
    file_time(pfm_time(), &d.ftCreationTime); d.ftLastAccessTime = d.ftLastWriteTime = d.ftCreationTime;
    f->fill(&d, f->fi);   // Dokan's fill function always returns 0, whether or not the entry was taken
    return TRUE;
}

static NTSTATUS DOKAN_CALLBACK Find(LPCWSTR name, PFillFindData fill, PDOKAN_FILE_INFO fi)
{
    (void)name;
    Handle *h = (Handle *)(ULONG_PTR)fi->Context;
    if (!h) return STATUS_INVALID_PARAMETER;
    Fill f = { fill, fi };
    return pfm_list(h->file, add_entry, &f);
}

static NTSTATUS DOKAN_CALLBACK VolInfo(LPWSTR vol, DWORD vsz, LPDWORD serial, LPDWORD maxc, LPDWORD flags, LPWSTR fsn, DWORD fsz, PDOKAN_FILE_INFO fi)
{
    (void)fi;
    wcsncpy_s(vol, vsz, L"PipeFile", _TRUNCATE);
    *serial = 0x50464d54; *maxc = MAX_COMPONENT;
    *flags = FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK | FILE_PERSISTENT_ACLS | FILE_READ_ONLY_VOLUME;
    wcsncpy_s(fsn, fsz, L"PipeFile", _TRUNCATE);
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK DiskFree(PULONGLONG avail, PULONGLONG total, PULONGLONG totalfree, PDOKAN_FILE_INFO fi)
{
    (void)fi;
    *avail = 0; *total = 1 << 20; *totalfree = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK GetSec(LPCWSTR name, PSECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd, ULONG len, PULONG need, PDOKAN_FILE_INFO fi)
{
    (void)name; (void)si; (void)fi;
    SIZE_T sz = len;
    NTSTATUS st = pfm_get_security(sd, &sz);
    *need = (ULONG)sz;
    return st;
}

// ---------------------------------------------------------------- serving

// The Dokan library is delay-loaded: a missing install shows up as a failed load, not as a loader error at start.
static BOOL load(void)
{
    HMODULE m = LoadLibraryExW(L"dokan2.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return FALSE;
    FreeLibrary(m);
    return TRUE;
}

// "K:" or "K:\" -> TRUE. "*" picks the first free drive letter from Z down.
static BOOL is_drive_mount(const WCHAR *m) { return m[0] && m[1] == L':' && (m[2] == 0 || (m[2] == L'\\' && m[3] == 0)); }

static int start(const WCHAR *mount)
{
    if (mount[0] == L'*' && mount[1] == 0) {
        DWORD used = GetLogicalDrives();
        WCHAR letter = 0;
        for (WCHAR c = L'Z'; c >= L'D'; c--) if (!(used & (1u << (c - L'A')))) { letter = c; break; }
        if (!letter) { fprintf(stderr, "no free drive letter\n"); return 1; }
        swprintf_s(g_mount, MAX_PATHW, L"%lc:", letter);
        pfm_log("using drive %ls", g_mount);
    } else {
        wcscpy_s(g_mount, MAX_PATHW, mount);
    }
    g_folder_mount = !is_drive_mount(g_mount);
    if (g_folder_mount) {
        // A Dokan mount point folder must exist and be empty.
        g_folder_existed = GetFileAttributesW(g_mount) != INVALID_FILE_ATTRIBUTES;
        if (!g_folder_existed && !CreateDirectoryW(g_mount, NULL)) { fprintf(stderr, "cannot create the mount folder: %lu\n", GetLastError()); return 1; }
    }
    __try { DokanInit(NULL); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fprintf(stderr, "Dokan cannot be loaded\n"); return 1; }

    DOKAN_OPTIONS o; memset(&o, 0, sizeof o);
    o.Version = DOKAN_VERSION;
    o.MountPoint = g_mount;
    // Only the starting user's logon session sees the volume.
    o.Options = DOKAN_OPTION_WRITE_PROTECT | DOKAN_OPTION_CURRENT_SESSION;
    o.Timeout = DOKAN_REQUEST_TIMEOUT_MS;
    o.AllocationUnitSize = 4096; o.SectorSize = 512;
    DOKAN_OPERATIONS ops; memset(&ops, 0, sizeof ops);
    ops.ZwCreateFile = ZwCreate; ops.Cleanup = Cleanup; ops.CloseFile = CloseFile; ops.ReadFile = ReadFile_;
    ops.GetFileInformation = GetInfo; ops.FindFiles = Find; ops.GetVolumeInformation = VolInfo;
    ops.GetDiskFreeSpace = DiskFree; ops.GetFileSecurity = GetSec;
    int rc = DokanCreateFileSystem(&o, &ops, &g_inst);
    if (rc != DOKAN_SUCCESS) {
        fprintf(stderr, "DokanCreateFileSystem failed: %d\n", rc);
        DokanShutdown();
        return 1;
    }
    return 0;
}

static void stop(void)
{
    DokanRemoveMountPoint(g_mount);
    DokanWaitForFileSystemClosed(g_inst, 5000);
    DokanCloseHandle(g_inst);
    g_inst = NULL;
    DokanShutdown();
    if (g_folder_mount) {   // the junction the unmount leaves behind
        RemoveDirectoryW(g_mount);
        if (g_folder_existed) CreateDirectoryW(g_mount, NULL);
    }
}

const PfmBackend pfm_backend = {
    "Dokan is not installed. Get it from https://github.com/dokan-dev/dokany/releases",
    load, start, stop,
};
