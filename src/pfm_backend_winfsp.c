// The WinFsp backend of pipe-file-mount: serves the core's tree on a WinFsp volume.
//
// MIT licensed. Uses WinFsp - Windows File System Proxy,
// Copyright (C) Bill Zissimopoulos - https://github.com/winfsp/winfsp
#include <winfsp/winfsp.h>
#include <stdio.h>
#include <stdlib.h>
#include "pfm_backend.h"

typedef struct { void *file; PVOID dirbuf; } Handle;   // the core's node plus WinFsp's directory buffer

static FSP_FILE_SYSTEM *g_fs;

static void fill_info(const PfmInfo *i, FSP_FSCTL_FILE_INFO *fi)
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
    vi->VolumeLabelLength = (UINT16)(sizeof(L"PipeFile") - sizeof(WCHAR));
    memcpy(vi->VolumeLabel, L"PipeFile", vi->VolumeLabelLength);
    return STATUS_SUCCESS;
}

static NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM *fs, PWSTR name, PUINT32 attrs, PSECURITY_DESCRIPTOR sd, SIZE_T *sdsz)
{
    (void)fs;
    BOOL dir;
    NTSTATUS st = pfm_stat(name, &dir, sd, sdsz);
    if (NT_SUCCESS(st) && attrs) *attrs = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
    return st;
}

// WinFsp refuses to dispatch creates unless Create and Overwrite exist.
static NTSTATUS Create(FSP_FILE_SYSTEM *fs, PWSTR name, UINT32 opts, UINT32 access, UINT32 attrs,
    PSECURITY_DESCRIPTOR sd, UINT64 alloc, PVOID *pctx, FSP_FSCTL_FILE_INFO *fi)
{
    (void)fs; (void)opts; (void)access; (void)attrs; (void)sd; (void)alloc; (void)pctx; (void)fi;
    pfm_log("create refused %ls", name);
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
    PfmInfo info;
    // The caller is only known here: FspFileSystemOperationProcessIdF() is 0 in the other callbacks.
    NTSTATUS st = pfm_open(name, access, FspFileSystemOperationProcessIdF(), &h->file, &info);
    if (!NT_SUCCESS(st)) { free(h); return st; }
    fill_info(&info, fi);
    *pctx = h;
    return STATUS_SUCCESS;
}

static VOID Close(FSP_FILE_SYSTEM *fs, PVOID ctx)
{
    (void)fs;
    Handle *h = (Handle *)ctx;
    pfm_close(h->file);
    FspFileSystemDeleteDirectoryBuffer(&h->dirbuf);
    free(h);
}

static NTSTATUS Read(FSP_FILE_SYSTEM *fs, PVOID ctx, PVOID buf, UINT64 off, ULONG len, PULONG done)
{
    (void)fs;
    return pfm_read(((Handle *)ctx)->file, off, len, buf, done);
}

static NTSTATUS GetFileInfo(FSP_FILE_SYSTEM *fs, PVOID ctx, FSP_FSCTL_FILE_INFO *fi)
{
    (void)fs;
    PfmInfo info;
    NTSTATUS st = pfm_get_info(((Handle *)ctx)->file, &info);
    if (NT_SUCCESS(st)) fill_info(&info, fi);
    return st;
}

typedef struct { Handle *h; NTSTATUS result; } Fill;

static BOOL add_entry(void *ctx, const WCHAR *leaf, BOOL dir)
{
    Fill *f = (Fill *)ctx;
    union { UINT8 b[sizeof(FSP_FSCTL_DIR_INFO) + (MAX_COMPONENT + 1) * sizeof(WCHAR)]; FSP_FSCTL_DIR_INFO d; } u;
    memset(&u, 0, sizeof u);
    size_t bytes = wcslen(leaf) * sizeof(WCHAR);
    u.d.Size = (UINT16)(FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + bytes);
    u.d.FileInfo.FileAttributes = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
    u.d.FileInfo.CreationTime = u.d.FileInfo.LastAccessTime = u.d.FileInfo.LastWriteTime = u.d.FileInfo.ChangeTime = pfm_time();
    memcpy(u.d.FileNameBuf, leaf, bytes);
    FspFileSystemFillDirectoryBuffer(&f->h->dirbuf, &u.d, &f->result);
    return NT_SUCCESS(f->result);
}

static NTSTATUS ReadDirectory(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR pattern, PWSTR marker, PVOID buf, ULONG len, PULONG done)
{
    (void)fs; (void)pattern;
    Handle *h = (Handle *)ctx;
    PfmInfo info;
    if (!NT_SUCCESS(pfm_get_info(h->file, &info)) || !info.dir) return STATUS_NOT_A_DIRECTORY;
    Fill f = { h, STATUS_SUCCESS };
    if (FspFileSystemAcquireDirectoryBuffer(&h->dirbuf, 0 == marker, &f.result)) {
        pfm_list(h->file, add_entry, &f);
        FspFileSystemReleaseDirectoryBuffer(&h->dirbuf);
    }
    if (!NT_SUCCESS(f.result)) return f.result;
    FspFileSystemReadDirectoryBuffer(&h->dirbuf, marker, buf, len, done);
    return STATUS_SUCCESS;
}

static FSP_FILE_SYSTEM_INTERFACE g_iface = {
    .GetVolumeInfo = GetVolumeInfo,
    .GetSecurityByName = GetSecurityByName,
    .Create = Create,
    .Overwrite = Overwrite,
    .Open = Open,
    .Close = Close,
    .Read = Read,
    .GetFileInfo = GetFileInfo,
    .ReadDirectory = ReadDirectory,
};

static BOOL load(void) { return NT_SUCCESS(FspLoad(0)); }

static int start(const WCHAR *mount)
{
    FSP_FSCTL_VOLUME_PARAMS vp; memset(&vp, 0, sizeof vp);
    vp.SectorSize = 512; vp.SectorsPerAllocationUnit = 1;
    vp.VolumeCreationTime = pfm_time();
    vp.FileInfoTimeout = 1000;       // short attribute cache (the sample default)
    vp.CasePreservedNames = 1; vp.UnicodeOnDisk = 1; vp.PersistentAcls = 1;
    vp.FlushAndPurgeOnCleanup = 1;   // drop cached data when the last handle closes
    vp.UmFileContextIsUserContext2 = 1; vp.PostCleanupWhenModifiedOnly = 1;
    wcscpy_s(vp.FileSystemName, sizeof vp.FileSystemName / sizeof(WCHAR), L"PipeFile"); // at most 15 characters: the field holds 16 wide chars including the NUL

    NTSTATUS st = FspFileSystemCreate(L"" FSP_FSCTL_DISK_DEVICE_NAME, &vp, &g_iface, &g_fs);
    if (!NT_SUCCESS(st)) { fprintf(stderr, "FspFileSystemCreate failed: 0x%08x\n", (unsigned)st); return 1; }
    st = FspFileSystemSetMountPoint(g_fs, (L'*' == mount[0] && 0 == mount[1]) ? 0 : (PWSTR)mount);
    if (!NT_SUCCESS(st)) { fprintf(stderr, "SetMountPoint failed: 0x%08x\n", (unsigned)st); FspFileSystemDelete(g_fs); return 1; }
    st = FspFileSystemStartDispatcher(g_fs, 0);
    if (!NT_SUCCESS(st)) { fprintf(stderr, "StartDispatcher failed: 0x%08x\n", (unsigned)st); FspFileSystemDelete(g_fs); return 1; }
    return 0;
}

static void stop(void)
{
    FspFileSystemStopDispatcher(g_fs);
    FspFileSystemDelete(g_fs);
    g_fs = NULL;
}

const PfmBackend pfm_backend = {
    "WinFsp is not installed. Get it from https://winfsp.dev/ or https://github.com/winfsp/winfsp/releases",
    load, start, stop,
};
