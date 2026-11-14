// The line between the driver-independent core of pipe-file-mount (path handling, reader-info protocol,
// fetch on first read, the volume's security descriptor) and a user-mode file system driver backend. A
// build links the core with exactly one backend, so a shipped exe references exactly one driver.
//
// The core serves one read-only file at a path on a volume, with virtual read-only directories above it.
// Paths are passed the way every Windows user-mode file system library presents them: a leading
// backslash, components separated by backslashes.
#pragma once
#define _CRT_SECURE_NO_WARNINGS
#define UNICODE
#define _UNICODE
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#endif

#define MAX_COMPONENT 255   // longest name of one path component

typedef struct {
    BOOL dir;
    UINT64 size;   // file size; 0 until the first read of a handle has fetched the content
    UINT64 id;
    UINT64 time;   // FILETIME used for every timestamp
} PfmInfo;

// ---- implemented by the core, called by the backend ------------------------------------------

// Opens a node of the served tree. `caller_pid` must be the process that issued the open (0 only if the
// driver cannot tell). Opens without FILE_READ_DATA in `access` never fetch content; a failed
// identification of the caller in reader-info mode refuses the open (STATUS_ACCESS_DENIED).
NTSTATUS pfm_open(const WCHAR *path, UINT32 access, UINT32 caller_pid, void **file, PfmInfo *info);
void pfm_close(void *file);
NTSTATUS pfm_read(void *file, UINT64 offset, ULONG length, void *buf, ULONG *done);   // STATUS_END_OF_FILE past the end
NTSTATUS pfm_get_info(void *file, PfmInfo *info);
// Self-relative security descriptor of every node: the user who started the program, and SYSTEM.
// With sd == NULL only *sdsz is set. STATUS_BUFFER_OVERFLOW if too small.
NTSTATUS pfm_get_security(PSECURITY_DESCRIPTOR sd, SIZE_T *sdsz);
// Like open, but by path and without opening: for the security-by-name callback of the driver.
NTSTATUS pfm_stat(const WCHAR *path, BOOL *dir, PSECURITY_DESCRIPTOR sd, SIZE_T *sdsz);
// Calls visit(ctx, name, dir) for the children of the directory `file` ("." and ".." included below the
// root); stops when it returns FALSE. STATUS_NOT_A_DIRECTORY for a file.
NTSTATUS pfm_list(void *file, BOOL (*visit)(void *ctx, const WCHAR *name, BOOL dir), void *ctx);
UINT64 pfm_time(void);
void pfm_log(const char *fmt, ...);

// ---- implemented by the backend, called by the core ------------------------------------------

typedef struct {
    // Printed (and the program exits with 1) when load() fails.
    const char *not_installed_message;
    // Loads the driver library. FALSE when the driver is not installed.
    BOOL (*load)(void);
    // Creates the volume and mounts it at `mount` (a drive letter such as "K:", or "*" for any free
    // one) and starts serving. Returns 0, or nonzero after printing why it failed.
    int (*start)(const WCHAR *mount);
    void (*stop)(void);
} PfmBackend;

extern const PfmBackend pfm_backend;
