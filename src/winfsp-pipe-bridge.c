// winfsp-pipe-bridge (experimental): expose one read-only file on a WinFsp
// volume whose content is read from a Windows named pipe each time the file is
// opened for reading. Nothing is written to disk. The caller's PID and image
// are logged at open time, which is where WinFsp reports them.
//
// MIT licensed. Uses WinFsp - Windows File System Proxy,
// Copyright (C) Bill Zissimopoulos - https://github.com/winfsp/winfsp
#define UNICODE
#define _UNICODE
#include <winfsp/winfsp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sddl.h>

#define FILE_NAME L"\\property-file.env"
#define DENIED_PREFIX "Access denied: you do not have permission"

static WCHAR g_pipe[512];
static PVOID g_dirbuf;
static HANDLE g_stop;
static UINT64 g_now;

typedef struct { BOOL root; BYTE *data; SIZE_T len; } Ctx;

static void logf_(const char *fmt, ...)
{
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(stderr, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr); fflush(stderr);
}

static void caller(char *out, size_t n)
{
    UINT32 pid = FspFileSystemOperationProcessIdF();
    WCHAR path[MAX_PATH] = L"?";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) { DWORD l = MAX_PATH; QueryFullProcessImageNameW(h, 0, path, &l); CloseHandle(h); }
    snprintf(out, n, "pid=%u image=%ls", pid, path);
}

static NTSTATUS fetch(BYTE **pdata, SIZE_T *plen)
{
    DWORD deadline = GetTickCount() + 10000;
    HANDLE h;
    for (;;) {
        h = CreateFileW(g_pipe, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) break;
        DWORD e = GetLastError();
        if ((e == ERROR_PIPE_BUSY || e == ERROR_FILE_NOT_FOUND) && GetTickCount() < deadline) { Sleep(50); continue; }
        logf_("  pipe open failed: %lu", e);
        return STATUS_IO_DEVICE_ERROR;
    }
    SIZE_T cap = 4096, len = 0;
    BYTE *buf = (BYTE *)malloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; buf = (BYTE *)realloc(buf, cap); }
        DWORD got = 0;
        if (!ReadFile(h, buf + len, (DWORD)(cap - len), &got, NULL) || got == 0) break;
        len += got;
    }
    CloseHandle(h);
    if (len >= sizeof(DENIED_PREFIX) - 1 && memcmp(buf, DENIED_PREFIX, sizeof(DENIED_PREFIX) - 1) == 0) {
        logf_("  broker denied the read");
        SecureZeroMemory(buf, cap); free(buf);
        return STATUS_ACCESS_DENIED;
    }
    *pdata = buf; *plen = len;
    return STATUS_SUCCESS;
}

static void fill_info(const Ctx *c, FSP_FSCTL_FILE_INFO *fi)
{
    memset(fi, 0, sizeof *fi);
    fi->FileAttributes = c->root ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
    fi->FileSize = c->root ? 0 : c->len;
    fi->AllocationSize = (fi->FileSize + 4095) / 4096 * 4096;
    fi->CreationTime = fi->LastAccessTime = fi->LastWriteTime = fi->ChangeTime = g_now;
}

static NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM *fs, FSP_FSCTL_VOLUME_INFO *vi)
{
    (void)fs; logf_("GetVolumeInfo");
    vi->TotalSize = 1 << 20; vi->FreeSize = 0;
    vi->VolumeLabelLength = (UINT16)(sizeof(L"PipeBridge") - sizeof(WCHAR));
    memcpy(vi->VolumeLabel, L"PipeBridge", vi->VolumeLabelLength);
    return STATUS_SUCCESS;
}

// Only the user who started the bridge (and SYSTEM) may open anything on the
// volume: the volume is a front door to a pipe that only this user may read.
static PSECURITY_DESCRIPTOR g_sd;
static SIZE_T g_sdsz;

static BOOL build_sd(void)
{
    HANDLE tok; BYTE buf[256]; DWORD n = 0; LPWSTR sid = NULL; WCHAR sddl[512];
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return FALSE;
    BOOL ok = GetTokenInformation(tok, TokenUser, buf, sizeof buf, &n);
    CloseHandle(tok);
    if (!ok || !ConvertSidToStringSidW(((TOKEN_USER *)buf)->User.Sid, &sid)) return FALSE;
    swprintf_s(sddl, 512, L"O:%lsG:%lsD:P(A;;FA;;;SY)(A;;FRFX;;;%ls)", sid, sid, sid);
    LocalFree(sid);
    ULONG sz = 0;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &g_sd, &sz)) return FALSE;
    g_sdsz = sz;
    return TRUE;
}

static NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM *fs, PWSTR name, PUINT32 attrs,
    PSECURITY_DESCRIPTOR sd, SIZE_T *sdsz)
{
    (void)fs; logf_("GetSecurityByName %ls", name);
    if (0 == wcscmp(name, L"\\")) { if (attrs) *attrs = FILE_ATTRIBUTE_DIRECTORY; }
    else if (0 == _wcsicmp(name, FILE_NAME)) { if (attrs) *attrs = FILE_ATTRIBUTE_READONLY; }
    else return STATUS_OBJECT_NAME_NOT_FOUND;
    if (sdsz) {
        SIZE_T have = *sdsz;
        *sdsz = g_sdsz;
        if (sd) {
            if (have < g_sdsz) return STATUS_BUFFER_OVERFLOW;
            memcpy(sd, g_sd, g_sdsz);
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS Open(FSP_FILE_SYSTEM *fs, PWSTR name, UINT32 opts, UINT32 access,
    PVOID *pctx, FSP_FSCTL_FILE_INFO *fi)
{
    (void)fs; (void)opts;
    Ctx *c = (Ctx *)calloc(1, sizeof *c);
    char who[600]; caller(who, sizeof who);
    logf_("Open %ls", name);
    if (0 == wcscmp(name, L"\\")) {
        c->root = TRUE;
    } else if (0 == _wcsicmp(name, FILE_NAME)) {
        logf_("OPEN  %ls access=0x%08x %s", name, access, who);
        // Attribute-only opens (stat, directory listings, AV probes) must not read the secret.
        NTSTATUS st = (access & FILE_READ_DATA) ? fetch(&c->data, &c->len) : STATUS_SUCCESS;
        if (!NT_SUCCESS(st)) { free(c); logf_("  -> 0x%08x", (unsigned)st); return st; }
        logf_("  -> %zu bytes", c->len);
    } else {
        free(c);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    fill_info(c, fi);
    *pctx = c;
    return STATUS_SUCCESS;
}

static VOID Close(FSP_FILE_SYSTEM *fs, PVOID ctx)
{
    (void)fs;
    Ctx *c = (Ctx *)ctx;
    if (!c->root) logf_("CLOSE");
    if (c->data) { SecureZeroMemory(c->data, c->len); free(c->data); }
    free(c);
}

static NTSTATUS Read(FSP_FILE_SYSTEM *fs, PVOID ctx, PVOID buf, UINT64 off, ULONG len, PULONG done)
{
    (void)fs;
    Ctx *c = (Ctx *)ctx;
    char who[600]; caller(who, sizeof who);
    logf_("READ  off=%llu len=%lu %s", off, len, who);
    if (off >= c->len) return STATUS_END_OF_FILE;
    SIZE_T n = c->len - (SIZE_T)off; if (n > len) n = len;
    memcpy(buf, c->data + off, n);
    *done = (ULONG)n;
    return STATUS_SUCCESS;
}

static NTSTATUS GetFileInfo(FSP_FILE_SYSTEM *fs, PVOID ctx, FSP_FSCTL_FILE_INFO *fi)
{
    (void)fs; fill_info((Ctx *)ctx, fi); return STATUS_SUCCESS;
}

static NTSTATUS ReadDirectory(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR pattern, PWSTR marker,
    PVOID buf, ULONG len, PULONG done)
{
    (void)ctx; (void)pattern; logf_("ReadDirectory");
    NTSTATUS result;
    if (FspFileSystemAcquireDirectoryBuffer(&g_dirbuf, 0 == marker, &result)) {
        union { UINT8 b[sizeof(FSP_FSCTL_DIR_INFO) + MAX_PATH * sizeof(WCHAR)]; FSP_FSCTL_DIR_INFO d; } u;
        memset(&u, 0, sizeof u);
        const WCHAR *leaf = L"property-file.env";
        u.d.Size = (UINT16)(FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + wcslen(leaf) * sizeof(WCHAR));
        u.d.FileInfo.FileAttributes = FILE_ATTRIBUTE_READONLY;
        u.d.FileInfo.CreationTime = u.d.FileInfo.LastAccessTime = u.d.FileInfo.LastWriteTime = u.d.FileInfo.ChangeTime = g_now;
        memcpy(u.d.FileNameBuf, leaf, wcslen(leaf) * sizeof(WCHAR));
        FspFileSystemFillDirectoryBuffer(&g_dirbuf, &u.d, &result);
        FspFileSystemReleaseDirectoryBuffer(&g_dirbuf);
    }
    if (!NT_SUCCESS(result)) return result;
    FspFileSystemReadDirectoryBuffer(&g_dirbuf, marker, buf, len, done);
    (void)fs;
    return STATUS_SUCCESS;
}

static NTSTATUS Create(FSP_FILE_SYSTEM *fs, PWSTR name, UINT32 opts, UINT32 access, UINT32 attrs,
    PSECURITY_DESCRIPTOR sd, UINT64 alloc, PVOID *pctx, FSP_FSCTL_FILE_INFO *fi)
{
    (void)fs; (void)opts; (void)access; (void)attrs; (void)sd; (void)alloc; (void)pctx; (void)fi;
    logf_("Create refused %ls", name);
    return STATUS_MEDIA_WRITE_PROTECTED;
}

static NTSTATUS Overwrite(FSP_FILE_SYSTEM *fs, PVOID ctx, UINT32 attrs, BOOLEAN replace,
    UINT64 alloc, FSP_FSCTL_FILE_INFO *fi)
{
    (void)fs; (void)ctx; (void)attrs; (void)replace; (void)alloc; (void)fi;
    return STATUS_MEDIA_WRITE_PROTECTED;
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

static BOOL WINAPI on_ctrl(DWORD t) { (void)t; SetEvent(g_stop); return TRUE; }

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) {
        fwprintf(stderr, L"usage: spike <\\\\.\\pipe\\name> <mountpoint e.g. K:>\n");
        return 2;
    }
    wcsncpy_s(g_pipe, 512, argv[1], _TRUNCATE);
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    g_now = ((UINT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!build_sd()) { fprintf(stderr, "could not build the volume security descriptor\n"); return 1; }
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    FSP_FSCTL_VOLUME_PARAMS vp; memset(&vp, 0, sizeof vp);
    vp.SectorSize = 512; vp.SectorsPerAllocationUnit = 1;
    vp.VolumeCreationTime = g_now;
    vp.FileInfoTimeout = 1000;          // short attribute cache (the sample default)
    vp.CasePreservedNames = 1; vp.UnicodeOnDisk = 1; vp.PersistentAcls = 1;
    vp.FlushAndPurgeOnCleanup = 1; vp.UmFileContextIsUserContext2 = 1; vp.PostCleanupWhenModifiedOnly = 1;   // drop cached data when the last handle closes
    wcscpy_s(vp.FileSystemName, sizeof vp.FileSystemName / sizeof(WCHAR), L"WinFspPipeBridge");

    FSP_FILE_SYSTEM *fs = NULL;
    NTSTATUS st = FspFileSystemCreate(L"" FSP_FSCTL_DISK_DEVICE_NAME, &vp, &g_iface, &fs);
    if (!NT_SUCCESS(st)) { fprintf(stderr, "FspFileSystemCreate failed: 0x%08x\n", (unsigned)st); return 1; }
    FspFileSystemSetDebugLog(fs, (UINT32)-1);
    st = FspFileSystemSetMountPoint(fs, (L'*' == argv[2][0] && 0 == argv[2][1]) ? 0 : argv[2]);
    if (!NT_SUCCESS(st)) { fprintf(stderr, "SetMountPoint failed: 0x%08x\n", (unsigned)st); FspFileSystemDelete(fs); return 1; }
    st = FspFileSystemStartDispatcher(fs, 0);
    if (!NT_SUCCESS(st)) { fprintf(stderr, "StartDispatcher failed: 0x%08x\n", (unsigned)st); FspFileSystemDelete(fs); return 1; }
    logf_("mounted %ls -> %ls (Ctrl-C to stop)", argv[2], g_pipe);
    WaitForSingleObject(g_stop, INFINITE);
    FspFileSystemStopDispatcher(fs);
    FspFileSystemDelete(fs);
    FspFileSystemDeleteDirectoryBuffer(&g_dirbuf);
    logf_("stopped");
    return 0;
}
