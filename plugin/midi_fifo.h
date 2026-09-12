// Shared FIFO opening policy for the HAL host and the user MIDI agent.
#ifndef A8DJ_MIDI_FIFO_H
#define A8DJ_MIDI_FIFO_H
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static inline int A8DJ_OpenMidiFifo(const char *path)
{
    if (mkfifo(path, 0666) != 0 && errno != EEXIST) return -1;
    // Never follow a symlink or chmod an existing arbitrary file in /tmp.
    // O_RDWR keeps both FIFO ends open without changing process SIGPIPE state.
    int fd = open(path, O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISFIFO(info.st_mode)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    // Cross-user IPC is intentional: _coreaudiod and the GUI user differ.
    // Only the creator can undo its umask; never chmod another user's path.
    if (info.st_uid == geteuid() && fchmod(fd, 0666) != 0) {
        int error = errno;
        close(fd); errno = error;
        return -1;
    }
    return fd;
}
#endif
