/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmFileSet.h"

#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cm/optional>
#include <cmext/algorithm>
#include <cmext/string_view>

#include "cmsys/RegularExpression.hxx"

#include "cmGeneratorExpression.h"
#include "cmList.h"
#include "cmListFileCache.h"
#include "cmLocalGenerator.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmake.h"

cm::static_string_view cmFileSetVisibilityToName(cmFileSetVisibility vis)
{
  switch (vis) {
    case cmFileSetVisibility::Interface:
      return "INTERFACE"_s;
    case cmFileSetVisibility::Public:
      return "PUBLIC"_s;
    case cmFileSetVisibility::Private:
      return "PRIVATE"_s;
  }
  return ""_s;
}

cmFileSetVisibility cmFileSetVisibilityFromName(cm::string_view name,
                                                cmMakefile* mf)
{
  if (name == "INTERFACE"_s) {
    return cmFileSetVisibility::Interface;
  }
  if (name == "PUBLIC"_s) {
    return cmFileSetVisibility::Public;
  }
  if (name == "PRIVATE"_s) {
    return cmFileSetVisibility::Private;
  }
  auto msg = cmStrCat("File set visibility \"", name, "\" is not valid.");
  if (mf) {
    mf->IssueMessage(MessageType::FATAL_ERROR, msg);
  } else {
    cmSystemTools::Error(msg);
  }
  return cmFileSetVisibility::Private;
}

bool cmFileSetVisibilityIsForSelf(cmFileSetVisibility vis)
{
  switch (vis) {
    case cmFileSetVisibility::Interface:
      return false;
    case cmFileSetVisibility::Public:
    case cmFileSetVisibility::Private:
      return true;
  }
  return false;
}

bool cmFileSetVisibilityIsForInterface(cmFileSetVisibility vis)
{
  switch (vis) {
    case cmFileSetVisibility::Interface:
    case cmFileSetVisibility::Public:
      return true;
    case cmFileSetVisibility::Private:
      return false;
  }
  return false;
}

bool cmFileSetTypeCanBeIncluded(std::string const& type)
{
  return type == "HEADERS"_s;
}

cmFileSet::cmFileSet(CMake& cmakeInstance, std::string name, std::string type,
                     cmFileSetVisibility visibility)
  : CMakeInstance(cmakeInstance)
  , m_name(std::move(name))
  , Type(std::move(type))
  , Visibility(visibility)
{
}

void cmFileSet::CopyEntries(cmFileSet const* fs)
{
  cm::append(this->DirectoryEntries, fs->DirectoryEntries);
  cm::append(this->FileEntries, fs->FileEntries);
}

void cmFileSet::ClearDirectoryEntries()
{
  this->DirectoryEntries.clear();
}

void cmFileSet::AddDirectoryEntry(BT<std::string> directories)
{
  this->DirectoryEntries.push_back(std::move(directories));
}

void cmFileSet::ClearFileEntries()
{
  this->FileEntries.clear();
}

void cmFileSet::AddFileEntry(BT<std::string> files)
{
  this->FileEntries.push_back(std::move(files));
}

std::vector<std::unique_ptr<cmCompiledGeneratorExpression>>
cmFileSet::CompileFileEntries() const
{
  std::vector<std::unique_ptr<cmCompiledGeneratorExpression>> result;

  for (auto const& entry : this->FileEntries) {
    for (auto const& ex : cmList{ entry.Value }) {
      cmGeneratorExpression ge(this->CMakeInstance, entry.m_backtrace);
      auto cge = ge.Parse(ex);
      result.push_back(std::move(cge));
    }
  }

  return result;
}

std::vector<std::unique_ptr<cmCompiledGeneratorExpression>>
cmFileSet::CompileDirectoryEntries() const
{
  std::vector<std::unique_ptr<cmCompiledGeneratorExpression>> result;

  for (auto const& entry : this->DirectoryEntries) {
    for (auto const& ex : cmList{ entry.Value }) {
      cmGeneratorExpression ge(this->CMakeInstance, entry.m_backtrace);
      auto cge = ge.Parse(ex);
      result.push_back(std::move(cge));
    }
  }

  return result;
}

std::vector<std::string> cmFileSet::EvaluateDirectoryEntries(
  std::vector<std::unique_ptr<cmCompiledGeneratorExpression>> const& cges,
  cmLocalGenerator* lg, std::string const& config,
  cmGeneratorTarget const* target,
  cmGeneratorExpressionDAGChecker* dagChecker) const
{
  struct DirCacheEntry
  {
    std::string collapsedDir;
    cm::optional<cmSystemTools::FileId> fileId;
  };

  std::unordered_map<std::string, DirCacheEntry> dirCache;
  std::vector<std::string> result;
  for (auto const& cge : cges) {
    auto entry = cge->Evaluate(lg, config, target, dagChecker);
    cmList dirs{ entry };
    for (std::string dir : dirs) {
      if (!cmSystemTools::FileIsFullPath(dir)) {
        dir = cmStrCat(lg->GetCurrentSourceDirectory(), '/', dir);
      }

      auto dirCacheResult = dirCache.emplace(dir, DirCacheEntry());
      auto& dirCacheEntry = dirCacheResult.first->second;
      auto const isNewCacheEntry = dirCacheResult.second;

      if (isNewCacheEntry) {
        cmSystemTools::FileId fileId;
        auto isFileIdValid = cmSystemTools::GetFileId(dir, fileId);
        dirCacheEntry.collapsedDir = cmSystemTools::CollapseFullPath(dir);
        dirCacheEntry.fileId =
          isFileIdValid ? cm::optional<decltype(fileId)>(fileId) : cm::nullopt;
      }

      for (auto const& priorDir : result) {
        auto priorDirCacheEntry = dirCache.at(priorDir);
        bool sameFile = dirCacheEntry.fileId.has_value() &&
          priorDirCacheEntry.fileId.has_value() &&
          (*dirCacheEntry.fileId == *priorDirCacheEntry.fileId);
        if (!sameFile &&
            (cmSystemTools::IsSubDirectory(dirCacheEntry.collapsedDir,
                                           priorDirCacheEntry.collapsedDir) ||
             cmSystemTools::IsSubDirectory(priorDirCacheEntry.collapsedDir,
                                           dirCacheEntry.collapsedDir))) {
          lg->GetCMakeInstance()->IssueMessage(
            MessageType::FATAL_ERROR,
            cmStrCat(
              "Base directories in file set cannot be subdirectories of each "
              "other:\n  ",
              priorDir, "\n  ", dir),
            cge->GetBacktrace());
          return {};
        }
      }
      result.push_back(dir);
    }
  }
  return result;
}

void cmFileSet::EvaluateFileEntry(
  std::vector<std::string> const& dirs,
  std::map<std::string, std::vector<std::string>>& filesPerDir,
  std::unique_ptr<cmCompiledGeneratorExpression> const& cge,
  cmLocalGenerator* lg, std::string const& config,
  cmGeneratorTarget const* target,
  cmGeneratorExpressionDAGChecker* dagChecker) const
{
  auto files = cge->Evaluate(lg, config, target, dagChecker);
  for (std::string file : cmList{ files }) {
    if (!cmSystemTools::FileIsFullPath(file)) {
      file = cmStrCat(lg->GetCurrentSourceDirectory(), '/', file);
    }
    auto collapsedFile = cmSystemTools::CollapseFullPath(file);
    bool found = false;
    std::string relDir;
    for (auto const& dir : dirs) {
      auto collapsedDir = cmSystemTools::CollapseFullPath(dir);
      if (cmSystemTools::IsSubDirectory(collapsedFile, collapsedDir)) {
        found = true;
        relDir = cmSystemTools::GetParentDirectory(
          cmSystemTools::RelativePath(collapsedDir, collapsedFile));
        break;
      }
    }
    if (!found) {
      std::ostringstream e;
      e << "File:\n  " << file
        << "\nmust be in one of the file set's base directories:";
      for (auto const& dir : dirs) {
        e << "\n  " << dir;
      }
      lg->GetCMakeInstance()->IssueMessage(MessageType::FATAL_ERROR, e.str(),
                                           cge->GetBacktrace());
      return;
    }

    filesPerDir[relDir].push_back(file);
  }
}

bool cmFileSet::IsValidName(std::string const& name)
{
  static cmsys::RegularExpression const regex("^[a-z0-9][a-zA-Z0-9_]*$");

  cmsys::RegularExpressionMatch match;
  return regex.find(name.c_str(), match);
}
