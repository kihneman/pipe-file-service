// The line between the driver-independent core of PipeFileService (control pipe, registrations, limits,
// placeholder links, forwarded reads, fetch on first read, events, service install) and a user-mode
// file system driver backend. A build links the core with exactly one backend, so a shipped exe
// references exactly one driver.
//
// The core serves a flat, read-only directory: the root "\" and one file "\<name>" per registration.
// Paths are passed the way every Windows user-mode file system library presents them: a leading
// backslash, no further backslash.
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

#define MAX_NAME 255   // longest file name on the volume

typedef struct {
    BOOL dir;
    UINT64 size;   // file size; 0 until the first read of a handle has fetched the content
    UINT64 id;     // stable per registration; 1 is the root
    UINT64 time;   // FILETIME used for every timestamp
} PfsInfo;

// ---- implemented by the core, called by the backend ------------------------------------------

// Opens the root or a registered file. `caller_pid` must be the process that issued the open (0 only if
// the driver cannot tell). Opens without FILE_READ_DATA in `access` never fetch content; a failed
// identification of the caller for a forwarded read refuses the open (STATUS_ACCESS_DENIED).
NTSTATUS pfs_open(const WCHAR *path, UINT32 access, UINT32 caller_pid, void **file, PfsInfo *info);
void pfs_close(void *file);
NTSTATUS pfs_read(void *file, UINT64 offset, ULONG length, void *buf, ULONG *done);   // STATUS_END_OF_FILE past the end
NTSTATUS pfs_get_info(void *file, PfsInfo *info);
// Self-relative security descriptor; with sd == NULL only *sdsz is set. STATUS_BUFFER_OVERFLOW if too small.
NTSTATUS pfs_get_security(void *file, PSECURITY_DESCRIPTOR sd, SIZE_T *sdsz);
// Like open, but by path and without opening: for the security-by-name callback of the driver.
NTSTATUS pfs_stat(const WCHAR *path, BOOL *dir, PSECURITY_DESCRIPTOR sd, SIZE_T *sdsz);
// Calls visit(ctx, name, id) for each registered file; stops when it returns FALSE.
void pfs_list(BOOL (*visit)(void *ctx, const WCHAR *name, UINT64 id), void *ctx);
UINT64 pfs_time(void);
void pfs_log(const char *fmt, ...);

// ---- implemented by the backend, called by the core ------------------------------------------

typedef struct {
    const WCHAR *driver_name;   // for messages: "WinFsp"
    const char *driver_url;     // where to get the driver
    // True when the driver is installed. Loads no driver code (the driver library is delay-loaded).
    BOOL (*installed)(void);
    // Loads the driver, creates the volume, mounts it at `mount` and starts serving the core's files.
    // Returns 0, or a nonzero exit code after logging why it failed. Called once.
    int (*start)(const WCHAR *mount);
    void (*stop)(void);
    // Backend-specific `run` option (development only). Returns TRUE when it consumed the flag and its value.
    BOOL (*parse_run_option)(const WCHAR *flag, const WCHAR *value);
    const WCHAR *run_option_synopsis;   // for the usage text, e.g. L"--winfsp-dir <folder>"
    const WCHAR *run_option_help;       // one explanatory usage line
} PfsBackend;

extern const PfsBackend pfs_backend;
