#ifndef A8DJ_STATUS_FILE_H
#define A8DJ_STATUS_FILE_H
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static inline FILE *A8DJ_OpenStatusFile(const char *path)
{
    // Check the opened inode before truncating. A symlink, FIFO or hard link
    // planted at the public diagnostic path must not redirect/capture writes.
    int fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0644);
    if (fd < 0) return NULL;
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_nlink != 1 || info.st_uid != geteuid() || ftruncate(fd, 0) != 0) {
        close(fd);
        return NULL;
    }
    FILE *file = fdopen(fd, "w");
    if (!file) close(fd);
    return file;
}
#endif
