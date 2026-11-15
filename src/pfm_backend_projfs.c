// The ProjFS backend of pipe-file-mount: projects the core's tree (one file, with virtual directories above it) into
// a real folder with the Windows Projected File System (the optional Windows feature Client-ProjFS; Microsoft's own
// library and filter driver, no third-party driver).
//
// ProjFS is not a volume, so this backend cannot keep every guarantee of the WinFsp and Dokan ones:
//  - A file read through ProjFS is WRITTEN to the folder (hydrated) as a normal NTFS file, so the content is on disk
//    until it is removed. The backend removes the file again when the handle that read it closes ("dehydrate on
//    close"), but a reader that opens the file while it is hydrated gets the data without a callback, so the data
//    server is not asked about that read, and if this program is killed the hydrated file stays on disk.
//  - The folder allows only the user who started the program, and SYSTEM; the files inherit that.
//  - The file size must be known before the first read, so the program needs `--size` (requires_size).
//  - The mount point is a folder, not a drive letter. The folder must not exist or must be empty (a folder left behind
//    by a killed run of this program is cleaned); it is removed when the program stops.
//
// MIT licensed. Uses the Windows Projected File System API (projectedfslib.h); the library is delay-loaded.
#include "pfm_backend.h"
#include "projfs_common.h"

#define CHUNK (1u << 20)   // bytes written to ProjFS per call (a multiple of any sector size)

static WCHAR g_root[MAX_PATHW];
static BOOL g_existed;   // the folder existed (and was empty) before we started: it is put back empty on stop
static PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT g_ctx;

// ProjFS path ("a\b\file", "" for the root) -> the core's path ("\a\b\file", "\").
static BOOL core_path(const WCHAR *p, WCHAR *out, size_t cap)
{
    if (!p) return FALSE;
    return swprintf_s(out, cap, L"\\%ls", p) > 0;
}

// ---------------------------------------------------------------- directory enumeration

typedef struct { WCHAR name[MAX_COMPONENT + 1]; BOOL dir; UINT64 size; } Item;
typedef struct Enum {
    struct Enum *next; GUID id; Item *items; size_t n, pos; BOOL has_search; WCHAR search[MAX_COMPONENT + 1];
} Enum;
static CRITICAL_SECTION g_enum_lock;
static Enum *g_enums;

typedef struct { Item *items; size_t n, cap; const WCHAR *dirpath; } Gather;

static BOOL gather_one(void *ctx, const WCHAR *leaf, BOOL dir)
{
    Gather *g = (Gather *)ctx;
    if (0 == wcscmp(leaf, L".") || 0 == wcscmp(leaf, L"..")) return TRUE;
    if (g->n == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 4;
        g->items = (Item *)realloc(g->items, g->cap * sizeof(Item));
        if (!g->items) return FALSE;
    }
    Item *it = &g->items[g->n++];
    wcscpy_s(it->name, MAX_COMPONENT + 1, leaf);
    it->dir = dir; it->size = 0;
    if (!dir) {
        WCHAR p[MAX_PATHW];
        swprintf_s(p, MAX_PATHW, L"%ls%ls%ls", g->dirpath, g->dirpath[1] ? L"\\" : L"", leaf);
        void *f; PfmInfo info;
        if (NT_SUCCESS(pfm_open(p, FILE_READ_ATTRIBUTES, 0, &f, &info))) { it->size = info.size; pfm_close(f); }
    }
    return TRUE;
}

static int __cdecl cmp_items(const void *a, const void *b) { return PrjFileNameCompare(((const Item *)a)->name, ((const Item *)b)->name); }

static HRESULT CALLBACK StartEnum(const PRJ_CALLBACK_DATA *d, const GUID *id)
{
    WCHAR p[MAX_PATHW];
    if (!core_path(d->FilePathName, p, MAX_PATHW)) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    void *dir; PfmInfo info;
    NTSTATUS st = pfm_open(p, FILE_LIST_DIRECTORY, d->TriggeringProcessId, &dir, &info);
    if (!NT_SUCCESS(st)) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    Enum *e = (Enum *)calloc(1, sizeof *e);
    if (!e) { pfm_close(dir); return E_OUTOFMEMORY; }
    e->id = *id;
    Gather g = {0}; g.dirpath = p;
    if (info.dir) pfm_list(dir, gather_one, &g);
    pfm_close(dir);
    if (g.n) qsort(g.items, g.n, sizeof(Item), cmp_items);
    e->items = g.items; e->n = g.n;
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

static HRESULT CALLBACK GetEnum(const PRJ_CALLBACK_DATA *d, const GUID *id, PCWSTR search, PRJ_DIR_ENTRY_BUFFER_HANDLE h)
{
    EnterCriticalSection(&g_enum_lock);
    Enum *e = g_enums;
    while (e && !IsEqualGUID(&e->id, id)) e = e->next;
    if (!e) { LeaveCriticalSection(&g_enum_lock); return E_INVALIDARG; }
    if ((d->Flags & PRJ_CB_DATA_FLAG_ENUM_RESTART_SCAN) || (!e->has_search && e->pos == 0)) {
        e->pos = 0;
        e->has_search = search != NULL;
        if (search) wcscpy_s(e->search, MAX_COMPONENT + 1, search);
    }
    HRESULT hr = S_OK; BOOL added = FALSE;
    for (; e->pos < e->n; e->pos++) {
        Item *it = &e->items[e->pos];
        if (e->has_search && !PrjFileNameMatch(it->name, e->search)) continue;
        PRJ_FILE_BASIC_INFO b; projfs_basic_info(&b, it->dir, it->size, pfm_time());
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
    WCHAR p[MAX_PATHW];
    if (!core_path(d->FilePathName, p, MAX_PATHW)) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    BOOL dir;
    if (!NT_SUCCESS(pfm_stat(p, &dir, NULL, NULL))) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    if (!projfs_pid_may(pfm_stat, d->TriggeringProcessId, p, FILE_READ_ATTRIBUTES)) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    void *f; PfmInfo info;
    NTSTATUS st = pfm_open(p, FILE_READ_ATTRIBUTES, d->TriggeringProcessId, &f, &info);   // no read access: never fetches
    if (!NT_SUCCESS(st)) return projfs_hr_from_status(st);
    pfm_close(f);
    PRJ_PLACEHOLDER_INFO pi; ZeroMemory(&pi, sizeof pi);
    projfs_basic_info(&pi.FileBasicInfo, info.dir, info.size, info.time);
    return PrjWritePlaceholderInfo(g_ctx, d->FilePathName, &pi, sizeof pi);
}

// The first data callback for a file fetches the content (the core records this caller as the reader) and writes
// the WHOLE file, whatever range was asked for: the core fetches once per open, so a second callback for another
// range would ask the data server again.
static HRESULT CALLBACK GetData(const PRJ_CALLBACK_DATA *d, UINT64 off, UINT32 len)
{
    (void)off; (void)len;
    WCHAR p[MAX_PATHW];
    if (!core_path(d->FilePathName, p, MAX_PATHW)) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    if (!projfs_pid_may(pfm_stat, d->TriggeringProcessId, p, FILE_READ_DATA)) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    void *f; PfmInfo info;
    NTSTATUS st = pfm_open(p, FILE_READ_DATA, d->TriggeringProcessId, &f, &info);
    if (!NT_SUCCESS(st)) return projfs_hr_from_status(st);
    HRESULT hr = S_OK;
    void *buf = PrjAllocateAlignedBuffer(g_ctx, CHUNK);
    if (!buf) hr = E_OUTOFMEMORY;
    for (UINT64 pos = 0; SUCCEEDED(hr) && pos < info.size; ) {
        ULONG want = (info.size - pos) < CHUNK ? (ULONG)(info.size - pos) : CHUNK, got = 0;
        st = pfm_read(f, pos, want, buf, &got);
        if (!NT_SUCCESS(st) || got != want) { hr = NT_SUCCESS(st) ? HRESULT_FROM_WIN32(ERROR_IO_DEVICE) : projfs_hr_from_status(st); break; }
        hr = PrjWriteFileData(g_ctx, &d->DataStreamId, buf, pos, got);
        pos += got;
    }
    if (buf) { SecureZeroMemory(buf, CHUNK); PrjFreeAlignedBuffer(buf); }
    pfm_close(f);
    return hr;
}

static HRESULT CALLBACK Notify(const PRJ_CALLBACK_DATA *d, BOOLEAN dir, PRJ_NOTIFICATION n, PCWSTR dest, PRJ_NOTIFICATION_PARAMETERS *p)
{
    (void)dest; (void)p;
    return projfs_notify(g_ctx, d, dir, n);
}

// ---------------------------------------------------------------- serving

static BOOL load(void) { return projfs_installed(); }

// SDDL: SYSTEM and the user who started the program, nobody else. The user needs full control of every projected
// folder because this program, running as the user, creates the placeholders inside them; the files are still
// read-only (the attribute, and delete, rename and writes are vetoed).
static BOOL folder_sd(PSECURITY_DESCRIPTOR *sd)
{
    HANDLE tok; BYTE buf[256]; DWORD n = 0; LPWSTR sid = NULL; WCHAR sddl[512];
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return FALSE;
    BOOL ok = GetTokenInformation(tok, TokenUser, buf, sizeof buf, &n);
    CloseHandle(tok);
    if (!ok || !ConvertSidToStringSidW(((TOKEN_USER *)buf)->User.Sid, &sid)) return FALSE;
    swprintf_s(sddl, 512, L"O:%lsG:%lsD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;%ls)", sid, sid, sid);
    LocalFree(sid);
    return ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, sd, NULL);
}

static int start(const WCHAR *mount)
{
    if (projfs_is_drive_mount(mount)) { fprintf(stderr, "the ProjFS build mounts a folder, not a drive letter\n"); return 1; }
    if (!GetFullPathNameW(mount, MAX_PATHW, g_root, NULL)) { fprintf(stderr, "bad mount folder\n"); return 1; }
    size_t rl = wcslen(g_root);
    while (rl > 3 && g_root[rl - 1] == L'\\') g_root[--rl] = 0;
    InitializeCriticalSection(&g_enum_lock);

    // Never delete anything that is not ours: the folder must be missing, empty, or a ProjFS root a killed run left
    // behind (which holds only what we projected).
    DWORD attrs = GetFileAttributesW(g_root);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) { fprintf(stderr, "%ls exists and is not a folder\n", g_root); return 1; }
        if (projfs_is_root(g_root)) projfs_remove_tree(g_root, TRUE);   // a leftover root
        else if (!projfs_is_empty_dir(g_root)) { fprintf(stderr, "%ls is not empty: choose a folder that does not exist or is empty\n", g_root); return 1; }
        else { g_existed = TRUE; RemoveDirectoryW(g_root); }
    }
    projfs_make_parents(g_root);
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!folder_sd(&sd)) { fprintf(stderr, "cannot build the folder security descriptor: %lu\n", GetLastError()); return 1; }
    SECURITY_ATTRIBUTES sa = { sizeof sa, sd, FALSE };
    BOOL made = CreateDirectoryW(g_root, &sa);
    LocalFree(sd);
    if (!made) { fprintf(stderr, "cannot create the folder %ls: %lu\n", g_root, GetLastError()); return 1; }

    GUID inst; CoCreateGuid(&inst);
    HRESULT hr;
    // The ProjFS library is delay-loaded: a broken install raises here instead of failing the whole process at start.
    __try { hr = PrjMarkDirectoryAsPlaceholder(g_root, NULL, NULL, &inst); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fprintf(stderr, "The Windows Projected File System is installed but cannot be loaded. Enable the Windows feature Client-ProjFS.\n"); RemoveDirectoryW(g_root); return 1; }
    if (FAILED(hr)) { fprintf(stderr, "PrjMarkDirectoryAsPlaceholder failed: 0x%08x\n", (unsigned)hr); RemoveDirectoryW(g_root); return 1; }

    PRJ_CALLBACKS cb; ZeroMemory(&cb, sizeof cb);
    cb.StartDirectoryEnumerationCallback = StartEnum; cb.EndDirectoryEnumerationCallback = EndEnum;
    cb.GetDirectoryEnumerationCallback = GetEnum; cb.GetPlaceholderInfoCallback = GetPlaceholder;
    cb.GetFileDataCallback = GetData; cb.NotificationCallback = Notify;
    PRJ_NOTIFICATION_MAPPING map = { PROJFS_NOTIFY_MASK, L"" };
    PRJ_STARTVIRTUALIZING_OPTIONS opt; ZeroMemory(&opt, sizeof opt);
    opt.NotificationMappings = &map; opt.NotificationMappingsCount = 1;
    hr = PrjStartVirtualizing(g_root, &cb, NULL, &opt, &g_ctx);
    if (FAILED(hr)) { fprintf(stderr, "PrjStartVirtualizing failed: 0x%08x\n", (unsigned)hr); projfs_remove_tree(g_root, TRUE); return 1; }
    return 0;
}

static void stop(void)
{
    PrjStopVirtualizing(g_ctx);
    g_ctx = NULL;
    projfs_remove_tree(g_root, TRUE);   // no plaintext is left behind on a clean stop
    if (g_existed) CreateDirectoryW(g_root, NULL);   // a folder that was there is put back, empty
}

const PfmBackend pfm_backend = {
    "The Windows Projected File System is not enabled. Enable the Windows feature Client-ProjFS "
    "(Enable-WindowsOptionalFeature -Online -FeatureName Client-ProjFS), then run this again.",
    load, start, stop,
    TRUE,   // requires_size
};
