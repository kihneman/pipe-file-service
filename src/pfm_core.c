// pipe-file-mount (experimental): expose one read-only file on a user-mode file system
// volume (WinFsp or Dokan, see pfm_backend.h) whose content is read from a Windows named pipe
// the first time a handle reads it. Nothing is written to disk. The file can sit at any path
// on the volume; the directories above it are virtual and read-only. The caller's PID and
// image are logged at open time, which is where the drivers report them.
//
// MIT licensed. This file is driver independent; the backend that is linked with it carries the
// driver's notices.
#include "pfm_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sddl.h>
#define MOUNT_VERSION "0.6.0"   // scripts/release.ps1 reads this; release tags must match it
#define DEFAULT_PATH L"\\property-file.env"
#define DENIED_PREFIX "Access denied: you do not have permission"
#define MAX_DEPTH 32
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
// The content is fetched on the first Read of a handle, not at Open: programs
// that open the file and close it (or only stat it) never reach the data server.
// Open records who is asking (pid, creation time, a process handle that keeps the
// pid from being reused); the first Read uses that identity. `lock` makes
// concurrent first reads on one handle share one fetch; `result` is remembered so
// later reads repeat the outcome without asking the server again.
typedef struct {
    int depth; BYTE *data; SIZE_T len;
    BOOL want_data;                 // opened with read access
    UINT32 pid; UINT64 start; HANDLE proc;   // reader identity (reader-info mode)
    SRWLOCK lock; BOOL done; NTSTATUS result;
} Ctx;

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
static int lookup(const WCHAR *name)
{
    if (0 == wcscmp(name, L"\\")) return 0;
    for (int i = 0; i < g_ncomp; i++)
        if (0 == _wcsicmp(name, g_prefix[i])) return i + 1;
    return -1;
}

static BOOL is_file(int depth) { return depth == g_ncomp; }

static void caller(UINT32 pid, char *out, size_t n)
{
    WCHAR path[MAX_PATH] = L"?";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) { DWORD l = MAX_PATH; QueryFullProcessImageNameW(h, 0, path, &l); CloseHandle(h); }
    snprintf(out, n, "pid=%u image=%ls", pid, path);
}

// Reader info (--reader-info-suffix <suffix>). With the option, the content is
// not read from the data pipe directly: the mount tool opens a second pipe named
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
static NTSTATUS fetch_forwarded(UINT32 pid, UINT64 start, BYTE **pdata, SIZE_T *plen)
{
    DWORD first = GetTickCount(), deadline = first + 10000;
    HANDLE h;
    for (;;) {
        h = CreateFileW(g_reader_info_pipe, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) break;
        DWORD e = GetLastError();
        // Between instances the pipe can be briefly absent; a pipe that stays absent is an error.
        if (e == ERROR_FILE_NOT_FOUND && GetTickCount() - first >= 2000) {
            logf_("  the reader-info pipe is not served: %ls", g_reader_info_pipe);
            return STATUS_IO_DEVICE_ERROR;
        }
        if ((e == ERROR_PIPE_BUSY || e == ERROR_FILE_NOT_FOUND) && GetTickCount() < deadline) { Sleep(50); continue; }
        logf_("  reader-info pipe open failed: %lu", e);
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
    return result;
}

// Holding the process open from Open until Close keeps its PID from being reused
// while the handle waits for its first Read.
static NTSTATUS identify_reader(Ctx *c, UINT32 pid)
{
    FILETIME created, exited, kernel, user;
    c->proc = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : NULL;
    if (!c->proc || !GetProcessTimes(c->proc, &created, &exited, &kernel, &user)) {
        logf_("  cannot identify the calling process (pid %u); refusing", pid);
        if (c->proc) { CloseHandle(c->proc); c->proc = NULL; }
        return STATUS_ACCESS_DENIED;
    }
    c->pid = pid;
    c->start = ((UINT64)created.dwHighDateTime << 32) | created.dwLowDateTime;
    return STATUS_SUCCESS;
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

// Fetches the content once per handle, on its first Read. A failure is remembered
// and repeated without asking the server again: STATUS_ACCESS_DENIED when the reader
// was refused, STATUS_IO_DEVICE_ERROR when the server could not be used.
static NTSTATUS ensure_data(Ctx *c)
{
    if (!c->want_data) return STATUS_ACCESS_DENIED;
    AcquireSRWLockExclusive(&c->lock);
    if (!c->done) {
        logf_("FETCH pid=%u", c->pid);
        c->result = c->proc ? fetch_forwarded(c->pid, c->start, &c->data, &c->len)
                            : fetch(&c->data, &c->len);
        c->done = TRUE;
        logf_("  -> 0x%08x, %zu bytes", (unsigned)c->result, c->len);
        if (c->proc) { CloseHandle(c->proc); c->proc = NULL; }
    }
    NTSTATUS r = c->result;
    ReleaseSRWLockExclusive(&c->lock);
    return r;
}


// Only the user who started the mount tool (and SYSTEM) may open anything on the
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

// ---------------------------------------------------------------- the nodes the backend serves

UINT64 pfm_time(void) { return g_now; }

void pfm_log(const char *fmt, ...)
{
    char msg[512];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    logf_("%s", msg);
}

static void fill_info(const Ctx *c, PfmInfo *i)
{
    BOOL file = is_file(c->depth);
    i->dir = !file;
    i->size = file ? c->len : 0;
    i->id = (UINT64)c->depth + 1;
    i->time = g_now;
}

static NTSTATUS copy_sd(PSECURITY_DESCRIPTOR sd, SIZE_T *sdsz)
{
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

NTSTATUS pfm_get_security(PSECURITY_DESCRIPTOR sd, SIZE_T *sdsz) { return copy_sd(sd, sdsz); }

NTSTATUS pfm_stat(const WCHAR *name, BOOL *dir, PSECURITY_DESCRIPTOR sd, SIZE_T *sdsz)
{
    int depth = lookup(name);
    if (depth < 0) return STATUS_OBJECT_NAME_NOT_FOUND;
    *dir = !is_file(depth);
    return copy_sd(sd, sdsz);
}

NTSTATUS pfm_open(const WCHAR *name, UINT32 access, UINT32 pid, void **pfile, PfmInfo *info)
{
    int depth = lookup(name);
    if (depth < 0) return STATUS_OBJECT_NAME_NOT_FOUND;
    Ctx *c = (Ctx *)calloc(1, sizeof *c);
    c->depth = depth;
    if (is_file(depth)) {
        char who[600]; caller(pid, who, sizeof who);
        logf_("OPEN  %ls access=0x%08x %s", name, access, who);
        // Opens that are not for reading data (stat, directory listings, AV probes) never
        // read the secret, and neither do opens that close without a Read: the fetch is
        // made by the first Read. Here we only note who is asking.
        if (access & FILE_READ_DATA) {
            c->want_data = TRUE;
            if (g_reader_info_pipe[0]) {
                NTSTATUS st = identify_reader(c, pid);
                if (!NT_SUCCESS(st)) { free(c); logf_("  -> 0x%08x", (unsigned)st); return st; }
            }
        }
    }
    fill_info(c, info);
    *pfile = c;
    return STATUS_SUCCESS;
}

void pfm_close(void *file)
{
    Ctx *c = (Ctx *)file;
    if (is_file(c->depth)) logf_("CLOSE");
    if (c->data) { SecureZeroMemory(c->data, c->len); free(c->data); }
    if (c->proc) CloseHandle(c->proc);
    free(c);
}

NTSTATUS pfm_read(void *file, UINT64 off, ULONG len, void *buf, ULONG *done)
{
    Ctx *c = (Ctx *)file;
    if (!is_file(c->depth)) return STATUS_INVALID_DEVICE_REQUEST;
    NTSTATUS st = ensure_data(c);
    if (!NT_SUCCESS(st)) return st;
    if (off >= c->len) return STATUS_END_OF_FILE;
    SIZE_T n = c->len - (SIZE_T)off; if (n > len) n = len;
    memcpy(buf, c->data + off, n);
    *done = (ULONG)n;
    return STATUS_SUCCESS;
}

NTSTATUS pfm_get_info(void *file, PfmInfo *info)
{
    Ctx *c = (Ctx *)file;
    AcquireSRWLockShared(&c->lock);   // c->len changes when the first Read fetches
    fill_info(c, info);
    ReleaseSRWLockShared(&c->lock);
    return STATUS_SUCCESS;
}

NTSTATUS pfm_list(void *file, BOOL (*visit)(void *ctx, const WCHAR *name, BOOL dir), void *ctx)
{
    Ctx *c = (Ctx *)file;
    if (is_file(c->depth)) return STATUS_NOT_A_DIRECTORY;
    if (c->depth > 0) {
        if (!visit(ctx, L".", TRUE)) return STATUS_SUCCESS;
        if (!visit(ctx, L"..", TRUE)) return STATUS_SUCCESS;
    }
    // The only child of a directory is the next component on the served path.
    visit(ctx, g_comp[c->depth], !is_file(c->depth + 1));
    return STATUS_SUCCESS;
}

static BOOL WINAPI on_ctrl(DWORD t) { (void)t; SetEvent(g_stop); return TRUE; }

int wmain(int argc, wchar_t **argv)
{
    if (argc == 2 && (0 == wcscmp(argv[1], L"--version") || 0 == wcscmp(argv[1], L"-V"))) {
        printf("pipe-file-mount %s\n", MOUNT_VERSION);
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
            L"usage: pipe-file-mount <\\\\.\\pipe\\name> <mountpoint: K: or *> [path on the volume] [--reader-info-suffix <suffix>]\n"
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
    // The driver's DLL is delay-loaded so a missing driver gives this message, not a loader error.
    if (!pfm_backend.load()) {
        fprintf(stderr, "%s\n", pfm_backend.not_installed_message);
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

    if (pfm_backend.start(argv[2])) return 1;
    logf_("serving %ls on %ls from %ls (Ctrl-C to stop)", g_prefix[g_ncomp - 1], argv[2], g_pipe);
    if (g_reader_info_pipe[0]) logf_("reader info: asking %ls who opened the file", g_reader_info_pipe);
    WaitForSingleObject(g_stop, INFINITE);
    pfm_backend.stop();
    logf_("stopped");
    return 0;
}
