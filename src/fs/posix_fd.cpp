#include "fs/posix_fd.h"

#include "fs/file_error.h"

#include <cerrno>
#include <unistd.h>
#include <utility>

luciole::Fd::~Fd() {
  if (m_fd >= 0) {
    ::close(m_fd);
  }
}

luciole::Fd::Fd(Fd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}

luciole::Fd& luciole::Fd::operator=(Fd&& other) noexcept {
  if (this != &other) {
    if (m_fd >= 0) {
      ::close(m_fd);
    }
    m_fd = std::exchange(other.m_fd, -1);
  }
  return *this;
}

int luciole::Fd::close() { return ::close(std::exchange(m_fd, -1)); }

luciole::Fd luciole::checkedFd(int fd) {
  if (fd < 0) {
    throwErrno(errno);
  }
  return Fd(fd);
}

struct stat luciole::statFd(int fd) {
  struct stat value{};
  if (::fstat(fd, &value) != 0) {
    throwErrno(errno);
  }
  return value;
}

bool luciole::sameIdentity(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino && (a.st_mode & S_IFMT) == (b.st_mode & S_IFMT);
}
