/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// POSIX (macOS, Linux) versions of the OS-specific parts of File.cpp

#include "base/Base.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#if OS_MAC
#include <mach-o/dyld.h>
#endif

#include "base/File.h"

// we pad data read with 3 zeros for convenience, like File.cpp
constexpr int kZeroPaddingCount = 3;

static char* PathZTemp(Str path) {
    return CStrTemp(path);
}

static bool StatPath(Str path, struct stat& st) {
    if (len(path) == 0) {
        return false;
    }
    return stat(PathZTemp(path), &st) == 0;
}

static FILETIME FileTimeFromTimespec(time_t sec, long nsec) {
    u64 t = ((u64)sec * 1000000000ULL) + (u64)nsec;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)t;
    ft.dwHighDateTime = (DWORD)(t >> 32);
    return ft;
}

static u64 FileTimeToNs(FILETIME ft) {
    return ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

#if OS_MAC
static timespec StatAccessTime(const struct stat& st) {
    return st.st_atimespec;
}

static timespec StatModificationTime(const struct stat& st) {
    return st.st_mtimespec;
}
#else
static timespec StatAccessTime(const struct stat& st) {
    return st.st_atim;
}

static timespec StatModificationTime(const struct stat& st) {
    return st.st_mtim;
}
#endif

static FILETIME FileTimeFromTimespec(timespec ts) {
    return FileTimeFromTimespec(ts.tv_sec, ts.tv_nsec);
}

static timespec TimespecFromFileTime(FILETIME ft) {
    u64 ns = FileTimeToNs(ft);
    timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ULL);
    ts.tv_nsec = (long)(ns % 1000000000ULL);
    return ts;
}

namespace path {

Type GetType(Str path) {
    struct stat st;
    if (!StatPath(path, st)) {
        return Type::None;
    }
    if (S_ISDIR(st.st_mode)) {
        return Type::Dir;
    }
    return Type::File;
}

bool IsDirectory(Str path) {
    struct stat st;
    return StatPath(path, st) && S_ISDIR(st.st_mode);
}

// No cache on non-Windows — same as an uncached attribute query.
// Like GetFileAttributesW: returns attributes or INVALID_FILE_ATTRIBUTES.
// On Windows, network-drive path results are cached for 1 hour (shared with
// GetCachedAttributesEx). Offline non-fixed drives (mapped/UNC/removable) are
// also remembered for ~4 minutes so later queries fail fast.
// Non-Windows: no cache (same as an uncached attribute query).
DWORD GetCachedAttributes(Str path) {
    struct stat st;
    if (!StatPath(path, st)) {
        return (DWORD)-1; // INVALID_FILE_ATTRIBUTES
    }
    return (DWORD)st.st_mode;
}

TempStr NormalizeTemp(Str path) {
    char resolved[PATH_MAX];
    if (realpath(PathZTemp(path), resolved)) {
        return str::DupTemp(Str(resolved));
    }
    if (IsAbsolute(path)) {
        return str::DupTemp(path);
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        return str::DupTemp(path);
    }
    return path::JoinTemp(Str(cwd), path);
}

TempStr ShortPathTemp(Str path) {
    return NormalizeTemp(path);
}

bool IsSame(Str path1, Str path2) {
    if (str::IsNull(path1) || str::IsNull(path2)) {
        return false;
    }

    struct stat st1;
    struct stat st2;
    if (StatPath(path1, st1) && StatPath(path2, st2)) {
        return st1.st_dev == st2.st_dev && st1.st_ino == st2.st_ino;
    }

    TempStr npath1 = NormalizeTemp(path1);
    TempStr npath2 = NormalizeTemp(path2);
    return npath1 && str::Eq(npath1, npath2);
}

bool HasVariableDriveLetter(Str /*path*/) {
    return false;
}

bool IsOnNetworkDrive(Str /*path*/) {
    return false;
}

bool IsCloudPlaceholder(Str /*path*/) {
    return false;
}

bool IsEphemeralHostFile(Str /*path*/) {
    return false;
}

bool IsOnFixedDrive(Str /*path*/) {
    return true;
}

bool IsOnAvailableDrive(Str path) {
    if (len(path) == 0) {
        return false;
    }
    if (file::Exists(path) || dir::Exists(path)) {
        return true;
    }
    TempStr dir = path::GetDirTemp(path);
    if (len(dir) == 0 || str::Eq(dir, path)) {
        return false;
    }
    return dir::Exists(dir);
}

bool SupportsChangeNotifications(Str /*path*/) {
    return false;
}

bool IsAbsolute(Str path) {
    return len(path) > 0 && IsSep(path.s[0]);
}

TempStr GetNonVirtualTemp(Str virtualPath) {
    return virtualPath;
}

} // namespace path

TempStr GetTempFilePathTemp(Str filePrefix) {
    const char* tmpDir = getenv("TMPDIR");
    if (!tmpDir || !tmpDir[0]) {
        tmpDir = "/tmp";
    }
    if (len(filePrefix) == 0) {
        return str::DupTemp(Str(tmpDir));
    }

    TempStr name = fmt("%sXXXXXX", filePrefix);
    TempStr path = path::JoinTemp(Str(tmpDir), name);
    char* pathZ = CStrTemp(path);
    int fd = mkstemp(pathZ);
    if (fd < 0) {
        return {};
    }
    close(fd);
    return Str(pathZ);
}

// Path of this process image (exe or DLL that contains this code).
TempStr GetSelfExePathTemp() {
#if OS_MAC
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) {
        return {};
    }
    char resolved[PATH_MAX];
    if (realpath(buf, resolved)) {
        return str::DupTemp(Str(resolved));
    }
    return str::DupTemp(Str(buf));
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) {
        return {};
    }
    buf[n] = 0;
    return str::DupTemp(Str(buf));
#endif
}

// Directory containing GetSelfExePathTemp().
TempStr GetSelfExeDirTemp() {
    TempStr path = GetSelfExePathTemp();
    if (len(path) == 0) {
        return {};
    }
    return path::GetDirTemp(path);
}

TempStr GetPathInExeDirTemp(Str fileName) {
    TempStr dir = GetSelfExeDirTemp();
    if (len(dir) == 0) {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) {
            return fileName;
        }
        dir = Str(cwd);
    }
    return path::NormalizeTemp(path::JoinTemp(dir, fileName));
}

namespace file {

FILE* OpenFILE(Str path) {
    ReportIf(len(path) == 0);
    if (len(path) == 0) {
        return nullptr;
    }
    return fopen(PathZTemp(path), "rb");
}

FileHandle OpenReadOnly(Str path) {
    return open(PathZTemp(path), O_RDONLY);
}

void Close(FileHandle h) {
    if (h != kInvalidFileHandle) {
        close(h);
    }
}

bool Flush(FileHandle h) {
    return fsync(h) == 0;
}

static i64 GetSizeFromHandle(FileHandle h) {
    if (h == kInvalidFileHandle) {
        return -1;
    }
    struct stat st;
    if (fstat(h, &st) != 0 || S_ISDIR(st.st_mode)) {
        return -1;
    }
    return (i64)st.st_size;
}

Str ReadFileWithArena(Str filePath, Arena* a) {
    ReportIf(len(filePath) == 0);
    int fd = OpenReadOnly(filePath);
    if (fd < 0) {
        return {};
    }
    AutoCall closeFile(close, fd);

    i64 fileSize = GetSizeFromHandle(fd);
    if (fileSize < 0 || fileSize > (i64)(INT_MAX - kZeroPaddingCount)) {
        return {};
    }
    int size = (int)fileSize;
    char* d = (char*)Alloc(a, (size_t)size + kZeroPaddingCount);
    if (!d) {
        return {};
    }
    memset(d + size, 0, kZeroPaddingCount);

    int nTotal = 0;
    while (nTotal < size) {
        ssize_t n = read(fd, d + nTotal, (size_t)(size - nTotal));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            logf("ReadFileWithArena: read() failed, path: '%s', size: %d, nRead: %d, errno: %d\n", filePath, size,
                 nTotal, errno);
            Free(a, (void*)d);
            return {};
        }
        nTotal += (int)n;
    }
    return Str(d, size);
}

int ReadN(Str path, u8* buf, size_t toRead) {
    FILE* fp = OpenFILE(path);
    if (!fp) {
        return -1;
    }
    AutoCall closeFile(fclose, fp);
    ZeroMemory(buf, toRead);
    size_t nRead = fread((void*)buf, 1, toRead, fp);
    if (nRead == 0 && ferror(fp)) {
        return -1;
    }
    return (int)nRead;
}

bool Exists(Str path) {
    struct stat st;
    return StatPath(path, st) && S_ISREG(st.st_mode);
}

i64 GetSize(Str path) {
    if (len(path) == 0) {
        return -1;
    }
    struct stat st;
    if (!StatPath(path, st) || S_ISDIR(st.st_mode)) {
        return -1;
    }
    return (i64)st.st_size;
}

// Maps the whole file at path into memory as a read-only view backed by the
// OS page cache. Unlike ReadFile() this doesn't allocate private memory for
// the file content: pages are faulted in from disk on first access and can
// be discarded by the OS under memory pressure. Caveat: if the backing file
// becomes unreadable while mapped (e.g. a network mount disconnects) or is
// truncated by another process, touching a mapped page raises SIGBUS instead
// of returning an error, so avoid mapping files on unreliable media.
bool MemoryMap(Str path, Mapping* res) {
    int fd = open(PathZTemp(path), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    i64 size = GetSizeFromHandle(fd);
    if (size <= 0) {
        close(fd);
        return false;
    }
    void* data = mmap(nullptr, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0);
    // the mapping stays valid after the fd is closed
    close(fd);
    if (data == MAP_FAILED) {
        return false;
    }
    res->data = (u8*)data;
    res->size = size;
    return true;
}

void MemoryUnmap(Mapping* m) {
    if (m->data) {
        munmap(m->data, (size_t)m->size);
    }
    *m = {};
}

bool WriteFile(Str path, Str d) {
    int fd = open(PathZTemp(path), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return false;
    }
    AutoCall closeFile(close, fd);

    const char* data = d.s;
    size_t left = (size_t)d.len;
    while (left > 0) {
        ssize_t n = write(fd, data, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data += n;
        left -= (size_t)n;
    }
    return true;
}

bool Delete(Str path) {
    if (len(path) == 0) {
        return false;
    }
    if (unlink(PathZTemp(path)) == 0) {
        return true;
    }
    return errno == ENOENT;
}

bool DeleteFileToTrash(Str path) {
    return Delete(path);
}

bool Copy(Str dst, Str src, bool dontOverwrite) {
    return Copy(dst, src, dontOverwrite, {});
}

bool Copy(Str dst, Str src, bool dontOverwrite, const CopyProgressCb& cbProgress) {
    int srcFd = open(PathZTemp(src), O_RDONLY);
    if (srcFd < 0) {
        return false;
    }
    AutoCall closeSrc(close, srcFd);

    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (dontOverwrite) {
        flags |= O_EXCL;
    }
    int dstFd = open(PathZTemp(dst), flags, 0666);
    if (dstFd < 0) {
        return false;
    }
    AutoCall closeDst(close, dstFd);

    i64 total = GetSize(src);
    i64 copied = 0;
    u8 buf[64 * 1024];
    for (;;) {
        ssize_t nRead = read(srcFd, buf, sizeof(buf));
        if (nRead < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (nRead == 0) {
            return true;
        }

        u8* p = buf;
        ssize_t left = nRead;
        while (left > 0) {
            ssize_t nWritten = write(dstFd, p, (size_t)left);
            if (nWritten < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            p += nWritten;
            left -= nWritten;
        }

        copied += nRead;
        if (cbProgress.IsValid()) {
            CopyProgress progress{copied, total < 0 ? 0 : total};
            cbProgress.Call(&progress);
        }
    }
}

bool SetAccessTime(Str path, FILETIME accessTime) {
    struct stat st;
    if (!StatPath(path, st)) {
        return false;
    }
    timespec ts[2];
    ts[0] = TimespecFromFileTime(accessTime);
    ts[1] = StatModificationTime(st);
    return utimensat(AT_FDCWD, PathZTemp(path), ts, 0) == 0;
}

FILETIME GetModificationTime(Str path) {
    struct stat st;
    if (!StatPath(path, st)) {
        return {};
    }
    return FileTimeFromTimespec(StatModificationTime(st));
}

bool SetModificationTime(Str path, FILETIME lastMod) {
    struct stat st;
    if (!StatPath(path, st)) {
        return false;
    }
    timespec ts[2];
    ts[0] = StatAccessTime(st);
    ts[1] = TimespecFromFileTime(lastMod);
    return utimensat(AT_FDCWD, PathZTemp(path), ts, 0) == 0;
}

DWORD GetAttributes(Str path) {
    struct stat st;
    if (!StatPath(path, st)) {
        return (DWORD)-1;
    }
    return (DWORD)st.st_mode;
}

bool SetAttributes(Str path, DWORD attrs) {
    return chmod(PathZTemp(path), (mode_t)(attrs & 07777)) == 0;
}

int GetZoneIdentifier(Str /*path*/) {
    return URLZONE_INVALID;
}

bool SetZoneIdentifier(Str /*path*/, int /*zoneId*/) {
    return true;
}

bool DeleteZoneIdentifier(Str /*path*/) {
    return true;
}

bool Rename(Str newPath, Str oldPath) {
    if (len(newPath) == 0 || len(oldPath) == 0) {
        return false;
    }
    return rename(PathZTemp(oldPath), PathZTemp(newPath)) == 0;
}

// rename() already replaces an existing newPath, so this is Rename().
bool RenameReplace(Str newPath, Str oldPath) {
    return Rename(newPath, oldPath);
}

bool OverwriteAtomicRetry(Str dst, Str src, int retryCount, int retrySleepMs) {
    if (len(dst) == 0 || len(src) == 0) {
        return false;
    }

    TempStr dstDir = path::GetDirTemp(dst);
    TempStr dstName = path::GetBaseNameTemp(dst);
    TempStr tempTemplate = fmt("%s/.%s.tmp.XXXXXX", dstDir, dstName);
    char* tempPathZ = CStrTemp(tempTemplate);
    int tempFd = mkstemp(tempPathZ);
    if (tempFd < 0) {
        return false;
    }
    close(tempFd);

    TempStr tempPath = Str(tempPathZ);
    if (!Copy(tempPath, src, false)) {
        Delete(tempPath);
        return false;
    }

    retryCount = std::max(retryCount, 1);
    for (int i = 0; i < retryCount; i++) {
        if (rename(PathZTemp(tempPath), PathZTemp(dst)) == 0) {
            return true;
        }
        if (i + 1 < retryCount && retrySleepMs > 0) {
            usleep((useconds_t)retrySleepMs * 1000);
        }
    }

    Delete(tempPath);
    return false;
}

} // namespace file

int FileTimeDiffInSecs(const FILETIME& ft1, const FILETIME& ft2) {
    i64 diff = (i64)FileTimeToNs(ft1) - (i64)FileTimeToNs(ft2);
    return (int)(diff / 1000000000LL);
}

namespace dir {

bool Create(Str dir) {
    if (mkdir(PathZTemp(dir), 0777) == 0) {
        return true;
    }
    return errno == EEXIST && Exists(dir);
}

// Create dir and all missing parents (like mkdir -p).
bool CreateAll(Str dir, int* errOut) {
    if (errOut) {
        *errOut = 0;
    }
    if (len(dir) == 0) {
        return false;
    }
    if (Exists(dir)) {
        return true;
    }
    TempStr parent = path::GetDirTemp(dir);
    if (!str::Eq(parent, dir) && len(parent) > 0 && !str::Eq(parent, StrL("."))) {
        if (!Exists(parent) && !CreateAll(parent, errOut)) {
            return false;
        }
    }
    if (Create(dir)) {
        return true;
    }
    if (errOut) {
        *errOut = errno;
    }
    return false;
}

// deletes everything inside dir; also removes dir itself when removeDir.
// a missing dir counts as success, matching the previous RemoveAll behavior.
static bool RemoveDirContentsZ(const char* dir, bool removeDir) {
    DIR* d = opendir(dir);
    if (!d) {
        return errno == ENOENT;
    }
    AutoCall closeDir(closedir, d);

    for (;;) {
        errno = 0;
        dirent* ent = readdir(d);
        if (!ent) {
            break;
        }
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        TempStr child = path::JoinTemp(Str(dir), Str(ent->d_name));
        struct stat st;
        if (lstat(child.s, &st) != 0) {
            return false;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!RemoveDirContentsZ(child.s, true)) {
                return false;
            }
        } else if (unlink(child.s) != 0) {
            return false;
        }
    }
    if (errno != 0) {
        return false;
    }
    if (!removeDir) {
        return true;
    }
    return rmdir(dir) == 0;
}

bool RemoveAll(Str dir) {
    return RemoveDirContentsZ(PathZTemp(dir), true);
}

// Delete everything inside dir but keep dir itself, so code that races with us
// still finds the directory there (see SaveThumbnail / dir::CreateAll).
bool Empty(Str dir) {
    return RemoveDirContentsZ(PathZTemp(dir), false);
}

bool HasWriteAccess(Str dir) {
    if (len(dir) == 0) {
        return false;
    }
    TempStr path = path::JoinTemp(dir, StrL("__sumatra_write_test__.tmp"));
    int fd = open(PathZTemp(path), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        return false;
    }
    close(fd);
    unlink(PathZTemp(path));
    return true;
}

} // namespace dir
