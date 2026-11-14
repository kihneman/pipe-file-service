// Shared by the Dokan backends. Dokan opens whatever the file system lets it open and does not enforce
// per-file permissions, so a backend checks the security descriptor itself, against the token of the process
// that issued the open. Include after dokan.h.
#pragma once

typedef NTSTATUS (*DokanStatFn)(const WCHAR *path, BOOL *dir, PSECURITY_DESCRIPTOR sd, SIZE_T *sdsz);

// SYNCHRONIZE is implied; an open that asks for nothing else (just a handle) is not checked.
static NTSTATUS dokan_check_access(LPCWSTR name, ACCESS_MASK access, PDOKAN_FILE_INFO fi, DokanStatFn stat)
{
    GENERIC_MAPPING gm = { FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
    ACCESS_MASK want = access & ~(ACCESS_MASK)SYNCHRONIZE;
    MapGenericMask(&want, &gm);
    if (!want) return STATUS_SUCCESS;

    SIZE_T sdsz = 0; BOOL dir;
    NTSTATUS st = stat(name, &dir, NULL, &sdsz);
    if (!NT_SUCCESS(st)) return st;
    PSECURITY_DESCRIPTOR sd = malloc(sdsz);
    if (!sd) return STATUS_INSUFFICIENT_RESOURCES;
    st = stat(name, &dir, sd, &sdsz);
    if (!NT_SUCCESS(st)) { free(sd); return st; }

    NTSTATUS result = STATUS_ACCESS_DENIED;
    HANDLE tok = DokanOpenRequestorToken(fi), imp = NULL;
    if (tok && DuplicateToken(tok, SecurityImpersonation, &imp)) {
        PRIVILEGE_SET ps; DWORD psl = sizeof ps, got = 0; BOOL granted = FALSE;
        if (AccessCheck(sd, imp, want, &gm, &ps, &psl, &got, &granted) && granted) result = STATUS_SUCCESS;
    }
    if (imp) CloseHandle(imp);
    if (tok) CloseHandle(tok);
    free(sd);
    return result;
}
