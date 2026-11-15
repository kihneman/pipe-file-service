// The ProjFS backend of PipeFileService: projects the core's flat read-only directory into a real folder with the
// Windows Projected File System (the optional Windows feature Client-ProjFS; projectedfslib.dll and prjflt.sys are
// Microsoft's, there is no third-party driver).
//
// ProjFS is not a volume, so this backend cannot keep every guarantee of the WinFsp and Dokan ones:
//  - A file read through ProjFS is WRITTEN to the folder (hydrated) as a normal NTFS file, so the content is on disk
//    until it is removed. The backend removes the file again when the handle that read it closes ("dehydrate on
//    close"), but a reader that opens the file while it is hydrated gets the data without a callback, so the data
//    server is not asked about that read, and a crash of the service leaves the hydrated file behind (it is deleted
//    at the next start).
//  - A projected file takes the access control of the folder, not its own: the folder allows SYSTEM full control and
//    authenticated users read. The per-file security descriptor is enforced only when ProjFS calls the provider
//    (placeholder and data requests), against the token of the process that triggered the call.
//  - The file size must be known before the first read, so registrations must declare a `size` (requires_size).
//
// MIT licensed. Uses the Windows Projected File System API (projectedfslib.h); the library is delay-loaded.
#include "pfs_backend.h"
#include <sddl.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <projectedfslib.h>

#pragma comment(lib, "ole32.lib")   // CoCreateGuid
#define MAX_PATHW 1024
#define CHUNK (1u << 20)   // bytes written to ProjFS per call (a multiple of any sector size)

static WCHAR g_root[MAX_PATHW];
static PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT g_ctx;

static HRESULT hr_from_status(NTSTATUS st)
{
    switch (st) {
    case STATUS_ACCESS_DENIED: return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    case STATUS_OBJECT_NAME_NOT_FOUND: return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    case STATUS_INSUFFICIENT_RESOURCES: return E_OUTOFMEMORY;
    default: return HRESULT_FROM_WIN32(ERROR_IO_DEVICE);
    }
}

static void file_time(UINT64 t, LARGE_INTEGER *li) { li->QuadPart = (LONGLONG)t; }

// "name" (ProjFS path, no leading backslash) -> "\name" (the core's path). FALSE for anything but a top-level name.
static BOOL core_path(const WCHAR *p, WCHAR *out, size_t cap)
{
    if (!p || !p[0] || wcschr(p, L'\\')) return FALSE;
    return swprintf_s(out, cap, L"\\%ls", p) > 0;
}

// The registering user's permission, checked against the token of the process that triggered the callback. The
// callbacks carry no token, only a pid; a process that has exited is refused.
static BOOL pid_may(UINT32 pid, const WCHAR *cpath, ACCESS_MASK want)
{
    BOOL dir; SIZE_T sdsz = 0;
    if (!NT_SUCCESS(pfs_stat(cpath, &dir, NULL, &sdsz))) return FALSE;
    PSECURITY_DESCRIPTOR sd = malloc(sdsz);
    if (!sd) return FALSE;
    if (!NT_SUCCESS(pfs_stat(cpath, &dir, sd, &sdsz))) { free(sd); return FALSE; }
    BOOL ok = FALSE;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid), tok = NULL, imp = NULL;
    if (proc && OpenProcessToken(proc, TOKEN_QUERY | TOKEN_DUPLICATE, &tok) && DuplicateToken(tok, SecurityImpersonation, &imp)) {
        GENERIC_MAPPING gm = { FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
        PRIVILEGE_SET ps; DWORD psl = sizeof ps, got = 0; BOOL granted = FALSE;
        MapGenericMask(&want, &gm);
        ok = AccessCheck(sd, imp, want, &gm, &ps, &psl, &got, &granted) && granted;
    }
    if (imp) CloseHandle(imp);
    if (tok) CloseHandle(tok);
    if (proc) CloseHandle(proc);
    free(sd);
    return ok;
}

// ---------------------------------------------------------------- directory enumeration

typedef struct { WCHAR name[MAX_NAME + 1]; UINT64 size; UINT64 id; } Item;
typedef struct Enum {
    struct Enum *next; GUID id; Item *items; size_t n, pos; BOOL has_search; WCHAR search[MAX_NAME + 1];
} Enum;
static CRITICAL_SECTION g_enum_lock;
static Enum *g_enums;

typedef struct { Item *items; size_t n, cap; } Gather;

static BOOL gather_one(void *ctx, const WCHAR *leaf, UINT64 id)
{
    Gather *g = (Gather *)ctx;
    if (g->n == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 16;
        g->items = (Item *)realloc(g->items, g->cap * sizeof(Item));
        if (!g->items) return FALSE;
    }
    Item *it = &g->items[g->n++];
    wcscpy_s(it->name, MAX_NAME + 1, leaf);
    it->id = id; it->size = 0;
    WCHAR p[MAX_NAME + 2]; swprintf_s(p, MAX_NAME + 2, L"\\%ls", leaf);
    void *f; PfsInfo info;
    if (NT_SUCCESS(pfs_open(p, FILE_READ_ATTRIBUTES, 0, &f, &info))) { it->size = info.size; pfs_close(f); }
    return TRUE;
}

static int __cdecl cmp_items(const void *a, const void *b) { return PrjFileNameCompare(((const Item *)a)->name, ((const Item *)b)->name); }

static HRESULT CALLBACK StartEnum(const PRJ_CALLBACK_DATA *d, const GUID *id)
{
    Enum *e = (Enum *)calloc(1, sizeof *e);
    if (!e) return E_OUTOFMEMORY;
    e->id = *id;
    if (d->FilePathName[0] == 0) {   // only the root has entries
        Gather g = {0};
        pfs_list(gather_one, &g);
        if (g.n) qsort(g.items, g.n, sizeof(Item), cmp_items);
        e->items = g.items; e->n = g.n;
    }
    EnterCriticalSection(&g_enum_lock);
    e->next = g_enums; g_enums = e;
    LeaveCriticalSection(&g_enum_lock);
    return S_OK;
}

static HRESULT CALLBACK EndEnum(const PRJ_CALLBACK_DATA *d, const GUID *id)
{
    (void)d;
    EnterCriticalSection(&g_enum_lock);
    for (Enum **pp = &g_enums; *pp; pp = &(*pp)->next) {
        if (IsEqualGUID(&(*pp)->id, id)) { Enum *e = *pp; *pp = e->next; free(e->items); free(e); break; }
    }
    LeaveCriticalSection(&g_enum_lock);
    return S_OK;
}

static void basic_info(PRJ_FILE_BASIC_INFO *b, BOOL dir, UINT64 size, UINT64 time)
{
    ZeroMemory(b, sizeof *b);
    b->IsDirectory = dir ? TRUE : FALSE;
    b->FileSize = dir ? 0 : (INT64)size;
    b->FileAttributes = dir ? FILE_ATTRIBUTE_DIRECTORY : (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_NORMAL);
    file_time(time, &b->CreationTime); b->LastAccessTime = b->LastWriteTime = b->ChangeTime = b->CreationTime;
}

static HRESULT CALLBACK GetEnum(const PRJ_CALLBACK_DATA *d, const GUID *id, PCWSTR search, PRJ_DIR_ENTRY_BUFFER_HANDLE h)
{
    EnterCriticalSection(&g_enum_lock);
    Enum *e = g_enums;
    while (e && !IsEqualGUID(&e->id, id)) e = e->next;
    if (!e) { LeaveCriticalSection(&g_enum_lock); return E_INVALIDARG; }
    if ((d->Flags & PRJ_CB_DATA_FLAG_ENUM_RESTART_SCAN) || (!e->has_search && e->pos == 0)) {
        e->pos = 0;
        e->has_search = search != NULL;
        if (search) wcscpy_s(e->search, MAX_NAME + 1, search);
    }
    HRESULT hr = S_OK; BOOL added = FALSE;
    for (; e->pos < e->n; e->pos++) {
        Item *it = &e->items[e->pos];
        if (e->has_search && !PrjFileNameMatch(it->name, e->search)) continue;
        PRJ_FILE_BASIC_INFO b; basic_info(&b, FALSE, it->size, pfs_time());
        hr = PrjFillDirEntryBuffer(it->name, &b, h);
        if (FAILED(hr)) {
            // The buffer is full: what was added is returned now, the rest on the next call.
            if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && added) hr = S_OK;
            break;
        }
        added = TRUE;
    }
    LeaveCriticalSection(&g_enum_lock);
    return hr;
}

// ---------------------------------------------------------------- placeholders and data

static HRESULT CALLBACK GetPlaceholder(const PRJ_CALLBACK_DATA *d)
{
    WCHAR p[MAX_NAME + 2];
    if (!core_path(d->FilePathName, p, MAX_NAME + 2)) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    if (!pid_may(d->TriggeringProcessId, p, FILE_READ_ATTRIBUTES)) {
        BOOL dir; if (!NT_SUCCESS(pfs_stat(p, &dir, NULL, NULL))) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    }
    void *f; PfsInfo info;
    NTSTATUS st = pfs_open(p, FILE_READ_ATTRIBUTES, 0, &f, &info);   // no read access: never fetches
    if (!NT_SUCCESS(st)) return hr_from_status(st);
    pfs_close(f);
    PRJ_PLACEHOLDER_INFO pi; ZeroMemory(&pi, sizeof pi);
    basic_info(&pi.FileBasicInfo, info.dir, info.size, info.time);
    return PrjWritePlaceholderInfo(g_ctx, d->FilePathName, &pi, sizeof pi);
}

// The first data callback for a file fetches the content (the core records this caller as the reader) and writes
// the WHOLE file, whatever range was asked for: the core fetches once per open, so a second callback for another
// range would ask the data server again.
static HRESULT CALLBACK GetData(const PRJ_CALLBACK_DATA *d, UINT64 off, UINT32 len)
{
    (void)off; (void)len;
    WCHAR p[MAX_NAME + 2];
    if (!core_path(d->FilePathName, p, MAX_NAME + 2)) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    if (!pid_may(d->TriggeringProcessId, p, FILE_READ_DATA)) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    void *f; PfsInfo info;
    NTSTATUS st = pfs_open(p, FILE_READ_DATA, d->TriggeringProcessId, &f, &info);
    if (!NT_SUCCESS(st)) return hr_from_status(st);
    HRESULT hr = S_OK;
    void *buf = PrjAllocateAlignedBuffer(g_ctx, CHUNK);
    if (!buf) hr = E_OUTOFMEMORY;
    for (UINT64 pos = 0; SUCCEEDED(hr) && pos < info.size; ) {
        ULONG want = (info.size - pos) < CHUNK ? (ULONG)(info.size - pos) : CHUNK, got = 0;
        st = pfs_read(f, pos, want, buf, &got);
        if (!NT_SUCCESS(st) || got != want) { hr = NT_SUCCESS(st) ? HRESULT_FROM_WIN32(ERROR_IO_DEVICE) : hr_from_status(st); break; }
        hr = PrjWriteFileData(g_ctx, &d->DataStreamId, buf, pos, got);
        pos += got;
    }
    if (buf) { SecureZeroMemory(buf, CHUNK); PrjFreeAlignedBuffer(buf); }
    pfs_close(f);
    return hr;
}

// ---------------------------------------------------------------- notifications

static HRESULT CALLBACK Notify(const PRJ_CALLBACK_DATA *d, BOOLEAN dir, PRJ_NOTIFICATION n, PCWSTR dest, PRJ_NOTIFICATION_PARAMETERS *p)
{
    (void)dest; (void)p;
    switch (n) {
    case PRJ_NOTIFICATION_PRE_DELETE: case PRJ_NOTIFICATION_PRE_RENAME:
    case PRJ_NOTIFICATION_PRE_SET_HARDLINK: case PRJ_NOTIFICATION_FILE_PRE_CONVERT_TO_FULL:
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);   // the folder is read-only
    case PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_NO_MODIFICATION:
        // Dehydrate on close: remove the file so the next read is a new request to the provider. This runs for every
        // close, including a stat; the name is projected again on the next lookup. Best effort: it fails while
        // another handle has the file open, and then the data stays until a later close.
        if (!dir && d->FilePathName && d->FilePathName[0]) {
            PRJ_UPDATE_FAILURE_CAUSES why = 0;
            PrjDeleteFile(g_ctx, d->FilePathName,
                PRJ_UPDATE_ALLOW_DIRTY_METADATA | PRJ_UPDATE_ALLOW_DIRTY_DATA | PRJ_UPDATE_ALLOW_TOMBSTONE | PRJ_UPDATE_ALLOW_READ_ONLY, &why);
        }
        return S_OK;
    default:
        return S_OK;
    }
}

// ---------------------------------------------------------------- is ProjFS installed?

// True when the Windows feature Client-ProjFS is on: projectedfslib.dll is in the system directory and the PrjFlt
// filter driver service exists. Loads no ProjFS code.
static BOOL installed(void)
{
    WCHAR sys[MAX_PATH], path[MAX_PATHW];
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return FALSE;
    swprintf_s(path, MAX_PATHW, L"%ls\\projectedfslib.dll", sys);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return FALSE;
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return TRUE;   // cannot ask; the DLL is there, so let the real load decide
    SC_HANDLE s = OpenServiceW(scm, L"PrjFlt", SERVICE_QUERY_STATUS);
    DWORD err = s ? 0 : GetLastError();
    if (s) CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return s != NULL || (err != ERROR_SERVICE_DOES_NOT_EXIST && err != ERROR_INVALID_NAME);
}

// ---------------------------------------------------------------- serving

static BOOL is_drive_mount(const WCHAR *m) { return m[0] && m[1] == L':' && (m[2] == 0 || (m[2] == L'\\' && m[3] == 0)); }

// Deletes everything under `dir` (files left hydrated by a crashed run) and then the folder itself. Works on a
// ProjFS root without a provider running.
static void remove_tree(const WCHAR *dir)
{
    WCHAR pat[MAX_PATHW]; swprintf_s(pat, MAX_PATHW, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (0 == wcscmp(fd.cFileName, L".") || 0 == wcscmp(fd.cFileName, L"..")) continue;
            WCHAR child[MAX_PATHW]; swprintf_s(child, MAX_PATHW, L"%ls\\%ls", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(child);
            else { SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL); DeleteFileW(child); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static void make_parents(const WCHAR *path)
{
    WCHAR parent[MAX_PATHW]; wcscpy_s(parent, MAX_PATHW, path);
    for (WCHAR *s = parent + 3; *s; s++) {
        if (*s != L'\\') continue;
        *s = 0; CreateDirectoryW(parent, NULL); *s = L'\\';
    }
}

static int start(const WCHAR *mount)
{
    if (is_drive_mount(mount)) { pfs_log("the ProjFS build serves a folder, not a drive letter: use --mount <folder>"); return 14; }
    wcscpy_s(g_root, MAX_PATHW, mount);
    InitializeCriticalSection(&g_enum_lock);

    // Start from an empty folder we own: whatever a previous run left (hydrated files, a stale ProjFS root) is removed.
    make_parents(mount);
    remove_tree(mount);
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:SYG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FRFX;;;AU)", SDDL_REVISION_1, &sd, NULL)) {
        pfs_log("cannot build the folder security descriptor: %lu", GetLastError()); return 12;
    }
    SECURITY_ATTRIBUTES sa = { sizeof sa, sd, FALSE };
    BOOL made = CreateDirectoryW(mount, &sa);
    LocalFree(sd);
    if (!made) { pfs_log("cannot create the folder %ls: %lu", mount, GetLastError()); return 13; }

    GUID inst; CoCreateGuid(&inst);
    HRESULT hr;
    // The ProjFS library is delay-loaded: a broken install raises here instead of failing the whole process at start.
    __try { hr = PrjMarkDirectoryAsPlaceholder(mount, NULL, NULL, &inst); }
    __except (EXCEPTION_EXECUTE_HANDLER) { pfs_log("The Windows Projected File System is installed but cannot be loaded. Enable the Windows feature Client-ProjFS."); return 11; }
    if (FAILED(hr)) { pfs_log("PrjMarkDirectoryAsPlaceholder failed: 0x%08x", (unsigned)hr); RemoveDirectoryW(mount); return 15; }

    PRJ_CALLBACKS cb; ZeroMemory(&cb, sizeof cb);
    cb.StartDirectoryEnumerationCallback = StartEnum; cb.EndDirectoryEnumerationCallback = EndEnum;
    cb.GetDirectoryEnumerationCallback = GetEnum; cb.GetPlaceholderInfoCallback = GetPlaceholder;
    cb.GetFileDataCallback = GetData; cb.NotificationCallback = Notify;
    PRJ_NOTIFICATION_MAPPING map = { (PRJ_NOTIFY_TYPES)(PRJ_NOTIFY_PRE_DELETE | PRJ_NOTIFY_PRE_RENAME | PRJ_NOTIFY_PRE_SET_HARDLINK |
        PRJ_NOTIFY_FILE_PRE_CONVERT_TO_FULL | PRJ_NOTIFY_FILE_HANDLE_CLOSED_NO_MODIFICATION), L"" };
    PRJ_STARTVIRTUALIZING_OPTIONS opt; ZeroMemory(&opt, sizeof opt);
    opt.NotificationMappings = &map; opt.NotificationMappingsCount = 1;
    hr = PrjStartVirtualizing(mount, &cb, NULL, &opt, &g_ctx);
    if (FAILED(hr)) { pfs_log("PrjStartVirtualizing failed: 0x%08x", (unsigned)hr); remove_tree(mount); return 16; }
    return 0;
}

static void stop(void)
{
    PrjStopVirtualizing(g_ctx);
    g_ctx = NULL;
    remove_tree(g_root);   // no plaintext is left behind on a clean stop
}

const PfsBackend pfs_backend = {
    L"Windows Projected File System", "https://learn.microsoft.com/windows/win32/projfs/projected-file-system (enable the Windows feature Client-ProjFS)",
    installed, start, stop, NULL, NULL, NULL,
    TRUE,   // requires_size
};
