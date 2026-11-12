/*
 * syscalls.c - Minimal newlib system-call stubs. The firmware has no file
 * system or console; these only satisfy libc's link-time references.
 */
#include <errno.h>
#include <sys/stat.h>

int _close(int fd)
{
  (void)fd;
  errno = EBADF;
  return -1;
}

int _fstat(int fd, struct stat *st)
{
  (void)fd;
  st->st_mode = S_IFCHR;
  return 0;
}

int _isatty(int fd)
{
  (void)fd;
  return 1;
}

int _lseek(int fd, int offset, int whence)
{
  (void)fd;
  (void)offset;
  (void)whence;
  return 0;
}

int _read(int fd, char *buf, int len)
{
  (void)fd;
  (void)buf;
  (void)len;
  errno = EIO;
  return -1;
}

int _write(int fd, const char *buf, int len)
{
  (void)fd;
  (void)buf;
  (void)len;
  errno = EIO;
  return -1;
}

int _getpid(void)
{
  return 1;
}

int _kill(int pid, int sig)
{
  (void)pid;
  (void)sig;
  errno = EINVAL;
  return -1;
}

void _exit(int status)
{
  (void)status;
  for (;;)
  {
  }
}
