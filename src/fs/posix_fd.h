#pragma once

#include <sys/stat.h>

namespace luciole {
  // Owned POSIX descriptor; -1 is empty.
  class Fd {
  public:
    Fd() = default;
    explicit Fd(int fd) : m_fd(fd) {}
    ~Fd();
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;

    int get() const { return m_fd; }
    bool valid() const { return m_fd >= 0; }
    // Closes now and returns close()'s result, so callers can check deferred write errors.
    int close();

  private:
    int m_fd = -1;
  };

  // Takes ownership of an open()/openat() result; a negative result throws errno as a FileErrorException.
  Fd checkedFd(int fd);
  // fstat; throws errno as a FileErrorException.
  struct stat statFd(int fd);
  // Same inode and file type.
  bool sameIdentity(const struct stat& a, const struct stat& b);
  // Rename history ignores access time; metadata/content changes invalidate a captured revision.
  bool sameFileRevision(const struct stat& a, const struct stat& b);
} // namespace luciole
