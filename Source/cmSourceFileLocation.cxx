/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmSourceFileLocation.h"

#include <cassert>

#include <cm/string_view>

#include "cmGlobalGenerator.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmPolicies.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmake.h"

// if CMP0187 and CMP0115 are NEW, then we assume that source files that do not
// include a file extension are not ambiguous but intentionally do not have an
// extension.
bool NoAmbiguousExtensions(cmMakefile const& makefile)
{
  return makefile.GetPolicyStatus(cmPolicies::CMP0115) == cmPolicies::NEW &&
    makefile.GetPolicyStatus(cmPolicies::CMP0187) == cmPolicies::NEW;
}

cmSourceFileLocation::cmSourceFileLocation(cmSourceFileLocation const& loc)
  : m_pMakefile(loc.m_pMakefile)
{
  this->AmbiguousDirectory = loc.AmbiguousDirectory;
  this->AmbiguousExtension = loc.AmbiguousExtension;
  this->Directory = loc.Directory;
  this->m_name = loc.m_name;
}

cmSourceFileLocation::cmSourceFileLocation(cmMakefile const* mf,
                                           std::string const& name,
                                           cmSourceFileLocationKind kind)
  : m_pMakefile(mf)
{
  this->AmbiguousDirectory = !cmSystemTools::FileIsFullPath(name);
  // If ambiguous extensions are allowed then the extension is assumed to be
  // ambiguous unless the name has an extension, in which case
  // `UpdateExtension` will update this. If ambiguous extensions are not
  // allowed, then set this to false as the file extension must be provided or
  // the file doesn't have an extension.
  this->AmbiguousExtension = !NoAmbiguousExtensions(*mf);
  this->Directory = cmSystemTools::GetFilenamePath(name);
  if (cmSystemTools::FileIsFullPath(this->Directory)) {
    this->Directory = cmSystemTools::CollapseFullPath(this->Directory);
  }
  this->m_name = cmSystemTools::GetFilenameName(name);
  if (kind == cmSourceFileLocationKind::Known) {
    this->DirectoryUseSource();
    this->AmbiguousExtension = false;
  } else {
    this->UpdateExtension(name);
  }
}

std::string cmSourceFileLocation::GetFullPath() const
{
  std::string path = this->GetDirectory();
  if (!path.empty()) {
    path += '/';
  }
  path += this->GetName();
  return path;
}

void cmSourceFileLocation::Update(cmSourceFileLocation const& loc)
{
  if (this->AmbiguousDirectory && !loc.AmbiguousDirectory) {
    this->Directory = loc.Directory;
    this->AmbiguousDirectory = false;
  }
  if (this->AmbiguousExtension && !loc.AmbiguousExtension) {
    this->m_name = loc.m_name;
    this->AmbiguousExtension = false;
  }
}

void cmSourceFileLocation::DirectoryUseSource()
{
  assert(this->m_pMakefile);
  if (this->AmbiguousDirectory) {
    this->Directory = cmSystemTools::CollapseFullPath(
      this->Directory, this->m_pMakefile->GetCurrentSourceDirectory());
    this->AmbiguousDirectory = false;
  }
}

void cmSourceFileLocation::DirectoryUseBinary()
{
  assert(this->m_pMakefile);
  if (this->AmbiguousDirectory) {
    this->Directory = cmSystemTools::CollapseFullPath(
      this->Directory, this->m_pMakefile->GetCurrentBinaryDirectory());
    this->AmbiguousDirectory = false;
  }
}

void cmSourceFileLocation::UpdateExtension(std::string const& name)
{
  assert(this->m_pMakefile);
  // Check the extension.
  std::string ext = cmSystemTools::GetFilenameLastExtension(name);
  if (!ext.empty()) {
    ext = ext.substr(1);
  }

  // The global generator checks extensions of enabled languages.
  cmGlobalGenerator* gg = this->m_pMakefile->GetGlobalGenerator();
  cmMakefile const* mf = this->m_pMakefile;
  auto* cm = mf->GetCMakeInstance();
  if (!gg->GetLanguageFromExtension(ext.c_str()).empty() ||
      cm->IsAKnownExtension(ext)) {
    // This is a known extension.  Use the given filename with extension.
    this->m_name = cmSystemTools::GetFilenameName(name);
    this->AmbiguousExtension = false;
  } else {
    // This is not a known extension.  See if the file exists on disk as
    // named.
    std::string tryPath;
    if (this->AmbiguousDirectory) {
      // Check the source tree only because a file in the build tree should
      // be specified by full path at least once.  We do not want this
      // detection to depend on whether the project has already been built.
      tryPath = cmStrCat(this->m_pMakefile->GetCurrentSourceDirectory(), '/');
    }
    if (!this->Directory.empty()) {
      tryPath += this->Directory;
      tryPath += "/";
    }
    tryPath += this->m_name;
    if (cmSystemTools::FileExists(tryPath, true)) {
      // We found a source file named by the user on disk.  Trust it's
      // extension.
      this->m_name = cmSystemTools::GetFilenameName(name);
      this->AmbiguousExtension = false;

      // If the directory was ambiguous, it isn't anymore.
      if (this->AmbiguousDirectory) {
        this->DirectoryUseSource();
      }
    }
  }
}

bool cmSourceFileLocation::MatchesAmbiguousExtension(
  cmSourceFileLocation const& loc) const
{
  assert(this->m_pMakefile);
  // This location's extension is not ambiguous but loc's extension
  // is.  See if the names match as-is.
  if (this->m_name == loc.m_name) {
    return true;
  }

  // Check if loc's name could possibly be extended to our name by
  // adding an extension.
  if (!(this->m_name.size() > loc.m_name.size() &&
        this->m_name[loc.m_name.size()] == '.' &&
        cmHasPrefix(this->m_name, loc.m_name))) {
    return false;
  }

  // Only a fixed set of extensions will be tried to match a file on
  // disk.  One of these must match if loc refers to this source file.
  auto ext = cm::string_view(this->m_name).substr(loc.m_name.size() + 1);
  cmMakefile const* mf = this->m_pMakefile;
  auto* cm = mf->GetCMakeInstance();
  return cm->IsAKnownExtension(ext);
}

bool cmSourceFileLocation::Matches(cmSourceFileLocation const& loc)
{
  assert(this->m_pMakefile);
  if (this->AmbiguousExtension == loc.AmbiguousExtension) {
    // Both extensions are similarly ambiguous.  Since only the old fixed set
    // of extensions will be tried, the names must match at this point to be
    // the same file.
    if (this->m_name.size() != loc.m_name.size() ||
        !cmSystemTools::PathsEqual(this->m_name, loc.m_name)) {
      return false;
    }
  } else {
    cmSourceFileLocation const* loc1;
    cmSourceFileLocation const* loc2;
    if (this->AmbiguousExtension) {
      // Only "this" extension is ambiguous.
      loc1 = &loc;
      loc2 = this;
    } else {
      // Only "loc" extension is ambiguous.
      loc1 = this;
      loc2 = &loc;
    }
    if (!loc1->MatchesAmbiguousExtension(*loc2)) {
      return false;
    }
  }

  if (!this->AmbiguousDirectory && !loc.AmbiguousDirectory) {
    // Both sides have absolute directories.
    if (this->Directory != loc.Directory) {
      return false;
    }
  } else if (this->AmbiguousDirectory && loc.AmbiguousDirectory) {
    if (this->m_pMakefile == loc.m_pMakefile) {
      // Both sides have directories relative to the same location.
      if (this->Directory != loc.Directory) {
        return false;
      }
    } else {
      // Each side has a directory relative to a different location.
      // This can occur when referencing a source file from a different
      // directory.  This is not yet allowed.
      this->m_pMakefile->IssueMessage(
        MessageType::INTERNAL_ERROR,
        "Matches error: Each side has a directory relative to a different "
        "location. This can occur when referencing a source file from a "
        "different directory.  This is not yet allowed.");
      return false;
    }
  } else if (this->AmbiguousDirectory) {
    // Compare possible directory combinations.
    std::string const srcDir = cmSystemTools::CollapseFullPath(
      this->Directory, this->m_pMakefile->GetCurrentSourceDirectory());
    std::string const binDir = cmSystemTools::CollapseFullPath(
      this->Directory, this->m_pMakefile->GetCurrentBinaryDirectory());
    if (srcDir != loc.Directory && binDir != loc.Directory) {
      return false;
    }
  } else if (loc.AmbiguousDirectory) {
    // Compare possible directory combinations.
    std::string const srcDir = cmSystemTools::CollapseFullPath(
      loc.Directory, loc.m_pMakefile->GetCurrentSourceDirectory());
    std::string const binDir = cmSystemTools::CollapseFullPath(
      loc.Directory, loc.m_pMakefile->GetCurrentBinaryDirectory());
    if (srcDir != this->Directory && binDir != this->Directory) {
      return false;
    }
  }

  // File locations match.
  this->Update(loc);
  return true;
}
