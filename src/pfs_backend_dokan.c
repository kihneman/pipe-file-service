// The Dokan backend of PipeFileService: serves the core's flat read-only directory on a Dokan volume
// (Dokany 2.x). The kernel driver does not enforce per-file permissions, so the create callback checks
// the caller's token against the file's security descriptor itself.
//
// MIT licensed. Uses Dokan (Dokany), https://github.com/dokan-dev/dokany: dokan2.dll and the driver are
// LGPL licensed and are loaded dynamically (delay-loaded); they are not part of this program.
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include "pfs_backend.h"
#include "dokan.h"
#include "dokan_access.h"

#define MAX_PATHW 1024
#define DOKAN_REQUEST_TIMEOUT_MS (5 * 60 * 1000)   // the driver unmounts the volume when one request exceeds this; 5 minutes is its maximum

typedef struct { void *file; } Handle;

static DOKAN_HANDLE g_inst;
static WCHAR g_mount[MAX_PATHW];
static BOOL g_folder_mount;
static WCHAR g_dokan_dir[MAX_PATHW];   // development override (run mode only); empty = use the driver service and System32

static void file_time(UINT64 t, FILETIME *ft) { ft->dwLowDateTime = (DWORD)t; ft->dwHighDateTime = (DWORD)(t >> 32); }

// ---------------------------------------------------------------- callbacks

static NTSTATUS DOKAN_CALLBACK ZwCreate(LPCWSTR name, PDOKAN_IO_SECURITY_CONTEXT sc, ACCESS_MASK access, ULONG attrs,
    ULONG share, ULONG disp, ULONG opts, PDOKAN_FILE_INFO fi)
{
    (void)sc; (void)attrs; (void)share;
    BOOL dir = FALSE;
    NTSTATUS st = pfs_stat(name, &dir, NULL, NULL);
    if (!NT_SUCCESS(st)) {
        // A name that does not exist: opening it fails, creating it is refused (the volume is read-only).
        if (st == STATUS_OBJECT_NAME_NOT_FOUND && disp != FILE_OPEN && disp != FILE_OPEN_IF) return STATUS_MEDIA_WRITE_PROTECTED;
        return st;
    }
    if (disp == FILE_CREATE) return STATUS_OBJECT_NAME_COLLISION;
    if (disp != FILE_OPEN && disp != FILE_OPEN_IF) return STATUS_MEDIA_WRITE_PROTECTED;   // supersede / overwrite
    if (dir && (opts & FILE_NON_DIRECTORY_FILE)) return STATUS_FILE_IS_A_DIRECTORY;
    if (!dir && (opts & FILE_DIRECTORY_FILE)) return STATUS_NOT_A_DIRECTORY;
    st = dokan_check_access(name, access, fi, pfs_stat);
    if (!NT_SUCCESS(st)) return st;

    Handle *h = (Handle *)calloc(1, sizeof *h);
    PfsInfo info;
    // The caller is the process that issued this open (the same pid is set in every callback of the request).
    st = pfs_open(name, access, fi->ProcessId, &h->file, &info);
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
    pfs_close(h->file);
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
    NTSTATUS st = pfs_read(h->file, (UINT64)off, len, buf, &n);
    if (st == STATUS_END_OF_FILE) { *done = 0; return STATUS_SUCCESS; }
    if (NT_SUCCESS(st)) *done = n;
    return st;
}

static NTSTATUS DOKAN_CALLBACK GetInfo(LPCWSTR name, LPBY_HANDLE_FILE_INFORMATION b, PDOKAN_FILE_INFO fi)
{
    (void)name;
    Handle *h = (Handle *)(ULONG_PTR)fi->Context;
    if (!h) return STATUS_INVALID_PARAMETER;
    PfsInfo info;
    NTSTATUS st = pfs_get_info(h->file, &info);
    if (!NT_SUCCESS(st)) return st;
    memset(b, 0, sizeof *b);
    b->dwFileAttributes = info.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
    file_time(info.time, &b->ftCreationTime); b->ftLastAccessTime = b->ftLastWriteTime = b->ftCreationTime;
    b->dwVolumeSerialNumber = 0x50464653;
    b->nFileSizeHigh = (DWORD)(info.size >> 32); b->nFileSizeLow = (DWORD)info.size;
    b->nNumberOfLinks = 1;
    b->nFileIndexHigh = (DWORD)(info.id >> 32); b->nFileIndexLow = (DWORD)info.id;
    return STATUS_SUCCESS;
}

typedef struct { PFillFindData fill; PDOKAN_FILE_INFO fi; } Fill;

static BOOL add_entry(void *ctx, const WCHAR *leaf, UINT64 id)
{
    (void)id;
    Fill *f = (Fill *)ctx;
    WIN32_FIND_DATAW d; memset(&d, 0, sizeof d);
    wcscpy_s(d.cFileName, MAX_PATH, leaf);
    d.dwFileAttributes = FILE_ATTRIBUTE_READONLY;
    file_time(pfs_time(), &d.ftCreationTime); d.ftLastAccessTime = d.ftLastWriteTime = d.ftCreationTime;
    f->fill(&d, f->fi);   // Dokan's fill function always returns 0, whether or not the entry was taken
    return TRUE;
}

static NTSTATUS DOKAN_CALLBACK Find(LPCWSTR name, PFillFindData fill, PDOKAN_FILE_INFO fi)
{
    Handle *h = (Handle *)(ULONG_PTR)fi->Context;
    PfsInfo info;
    if (!h || !NT_SUCCESS(pfs_get_info(h->file, &info)) || !info.dir) return STATUS_NOT_A_DIRECTORY;
    (void)name;
    Fill f = { fill, fi };
    pfs_list(add_entry, &f);
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK VolInfo(LPWSTR vol, DWORD vsz, LPDWORD serial, LPDWORD maxc, LPDWORD flags, LPWSTR fsn, DWORD fsz, PDOKAN_FILE_INFO fi)
{
    (void)fi;
    wcsncpy_s(vol, vsz, L"PipeFiles", _TRUNCATE);
    *serial = 0x50464653; *maxc = MAX_NAME;
    *flags = FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK | FILE_PERSISTENT_ACLS | FILE_READ_ONLY_VOLUME;
    wcsncpy_s(fsn, fsz, L"PipeFiles", _TRUNCATE);
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
    (void)name; (void)si;
    Handle *h = (Handle *)(ULONG_PTR)fi->Context;
    if (!h) return STATUS_INVALID_PARAMETER;
    SIZE_T sz = len;
    NTSTATUS st = pfs_get_security(h->file, sd, &sz);
    *need = (ULONG)sz;
    return st;
}

// ---------------------------------------------------------------- is Dokan installed?

// The Dokan kernel driver is the file system driver service "Dokan2" (Dokan 2.x).
static BOOL dokan_driver_registered(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return TRUE;   // cannot ask; the DLL is there, so let the real load decide
    SC_HANDLE s = OpenServiceW(scm, L"Dokan2", SERVICE_QUERY_STATUS);
    DWORD err = s ? 0 : GetLastError();
    if (s) CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return s != NULL || (err != ERROR_SERVICE_DOES_NOT_EXIST && err != ERROR_INVALID_NAME);
}

// True when Dokan is installed: dokan2.dll is where the delay-load will look (the system directory, where the
// installer puts it) and the Dokan2 driver service exists. Loads no Dokan code.
static BOOL installed(void)
{
    WCHAR path[MAX_PATHW];
    if (g_dokan_dir[0]) {
        swprintf_s(path, MAX_PATHW, L"%ls\\dokan2.dll", g_dokan_dir);
        return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    }
    WCHAR sys[MAX_PATH];
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return FALSE;
    swprintf_s(path, MAX_PATHW, L"%ls\\dokan2.dll", sys);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return FALSE;
    return dokan_driver_registered();
}

// ---------------------------------------------------------------- serving

static BOOL is_drive_mount(const WCHAR *m) { return m[0] && m[1] == L':' && (m[2] == 0 || (m[2] == L'\\' && m[3] == 0)); }

static int start(const WCHAR *mount)
{
    wcscpy_s(g_mount, MAX_PATHW, mount);
    g_folder_mount = !is_drive_mount(mount);
    // The Dokan library is delay-loaded: a broken install raises here instead of failing the whole process at start.
    __try { DokanInit(NULL); }
    __except (EXCEPTION_EXECUTE_HANDLER) { pfs_log("Dokan is installed but cannot be loaded. Get it from https://github.com/dokan-dev/dokany/releases"); return 11; }

    if (g_folder_mount) {
        // A Dokan mount point folder must exist and be empty. A clean unmount leaves a stale mount-point
        // junction behind, so clear whatever empty directory or junction is there and make a new one.
        WCHAR parent[MAX_PATHW]; wcscpy_s(parent, MAX_PATHW, mount);
        WCHAR *slash = wcsrchr(parent, L'\\');
        if (slash && slash > parent + 2) {
            *slash = 0;
            WCHAR *up = wcsrchr(parent, L'\\');
            if (up && up > parent + 2) { *up = 0; CreateDirectoryW(parent, NULL); *up = L'\\'; }
            CreateDirectoryW(parent, NULL);
        }
        RemoveDirectoryW(mount);   // succeeds only for a stale junction or an empty folder
        if (!CreateDirectoryW(mount, NULL))
            pfs_log("cannot create the mount folder %ls: %lu", mount, GetLastError());
    }

    DOKAN_OPTIONS o; memset(&o, 0, sizeof o);
    o.Version = DOKAN_VERSION;
    o.MountPoint = g_mount;
    o.Options = DOKAN_OPTION_WRITE_PROTECT;
    o.Timeout = DOKAN_REQUEST_TIMEOUT_MS;
    o.AllocationUnitSize = 4096; o.SectorSize = 512;
    DOKAN_OPERATIONS ops; memset(&ops, 0, sizeof ops);
    ops.ZwCreateFile = ZwCreate; ops.Cleanup = Cleanup; ops.CloseFile = CloseFile; ops.ReadFile = ReadFile_;
    ops.GetFileInformation = GetInfo; ops.FindFiles = Find; ops.GetVolumeInformation = VolInfo;
    ops.GetDiskFreeSpace = DiskFree; ops.GetFileSecurity = GetSec;
    int rc = DokanCreateFileSystem(&o, &ops, &g_inst);
    if (rc != DOKAN_SUCCESS) {
        pfs_log("DokanCreateFileSystem failed: %d", rc);
        DokanShutdown();
        return 13;
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
    if (g_folder_mount) RemoveDirectoryW(g_mount);   // the junction the unmount leaves behind
}

// `--dokan-dir <folder>`: never read in service mode.
static BOOL parse_run_option(const WCHAR *flag, const WCHAR *value)
{
    if (0 != wcscmp(flag, L"--dokan-dir") || !value[0]) return FALSE;
    wcscpy_s(g_dokan_dir, MAX_PATHW, value);
    return TRUE;
}

const PfsBackend pfs_backend = {
    L"Dokan", "https://github.com/dokan-dev/dokany/releases", installed, start, stop, parse_run_option,
    L"--dokan-dir <folder>",
    L"--dokan-dir makes the Dokan check look for <folder>\\dokan2.dll instead of the real install",
};
