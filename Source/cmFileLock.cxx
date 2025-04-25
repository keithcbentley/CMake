/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmFileLock.h"

#include <cassert>
#include <utility>

#include "cmFileLockResult.h"

// Common implementation

cmFileLock::cmFileLock(cmFileLock&& other) noexcept
{
  this->File = other.File;
  other.File = (decltype(other.File))-1;
  this->m_filename = std::move(other.m_filename);
#if defined(_WIN32)
  this->Overlapped = std::move(other.Overlapped);
#endif
}

cmFileLock::~cmFileLock()
{
  if (!this->m_filename.empty()) {
    cmFileLockResult const result = this->Release();
    static_cast<void>(result);
    assert(result.IsOk());
  }
}

cmFileLock& cmFileLock::operator=(cmFileLock&& other) noexcept
{
  this->File = other.File;
  other.File = (decltype(other.File))-1;
  this->m_filename = std::move(other.m_filename);
#if defined(_WIN32)
  this->Overlapped = std::move(other.Overlapped);
#endif

  return *this;
}

cmFileLockResult cmFileLock::Lock(std::string const& filename,
                                  unsigned long timeout)
{
  if (filename.empty()) {
    // Error is internal since all the directories and file must be created
    // before actual lock called.
    return cmFileLockResult::MakeInternal();
  }

  if (!this->m_filename.empty()) {
    // Error is internal since double-lock must be checked in class
    // cmFileLockPool by the cmFileLock::IsLocked method.
    return cmFileLockResult::MakeInternal();
  }

  this->m_filename = filename;
  cmFileLockResult result = this->OpenFile();
  if (result.IsOk()) {
    if (timeout == static_cast<unsigned long>(-1)) {
      result = this->LockWithoutTimeout();
    } else {
      result = this->LockWithTimeout(timeout);
    }
  }

  if (!result.IsOk()) {
    this->m_filename.clear();
  }

  return result;
}

bool cmFileLock::IsLocked(std::string const& filename) const
{
  return filename == this->m_filename;
}

#if defined(_WIN32)
// NOLINTNEXTLINE(bugprone-suspicious-include)
#  include "cmFileLockWin32.cxx"
#else
// NOLINTNEXTLINE(bugprone-suspicious-include)
#  include "cmFileLockUnix.cxx"
#endif
