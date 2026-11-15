// Shared by the two ProjFS backends (the service and the mount tool). Include after pfs_backend.h or pfm_backend.h.
// Everything here is independent of which core is linked: the core-specific calls are passed in.
#pragma once
#include <sddl.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <projectedfslib.h>

#pragma comment(lib, "ole32.lib")   // CoCreateGuid

#ifndef MAX_PATHW
#define MAX_PATHW 1024
#endif

typedef NTSTATUS (*ProjfsStatFn)(const WCHAR *path, BOOL *dir, PSECURITY_DESCRIPTOR sd, SIZE_T *sdsz);

static HRESULT projfs_hr_from_status(NTSTATUS st)
{
    switch (st) {
    case STATUS_ACCESS_DENIED: return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    case STATUS_OBJECT_NAME_NOT_FOUND: return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    case STATUS_INSUFFICIENT_RESOURCES: return E_OUTOFMEMORY;
    default: return HRESULT_FROM_WIN32(ERROR_IO_DEVICE);
    }
}

static void projfs_basic_info(PRJ_FILE_BASIC_INFO *b, BOOL dir, UINT64 size, UINT64 time)
{
    ZeroMemory(b, sizeof *b);
    b->IsDirectory = dir ? TRUE : FALSE;
    b->FileSize = dir ? 0 : (INT64)size;
    b->FileAttributes = dir ? FILE_ATTRIBUTE_DIRECTORY : (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_NORMAL);
    b->CreationTime.QuadPart = (LONGLONG)time;
    b->LastAccessTime = b->LastWriteTime = b->ChangeTime = b->CreationTime;
}

// The permission the file's security descriptor gives to the process `pid`, which is the only identity a ProjFS
// callback carries (a thread that impersonates someone else is not seen). A process that has exited is refused.
static BOOL projfs_pid_may(ProjfsStatFn stat, UINT32 pid, const WCHAR *cpath, ACCESS_MASK want)
{
    BOOL dir; SIZE_T sdsz = 0;
    if (!NT_SUCCESS(stat(cpath, &dir, NULL, &sdsz))) return FALSE;
    PSECURITY_DESCRIPTOR sd = malloc(sdsz);
    if (!sd) return FALSE;
    if (!NT_SUCCESS(stat(cpath, &dir, sd, &sdsz))) { free(sd); return FALSE; }
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

// Dehydrate on close, and the folder is read-only. The delete runs for every close, including a stat; the name is
// projected again on the next lookup. Best effort: it fails while another handle has the file open, and then the
// data stays until a later close.
static HRESULT projfs_notify(PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT ctx, const PRJ_CALLBACK_DATA *d, BOOLEAN dir, PRJ_NOTIFICATION n)
{
    switch (n) {
    case PRJ_NOTIFICATION_PRE_DELETE: case PRJ_NOTIFICATION_PRE_RENAME:
    case PRJ_NOTIFICATION_PRE_SET_HARDLINK: case PRJ_NOTIFICATION_FILE_PRE_CONVERT_TO_FULL:
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    case PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_NO_MODIFICATION:
        if (!dir && d->FilePathName && d->FilePathName[0]) {
            PRJ_UPDATE_FAILURE_CAUSES why = 0;
            PrjDeleteFile(ctx, d->FilePathName,
                PRJ_UPDATE_ALLOW_DIRTY_METADATA | PRJ_UPDATE_ALLOW_DIRTY_DATA | PRJ_UPDATE_ALLOW_TOMBSTONE | PRJ_UPDATE_ALLOW_READ_ONLY, &why);
        }
        return S_OK;
    default:
        return S_OK;
    }
}

static const PRJ_NOTIFY_TYPES PROJFS_NOTIFY_MASK = (PRJ_NOTIFY_TYPES)(PRJ_NOTIFY_PRE_DELETE | PRJ_NOTIFY_PRE_RENAME |
    PRJ_NOTIFY_PRE_SET_HARDLINK | PRJ_NOTIFY_FILE_PRE_CONVERT_TO_FULL | PRJ_NOTIFY_FILE_HANDLE_CLOSED_NO_MODIFICATION);

// True when the Windows feature Client-ProjFS is on: projectedfslib.dll is in the system directory and the PrjFlt
// filter driver service exists. Loads no ProjFS code.
static BOOL projfs_installed(void)
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

// "K:", "K:\" (and "*" for the mount tool) are drive-letter mounts, which ProjFS cannot do.
static BOOL projfs_is_drive_mount(const WCHAR *m)
{
    return (m[0] == L'*' && m[1] == 0) || (m[0] && m[1] == L':' && (m[2] == 0 || (m[2] == L'\\' && m[3] == 0)));
}

// Deletes everything under `dir` (files left hydrated by a crashed run) and then the folder itself, when
// `remove_root`. Works on a ProjFS root without a provider running.
static void projfs_remove_tree(const WCHAR *dir, BOOL remove_root)
{
    WCHAR pat[MAX_PATHW]; swprintf_s(pat, MAX_PATHW, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (0 == wcscmp(fd.cFileName, L".") || 0 == wcscmp(fd.cFileName, L"..")) continue;
            WCHAR child[MAX_PATHW]; swprintf_s(child, MAX_PATHW, L"%ls\\%ls", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) projfs_remove_tree(child, TRUE);
            else { SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL); DeleteFileW(child); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (remove_root) RemoveDirectoryW(dir);
}

// True when `dir` is a ProjFS virtualization root (its reparse tag is IO_REPARSE_TAG_PROJFS): the folder a killed run
// leaves behind. Such a folder holds only what a provider projected into it.
static BOOL projfs_is_root(const WCHAR *dir)
{
    HANDLE h = CreateFileW(dir, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BYTE buf[MAXIMUM_REPARSE_DATA_BUFFER_SIZE]; DWORD n = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, buf, sizeof buf, &n, NULL);
    CloseHandle(h);
    return ok && n >= 4 && *(DWORD *)buf == 0x9000001Cu;
}

static BOOL projfs_is_empty_dir(const WCHAR *dir)
{
    WCHAR pat[MAX_PATHW]; swprintf_s(pat, MAX_PATHW, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd; BOOL empty = TRUE;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) return TRUE;
    do { if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) { empty = FALSE; break; } } while (FindNextFileW(h, &fd));
    FindClose(h);
    return empty;
}

static void projfs_make_parents(const WCHAR *path)
{
    WCHAR parent[MAX_PATHW]; wcscpy_s(parent, MAX_PATHW, path);
    for (WCHAR *s = parent + 3; *s; s++) {
        if (*s != L'\\') continue;
        *s = 0; CreateDirectoryW(parent, NULL); *s = L'\\';
    }
}
