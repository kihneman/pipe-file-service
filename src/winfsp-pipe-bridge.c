// winfsp-pipe-bridge (experimental): expose one read-only file on a WinFsp
// volume whose content is read from a Windows named pipe each time the file is
// opened for reading. Nothing is written to disk. The file can sit at any path
// on the volume; the directories above it are virtual and read-only. The
// caller's PID and image are logged at open time, which is where WinFsp
// reports them.
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

#define BRIDGE_VERSION "0.3.0"   // scripts/release.ps1 reads this; release tags must match it
#define DEFAULT_PATH L"\\property-file.env"
#define DENIED_PREFIX "Access denied: you do not have permission"
#define MAX_DEPTH 32
#define MAX_COMPONENT 255
#define MAX_PATH_CHARS 2048

static WCHAR g_pipe[512];
static HANDLE g_stop;
static UINT64 g_now;

// The served path split into components. g_prefix[i] is the volume-relative
// path of the first i+1 components, so g_prefix[g_ncomp - 1] is the file.
static WCHAR g_comp[MAX_DEPTH][MAX_COMPONENT + 1];
static WCHAR g_prefix[MAX_DEPTH][MAX_PATH_CHARS];
static int g_ncomp;

// depth 0 is the volume root, 1..g_ncomp-1 are directories, g_ncomp is the file.
typedef struct { int depth; BYTE *data; SIZE_T len; PVOID dirbuf; } Ctx;

static void logf_(const char *fmt, ...)
{
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(stderr, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr); fflush(stderr);
}

// Splits a volume path such as "\a\b\file.env" (or "a/b/file.env") into
// g_comp / g_prefix. Rejects empty, "." and ".." components, characters that
// are invalid in Windows names, and anything deeper than MAX_DEPTH.
static BOOL parse_path(const wchar_t *in, const char **why)
{
    static WCHAR norm[MAX_PATH_CHARS];
    size_t n = wcslen(in);
    if (n == 0 || n + 2 >= MAX_PATH_CHARS) { *why = "path is empty or too long"; return FALSE; }
    norm[0] = L'\\'; norm[1] = 0;
    wcscat_s(norm, MAX_PATH_CHARS, (in[0] == L'\\' || in[0] == L'/') ? in + 1 : in);
    for (WCHAR *p = norm; *p; p++) if (*p == L'/') *p = L'\\';

    g_ncomp = 0;
    WCHAR *start = norm + 1;
    for (;;) {
        WCHAR *end = wcschr(start, L'\\');
        size_t len = end ? (size_t)(end - start) : wcslen(start);
        if (len == 0) { *why = "path has an empty component (double or trailing backslash)"; return FALSE; }
        if (len > MAX_COMPONENT) { *why = "a path component is too long"; return FALSE; }
        if (g_ncomp == MAX_DEPTH) { *why = "path is too deep"; return FALSE; }
        wcsncpy_s(g_comp[g_ncomp], MAX_COMPONENT + 1, start, len);
        if (0 == wcscmp(g_comp[g_ncomp], L".") || 0 == wcscmp(g_comp[g_ncomp], L"..")) {
            *why = "'.' and '..' are not allowed"; return FALSE;
        }
        for (size_t i = 0; i < len; i++) {
            if (g_comp[g_ncomp][i] < 32 || wcschr(L"<>:\"|?*", g_comp[g_ncomp][i])) {
                *why = "path contains a character that is invalid in Windows names"; return FALSE;
            }
        }
        size_t prefix_len = (size_t)((end ? end : start + len) - norm);
        wcsncpy_s(g_prefix[g_ncomp], MAX_PATH_CHARS, norm, prefix_len);
        g_ncomp++;
        if (!end) break;
        start = end + 1;
    }
    return TRUE;
}

// Returns the depth of `name` in the served tree, or -1 if it is not part of it.
static int lookup(PWSTR name)
{
    if (0 == wcscmp(name, L"\\")) return 0;
    for (int i = 0; i < g_ncomp; i++)
        if (0 == _wcsicmp(name, g_prefix[i])) return i + 1;
    return -1;
}

static BOOL is_file(int depth) { return depth == g_ncomp; }

static void caller(char *out, size_t n)
{
    UINT32 pid = FspFileSystemOperationProcessIdF();
    WCHAR path[MAX_PATH] = L"?";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) { DWORD l = MAX_PATH; QueryFullProcessImageNameW(h, 0, path, &l); CloseHandle(h); }
    snprintf(out, n, "pid=%u image=%ls", pid, path);
}

// Reader info (--reader-info-suffix <suffix>). With the option, the content is
// not read from the data pipe directly: the bridge opens a second pipe named
// <data pipe><suffix>, sends the PID and creation time of the program that
// opened the file, and the server answers for that program (it can ask the user
// about it, refuse it, or serve it). The protocol is described in the README.
// The option is a request, not a hint: if the second pipe is missing or the
// server refuses, the open fails. It never falls back to the data pipe, which
// would make this program the reader.
#define FORWARD_PROTOCOL 1
#define TAG_READ_REQUEST 72
#define TAG_READ_RESPONSE 73

static WCHAR g_reader_info_pipe[512];   // empty unless --reader-info-suffix was given

static BOOL read_exact(HANDLE h, BYTE *p, DWORD n)
{
    while (n) {
        DWORD got = 0;
        if (!ReadFile(h, p, n, &got, NULL) || got == 0) return FALSE;
        p += got; n -= got;
    }
    return TRUE;
}

static void read_to_eof(HANDLE h, BYTE **pdata, SIZE_T *plen, SIZE_T *pcap)
{
    SIZE_T cap = 4096, len = 0;
    BYTE *buf = (BYTE *)malloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; buf = (BYTE *)realloc(buf, cap); }
        DWORD got = 0;
        if (!ReadFile(h, buf + len, (DWORD)(cap - len), &got, NULL) || got == 0) break;
        len += got;
    }
    *pdata = buf; *plen = len; *pcap = cap;
}

// Asks the reader-info pipe for the content on behalf of process `pid`.
static NTSTATUS fetch_forwarded(UINT32 pid, BYTE **pdata, SIZE_T *plen)
{
    // Holding the process open keeps its PID from being reused while we ask.
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    FILETIME created, exited, kernel, user;
    if (pid == 0 || !proc || !GetProcessTimes(proc, &created, &exited, &kernel, &user)) {
        logf_("  cannot identify the calling process (pid %u); refusing", pid);
        if (proc) CloseHandle(proc);
        return STATUS_ACCESS_DENIED;
    }
    UINT64 start = ((UINT64)created.dwHighDateTime << 32) | created.dwLowDateTime;

    DWORD first = GetTickCount(), deadline = first + 10000;
    HANDLE h;
    for (;;) {
        h = CreateFileW(g_reader_info_pipe, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) break;
        DWORD e = GetLastError();
        // Between instances the pipe can be briefly absent; a pipe that stays absent is an error.
        if (e == ERROR_FILE_NOT_FOUND && GetTickCount() - first >= 2000) {
            logf_("  the reader-info pipe is not served: %ls", g_reader_info_pipe);
            CloseHandle(proc);
            return STATUS_IO_DEVICE_ERROR;
        }
        if ((e == ERROR_PIPE_BUSY || e == ERROR_FILE_NOT_FOUND) && GetTickCount() < deadline) { Sleep(50); continue; }
        logf_("  reader-info pipe open failed: %lu", e);
        CloseHandle(proc);
        return STATUS_IO_DEVICE_ERROR;
    }

    char body[128];
    int blen = snprintf(body, sizeof body, "{\"protocol\":%d,\"reader_pid\":%u,\"reader_start_time\":%llu}",
        FORWARD_PROTOCOL, pid, (unsigned long long)start);
    BYTE frame[5 + sizeof body];
    frame[0] = 0; frame[1] = 0; frame[2] = (BYTE)(blen >> 8); frame[3] = (BYTE)blen; frame[4] = TAG_READ_REQUEST;
    memcpy(frame + 5, body, blen);
    DWORD wrote = 0;
    NTSTATUS result = STATUS_ACCESS_DENIED;
    if (!WriteFile(h, frame, 5 + blen, &wrote, NULL)) {
        logf_("  reader-info pipe write failed: %lu", GetLastError());
        result = STATUS_IO_DEVICE_ERROR;
    } else {
        BYTE hdr[5];
        if (!read_exact(h, hdr, 5)) {
            logf_("  the server closed the reader-info pipe without answering: it probably does not trust this "
                  "program (check its log; it may require this program's SHA-256 to be on a trusted list)");
        } else {
            DWORD rlen = ((DWORD)hdr[0] << 24) | ((DWORD)hdr[1] << 16) | ((DWORD)hdr[2] << 8) | hdr[3];
            char rbody[256] = {0};
            if (hdr[4] != TAG_READ_RESPONSE || rlen >= sizeof rbody || !read_exact(h, (BYTE *)rbody, rlen)) {
                logf_("  unexpected answer on the reader-info pipe");
                result = STATUS_IO_DEVICE_ERROR;
            } else if (strstr(rbody, "\"ok\"") == NULL) {
                logf_("  the server denied the read");
            } else {
                SIZE_T cap;
                read_to_eof(h, pdata, plen, &cap);
                result = STATUS_SUCCESS;
            }
        }
    }
    CloseHandle(h);
    CloseHandle(proc);
    return result;
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
        logf_("  the pipe's server denied the read");
        SecureZeroMemory(buf, cap); free(buf);
        return STATUS_ACCESS_DENIED;
    }
    *pdata = buf; *plen = len;
    return STATUS_SUCCESS;
}

static void fill_info(const Ctx *c, FSP_FSCTL_FILE_INFO *fi)
{
    memset(fi, 0, sizeof *fi);
    BOOL file = is_file(c->depth);
    fi->FileAttributes = file ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_DIRECTORY;
    fi->FileSize = file ? c->len : 0;
    fi->AllocationSize = (fi->FileSize + 4095) / 4096 * 4096;
    fi->CreationTime = fi->LastAccessTime = fi->LastWriteTime = fi->ChangeTime = g_now;
    fi->IndexNumber = (UINT64)c->depth + 1;
}

// Only the user who started the bridge (and SYSTEM) may open anything on the
// volume: it is a front door to a pipe that only this user may read.
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

static NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM *fs, FSP_FSCTL_VOLUME_INFO *vi)
{
    (void)fs;
    vi->TotalSize = 1 << 20; vi->FreeSize = 0;
    vi->VolumeLabelLength = (UINT16)(sizeof(L"PipeBridge") - sizeof(WCHAR));
    memcpy(vi->VolumeLabel, L"PipeBridge", vi->VolumeLabelLength);
    return STATUS_SUCCESS;
}

static NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM *fs, PWSTR name, PUINT32 attrs,
    PSECURITY_DESCRIPTOR sd, SIZE_T *sdsz)
{
    (void)fs;
    int depth = lookup(name);
    if (depth < 0) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (attrs) *attrs = is_file(depth) ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_DIRECTORY;
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

// WinFsp refuses to dispatch creates unless Create and Overwrite exist.
static NTSTATUS Create(FSP_FILE_SYSTEM *fs, PWSTR name, UINT32 opts, UINT32 access, UINT32 attrs,
    PSECURITY_DESCRIPTOR sd, UINT64 alloc, PVOID *pctx, FSP_FSCTL_FILE_INFO *fi)
{
    (void)fs; (void)opts; (void)access; (void)attrs; (void)sd; (void)alloc; (void)pctx; (void)fi;
    logf_("create refused %ls", name);
    return STATUS_MEDIA_WRITE_PROTECTED;
}

static NTSTATUS Overwrite(FSP_FILE_SYSTEM *fs, PVOID ctx, UINT32 attrs, BOOLEAN replace,
    UINT64 alloc, FSP_FSCTL_FILE_INFO *fi)
{
    (void)fs; (void)ctx; (void)attrs; (void)replace; (void)alloc; (void)fi;
    return STATUS_MEDIA_WRITE_PROTECTED;
}

static NTSTATUS Open(FSP_FILE_SYSTEM *fs, PWSTR name, UINT32 opts, UINT32 access,
    PVOID *pctx, FSP_FSCTL_FILE_INFO *fi)
{
    (void)fs; (void)opts;
    int depth = lookup(name);
    if (depth < 0) return STATUS_OBJECT_NAME_NOT_FOUND;
    Ctx *c = (Ctx *)calloc(1, sizeof *c);
    c->depth = depth;
    if (is_file(depth)) {
        char who[600]; caller(who, sizeof who);
        logf_("OPEN  %ls access=0x%08x %s", name, access, who);
        // Attribute-only opens (stat, directory listings, AV probes) must not read the secret.
        NTSTATUS st = STATUS_SUCCESS;
        if (access & FILE_READ_DATA) {
            st = g_reader_info_pipe[0]
                ? fetch_forwarded(FspFileSystemOperationProcessIdF(), &c->data, &c->len)
                : fetch(&c->data, &c->len);
        }
        if (!NT_SUCCESS(st)) { free(c); logf_("  -> 0x%08x", (unsigned)st); return st; }
        logf_("  -> %zu bytes", c->len);
    }
    fill_info(c, fi);
    *pctx = c;
    return STATUS_SUCCESS;
}

static VOID Close(FSP_FILE_SYSTEM *fs, PVOID ctx)
{
    (void)fs;
    Ctx *c = (Ctx *)ctx;
    if (is_file(c->depth)) logf_("CLOSE");
    if (c->data) { SecureZeroMemory(c->data, c->len); free(c->data); }
    FspFileSystemDeleteDirectoryBuffer(&c->dirbuf);
    free(c);
}

static NTSTATUS Read(FSP_FILE_SYSTEM *fs, PVOID ctx, PVOID buf, UINT64 off, ULONG len, PULONG done)
{
    (void)fs;
    Ctx *c = (Ctx *)ctx;
    if (!is_file(c->depth)) return STATUS_INVALID_DEVICE_REQUEST;
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

static void add_entry(Ctx *c, const WCHAR *leaf, BOOL dir, NTSTATUS *result)
{
    union { UINT8 b[sizeof(FSP_FSCTL_DIR_INFO) + (MAX_COMPONENT + 1) * sizeof(WCHAR)]; FSP_FSCTL_DIR_INFO d; } u;
    memset(&u, 0, sizeof u);
    size_t bytes = wcslen(leaf) * sizeof(WCHAR);
    u.d.Size = (UINT16)(FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + bytes);
    u.d.FileInfo.FileAttributes = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
    u.d.FileInfo.CreationTime = u.d.FileInfo.LastAccessTime = u.d.FileInfo.LastWriteTime = u.d.FileInfo.ChangeTime = g_now;
    memcpy(u.d.FileNameBuf, leaf, bytes);
    FspFileSystemFillDirectoryBuffer(&c->dirbuf, &u.d, result);
}

static NTSTATUS ReadDirectory(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR pattern, PWSTR marker,
    PVOID buf, ULONG len, PULONG done)
{
    (void)fs; (void)pattern;
    Ctx *c = (Ctx *)ctx;
    if (is_file(c->depth)) return STATUS_NOT_A_DIRECTORY;
    NTSTATUS result;
    if (FspFileSystemAcquireDirectoryBuffer(&c->dirbuf, 0 == marker, &result)) {
        if (c->depth > 0) {
            add_entry(c, L".", TRUE, &result);
            if (NT_SUCCESS(result)) add_entry(c, L"..", TRUE, &result);
        }
        // The only child of a directory is the next component on the served path.
        if (NT_SUCCESS(result)) add_entry(c, g_comp[c->depth], !is_file(c->depth + 1), &result);
        FspFileSystemReleaseDirectoryBuffer(&c->dirbuf);
    }
    if (!NT_SUCCESS(result)) return result;
    FspFileSystemReadDirectoryBuffer(&c->dirbuf, marker, buf, len, done);
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

static BOOL WINAPI on_ctrl(DWORD t) { (void)t; SetEvent(g_stop); return TRUE; }

int wmain(int argc, wchar_t **argv)
{
    if (argc == 2 && (0 == wcscmp(argv[1], L"--version") || 0 == wcscmp(argv[1], L"-V"))) {
        printf("winfsp-pipe-bridge %s\n", BRIDGE_VERSION);
        return 0;
    }
    const wchar_t *suffix = NULL;
    int bad_option = 0;
    for (int i = 1; i < argc; i++) {
        if (0 != wcscmp(argv[i], L"--reader-info-suffix")) continue;
        if (suffix || i + 1 >= argc || argv[i + 1][0] == 0) { bad_option = 1; break; }
        suffix = argv[i + 1];
        memmove(&argv[i], &argv[i + 2], (argc - i - 2) * sizeof(*argv));
        argc -= 2; i--;
    }
    if (bad_option || argc < 3 || argc > 4) {
        fwprintf(stderr,
            L"usage: winfsp-pipe-bridge <\\\\.\\pipe\\name> <mountpoint: K: or *> [path on the volume] [--reader-info-suffix <suffix>]\n"
            L"  --reader-info-suffix: do not read the pipe directly; open <pipe name><suffix>, tell the server which\n"
            L"  program opened the file (PID and start time) and read the content it serves for that program\n"
            L"  path defaults to %ls; directories above the file are created virtually,\n"
            L"  e.g. \\secrets\\app\\prod.env\n", DEFAULT_PATH);
        return 2;
    }
    const char *why = NULL;
    if (!parse_path(argc == 4 ? argv[3] : DEFAULT_PATH, &why)) {
        fprintf(stderr, "invalid path: %s\n", why);
        return 2;
    }
    // winfsp-x64.dll is delay-loaded so a missing WinFsp gives this message, not a loader error.
    if (!NT_SUCCESS(FspLoad(0))) {
        fprintf(stderr, "WinFsp is not installed. Get it from https://winfsp.dev/ or https://github.com/winfsp/winfsp/releases\n");
        return 1;
    }
    wcsncpy_s(g_pipe, 512, argv[1], _TRUNCATE);
    if (suffix) {
        if (wcslen(g_pipe) + wcslen(suffix) >= 512) { fprintf(stderr, "pipe name plus suffix is too long\n"); return 2; }
        swprintf_s(g_reader_info_pipe, 512, L"%ls%ls", g_pipe, suffix);
    }
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    g_now = ((UINT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!build_sd()) { fprintf(stderr, "could not build the volume security descriptor\n"); return 1; }
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    FSP_FSCTL_VOLUME_PARAMS vp; memset(&vp, 0, sizeof vp);
    vp.SectorSize = 512; vp.SectorsPerAllocationUnit = 1;
    vp.VolumeCreationTime = g_now;
    vp.FileInfoTimeout = 1000;       // short attribute cache (the sample default)
    vp.CasePreservedNames = 1; vp.UnicodeOnDisk = 1; vp.PersistentAcls = 1;
    vp.FlushAndPurgeOnCleanup = 1;   // drop cached data when the last handle closes
    vp.UmFileContextIsUserContext2 = 1; vp.PostCleanupWhenModifiedOnly = 1;
    wcscpy_s(vp.FileSystemName, sizeof vp.FileSystemName / sizeof(WCHAR), L"PipeBridge"); // at most 15 characters: the field holds 16 wide chars including the NUL

    FSP_FILE_SYSTEM *fs = NULL;
    NTSTATUS st = FspFileSystemCreate(L"" FSP_FSCTL_DISK_DEVICE_NAME, &vp, &g_iface, &fs);
    if (!NT_SUCCESS(st)) { fprintf(stderr, "FspFileSystemCreate failed: 0x%08x\n", (unsigned)st); return 1; }
    st = FspFileSystemSetMountPoint(fs, (L'*' == argv[2][0] && 0 == argv[2][1]) ? 0 : argv[2]);
    if (!NT_SUCCESS(st)) { fprintf(stderr, "SetMountPoint failed: 0x%08x\n", (unsigned)st); FspFileSystemDelete(fs); return 1; }
    st = FspFileSystemStartDispatcher(fs, 0);
    if (!NT_SUCCESS(st)) { fprintf(stderr, "StartDispatcher failed: 0x%08x\n", (unsigned)st); FspFileSystemDelete(fs); return 1; }
    logf_("serving %ls on %ls from %ls (Ctrl-C to stop)", g_prefix[g_ncomp - 1], argv[2], g_pipe);
    if (g_reader_info_pipe[0]) logf_("reader info: asking %ls who opened the file", g_reader_info_pipe);
    WaitForSingleObject(g_stop, INFINITE);
    FspFileSystemStopDispatcher(fs);
    FspFileSystemDelete(fs);
    logf_("stopped");
    return 0;
}
