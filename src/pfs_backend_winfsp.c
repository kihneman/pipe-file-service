// The WinFsp backend of PipeFileService: serves the core's flat read-only directory on a WinFsp volume.
//
// MIT licensed. Uses WinFsp - Windows File System Proxy,
// Copyright (C) Bill Zissimopoulos - https://github.com/winfsp/winfsp
#include <winfsp/winfsp.h>
#include <stdio.h>
#include <stdlib.h>
#include "pfs_backend.h"

#define MAX_PATHW 1024

typedef struct { void *file; PVOID dirbuf; } Handle;   // the core's file plus WinFsp's directory buffer

static FSP_FILE_SYSTEM *g_fs;
static WCHAR g_winfsp_dir[MAX_PATHW];   // development override (run mode only); empty = use the registry and the driver service

static void fill_info(const PfsInfo *i, FSP_FSCTL_FILE_INFO *fi)
{
    memset(fi, 0, sizeof *fi);
    fi->FileAttributes = i->dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
    fi->FileSize = i->dir ? 0 : i->size;
    fi->AllocationSize = (fi->FileSize + 4095) / 4096 * 4096;
    fi->CreationTime = fi->LastAccessTime = fi->LastWriteTime = fi->ChangeTime = i->time;
    fi->IndexNumber = i->id;
}

static NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM *fs, FSP_FSCTL_VOLUME_INFO *vi)
{
    (void)fs;
    vi->TotalSize = 1 << 20; vi->FreeSize = 0;
    vi->VolumeLabelLength = (UINT16)(sizeof(L"PipeFiles") - sizeof(WCHAR));
    memcpy(vi->VolumeLabel, L"PipeFiles", vi->VolumeLabelLength);
    return STATUS_SUCCESS;
}

static NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM *fs, PWSTR name, PUINT32 attrs, PSECURITY_DESCRIPTOR sd, SIZE_T *sdsz)
{
    (void)fs;
    BOOL dir;
    NTSTATUS st = pfs_stat(name, &dir, sd, sdsz);
    if (NT_SUCCESS(st) && attrs) *attrs = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
    return st;
}

// WinFsp refuses to dispatch creates unless Create and Overwrite exist.
static NTSTATUS Create(FSP_FILE_SYSTEM *fs, PWSTR name, UINT32 opts, UINT32 access, UINT32 attrs,
    PSECURITY_DESCRIPTOR sd, UINT64 alloc, PVOID *pctx, FSP_FSCTL_FILE_INFO *fi)
{
    (void)fs; (void)name; (void)opts; (void)access; (void)attrs; (void)sd; (void)alloc; (void)pctx; (void)fi;
    return STATUS_MEDIA_WRITE_PROTECTED;
}

static NTSTATUS Overwrite(FSP_FILE_SYSTEM *fs, PVOID ctx, UINT32 attrs, BOOLEAN replace, UINT64 alloc, FSP_FSCTL_FILE_INFO *fi)
{
    (void)fs; (void)ctx; (void)attrs; (void)replace; (void)alloc; (void)fi;
    return STATUS_MEDIA_WRITE_PROTECTED;
}

static NTSTATUS Open(FSP_FILE_SYSTEM *fs, PWSTR name, UINT32 opts, UINT32 access, PVOID *pctx, FSP_FSCTL_FILE_INFO *fi)
{
    (void)fs; (void)opts;
    Handle *h = (Handle *)calloc(1, sizeof *h);
    PfsInfo info;
    // The caller is only known here: FspFileSystemOperationProcessIdF() is 0 in the other callbacks.
    NTSTATUS st = pfs_open(name, access, FspFileSystemOperationProcessIdF(), &h->file, &info);
    if (!NT_SUCCESS(st)) { free(h); return st; }
    fill_info(&info, fi);
    *pctx = h;
    return STATUS_SUCCESS;
}

static VOID Close(FSP_FILE_SYSTEM *fs, PVOID ctx)
{
    (void)fs;
    Handle *h = (Handle *)ctx;
    pfs_close(h->file);
    FspFileSystemDeleteDirectoryBuffer(&h->dirbuf);
    free(h);
}

static NTSTATUS Read(FSP_FILE_SYSTEM *fs, PVOID ctx, PVOID buf, UINT64 off, ULONG len, PULONG done)
{
    (void)fs;
    return pfs_read(((Handle *)ctx)->file, off, len, buf, done);
}

static NTSTATUS GetSecurity(FSP_FILE_SYSTEM *fs, PVOID ctx, PSECURITY_DESCRIPTOR sd, SIZE_T *sdsz)
{
    (void)fs;
    return pfs_get_security(((Handle *)ctx)->file, sd, sdsz);
}

static NTSTATUS GetFileInfo(FSP_FILE_SYSTEM *fs, PVOID ctx, FSP_FSCTL_FILE_INFO *fi)
{
    (void)fs;
    PfsInfo info;
    NTSTATUS st = pfs_get_info(((Handle *)ctx)->file, &info);
    if (NT_SUCCESS(st)) fill_info(&info, fi);
    return st;
}

typedef struct { Handle *h; NTSTATUS result; } Fill;

static BOOL add_entry(void *ctx, const WCHAR *leaf, UINT64 id)
{
    Fill *f = (Fill *)ctx;
    union { UINT8 b[sizeof(FSP_FSCTL_DIR_INFO) + (MAX_NAME + 1) * sizeof(WCHAR)]; FSP_FSCTL_DIR_INFO d; } u;
    memset(&u, 0, sizeof u);
    size_t bytes = wcslen(leaf) * sizeof(WCHAR);
    u.d.Size = (UINT16)(FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + bytes);
    u.d.FileInfo.FileAttributes = FILE_ATTRIBUTE_READONLY;
    u.d.FileInfo.IndexNumber = id;
    u.d.FileInfo.CreationTime = u.d.FileInfo.LastAccessTime = u.d.FileInfo.LastWriteTime = u.d.FileInfo.ChangeTime = pfs_time();
    memcpy(u.d.FileNameBuf, leaf, bytes);
    FspFileSystemFillDirectoryBuffer(&f->h->dirbuf, &u.d, &f->result);
    return NT_SUCCESS(f->result);
}

static NTSTATUS ReadDirectory(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR pattern, PWSTR marker, PVOID buf, ULONG len, PULONG done)
{
    (void)fs; (void)pattern;
    Handle *h = (Handle *)ctx;
    PfsInfo info;
    if (!NT_SUCCESS(pfs_get_info(h->file, &info)) || !info.dir) return STATUS_NOT_A_DIRECTORY;
    Fill f = { h, STATUS_SUCCESS };
    if (FspFileSystemAcquireDirectoryBuffer(&h->dirbuf, 0 == marker, &f.result)) {
        pfs_list(add_entry, &f);
        FspFileSystemReleaseDirectoryBuffer(&h->dirbuf);
    }
    if (!NT_SUCCESS(f.result)) return f.result;
    FspFileSystemReadDirectoryBuffer(&h->dirbuf, marker, buf, len, done);
    return STATUS_SUCCESS;
}

static FSP_FILE_SYSTEM_INTERFACE g_iface = {
    .GetVolumeInfo = GetVolumeInfo, .GetSecurityByName = GetSecurityByName,
    .Create = Create, .Overwrite = Overwrite, .Open = Open, .Close = Close,
    .Read = Read, .GetSecurity = GetSecurity, .GetFileInfo = GetFileInfo, .ReadDirectory = ReadDirectory,
};

// ---------------------------------------------------------------- is WinFsp installed?

// WinFsp 2.x registers its file system driver under a unique name, "WinFsp+<timestamp>", so look for
// a file system driver service whose name starts with "WinFsp".
static BOOL winfsp_driver_registered(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return TRUE;   // cannot ask; the DLL is there, so let the real load decide
    BOOL found = FALSE;
    DWORD cap = 64 * 1024, need = 0, count = 0, resume = 0;
    BYTE *buf = (BYTE *)malloc(cap);
    for (;;) {
        BOOL ok = EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_FILE_SYSTEM_DRIVER, SERVICE_STATE_ALL,
            buf, cap, &need, &count, &resume, NULL);
        DWORD err = ok ? 0 : GetLastError();
        if (!ok && err != ERROR_MORE_DATA) { found = TRUE; break; }   // cannot tell: let the real load decide
        ENUM_SERVICE_STATUS_PROCESSW *svc = (ENUM_SERVICE_STATUS_PROCESSW *)buf;
        for (DWORD i = 0; i < count && !found; i++)
            found = 0 == _wcsnicmp(svc[i].lpServiceName, L"" FSP_FSCTL_DRIVER_NAME, wcslen(L"" FSP_FSCTL_DRIVER_NAME))
                && (svc[i].lpServiceName[wcslen(L"" FSP_FSCTL_DRIVER_NAME)] == 0 || svc[i].lpServiceName[wcslen(L"" FSP_FSCTL_DRIVER_NAME)] == L'+');
        if (ok || found) break;
    }
    free(buf);
    CloseServiceHandle(scm);
    return found;
}

// True when WinFsp is installed: the DLL FspLoad() would load is where the registry says, and the
// WinFsp driver service exists. Touches no WinFsp code, so it is safe before the delay-loaded DLL is.
static BOOL installed(void)
{
    WCHAR path[MAX_PATHW];
    if (g_winfsp_dir[0]) {
        swprintf_s(path, MAX_PATHW, L"%ls\\bin\\winfsp-x64.dll", g_winfsp_dir);
        return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    }
    WCHAR dir[MAX_PATH];
    DWORD sz = sizeof dir;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"" FSP_FSCTL_PRODUCT_FULL_REGKEY, L"InstallDir", RRF_RT_REG_SZ, NULL, dir, &sz) != ERROR_SUCCESS)
        return FALSE;
    swprintf_s(path, MAX_PATHW, L"%ls\\bin\\winfsp-x64.dll", dir);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return FALSE;
    return winfsp_driver_registered();
}

// ---------------------------------------------------------------- serving

static int start(const WCHAR *mount)
{
    if (!NT_SUCCESS(FspLoad(0))) { pfs_log("WinFsp is installed but cannot be loaded. Get it from https://winfsp.dev/"); return 11; }

    // A WinFsp mount point folder must not exist: create the parent, clear an empty leftover.
    if (mount[1] == L':' && mount[2] == L'\\') {
        WCHAR parent[MAX_PATHW]; wcscpy_s(parent, MAX_PATHW, mount);
        WCHAR *slash = wcsrchr(parent, L'\\');
        if (slash && slash > parent + 2) {
            *slash = 0;
            WCHAR *up = wcsrchr(parent, L'\\');
            if (up && up > parent + 2) { *up = 0; CreateDirectoryW(parent, NULL); *up = L'\\'; }
            CreateDirectoryW(parent, NULL);
        }
        RemoveDirectoryW(mount);   // succeeds only for an empty leftover folder
    }

    FSP_FSCTL_VOLUME_PARAMS vp; memset(&vp, 0, sizeof vp);
    vp.SectorSize = 512; vp.SectorsPerAllocationUnit = 1;
    vp.VolumeCreationTime = pfs_time();
    vp.FileInfoTimeout = 1000;
    vp.CasePreservedNames = 1; vp.UnicodeOnDisk = 1; vp.PersistentAcls = 1;
    vp.FlushAndPurgeOnCleanup = 1;
    vp.UmFileContextIsUserContext2 = 1; vp.PostCleanupWhenModifiedOnly = 1;
    wcscpy_s(vp.FileSystemName, sizeof vp.FileSystemName / sizeof(WCHAR), L"PipeFiles");

    NTSTATUS st = FspFileSystemCreate(L"" FSP_FSCTL_DISK_DEVICE_NAME, &vp, &g_iface, &g_fs);
    if (!NT_SUCCESS(st)) { pfs_log("FspFileSystemCreate failed: 0x%08x", (unsigned)st); return 13; }
    st = FspFileSystemSetMountPoint(g_fs, (PWSTR)mount);
    if (!NT_SUCCESS(st)) { pfs_log("SetMountPoint %ls failed: 0x%08x", mount, (unsigned)st); FspFileSystemDelete(g_fs); return 14; }
    st = FspFileSystemStartDispatcher(g_fs, 0);
    if (!NT_SUCCESS(st)) { pfs_log("StartDispatcher failed: 0x%08x", (unsigned)st); FspFileSystemDelete(g_fs); return 15; }
    return 0;
}

static void stop(void)
{
    FspFileSystemStopDispatcher(g_fs);
    FspFileSystemDelete(g_fs);
    g_fs = NULL;
}

// `--winfsp-dir <folder>`: never read in service mode.
static BOOL parse_run_option(const WCHAR *flag, const WCHAR *value)
{
    if (0 != wcscmp(flag, L"--winfsp-dir") || !value[0]) return FALSE;
    wcscpy_s(g_winfsp_dir, MAX_PATHW, value);
    return TRUE;
}

const PfsBackend pfs_backend = {
    L"WinFsp", "https://winfsp.dev/", installed, start, stop, parse_run_option,
    L"--winfsp-dir <folder>",
    L"--winfsp-dir makes the WinFsp check look for <folder>\\bin\\winfsp-x64.dll instead of the real install",
};
