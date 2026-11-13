// PipeFileService: a Windows service that shows named pipes as read-only
// files on one volume of a user-mode file system driver (see pfs_backend.h) and turns placeholder files into symlinks to
// them. It is the privileged counterpart of pipe-file-mount.exe: any local
// user's app asks it over a control pipe, and only a privileged process may
// create file symlinks without Developer Mode. The protocol is in the README.
//
// MIT licensed. This file is driver independent; the backend that is linked with it carries the driver's notices.
#include "pfs_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sddl.h>
#include <aclapi.h>

#define SERVICE_VERSION "0.6.0"   // keep equal to MOUNT_VERSION in pipe-file-mount.c
#define SVC_NAME L"PipeFileService"
#define SVC_DISPLAY L"Pipe File Service"
#define CONTROL_PIPE L"\\\\.\\pipe\\PipeFileService"
#define DEFAULT_MOUNT L"C:\\ProgramData\\PipeFileService\\mount"
#define PARAM_KEY L"SYSTEM\\CurrentControlSet\\Services\\" SVC_NAME L"\\Parameters"
#define PIPE_PREFIX L"\\\\.\\pipe\\"
#define DENIED_PREFIX "Access denied: you do not have permission"
#define MAX_PIPE 256
#define MAX_SUFFIX 64
#define MAX_PATHW 1024
#define MAX_MSG 65536

// Limits on the control pipe, which every interactive user can open. The defaults can be changed with
// `install --max-...` (kept in the service's Parameters key) or `run --max-...`; see usage().
typedef struct { const WCHAR *flag, *value; int def, hi; int cur; BOOL set; } Limit;
enum { LIM_REG_CONN, LIM_REG_USER, LIM_CONNS, LIM_CONNS_USER, LIM_COUNT };
static Limit g_limits[LIM_COUNT] = {
    { L"--max-reg-per-conn", L"MaxRegPerConn", 64, 10000, 64, FALSE },
    { L"--max-reg-per-user", L"MaxRegPerUser", 256, 100000, 256, FALSE },
    { L"--max-conn", L"MaxConn", 128, 10000, 128, FALSE },
    { L"--max-conn-per-user", L"MaxConnPerUser", 16, 10000, 16, FALSE },
};
#define LIM(i) (g_limits[i].cur)

// ---------------------------------------------------------------- logging

static FILE *g_log;
static void logf_(const char *fmt, ...)
{
    SYSTEMTIME st; GetLocalTime(&st);
    FILE *f = g_log ? g_log : stderr;
    fprintf(f, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fflush(f);
}

// ---------------------------------------------------------------- registry of entries

typedef struct Conn Conn;
typedef struct Entry {
    struct Entry *next;
    UINT64 id;
    WCHAR name[MAX_NAME + 1];          // the file name on the volume
    WCHAR pipe[MAX_PIPE + 1];          // the data pipe
    WCHAR info_pipe[MAX_PIPE + MAX_SUFFIX + 1]; // pipe + suffix; empty = read the data pipe directly
    PSECURITY_DESCRIPTOR sd; SIZE_T sdsz;
    Conn *conn;
    HANDLE link;                       // handle on the symlink this entry made, or NULL
    WCHAR link_path[MAX_PATHW];
    WCHAR link_target[MAX_PATHW];
} Entry;

struct Conn {
    struct Conn *next;
    HANDLE pipe, token, thread;       // token: impersonation token of the client
    PSID sid; BOOL elevated;
    CRITICAL_SECTION wlock;
    LONG refs; BOOL closed;
};

static CRITICAL_SECTION g_lock;       // protects g_entries, g_conns and entry/conn lifetime
static Entry *g_entries;
static Conn *g_conns;
static UINT64 g_next_id = 2;
static HANDLE g_stop;
static UINT64 g_now;
static WCHAR g_mount[MAX_PATHW];

static void conn_unref(Conn *c)
{
    if (InterlockedDecrement(&c->refs) != 0) return;
    DeleteCriticalSection(&c->wlock);
    if (c->token) CloseHandle(c->token);
    if (c->pipe && c->pipe != INVALID_HANDLE_VALUE) CloseHandle(c->pipe);
    free(c->sid); free(c);
}

// ---------------------------------------------------------------- JSON (flat objects only)

typedef struct { WCHAR key[32]; WCHAR val[MAX_PATHW]; BOOL is_str; } Kv;
typedef struct { Kv kv[12]; int n; } Json;

static const WCHAR *jget(const Json *j, const WCHAR *key)
{
    for (int i = 0; i < j->n; i++) if (0 == wcscmp(j->kv[i].key, key)) return j->kv[i].val;
    return NULL;
}

static const WCHAR *skipws(const WCHAR *p) { while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') p++; return p; }

static const WCHAR *jstring(const WCHAR *p, WCHAR *out, size_t cap)
{
    size_t n = 0;
    if (*p != L'"') return NULL;
    for (p++; *p && *p != L'"'; p++) {
        WCHAR ch = *p;
        if (ch == L'\\') {
            p++;
            switch (*p) {
            case L'"': ch = L'"'; break; case L'\\': ch = L'\\'; break; case L'/': ch = L'/'; break;
            case L'b': ch = L'\b'; break; case L'f': ch = L'\f'; break; case L'n': ch = L'\n'; break;
            case L'r': ch = L'\r'; break; case L't': ch = L'\t'; break;
            case L'u': {
                ch = 0;
                for (int i = 1; i <= 4; i++) {
                    WCHAR d = p[i]; int v;
                    if (d >= L'0' && d <= L'9') v = d - L'0';
                    else if (d >= L'a' && d <= L'f') v = d - L'a' + 10;
                    else if (d >= L'A' && d <= L'F') v = d - L'A' + 10;
                    else return NULL;
                    ch = (WCHAR)(ch * 16 + v);
                }
                p += 4; break; }
            default: return NULL;
            }
        } else if (ch < 0x20) return NULL;
        if (n + 1 >= cap) return NULL;
        out[n++] = ch;
    }
    if (*p != L'"') return NULL;
    out[n] = 0;
    return p + 1;
}

static BOOL jparse(const WCHAR *p, Json *j)
{
    j->n = 0;
    p = skipws(p);
    if (*p++ != L'{') return FALSE;
    p = skipws(p);
    if (*p == L'}') return *skipws(p + 1) == 0;
    for (;;) {
        if (j->n == 12) return FALSE;
        Kv *kv = &j->kv[j->n];
        p = skipws(p);
        if (!(p = jstring(p, kv->key, 32))) return FALSE;
        p = skipws(p);
        if (*p++ != L':') return FALSE;
        p = skipws(p);
        if (*p == L'"') { kv->is_str = TRUE; if (!(p = jstring(p, kv->val, MAX_PATHW))) return FALSE; }
        else {   // number, true, false, null: kept as text
            size_t n = 0; kv->is_str = FALSE;
            while (*p && *p != L',' && *p != L'}' && *p != L' ' && *p != L'\t' && *p != L'\r' && *p != L'\n') {
                if (*p == L'{' || *p == L'[' || n + 1 >= 32) return FALSE;
                kv->val[n++] = *p++;
            }
            kv->val[n] = 0;
            if (n == 0) return FALSE;
        }
        j->n++;
        p = skipws(p);
        if (*p == L',') { p++; continue; }
        if (*p == L'}') return *skipws(p + 1) == 0;
        return FALSE;
    }
}

// Appends a JSON string (quoted, escaped) for a wide string to a UTF-8 buffer.
static void jappend_str(char *buf, size_t cap, size_t *len, const WCHAR *s)
{
    char tmp[4 * MAX_PATHW];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, tmp, sizeof tmp, NULL, NULL);
    if (n <= 0) { tmp[0] = 0; }
    #define PUT(c) do { if (*len + 8 < cap) buf[(*len)++] = (c); } while (0)
    PUT('"');
    for (const unsigned char *q = (const unsigned char *)tmp; *q; q++) {
        if (*q == '"' || *q == '\\') { PUT('\\'); PUT((char)*q); }
        else if (*q < 0x20) { if (*len + 8 < cap) *len += snprintf(buf + *len, cap - *len, "\\u%04x", *q); }
        else PUT((char)*q);
    }
    PUT('"');
    #undef PUT
}

typedef struct { char buf[MAX_MSG / 4]; size_t len; } Msg;

static void mstart(Msg *m, const char *type)
{
    m->len = snprintf(m->buf, sizeof m->buf, "{\"type\":\"%s\"", type);
}
static void mint(Msg *m, const char *key, unsigned long long v)
{
    m->len += snprintf(m->buf + m->len, sizeof m->buf - m->len, ",\"%s\":%llu", key, v);
}
static void mbool(Msg *m, const char *key, BOOL v)
{
    m->len += snprintf(m->buf + m->len, sizeof m->buf - m->len, ",\"%s\":%s", key, v ? "true" : "false");
}
static void mstr(Msg *m, const char *key, const WCHAR *v)
{
    m->len += snprintf(m->buf + m->len, sizeof m->buf - m->len, ",\"%s\":", key);
    jappend_str(m->buf, sizeof m->buf, &m->len, v);
}
static void mstra(Msg *m, const char *key, const char *v)
{
    WCHAR w[512]; MultiByteToWideChar(CP_UTF8, 0, v, -1, w, 512); mstr(m, key, w);
}

static BOOL conn_send(Conn *c, Msg *m)
{
    if (m->len + 2 < sizeof m->buf) m->buf[m->len++] = '}';
    BOOL ok = FALSE;
    EnterCriticalSection(&c->wlock);
    if (!c->closed) {
        OVERLAPPED ov = {0}; ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        DWORD done = 0;
        if (WriteFile(c->pipe, m->buf, (DWORD)m->len, NULL, &ov) || GetLastError() == ERROR_IO_PENDING) {
            HANDLE w[2] = { ov.hEvent, g_stop };
            if (WaitForMultipleObjects(2, w, FALSE, 5000) == WAIT_OBJECT_0)
                ok = GetOverlappedResult(c->pipe, &ov, &done, FALSE);
            else CancelIoEx(c->pipe, &ov);
        }
        CloseHandle(ov.hEvent);
    }
    LeaveCriticalSection(&c->wlock);
    return ok;
}

static void send_event(Conn *c, const char *event, const WCHAR *file, UINT32 pid, const char *message)
{
    Msg m; mstart(&m, "event");
    mstra(&m, "event", event); mstr(&m, "file", file);
    if (pid) mint(&m, "reader_pid", pid);
    if (message) mstra(&m, "message", message);
    conn_send(c, &m);
}

static void send_reply(Conn *c, const WCHAR *id, const char *status, const char *code, const char *message,
    const WCHAR *path, int linked)
{
    Msg m; mstart(&m, "reply");
    if (id) { BOOL digits = id[0] != 0 && wcslen(id) < 20; for (const WCHAR *q = id; *q; q++) if (*q < L'0' || *q > L'9') digits = FALSE;
              if (digits) m.len += snprintf(m.buf + m.len, sizeof m.buf - m.len, ",\"id\":%ls", id); }
    mstra(&m, "status", status);
    if (code) mstra(&m, "code", code);
    if (message) mstra(&m, "message", message);
    if (path) mstr(&m, "path", path);
    if (linked >= 0) mbool(&m, "linked", linked);
    conn_send(c, &m);
}

// ---------------------------------------------------------------- names

static BOOL illegal_char(WCHAR ch) { return ch < 32 || wcschr(L"<>:\"/\\|?*", ch) != NULL; }

static BOOL reserved_name(const WCHAR *name)
{
    WCHAR base[16]; size_t n = 0;
    while (name[n] && name[n] != L'.' && n < 15) { base[n] = name[n]; n++; }
    if (name[n] && name[n] != L'.') return FALSE;   // longer than any reserved name
    base[n] = 0;
    static const WCHAR *fixed[] = { L"CON", L"PRN", L"AUX", L"NUL" };
    for (int i = 0; i < 4; i++) if (0 == _wcsicmp(base, fixed[i])) return TRUE;
    if (n == 4 && (0 == _wcsnicmp(base, L"COM", 3) || 0 == _wcsnicmp(base, L"LPT", 3)) && base[3] >= L'1' && base[3] <= L'9') return TRUE;
    return FALSE;
}

// The requested file name: one path component, usable as a Windows name.
static const char *check_file_name(const WCHAR *f)
{
    size_t n = wcslen(f);
    if (n == 0) return "file_name is empty";
    if (n > MAX_NAME) return "file_name is too long";
    if (0 == wcscmp(f, L".") || 0 == wcscmp(f, L"..")) return "file_name must not be . or ..";
    for (size_t i = 0; i < n; i++) if (illegal_char(f[i])) return "file_name contains a separator or a character that is invalid in Windows names";
    if (f[n - 1] == L'.' || f[n - 1] == L' ') return "file_name must not end with a dot or a space";
    if (reserved_name(f)) return "file_name is a reserved device name";
    return NULL;
}

// <pipe name without \\.\pipe\> + "-" + file name. Illegal characters in the
// pipe name become '_'; sub-paths (a backslash) are rejected.
static const char *build_volume_name(const WCHAR *pipe, const WCHAR *file, WCHAR *out)
{
    size_t pl = wcslen(PIPE_PREFIX);
    if (0 != _wcsnicmp(pipe, PIPE_PREFIX, pl)) return "pipe must start with \\\\.\\pipe\\";
    const WCHAR *rest = pipe + pl;
    size_t n = wcslen(rest);
    if (n == 0) return "pipe name is empty";
    if (n > MAX_PIPE) return "pipe name is too long";
    WCHAR mapped[MAX_PIPE + 1];
    for (size_t i = 0; i < n; i++) {
        if (rest[i] == L'\\') return "pipe names with sub-paths are not supported";
        mapped[i] = illegal_char(rest[i]) ? L'_' : rest[i];
    }
    mapped[n] = 0;
    while (n && (mapped[n - 1] == L'.' || mapped[n - 1] == L' ')) { mapped[--n] = L'_'; }  // cannot matter before the '-'
    const char *why = check_file_name(file);
    if (why) return why;
    if (n + 1 + wcslen(file) > MAX_NAME) return "pipe name plus file_name exceeds 255 characters";
    swprintf_s(out, MAX_NAME + 1, L"%ls-%ls", mapped, file);
    return NULL;
}

// ---------------------------------------------------------------- reading a pipe as the registered user

#define FORWARD_PROTOCOL 1
#define TAG_READ_REQUEST 72
#define TAG_READ_RESPONSE 73

static BOOL read_exact(HANDLE h, BYTE *p, DWORD n)
{
    while (n) {
        DWORD got = 0;
        if (!ReadFile(h, p, n, &got, NULL) || got == 0) return FALSE;
        p += got; n -= got;
    }
    return TRUE;
}

static void read_to_eof(HANDLE h, BYTE **pdata, SIZE_T *plen)
{
    SIZE_T cap = 4096, len = 0;
    BYTE *buf = (BYTE *)malloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; buf = (BYTE *)realloc(buf, cap); }
        DWORD got = 0;
        if (!ReadFile(h, buf + len, (DWORD)(cap - len), &got, NULL) || got == 0) break;
        len += got;
    }
    *pdata = buf; *plen = len;
}

// CreateFile on a pipe as the user whose token this is.
static HANDLE open_pipe_as(HANDLE token, const WCHAR *name, DWORD access, DWORD *err)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    if (!SetThreadToken(NULL, token)) { *err = GetLastError(); return h; }
    h = CreateFileW(name, access, 0, NULL, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, NULL);
    *err = GetLastError();
    SetThreadToken(NULL, NULL);
    return h;
}

static HANDLE open_pipe_retry(HANDLE token, const WCHAR *name, DWORD access, BOOL strict_missing)
{
    DWORD first = GetTickCount(), deadline = first + 10000, e = 0;
    for (;;) {
        HANDLE h = open_pipe_as(token, name, access, &e);
        if (h != INVALID_HANDLE_VALUE) return h;
        if (strict_missing && e == ERROR_FILE_NOT_FOUND && GetTickCount() - first >= 2000) break;
        if ((e == ERROR_PIPE_BUSY || e == ERROR_FILE_NOT_FOUND) && GetTickCount() < deadline) { Sleep(50); continue; }
        break;
    }
    logf_("  pipe open failed: %lu (%ls)", e, name);
    return INVALID_HANDLE_VALUE;
}

static NTSTATUS fetch_forwarded(HANDLE token, const WCHAR *info_pipe, UINT32 pid, UINT64 start, BYTE **pdata, SIZE_T *plen)
{
    HANDLE h = open_pipe_retry(token, info_pipe, GENERIC_READ | GENERIC_WRITE, TRUE);
    if (h == INVALID_HANDLE_VALUE) return STATUS_IO_DEVICE_ERROR;

    char body[128];
    int blen = snprintf(body, sizeof body, "{\"protocol\":%d,\"reader_pid\":%u,\"reader_start_time\":%llu}",
        FORWARD_PROTOCOL, pid, (unsigned long long)start);
    BYTE frame[5 + sizeof body];
    frame[0] = 0; frame[1] = 0; frame[2] = (BYTE)(blen >> 8); frame[3] = (BYTE)blen; frame[4] = TAG_READ_REQUEST;
    memcpy(frame + 5, body, blen);
    DWORD wrote = 0;
    NTSTATUS result = STATUS_ACCESS_DENIED;
    if (!WriteFile(h, frame, 5 + blen, &wrote, NULL)) {
        result = STATUS_IO_DEVICE_ERROR;
    } else {
        BYTE hdr[5];
        if (!read_exact(h, hdr, 5)) {
            logf_("  the server closed the reader-info pipe without answering");
        } else {
            DWORD rlen = ((DWORD)hdr[0] << 24) | ((DWORD)hdr[1] << 16) | ((DWORD)hdr[2] << 8) | hdr[3];
            char rbody[256] = {0};
            if (hdr[4] != TAG_READ_RESPONSE || rlen >= sizeof rbody || !read_exact(h, (BYTE *)rbody, rlen)) {
                result = STATUS_IO_DEVICE_ERROR;
            } else if (strstr(rbody, "\"ok\"") == NULL) {
                logf_("  the server denied the read");
            } else {
                read_to_eof(h, pdata, plen);
                result = STATUS_SUCCESS;
            }
        }
    }
    CloseHandle(h);
    return result;
}

static NTSTATUS fetch_direct(HANDLE token, const WCHAR *pipe, BYTE **pdata, SIZE_T *plen)
{
    HANDLE h = open_pipe_retry(token, pipe, GENERIC_READ, FALSE);
    if (h == INVALID_HANDLE_VALUE) return STATUS_IO_DEVICE_ERROR;
    BYTE *buf; SIZE_T len;
    read_to_eof(h, &buf, &len);
    CloseHandle(h);
    if (len >= sizeof(DENIED_PREFIX) - 1 && memcmp(buf, DENIED_PREFIX, sizeof(DENIED_PREFIX) - 1) == 0) {
        SecureZeroMemory(buf, len); free(buf);
        return STATUS_ACCESS_DENIED;
    }
    *pdata = buf; *plen = len;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------- the files the backend serves

// The content is fetched on the first Read of a handle, not at Open: programs that
// open the file and close it (or only stat it) never reach the data server. Open
// records who is asking (pid, creation time, and a process handle that keeps the pid
// from being reused) and keeps a reference on the registering connection; the first
// Read fetches under that user's token. `lock` makes concurrent first reads on one
// handle share one fetch; `result` is remembered so later reads repeat the outcome
// without asking the server again.
typedef struct {
    UINT64 id; BOOL root; BYTE *data; SIZE_T len; PSECURITY_DESCRIPTOR sd; SIZE_T sdsz;
    BOOL want_data;                       // opened with read access
    Conn *conn;                           // reference held while want_data and not yet fetched
    WCHAR fname[MAX_NAME + 1], pipe[MAX_PIPE + 1], info[MAX_PIPE + MAX_SUFFIX + 1];
    UINT32 pid; UINT64 start; HANDLE proc;
    SRWLOCK lock; BOOL done; NTSTATUS result;
} Ctx;

static PSECURITY_DESCRIPTOR g_root_sd; static SIZE_T g_root_sdsz;

static BOOL sddl_to_sd(const WCHAR *sddl, PSECURITY_DESCRIPTOR *sd, SIZE_T *sz)
{
    ULONG n = 0;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, sd, &n)) return FALSE;
    *sz = n; return TRUE;
}

static Entry *find_by_name(const WCHAR *name)   // g_lock held
{
    for (Entry *e = g_entries; e; e = e->next) if (0 == _wcsicmp(e->name, name)) return e;
    return NULL;
}

UINT64 pfs_time(void) { return g_now; }
void pfs_log(const char *fmt, ...)
{
    char msg[512];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    logf_("%s", msg);
}

static void fill_info(const Ctx *c, PfsInfo *i)
{
    i->dir = c->root;
    i->size = c->root ? 0 : c->len;
    i->id = c->id;
    i->time = g_now;
}

static NTSTATUS copy_sd(PSECURITY_DESCRIPTOR from, SIZE_T fromsz, PSECURITY_DESCRIPTOR sd, SIZE_T *sdsz)
{
    if (sdsz) {
        SIZE_T have = *sdsz;
        *sdsz = fromsz;
        if (sd) {
            if (have < fromsz) return STATUS_BUFFER_OVERFLOW;
            memcpy(sd, from, fromsz);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS pfs_stat(const WCHAR *name, BOOL *dir, PSECURITY_DESCRIPTOR sd, SIZE_T *sdsz)
{
    if (0 == wcscmp(name, L"\\")) {
        *dir = TRUE;
        return copy_sd(g_root_sd, g_root_sdsz, sd, sdsz);
    }
    if (name[0] != L'\\' || wcschr(name + 1, L'\\')) return STATUS_OBJECT_NAME_NOT_FOUND;
    NTSTATUS st = STATUS_OBJECT_NAME_NOT_FOUND;
    EnterCriticalSection(&g_lock);
    Entry *e = find_by_name(name + 1);
    if (e) {
        *dir = FALSE;
        st = copy_sd(e->sd, e->sdsz, sd, sdsz);
    }
    LeaveCriticalSection(&g_lock);
    return st;
}

// Fetches the content once per handle, on its first Read, and tells the registering
// client who read it. A failure is remembered and repeated without asking the server
// again: STATUS_ACCESS_DENIED when the reader was refused, STATUS_IO_DEVICE_ERROR when
// the server could not be used.
static NTSTATUS ensure_data(Ctx *c)
{
    if (!c->want_data) return STATUS_ACCESS_DENIED;
    AcquireSRWLockExclusive(&c->lock);
    if (!c->done) {
        logf_("FETCH %ls pid=%u", c->fname, c->pid);
        NTSTATUS st = c->info[0] ? fetch_forwarded(c->conn->token, c->info, c->pid, c->start, &c->data, &c->len)
                                 : fetch_direct(c->conn->token, c->pipe, &c->data, &c->len);
        if (NT_SUCCESS(st)) send_event(c->conn, "opened", c->fname, c->pid, NULL);
        else send_event(c->conn, st == STATUS_ACCESS_DENIED ? "denied" : "error", c->fname, c->pid, NULL);
        logf_("  -> 0x%08x, %zu bytes", (unsigned)st, c->len);
        c->result = st; c->done = TRUE;
        if (c->proc) { CloseHandle(c->proc); c->proc = NULL; }
    }
    NTSTATUS r = c->result;
    ReleaseSRWLockExclusive(&c->lock);
    return r;
}

NTSTATUS pfs_open(const WCHAR *name, UINT32 access, UINT32 pid, void **pfile, PfsInfo *info)
{
    Ctx *c = (Ctx *)calloc(1, sizeof *c);
    if (0 == wcscmp(name, L"\\")) { c->root = TRUE; c->id = 1; fill_info(c, info); *pfile = c; return STATUS_SUCCESS; }

    WCHAR fname[MAX_NAME + 1], pipe[MAX_PIPE + 1], infop[MAX_PIPE + MAX_SUFFIX + 1];
    Conn *conn = NULL;
    EnterCriticalSection(&g_lock);
    Entry *e = (name[0] == L'\\' && !wcschr(name + 1, L'\\')) ? find_by_name(name + 1) : NULL;
    if (e) {
        c->id = e->id;
        c->sd = malloc(e->sdsz); memcpy(c->sd, e->sd, e->sdsz); c->sdsz = e->sdsz;
        wcscpy_s(fname, MAX_NAME + 1, e->name); wcscpy_s(pipe, MAX_PIPE + 1, e->pipe);
        wcscpy_s(infop, MAX_PIPE + MAX_SUFFIX + 1, e->info_pipe);
        conn = e->conn; InterlockedIncrement(&conn->refs);
    }
    LeaveCriticalSection(&g_lock);
    if (!e) { free(c); return STATUS_OBJECT_NAME_NOT_FOUND; }

    // Opens that are not for reading data (stat, listings, AV probes) never fetch, and
    // neither do opens that close without a Read: the first Read fetches. Here we only
    // note who is asking.
    if (access & FILE_READ_DATA) {
        logf_("OPEN  %ls pid=%u", fname, pid);
        FILETIME created, exited, kernel, user;
        c->proc = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : NULL;
        if (infop[0] && (!c->proc || !GetProcessTimes(c->proc, &created, &exited, &kernel, &user))) {
            logf_("  cannot identify the calling process (pid %u); refusing", pid);
            if (c->proc) CloseHandle(c->proc);
            conn_unref(conn);
            free(c->sd); free(c);
            return STATUS_ACCESS_DENIED;
        }
        if (infop[0]) c->start = ((UINT64)created.dwHighDateTime << 32) | created.dwLowDateTime;
        c->want_data = TRUE; c->pid = pid; c->conn = conn;
        wcscpy_s(c->fname, MAX_NAME + 1, fname); wcscpy_s(c->pipe, MAX_PIPE + 1, pipe);
        wcscpy_s(c->info, MAX_PIPE + MAX_SUFFIX + 1, infop);
    } else {
        conn_unref(conn);
    }
    fill_info(c, info);
    *pfile = c;
    return STATUS_SUCCESS;
}

void pfs_close(void *file)
{
    Ctx *c = (Ctx *)file;
    if (c->data) { SecureZeroMemory(c->data, c->len); free(c->data); }
    if (c->proc) CloseHandle(c->proc);
    if (c->conn) conn_unref(c->conn);
    free(c->sd);
    free(c);
}

NTSTATUS pfs_read(void *file, UINT64 off, ULONG len, void *buf, ULONG *done)
{
    Ctx *c = (Ctx *)file;
    if (c->root) return STATUS_INVALID_DEVICE_REQUEST;
    NTSTATUS st = ensure_data(c);
    if (!NT_SUCCESS(st)) return st;
    if (off >= c->len) return STATUS_END_OF_FILE;
    SIZE_T n = c->len - (SIZE_T)off; if (n > len) n = len;
    memcpy(buf, c->data + off, n);
    *done = (ULONG)n;
    return STATUS_SUCCESS;
}

NTSTATUS pfs_get_security(void *file, PSECURITY_DESCRIPTOR sd, SIZE_T *sdsz)
{
    Ctx *c = (Ctx *)file;
    return c->root ? copy_sd(g_root_sd, g_root_sdsz, sd, sdsz) : copy_sd(c->sd, c->sdsz, sd, sdsz);
}

NTSTATUS pfs_get_info(void *file, PfsInfo *info)
{
    Ctx *c = (Ctx *)file;
    AcquireSRWLockShared(&c->lock);   // c->len changes when the first Read fetches
    fill_info(c, info);
    ReleaseSRWLockShared(&c->lock);
    return STATUS_SUCCESS;
}

void pfs_list(BOOL (*visit)(void *ctx, const WCHAR *name, UINT64 id), void *ctx)
{
    EnterCriticalSection(&g_lock);
    for (Entry *e = g_entries; e; e = e->next) if (!visit(ctx, e->name, e->id)) break;
    LeaveCriticalSection(&g_lock);
}

// ---------------------------------------------------------------- the placeholder-file link

#define IO_REPARSE_TAG_SYMLINK_ 0xA000000C

typedef struct {
    ULONG ReparseTag; USHORT ReparseDataLength, Reserved;
    USHORT SubstituteNameOffset, SubstituteNameLength, PrintNameOffset, PrintNameLength;
    ULONG Flags; WCHAR PathBuffer[1];
} SYMLINK_REPARSE;

static DWORD symlink_buffer(const WCHAR *target, BYTE *out, DWORD cap)
{
    WCHAR sub[MAX_PATHW + 8];
    swprintf_s(sub, MAX_PATHW + 8, L"\\??\\%ls", target);
    USHORT subb = (USHORT)(wcslen(sub) * 2), prb = (USHORT)(wcslen(target) * 2);
    DWORD total = 8 + 12 + subb + prb;
    if (total > cap) return 0;
    SYMLINK_REPARSE *r = (SYMLINK_REPARSE *)out;
    memset(out, 0, total);
    r->ReparseTag = IO_REPARSE_TAG_SYMLINK_;
    r->ReparseDataLength = (USHORT)(12 + subb + prb);
    r->SubstituteNameOffset = 0; r->SubstituteNameLength = subb;
    r->PrintNameOffset = subb; r->PrintNameLength = prb;
    memcpy(out + 20, sub, subb); memcpy(out + 20 + subb, target, prb);
    return total;
}

// Steps 1 to 3 of the placeholder protocol. Returns NULL on success (e->link is
// set) or a reason. Runs on the connection's thread, right after it read the request.
static const char *link_placeholder(Conn *c, Entry *e, const WCHAR *path, char *detail, size_t detail_cap)
{
    detail[0] = 0;
    if (wcslen(path) < 4 || path[1] != L':' || path[2] != L'\\') return "link_path must be an absolute path such as C:\\dir\\file";
    HANDLE h1;
    if (!ImpersonateNamedPipeClient(c->pipe)) return "cannot impersonate the caller";
    h1 = CreateFileW(path, READ_CONTROL | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    DWORD err = GetLastError();
    RevertToSelf();
    if (h1 == INVALID_HANDLE_VALUE) { snprintf(detail, detail_cap, "error %lu", err); return "the caller cannot open the placeholder for writing"; }

    const char *why = NULL;
    HANDLE h2 = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION bi;
    PSECURITY_DESCRIPTOR sd = NULL; PSID owner = NULL;
    if (!GetFileInformationByHandle(h1, &bi)) { why = "cannot query the placeholder"; goto done; }
    if (bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { why = "the placeholder is a directory"; goto done; }
    if (bi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { why = "the placeholder is already a link"; goto done; }
    if (bi.nFileSizeHigh || bi.nFileSizeLow) { why = "the placeholder is not empty"; goto done; }
    if (bi.nNumberOfLinks != 1) { why = "the placeholder has more than one hard link"; goto done; }
    if (GetSecurityInfo(h1, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, NULL, NULL, NULL, &sd) != ERROR_SUCCESS) {
        why = "cannot read the placeholder's owner"; goto done;
    }
    {
        BYTE adm[SECURITY_MAX_SID_SIZE]; DWORD n = sizeof adm;
        BOOL ok = EqualSid(owner, c->sid);
        if (!ok && c->elevated && CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, adm, &n) && EqualSid(owner, adm)) ok = TRUE;
        if (!ok) { why = "the placeholder is not owned by the caller"; goto done; }
    }
    {   // The privilege is checked against the identity that opened the handle, so
        // reopen the same file by ID as the service; compare identities.
        FILE_ID_DESCRIPTOR d; memset(&d, 0, sizeof d);
        d.dwSize = sizeof d; d.Type = FileIdType;
        d.FileId.QuadPart = ((LONGLONG)bi.nFileIndexHigh << 32) | bi.nFileIndexLow;
        h2 = OpenFileById(h1, &d, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS);
        if (h2 == INVALID_HANDLE_VALUE) { snprintf(detail, detail_cap, "error %lu", GetLastError()); why = "cannot reopen the placeholder"; goto done; }
        BY_HANDLE_FILE_INFORMATION b2;
        if (!GetFileInformationByHandle(h2, &b2) || b2.nFileIndexHigh != bi.nFileIndexHigh ||
            b2.nFileIndexLow != bi.nFileIndexLow || b2.dwVolumeSerialNumber != bi.dwVolumeSerialNumber ||
            b2.nFileSizeLow || b2.nFileSizeHigh) { why = "the placeholder changed while it was being checked"; goto done; }
        BYTE buf[4096]; DWORD n = symlink_buffer(e->link_target, buf, sizeof buf), ret;
        if (!n) { why = "link target is too long"; goto done; }
        if (!DeviceIoControl(h2, FSCTL_SET_REPARSE_POINT, buf, n, NULL, 0, &ret, NULL)) {
            snprintf(detail, detail_cap, "error %lu", GetLastError());
            why = "cannot make the link"; goto done;
        }
    }
    e->link = h2; h2 = INVALID_HANDLE_VALUE;
    wcscpy_s(e->link_path, MAX_PATHW, path);
done:
    if (h2 != INVALID_HANDLE_VALUE) CloseHandle(h2);
    if (sd) LocalFree(sd);
    CloseHandle(h1);
    return why;
}

// Step 4: remove the link if it is still a symlink to the target we set.
static void unlink_placeholder(Entry *e)
{
    if (!e->link) return;
    BYTE buf[17000]; DWORD ret = 0;
    if (DeviceIoControl(e->link, FSCTL_GET_REPARSE_POINT, NULL, 0, buf, sizeof buf, &ret, NULL)) {
        SYMLINK_REPARSE *r = (SYMLINK_REPARSE *)buf;
        WCHAR want[MAX_PATHW + 8]; swprintf_s(want, MAX_PATHW + 8, L"\\??\\%ls", e->link_target);
        size_t wl = wcslen(want) * 2;
        if (r->ReparseTag == IO_REPARSE_TAG_SYMLINK_ && r->SubstituteNameLength == wl &&
            0 == memcmp((BYTE *)r->PathBuffer + r->SubstituteNameOffset, want, wl)) {
            REPARSE_GUID_DATA_BUFFER hdr; memset(&hdr, 0, sizeof hdr);
            hdr.ReparseTag = IO_REPARSE_TAG_SYMLINK_;
            if (DeviceIoControl(e->link, FSCTL_DELETE_REPARSE_POINT, &hdr, REPARSE_GUID_DATA_BUFFER_HEADER_SIZE, NULL, 0, &ret, NULL))
                logf_("removed link %ls", e->link_path);
            else logf_("cannot remove link %ls: %lu", e->link_path, GetLastError());
        } else logf_("link %ls was changed by someone else; left alone", e->link_path);
    }
    CloseHandle(e->link); e->link = NULL;
}

// ---------------------------------------------------------------- registrations

static void remove_entry_locked(Entry *e)   // g_lock held
{
    for (Entry **pp = &g_entries; *pp; pp = &(*pp)->next)
        if (*pp == e) { *pp = e->next; break; }
    unlink_placeholder(e);
    if (e->sd) LocalFree(e->sd);
    logf_("unregistered %ls", e->name);
    free(e);
}

static BOOL probe_pipe(Conn *c, const WCHAR *name, DWORD access, DWORD *err)
{
    if (!ImpersonateNamedPipeClient(c->pipe)) { *err = GetLastError(); return FALSE; }
    HANDLE h = CreateFileW(name, access, 0, NULL, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, NULL);
    *err = GetLastError();
    RevertToSelf();
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return TRUE; }
    return *err == ERROR_PIPE_BUSY;   // exists, all instances busy
}

static void do_register(Conn *c, const Json *j, const WCHAR *id)
{
    const WCHAR *pipe = jget(j, L"pipe"), *file = jget(j, L"file_name");
    const WCHAR *suffix = jget(j, L"reader_info_suffix"), *link = jget(j, L"link_path");
    if (!pipe || !file) { send_reply(c, id, "error", "bad_request", "pipe and file_name are required", NULL, -1); return; }
    WCHAR name[MAX_NAME + 1];
    const char *why = build_volume_name(pipe, file, name);
    if (why) { send_reply(c, id, "error", "bad_name", why, NULL, -1); return; }
    // The driver can be removed while the service runs: existing registrations are left alone (they fail by themselves).
    if (!pfs_backend.installed()) {
        char m[128]; snprintf(m, sizeof m, "%ls is not installed; install it and try again", pfs_backend.driver_name);
        send_reply(c, id, "error", "driver_not_installed", m, NULL, -1); return;
    }
    if (suffix && (wcslen(suffix) == 0 || wcslen(suffix) > MAX_SUFFIX || wcschr(suffix, L'\\') || wcschr(suffix, L'/'))) {
        send_reply(c, id, "error", "bad_request", "reader_info_suffix must be 1 to 64 characters without a path separator", NULL, -1); return;
    }
    // The caller must be able to reach the pipe it registers (checked as the caller; only the pipe that will be read).
    Entry *e = (Entry *)calloc(1, sizeof *e);
    wcscpy_s(e->pipe, MAX_PIPE + 1, pipe);
    if (suffix) swprintf_s(e->info_pipe, MAX_PIPE + MAX_SUFFIX + 1, L"%ls%ls", pipe, suffix);
    DWORD err;
    if (!probe_pipe(c, suffix ? e->info_pipe : e->pipe, suffix ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ, &err)) {
        char m[128]; snprintf(m, sizeof m, "the caller cannot open the pipe (error %lu)", err);
        send_reply(c, id, "error", "pipe_unreachable", m, NULL, -1); free(e); return;
    }
    wcscpy_s(e->name, MAX_NAME + 1, name);
    e->conn = c;
    if (link) {
        if (g_mount[0] == 0) { send_reply(c, id, "error", "link_failed", "no mount", NULL, -1); free(e); return; }
        swprintf_s(e->link_target, MAX_PATHW, L"%ls\\%ls", g_mount, name);
    }
    WCHAR sddl[256], *sidstr = NULL;
    ConvertSidToStringSidW(c->sid, &sidstr);
    swprintf_s(sddl, 256, L"O:SYG:SYD:P(A;;FA;;;SY)(A;;FRFX;;;%ls)", sidstr);
    LocalFree(sidstr);
    if (!sddl_to_sd(sddl, &e->sd, &e->sdsz)) { send_reply(c, id, "error", "internal", "security descriptor", NULL, -1); free(e); return; }

    EnterCriticalSection(&g_lock);
    for (Entry *o = g_entries; o; o = o->next) {
        if (0 == _wcsicmp(o->pipe, pipe) || 0 == _wcsicmp(o->name, name)) {
            LeaveCriticalSection(&g_lock);
            send_reply(c, id, "error", "name_in_use", "that pipe or file name is already registered", NULL, -1);
            LocalFree(e->sd); free(e); return;
        }
    }
    int mine = 0, theirs = 0;   // this connection's registrations, and all of this user's
    for (Entry *o = g_entries; o; o = o->next) {
        if (o->conn == c) mine++;
        if (o->conn->sid && EqualSid(o->conn->sid, c->sid)) theirs++;
    }
    if (mine >= LIM(LIM_REG_CONN) || theirs >= LIM(LIM_REG_USER)) {
        LeaveCriticalSection(&g_lock);
        char m[96]; snprintf(m, sizeof m, "at most %d registrations per connection and %d per user", LIM(LIM_REG_CONN), LIM(LIM_REG_USER));
        send_reply(c, id, "error", "too_many_registrations", m, NULL, -1);
        LocalFree(e->sd); free(e); return;
    }
    if (link) {
        char detail[64]; const char *lw = link_placeholder(c, e, link, detail, sizeof detail);
        if (lw) {
            LeaveCriticalSection(&g_lock);
            char m[256]; snprintf(m, sizeof m, "%s%s%s", lw, detail[0] ? ": " : "", detail);
            send_reply(c, id, "error", "link_refused", m, NULL, -1);
            LocalFree(e->sd); free(e); return;
        }
    }
    e->id = g_next_id++;
    e->next = g_entries; g_entries = e;
    LeaveCriticalSection(&g_lock);
    WCHAR full[MAX_PATHW]; swprintf_s(full, MAX_PATHW, L"%ls\\%ls", g_mount, name);
    logf_("registered %ls -> %ls%ls", pipe, name, link ? L" (linked)" : L"");
    send_reply(c, id, "ok", NULL, NULL, full, link ? TRUE : FALSE);
}

static void do_unregister(Conn *c, const Json *j, const WCHAR *id)
{
    const WCHAR *pipe = jget(j, L"pipe");
    if (!pipe) { send_reply(c, id, "error", "bad_request", "pipe is required", NULL, -1); return; }
    BOOL found = FALSE;
    EnterCriticalSection(&g_lock);
    for (Entry *e = g_entries; e; e = e->next)
        if (e->conn == c && 0 == _wcsicmp(e->pipe, pipe)) { remove_entry_locked(e); found = TRUE; break; }
    LeaveCriticalSection(&g_lock);
    if (found) send_reply(c, id, "ok", NULL, NULL, NULL, -1);
    else send_reply(c, id, "error", "not_found", "you have no such registration", NULL, -1);
}

// ---------------------------------------------------------------- control pipe

static void drop_conn(Conn *c)
{
    EnterCriticalSection(&g_lock);
    for (Entry *e = g_entries; e;) { Entry *next = e->next; if (e->conn == c) remove_entry_locked(e); e = next; }
    for (Conn **pp = &g_conns; *pp; pp = &(*pp)->next) if (*pp == c) { *pp = c->next; break; }
    LeaveCriticalSection(&g_lock);
    EnterCriticalSection(&c->wlock);
    c->closed = TRUE;
    LeaveCriticalSection(&c->wlock);
}

static BOOL init_conn(Conn *c);

static DWORD WINAPI conn_thread(LPVOID arg)
{
    Conn *c = (Conn *)arg;
    WCHAR *wbuf = (WCHAR *)malloc(MAX_MSG * sizeof(WCHAR));
    char *buf = (char *)malloc(MAX_MSG);
    Json *j = (Json *)malloc(sizeof *j);
    OVERLAPPED ov = {0}; ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (;;) {
        ResetEvent(ov.hEvent);
        DWORD got = 0;
        BOOL ok = ReadFile(c->pipe, buf, MAX_MSG, NULL, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            HANDLE w[2] = { ov.hEvent, g_stop };
            if (WaitForMultipleObjects(2, w, FALSE, INFINITE) != WAIT_OBJECT_0) { CancelIoEx(c->pipe, &ov); break; }
            ok = GetOverlappedResult(c->pipe, &ov, &got, FALSE);
        } else if (ok) GetOverlappedResult(c->pipe, &ov, &got, FALSE);
        if (!ok) { if (GetLastError() == ERROR_MORE_DATA) logf_("client sent an oversized message; dropping it"); break; }
        if (!c->token) {   // the client's identity can only be taken after the first read
            if (!init_conn(c)) { logf_("rejected a client (no usable identity)"); break; }
            WCHAR *sid = NULL; ConvertSidToStringSidW(c->sid, &sid);
            EnterCriticalSection(&g_lock);
            int same = 0;
            for (Conn *o = g_conns; o; o = o->next) if (o->sid && EqualSid(o->sid, c->sid)) same++;
            LeaveCriticalSection(&g_lock);
            if (same > LIM(LIM_CONNS_USER)) { logf_("rejected a client: %ls already has %d connections", sid, LIM(LIM_CONNS_USER)); LocalFree(sid); break; }
            logf_("client connected: %ls%s", sid, c->elevated ? " (elevated)" : ""); LocalFree(sid);
        }
        int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf, (int)got, wbuf, MAX_MSG - 1);
        if (wn <= 0) { send_reply(c, NULL, "error", "bad_request", "message is not valid UTF-8", NULL, -1); continue; }
        wbuf[wn] = 0;
        if (!jparse(wbuf, j)) { send_reply(c, NULL, "error", "bad_request", "message is not a flat JSON object", NULL, -1); continue; }
        const WCHAR *id = jget(j, L"id"), *type = jget(j, L"type");
        if (!type) send_reply(c, id, "error", "bad_request", "type is required", NULL, -1);
        else if (0 == wcscmp(type, L"register")) do_register(c, j, id);
        else if (0 == wcscmp(type, L"unregister")) do_unregister(c, j, id);
        else send_reply(c, id, "error", "bad_request", "unknown type", NULL, -1);
    }
    CloseHandle(ov.hEvent);
    drop_conn(c);
    logf_("client gone");
    free(wbuf); free(buf); free(j);
    conn_unref(c);
    return 0;
}

// Takes the impersonation token and identity of the client that just connected.
static BOOL init_conn(Conn *c)
{
    if (!ImpersonateNamedPipeClient(c->pipe)) return FALSE;
    HANDLE t = NULL, dup = NULL;
    BOOL ok = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE, TRUE, &t);
    RevertToSelf();
    if (!ok) return FALSE;
    BYTE buf[SECURITY_MAX_SID_SIZE + 64]; DWORD n = 0;
    SECURITY_IMPERSONATION_LEVEL lvl;
    TOKEN_ELEVATION el;
    ok = GetTokenInformation(t, TokenImpersonationLevel, &lvl, sizeof lvl, &n) && lvl >= SecurityImpersonation
        && GetTokenInformation(t, TokenUser, buf, sizeof buf, &n)
        && DuplicateTokenEx(t, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenImpersonation, &dup);
    if (ok) {
        TOKEN_USER *u = (TOKEN_USER *)buf;
        DWORD sl = GetLengthSid(u->User.Sid);
        PSID mine = (PSID)malloc(sl); CopySid(sl, mine, u->User.Sid);
        EnterCriticalSection(&g_lock); c->sid = mine; LeaveCriticalSection(&g_lock);   // other connections read it under the lock
        c->elevated = GetTokenInformation(t, TokenElevation, &el, sizeof el, &n) && el.TokenIsElevated;
        c->token = dup;
    }
    CloseHandle(t);
    return ok;
}

static HANDLE new_pipe_instance(BOOL first)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, FALSE };
    // SYSTEM and Administrators: everything; interactive users: connect and talk.
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL))
        return INVALID_HANDLE_VALUE;
    HANDLE h = CreateNamedPipeW(CONTROL_PIPE,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, MAX_MSG / 4, MAX_MSG, 0, &sa);
    LocalFree(sa.lpSecurityDescriptor);
    return h;
}

static DWORD WINAPI accept_thread(LPVOID arg)
{
    (void)arg;
    BOOL first = TRUE;
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    while (WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0) {
        HANDLE p = new_pipe_instance(first);
        if (p == INVALID_HANDLE_VALUE) { logf_("cannot create the control pipe: %lu", GetLastError()); SetEvent(g_stop); break; }
        first = FALSE;
        OVERLAPPED ov = {0}; ov.hEvent = ev; ResetEvent(ev);
        BOOL ok = ConnectNamedPipe(p, &ov);
        DWORD e = GetLastError();
        if (!ok && e == ERROR_IO_PENDING) {
            HANDLE w[2] = { ev, g_stop };
            if (WaitForMultipleObjects(2, w, FALSE, INFINITE) != WAIT_OBJECT_0) { CancelIoEx(p, &ov); CloseHandle(p); break; }
            DWORD x; ok = GetOverlappedResult(p, &ov, &x, FALSE);
        } else if (!ok && e == ERROR_PIPE_CONNECTED) ok = TRUE;
        if (!ok) { CloseHandle(p); continue; }
        EnterCriticalSection(&g_lock);
        int open_conns = 0;
        for (Conn *o = g_conns; o; o = o->next) open_conns++;
        LeaveCriticalSection(&g_lock);
        if (open_conns >= LIM(LIM_CONNS)) {   // no thread, no buffers: just hang up
            static DWORD last_log;
            if (GetTickCount() - last_log > 5000) { last_log = GetTickCount(); logf_("too many control connections (%d); dropping new ones", LIM(LIM_CONNS)); }
            DisconnectNamedPipe(p); CloseHandle(p);
            continue;
        }
        Conn *c = (Conn *)calloc(1, sizeof *c);
        c->pipe = p; c->refs = 2;   // one for the thread, one for the list owner (released below)
        InitializeCriticalSection(&c->wlock);
        EnterCriticalSection(&g_lock);
        c->next = g_conns; g_conns = c;
        LeaveCriticalSection(&g_lock);
        c->thread = CreateThread(NULL, 0, conn_thread, c, 0, NULL);
        CloseHandle(c->thread);
        conn_unref(c);
    }
    CloseHandle(ev);
    return 0;
}
// ---------------------------------------------------------------- serving

static BOOL enable_privilege(const WCHAR *name)
{
    HANDLE t; LUID luid; TOKEN_PRIVILEGES tp;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) return FALSE;
    BOOL ok = LookupPrivilegeValueW(NULL, name, &luid);
    if (ok) {
        tp.PrivilegeCount = 1; tp.Privileges[0].Luid = luid; tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        ok = AdjustTokenPrivileges(t, FALSE, &tp, 0, NULL, NULL) && GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(t);
    return ok;
}

static int serve(const WCHAR *mount)
{
    if (!pfs_backend.installed()) {
        // Normal exit, no control pipe, nothing polling: start the service again once the driver is installed.
        logf_("%ls is not installed (get it from %s); exiting without opening the control pipe. Start the service again after installing it.",
            pfs_backend.driver_name, pfs_backend.driver_url);
        return 0;
    }
    InitializeCriticalSection(&g_lock);
    wcscpy_s(g_mount, MAX_PATHW, mount);
    size_t ml = wcslen(g_mount);
    while (ml > 3 && g_mount[ml - 1] == L'\\') g_mount[--ml] = 0;
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    g_now = ((UINT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    if (!enable_privilege(L"SeCreateSymbolicLinkPrivilege")) logf_("warning: SeCreateSymbolicLinkPrivilege is not available; links will fail");
    if (!sddl_to_sd(L"O:SYG:SYD:P(A;;FA;;;SY)(A;;FRFX;;;AU)", &g_root_sd, &g_root_sdsz)) { logf_("cannot build the root security descriptor"); return 12; }

    int rc = pfs_backend.start(g_mount);
    if (rc) return rc;

    HANDLE acc = CreateThread(NULL, 0, accept_thread, NULL, 0, NULL);
    logf_("serving on %ls; control pipe %ls", g_mount, CONTROL_PIPE);
    logf_("limits: %d registrations per connection, %d per user; %d connections, %d per user",
        LIM(LIM_REG_CONN), LIM(LIM_REG_USER), LIM(LIM_CONNS), LIM(LIM_CONNS_USER));
    WaitForSingleObject(g_stop, INFINITE);

    WaitForSingleObject(acc, 5000); CloseHandle(acc);
    for (int i = 0; i < 100; i++) {   // let connection threads remove their registrations
        EnterCriticalSection(&g_lock);
        BOOL none = g_conns == NULL;
        LeaveCriticalSection(&g_lock);
        if (none) break;
        Sleep(50);
    }
    EnterCriticalSection(&g_lock);
    while (g_entries) remove_entry_locked(g_entries);
    LeaveCriticalSection(&g_lock);
    pfs_backend.stop();
    logf_("stopped");
    return 0;
}


// ---------------------------------------------------------------- service plumbing

static SERVICE_STATUS_HANDLE g_ssh;
static SERVICE_STATUS g_status = { SERVICE_WIN32_OWN_PROCESS, SERVICE_START_PENDING, 0, 0, 0, 0, 3000 };

static void set_state(DWORD s, DWORD accept)
{
    g_status.dwCurrentState = s; g_status.dwControlsAccepted = accept; g_status.dwCheckPoint++;
    SetServiceStatus(g_ssh, &g_status);
}

static DWORD WINAPI svc_ctrl(DWORD ctl, DWORD type, LPVOID data, LPVOID ctx)
{
    (void)type; (void)data; (void)ctx;
    if (ctl == SERVICE_CONTROL_STOP || ctl == SERVICE_CONTROL_SHUTDOWN) {
        set_state(SERVICE_STOP_PENDING, 0);
        SetEvent(g_stop);
    }
    return NO_ERROR;
}

static void read_mount_param(WCHAR *out, DWORD cap)
{
    DWORD sz = cap * sizeof(WCHAR);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, PARAM_KEY, L"MountPoint", RRF_RT_REG_SZ, NULL, out, &sz) != ERROR_SUCCESS)
        wcscpy_s(out, cap, DEFAULT_MOUNT);
}

// Service mode: limits come from the Parameters key; a missing value keeps the default, a bad one is ignored.
static void read_limit_params(void)
{
    for (int i = 0; i < LIM_COUNT; i++) {
        DWORD v = 0, sz = sizeof v;
        LONG r = RegGetValueW(HKEY_LOCAL_MACHINE, PARAM_KEY, g_limits[i].value, RRF_RT_REG_DWORD, NULL, &v, &sz);
        if (r == ERROR_FILE_NOT_FOUND) continue;
        if (r != ERROR_SUCCESS || v < 1 || v > (DWORD)g_limits[i].hi) {
            logf_("ignoring %ls: it must be a number from 1 to %d; using %d", g_limits[i].value, g_limits[i].hi, g_limits[i].def);
            continue;
        }
        g_limits[i].cur = (int)v;
    }
}

static void WINAPI svc_main(DWORD argc, LPWSTR *argv)
{
    (void)argc; (void)argv;
    g_ssh = RegisterServiceCtrlHandlerExW(SVC_NAME, svc_ctrl, NULL);
    if (!g_ssh) return;
    CreateDirectoryW(L"C:\\ProgramData\\PipeFileService", NULL);
    g_log = _wfopen(L"C:\\ProgramData\\PipeFileService\\service.log", L"a");
    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    WCHAR mount[MAX_PATHW]; read_mount_param(mount, MAX_PATHW);
    read_limit_params();
    set_state(SERVICE_RUNNING, SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
    int rc = serve(mount);
    if (!rc && !g_log) rc = 0;
    g_status.dwWin32ExitCode = rc ? ERROR_SERVICE_SPECIFIC_ERROR : 0;
    g_status.dwServiceSpecificExitCode = rc;
    set_state(SERVICE_STOPPED, 0);
}

static BOOL WINAPI on_ctrl(DWORD t) { (void)t; SetEvent(g_stop); return TRUE; }

// Releases before 0.6.0 were called FspPipeFileService. An upgrade must not leave two services (and two
// volumes) behind, so install removes the old registration, its data folder and, when the old exe has the
// old file name, that exe and its folder (the folder only if nothing else is in it).
#define OLD_SVC_NAME L"FspPipeFileService"
static void remove_legacy_service(SC_HANDLE scm)
{
    SC_HANDLE s = OpenServiceW(scm, OLD_SVC_NAME, SERVICE_ALL_ACCESS);
    WCHAR oldexe[MAX_PATH] = L"";
    if (s) {
        DWORD need = 0;
        QueryServiceConfigW(s, NULL, 0, &need);
        QUERY_SERVICE_CONFIGW *cfg = need ? (QUERY_SERVICE_CONFIGW *)LocalAlloc(LPTR, need) : NULL;
        if (cfg && QueryServiceConfigW(s, cfg, need, &need) && cfg->lpBinaryPathName && cfg->lpBinaryPathName[0] == L'"') {
            const WCHAR *b = cfg->lpBinaryPathName + 1, *e = wcschr(b, L'"');
            if (e && (size_t)(e - b) < MAX_PATH) wcsncpy_s(oldexe, MAX_PATH, b, (size_t)(e - b));
        }
        if (cfg) LocalFree(cfg);
        SERVICE_STATUS st;
        ControlService(s, SERVICE_CONTROL_STOP, &st);
        for (int i = 0; i < 100 && QueryServiceStatus(s, &st) && st.dwCurrentState != SERVICE_STOPPED; i++) Sleep(100);
        if (DeleteService(s)) wprintf(L"removed the old service %ls\n", OLD_SVC_NAME);
        else fprintf(stderr, "warning: could not remove the old service %ls: %lu\n", OLD_SVC_NAME, GetLastError());
        CloseServiceHandle(s);
    }
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\" OLD_SVC_NAME L"\\Parameters");
    DeleteFileW(L"C:\\ProgramData\\" OLD_SVC_NAME L"\\service.log");
    RemoveDirectoryW(L"C:\\ProgramData\\" OLD_SVC_NAME L"\\mount");   // an empty leftover mount folder
    RemoveDirectoryW(L"C:\\ProgramData\\" OLD_SVC_NAME);               // only if nothing else is in it
    size_t n = wcslen(oldexe), tail = wcslen(L"\\" OLD_SVC_NAME L".exe");
    if (n > tail && 0 == _wcsicmp(oldexe + n - tail, L"\\" OLD_SVC_NAME L".exe")) {
        for (int i = 0; i < 20 && !DeleteFileW(oldexe) && GetFileAttributesW(oldexe) != INVALID_FILE_ATTRIBUTES; i++) Sleep(100);
        oldexe[n - tail] = 0;
        RemoveDirectoryW(oldexe);
    }
}

static int install(const WCHAR *mount, BOOL auto_start)
{
    WCHAR exe[MAX_PATH], cmd[MAX_PATH + 16];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf_s(cmd, MAX_PATH + 16, L"\"%ls\" service", exe);
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) { fprintf(stderr, "cannot open the service manager (run elevated): %lu\n", GetLastError()); return 1; }
    remove_legacy_service(scm);
    SC_HANDLE s = CreateServiceW(scm, SVC_NAME, SVC_DISPLAY, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        auto_start ? SERVICE_AUTO_START : SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, cmd, NULL, NULL, NULL, NULL, NULL);
    if (!s) { fprintf(stderr, "CreateService failed: %lu\n", GetLastError()); CloseServiceHandle(scm); return 1; }
    WCHAR desc[160];
    swprintf_s(desc, 160, L"Shows named pipes as read-only files on a %ls volume and links placeholder files to them.", pfs_backend.driver_name);
    SERVICE_DESCRIPTIONW d = { desc };
    ChangeServiceConfig2W(s, SERVICE_CONFIG_DESCRIPTION, &d);
    // A restricted service SID makes the token write-restricted: the service then cannot create its
    // folder or reopen a user's placeholder (checked live), so use a plain, unrestricted service SID.
    SERVICE_SID_INFO sid = { SERVICE_SID_TYPE_UNRESTRICTED };
    if (!ChangeServiceConfig2W(s, SERVICE_CONFIG_SERVICE_SID_INFO, &sid)) fprintf(stderr, "warning: service SID not set: %lu\n", GetLastError());
    static WCHAR privs[] = L"SeCreateSymbolicLinkPrivilege\0SeImpersonatePrivilege\0SeChangeNotifyPrivilege\0";
    SERVICE_REQUIRED_PRIVILEGES_INFOW rp = { privs };
    if (!ChangeServiceConfig2W(s, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, &rp)) fprintf(stderr, "warning: required privileges not set: %lu\n", GetLastError());
    // No network: the service needs no network access, so say so to the firewall.
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, PARAM_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"MountPoint", 0, REG_SZ, (const BYTE *)mount, (DWORD)((wcslen(mount) + 1) * sizeof(WCHAR)));
        for (int i = 0; i < LIM_COUNT; i++)
            if (g_limits[i].set) { DWORD v = (DWORD)g_limits[i].cur; RegSetValueExW(k, g_limits[i].value, 0, REG_DWORD, (const BYTE *)&v, sizeof v); }
        RegCloseKey(k);
    }
    CloseServiceHandle(s); CloseServiceHandle(scm);
    wprintf(L"installed %ls (mount %ls); start it with: Start-Service %ls\n", SVC_NAME, mount, SVC_NAME);
    return 0;
}

static int uninstall(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) { fprintf(stderr, "cannot open the service manager (run elevated): %lu\n", GetLastError()); return 1; }
    SC_HANDLE s = OpenServiceW(scm, SVC_NAME, SERVICE_ALL_ACCESS);
    if (!s) { fprintf(stderr, "service %ls is not installed\n", SVC_NAME); CloseServiceHandle(scm); return 1; }
    SERVICE_STATUS st;
    ControlService(s, SERVICE_CONTROL_STOP, &st);
    for (int i = 0; i < 100 && QueryServiceStatus(s, &st) && st.dwCurrentState != SERVICE_STOPPED; i++) Sleep(100);
    BOOL ok = DeleteService(s);
    CloseServiceHandle(s); CloseServiceHandle(scm);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\" SVC_NAME L"\\Parameters");
    DeleteFileW(L"C:\\ProgramData\\PipeFileService\\service.log");
    RemoveDirectoryW(L"C:\\ProgramData\\PipeFileService");   // only if nothing else is in it
    if (!ok) { fprintf(stderr, "DeleteService failed: %lu\n", GetLastError()); return 1; }
    wprintf(L"removed %ls\n", SVC_NAME);
    return 0;
}

static int usage(void)
{
    WCHAR syn[160] = L"";
    if (pfs_backend.run_option_synopsis) swprintf_s(syn, 160, L" [%ls]", pfs_backend.run_option_synopsis);
    fwprintf(stderr,
        L"usage: PipeFileService <command>\n"
        L"  install [--mount <folder that does not exist, or a drive letter>] [--auto-start] [limits]   (elevated)\n"
        L"  uninstall                                                           (elevated)\n"
        L"  run [--mount <...>]%ls [limits]   run in this console (elevated), for development;\n"
        L"  limits on the control pipe (defaults in brackets; install keeps them for the service):\n"
        L"      --max-reg-per-conn <n> [64]   --max-reg-per-user <n> [256]\n"
        L"      --max-conn <n> [128]          --max-conn-per-user <n> [16]\n"
        L"      %ls\n"
        L"  --version\n"
        L"default mount: %ls\n",
        syn, pfs_backend.run_option_help ? pfs_backend.run_option_help : L"", DEFAULT_MOUNT);
    return 2;
}

// `--max-... <n>`: sets the limit and returns TRUE when the flag is one of ours; exits with a message on a bad number.
static BOOL parse_limit_arg(const wchar_t *flag, const wchar_t *text)
{
    for (int i = 0; i < LIM_COUNT; i++) {
        if (0 != wcscmp(flag, g_limits[i].flag)) continue;
        wchar_t *end = NULL; long v = wcstol(text, &end, 10);
        if (text[0] == 0 || *end || v < 1 || v > g_limits[i].hi) {
            fwprintf(stderr, L"%ls must be a number from 1 to %d\n", flag, g_limits[i].hi);
            exit(2);
        }
        g_limits[i].cur = (int)v; g_limits[i].set = TRUE;
        return TRUE;
    }
    return FALSE;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc == 2 && (0 == wcscmp(argv[1], L"--version") || 0 == wcscmp(argv[1], L"-V"))) {
        printf("PipeFileService %s\n", SERVICE_VERSION);
        return 0;
    }
    if (argc < 2) return usage();
    const WCHAR *mount = DEFAULT_MOUNT;
    BOOL auto_start = FALSE;
    BOOL is_install = 0 == wcscmp(argv[1], L"install"), is_run = 0 == wcscmp(argv[1], L"run");
    if (0 == wcscmp(argv[1], L"uninstall") || 0 == wcscmp(argv[1], L"service")) { if (argc != 2) return usage(); }
    else for (int i = 2; i < argc; i++) {
        if (0 == wcscmp(argv[i], L"--mount") && i + 1 < argc && argv[i + 1][0]) mount = argv[++i];
        else if (is_install && 0 == wcscmp(argv[i], L"--auto-start")) auto_start = TRUE;
        else if (is_run && i + 1 < argc && pfs_backend.parse_run_option && pfs_backend.parse_run_option(argv[i], argv[i + 1])) i++;   // never read in service mode
        else if ((is_install || is_run) && i + 1 < argc && parse_limit_arg(argv[i], argv[i + 1])) i++;
        else return usage();
    }
    if (is_install) return install(mount, auto_start);
    if (0 == wcscmp(argv[1], L"uninstall") && argc == 2) return uninstall();
    if (is_run) {
        g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
        SetConsoleCtrlHandler(on_ctrl, TRUE);
        return serve(mount);
    }
    if (0 == wcscmp(argv[1], L"service") && argc == 2) {
        SERVICE_TABLE_ENTRYW table[] = { { (LPWSTR)SVC_NAME, svc_main }, { NULL, NULL } };
        if (!StartServiceCtrlDispatcherW(table)) { fprintf(stderr, "this command is for the service manager\n"); return 1; }
        return 0;
    }
    return usage();
}
