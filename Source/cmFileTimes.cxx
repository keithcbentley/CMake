/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmFileTimes.h"

#include <utility>

#include <cm/memory>

#include "cmsys/Status.hxx"

#include "cm_sys_stat.h"

#if defined(_WIN32)
#  include <windows.h>

#  include "cmSystemTools.h"
#else
#  include <cerrno>

#  include <utime.h>
#endif

#if defined(_WIN32) && (defined(_MSC_VER) || defined(__WATCOMC__) || defined(__MINGW32__))
#  include <io.h>
#endif

#ifdef _WIN32
//  TODO: Should this be separated? Is this really the only place we need Windows handles?
class cmFileTimes::WindowsHandle
{
public:
  WindowsHandle(HANDLE h)
    : handle_(h)
  {
  }
  ~WindowsHandle()
  {
    if (this->handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(this->handle_);
    }
  }
  explicit operator bool() const { return this->handle_ != INVALID_HANDLE_VALUE; }
  bool operator!() const { return this->handle_ == INVALID_HANDLE_VALUE; }
  operator HANDLE() const { return this->handle_; }

private:
  HANDLE handle_;
};
#endif

class cmFileTimes::Times
{
public:
#if defined(_WIN32) && !defined(__CYGWIN__)
  FILETIME timeCreation;
  FILETIME timeLastAccess;
  FILETIME timeLastWrite;
#else
  struct utimbuf timeBuf;
#endif
};

// cmFileTimes::cmFileTimes() = default;
cmFileTimes::cmFileTimes(std::string const& fileName)
{
  this->LoadFileTime(fileName);
}
cmFileTimes::~cmFileTimes() = default;

void cmFileTimes::LoadFileTime(std::string const& fileName)
{
  std::unique_ptr<Times> ptr;
  if (this->IsValid()) {
    // Invalidate this and reuse times
    ptr.swap(this->times);
  } else {
    ptr = cm::make_unique<Times>();
  }

#if defined(_WIN32) && !defined(__CYGWIN__)
  cmFileTimes::WindowsHandle handle = CreateFileW(
    cmSystemTools::ConvertToWindowsExtendedPath(fileName).c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING,
    FILE_FLAG_BACKUP_SEMANTICS, 0);
  if (!handle) {
    throw CMakeStatusException(ErrorKind::Windows);
    //    return cmsys::Status::Windows_GetLastError();
  }
  if (!GetFileTime(handle, &ptr->timeCreation, &ptr->timeLastAccess, &ptr->timeLastWrite)) {
    throw CMakeStatusException(ErrorKind::Windows);
    //    return cmsys::Status::Windows_GetLastError();
  }
#else
  struct stat st;
  if (stat(fileName.c_str(), &st) < 0) {
    return cmsys::Status::POSIX_errno();
  }
  ptr->timeBuf.actime = st.st_atime;
  ptr->timeBuf.modtime = st.st_mtime;
#endif
  // Accept times
  this->times = std::move(ptr);
  return;
}

void cmFileTimes::Store(std::string const& fileName) const
{
  if (!this->IsValid()) {
    throw CMakeStatusException(ErrorKind::POSIX);
    //    return cmsys::Status::POSIX(EINVAL);
  }

#if defined(_WIN32) && !defined(__CYGWIN__)
  cmFileTimes::WindowsHandle handle = CreateFileW(
    cmSystemTools::ConvertToWindowsExtendedPath(fileName).c_str(), FILE_WRITE_ATTRIBUTES, 0, 0, OPEN_EXISTING,
    FILE_FLAG_BACKUP_SEMANTICS, 0);
  if (!handle) {
    throw CMakeStatusException(ErrorKind::Windows);
    //    return cmsys::Status::Windows_GetLastError();
  }
  if (!SetFileTime(handle, &this->times->timeCreation, &this->times->timeLastAccess, &this->times->timeLastWrite)) {
    throw CMakeStatusException(ErrorKind::Windows);
    //    return cmsys::Status::Windows_GetLastError();
  }
#else
  if (utime(fileName.c_str(), &this->times->timeBuf) < 0) {
    return cmsys::Status::POSIX_errno();
  }
#endif
  return;
}

void cmFileTimes::CopyFileTimes(
  std::string const& fromFile,
  std::string const& toFile)
{
  cmFileTimes fileTimes(fromFile);
  fileTimes.Store(toFile);
  return;
}
