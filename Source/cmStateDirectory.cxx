/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */

#include "cmStateDirectory.h"

#include <algorithm>
#include <cassert>
#include <vector>

#include <cm/iterator>
#include <cm/string_view>
#include <cmext/algorithm>
#include <cmext/string_view>

#include "cmAlgorithms.h"
#include "cmList.h"
#include "cmListFileCache.h"
#include "cmProperty.h"
#include "cmPropertyMap.h"
#include "cmRange.h"
#include "cmState.h"
#include "cmStatePrivate.h"
#include "cmStateTypes.h"
#include "cmValue.h"

static std::string const kBINARY_DIR = "BINARY_DIR";
static std::string const kBUILDSYSTEM_TARGETS = "BUILDSYSTEM_TARGETS";
static std::string const kSOURCE_DIR = "SOURCE_DIR";
static std::string const kSUBDIRECTORIES = "SUBDIRECTORIES";

std::string const& cmStateDirectory::GetCurrentSource() const
{
  return this->DirectoryState->Location;
}

void cmStateDirectory::SetCurrentSource(std::string const& dir)
{
  this->DirectoryState->Location = dir;
  this->Snapshot_.SetDefinition("CMAKE_CURRENT_SOURCE_DIR",
                                this->DirectoryState->Location);
}

std::string const& cmStateDirectory::GetCurrentBinary() const
{
  return this->DirectoryState->OutputLocation;
}

void cmStateDirectory::SetCurrentBinary(std::string const& dir)
{
  this->DirectoryState->OutputLocation = dir;
  this->Snapshot_.SetDefinition("CMAKE_CURRENT_BINARY_DIR",
                                this->DirectoryState->OutputLocation);
}

cmStateDirectory::cmStateDirectory(
  cmLinkedTree<cmStateDetail::BuildsystemDirectoryStateType>::iterator iter,
  cmStateSnapshot const& snapshot)
  : DirectoryState(iter)
  , Snapshot_(snapshot)
{
}

template <typename T, typename U>
cmBTStringRange GetPropertyContent(T const& content, U contentEndPosition)
{
  auto end = content.begin() + contentEndPosition;

  auto rbegin = cm::make_reverse_iterator(end);
  rbegin = std::find(rbegin, content.rend(), cmPropertySentinel);

  return cmMakeRange(rbegin.base(), end);
}

template <typename T, typename U>
void AppendEntry(T& content, U& endContentPosition,
                 const BT<std::string>& value)
{
  if (value.Value.empty()) {
    return;
  }

  assert(endContentPosition == content.size());

  content.push_back(value);

  endContentPosition = content.size();
}

template <typename T, typename U>
void SetContent(T& content, U& endContentPosition, const BT<std::string>& vec)
{
  assert(endContentPosition == content.size());

  content.resize(content.size() + 2);

  content.back() = vec;

  endContentPosition = content.size();
}

template <typename T, typename U>
void ClearContent(T& content, U& endContentPosition)
{
  assert(endContentPosition == content.size());

  content.resize(content.size() + 1);

  endContentPosition = content.size();
}

cmBTStringRange cmStateDirectory::GetIncludeDirectoriesEntries() const
{
  return GetPropertyContent(
    this->DirectoryState->IncludeDirectories,
    this->Snapshot_.Position->IncludeDirectoryPosition);
}

void cmStateDirectory::AppendIncludeDirectoriesEntry(
  const BT<std::string>& vec)
{
  AppendEntry(this->DirectoryState->IncludeDirectories,
              this->Snapshot_.Position->IncludeDirectoryPosition, vec);
}

void cmStateDirectory::PrependIncludeDirectoriesEntry(
  const BT<std::string>& vec)
{
  auto entryEnd = this->DirectoryState->IncludeDirectories.begin() +
    this->Snapshot_.Position->IncludeDirectoryPosition;

  auto rend = this->DirectoryState->IncludeDirectories.rend();
  auto rbegin = cm::make_reverse_iterator(entryEnd);
  rbegin = std::find(rbegin, rend, cmPropertySentinel);

  auto entryIt = rbegin.base();

  this->DirectoryState->IncludeDirectories.insert(entryIt, vec);

  this->Snapshot_.Position->IncludeDirectoryPosition =
    this->DirectoryState->IncludeDirectories.size();
}

void cmStateDirectory::SetIncludeDirectories(const BT<std::string>& vec)
{
  SetContent(this->DirectoryState->IncludeDirectories,
             this->Snapshot_.Position->IncludeDirectoryPosition, vec);
}

void cmStateDirectory::ClearIncludeDirectories()
{
  ClearContent(this->DirectoryState->IncludeDirectories,
               this->Snapshot_.Position->IncludeDirectoryPosition);
}

cmBTStringRange cmStateDirectory::GetCompileDefinitionsEntries() const
{
  return GetPropertyContent(
    this->DirectoryState->CompileDefinitions,
    this->Snapshot_.Position->CompileDefinitionsPosition);
}

void cmStateDirectory::AppendCompileDefinitionsEntry(
  const BT<std::string>& vec)
{
  AppendEntry(this->DirectoryState->CompileDefinitions,
              this->Snapshot_.Position->CompileDefinitionsPosition, vec);
}

void cmStateDirectory::SetCompileDefinitions(const BT<std::string>& vec)
{
  SetContent(this->DirectoryState->CompileDefinitions,
             this->Snapshot_.Position->CompileDefinitionsPosition, vec);
}

void cmStateDirectory::ClearCompileDefinitions()
{
  ClearContent(this->DirectoryState->CompileDefinitions,
               this->Snapshot_.Position->CompileDefinitionsPosition);
}

cmBTStringRange cmStateDirectory::GetCompileOptionsEntries() const
{
  return GetPropertyContent(this->DirectoryState->CompileOptions,
                            this->Snapshot_.Position->CompileOptionsPosition);
}

void cmStateDirectory::AppendCompileOptionsEntry(const BT<std::string>& vec)
{
  AppendEntry(this->DirectoryState->CompileOptions,
              this->Snapshot_.Position->CompileOptionsPosition, vec);
}

void cmStateDirectory::SetCompileOptions(const BT<std::string>& vec)
{
  SetContent(this->DirectoryState->CompileOptions,
             this->Snapshot_.Position->CompileOptionsPosition, vec);
}

void cmStateDirectory::ClearCompileOptions()
{
  ClearContent(this->DirectoryState->CompileOptions,
               this->Snapshot_.Position->CompileOptionsPosition);
}

cmBTStringRange cmStateDirectory::GetLinkOptionsEntries() const
{
  return GetPropertyContent(this->DirectoryState->LinkOptions,
                            this->Snapshot_.Position->LinkOptionsPosition);
}

void cmStateDirectory::AppendLinkOptionsEntry(const BT<std::string>& vec)
{
  AppendEntry(this->DirectoryState->LinkOptions,
              this->Snapshot_.Position->LinkOptionsPosition, vec);
}

void cmStateDirectory::SetLinkOptions(const BT<std::string>& vec)
{
  SetContent(this->DirectoryState->LinkOptions,
             this->Snapshot_.Position->LinkOptionsPosition, vec);
}

void cmStateDirectory::ClearLinkOptions()
{
  ClearContent(this->DirectoryState->LinkOptions,
               this->Snapshot_.Position->LinkOptionsPosition);
}

cmBTStringRange cmStateDirectory::GetLinkDirectoriesEntries() const
{
  return GetPropertyContent(this->DirectoryState->LinkDirectories,
                            this->Snapshot_.Position->LinkDirectoriesPosition);
}

void cmStateDirectory::AppendLinkDirectoriesEntry(const BT<std::string>& vec)
{
  AppendEntry(this->DirectoryState->LinkDirectories,
              this->Snapshot_.Position->LinkDirectoriesPosition, vec);
}
void cmStateDirectory::PrependLinkDirectoriesEntry(const BT<std::string>& vec)
{
  auto entryEnd = this->DirectoryState->LinkDirectories.begin() +
    this->Snapshot_.Position->LinkDirectoriesPosition;

  auto rend = this->DirectoryState->LinkDirectories.rend();
  auto rbegin = cm::make_reverse_iterator(entryEnd);
  rbegin = std::find(rbegin, rend, cmPropertySentinel);

  auto entryIt = rbegin.base();

  this->DirectoryState->LinkDirectories.insert(entryIt, vec);

  this->Snapshot_.Position->LinkDirectoriesPosition =
    this->DirectoryState->LinkDirectories.size();
}

void cmStateDirectory::SetLinkDirectories(const BT<std::string>& vec)
{
  SetContent(this->DirectoryState->LinkDirectories,
             this->Snapshot_.Position->LinkDirectoriesPosition, vec);
}

void cmStateDirectory::ClearLinkDirectories()
{
  ClearContent(this->DirectoryState->LinkDirectories,
               this->Snapshot_.Position->LinkDirectoriesPosition);
}

void cmStateDirectory::SetProperty(std::string const& prop, cmValue value,
                                   cmListFileBacktrace const& lfbt)
{
  if (prop == "INCLUDE_DIRECTORIES") {
    if (!value) {
      this->ClearIncludeDirectories();
      return;
    }
    this->SetIncludeDirectories(BT<std::string>(value, lfbt));
    return;
  }
  if (prop == "COMPILE_OPTIONS") {
    if (!value) {
      this->ClearCompileOptions();
      return;
    }
    this->SetCompileOptions(BT<std::string>(value, lfbt));
    return;
  }
  if (prop == "COMPILE_DEFINITIONS") {
    if (!value) {
      this->ClearCompileDefinitions();
      return;
    }
    this->SetCompileDefinitions(BT<std::string>(value, lfbt));
    return;
  }
  if (prop == "LINK_OPTIONS") {
    if (!value) {
      this->ClearLinkOptions();
      return;
    }
    this->SetLinkOptions(BT<std::string>(value, lfbt));
    return;
  }
  if (prop == "LINK_DIRECTORIES") {
    if (!value) {
      this->ClearLinkDirectories();
      return;
    }
    this->SetLinkDirectories(BT<std::string>(value, lfbt));
    return;
  }

  this->DirectoryState->Properties.SetProperty(prop, value);
}

void cmStateDirectory::AppendProperty(std::string const& prop,
                                      std::string const& value, bool asString,
                                      cmListFileBacktrace const& lfbt)
{
  if (prop == "INCLUDE_DIRECTORIES") {
    this->AppendIncludeDirectoriesEntry(BT<std::string>(value, lfbt));
    return;
  }
  if (prop == "COMPILE_OPTIONS") {
    this->AppendCompileOptionsEntry(BT<std::string>(value, lfbt));
    return;
  }
  if (prop == "COMPILE_DEFINITIONS") {
    this->AppendCompileDefinitionsEntry(BT<std::string>(value, lfbt));
    return;
  }
  if (prop == "LINK_OPTIONS") {
    this->AppendLinkOptionsEntry(BT<std::string>(value, lfbt));
    return;
  }
  if (prop == "LINK_DIRECTORIES") {
    this->AppendLinkDirectoriesEntry(BT<std::string>(value, lfbt));
    return;
  }

  this->DirectoryState->Properties.AppendProperty(prop, value, asString);
}

cmValue cmStateDirectory::GetProperty(std::string const& prop) const
{
  bool const chain =
    this->Snapshot_.m_pState->IsPropertyChained(prop, cmProperty::DIRECTORY);
  return this->GetProperty(prop, chain);
}

cmValue cmStateDirectory::GetProperty(std::string const& prop,
                                      bool chain) const
{
  static std::string output;
  output.clear();
  if (prop == "PARENT_DIRECTORY") {
    cmStateSnapshot parent = this->Snapshot_.GetBuildsystemDirectoryParent();
    if (parent.IsValid()) {
      return cmValue(parent.GetDirectory().GetCurrentSource());
    }
    return cmValue(output);
  }
  if (prop == kBINARY_DIR) {
    output = this->GetCurrentBinary();
    return cmValue(output);
  }
  if (prop == kSOURCE_DIR) {
    output = this->GetCurrentSource();
    return cmValue(output);
  }
  if (prop == kSUBDIRECTORIES) {
    std::vector<std::string> child_dirs;
    std::vector<cmStateSnapshot> const& children =
      this->DirectoryState->Children;
    child_dirs.reserve(children.size());
    for (cmStateSnapshot const& ci : children) {
      child_dirs.push_back(ci.GetDirectory().GetCurrentSource());
    }
    output = cmList::to_string(child_dirs);
    return cmValue(output);
  }
  if (prop == kBUILDSYSTEM_TARGETS) {
    output = cmList::to_string(this->DirectoryState->NormalTargetNames);
    return cmValue(output);
  }
  if (prop == "IMPORTED_TARGETS"_s) {
    output = cmList::to_string(this->DirectoryState->ImportedTargetNames);
    return cmValue(output);
  }

  if (prop == "LISTFILE_STACK") {
    std::vector<std::string> listFiles;
    cmStateSnapshot snp = this->Snapshot_;
    while (snp.IsValid()) {
      listFiles.push_back(snp.GetExecutionListFile());
      snp = snp.GetCallStackParent();
    }
    std::reverse(listFiles.begin(), listFiles.end());
    output = cmList::to_string(listFiles);
    return cmValue(output);
  }
  if (prop == "CACHE_VARIABLES") {
    output = cmList::to_string(this->Snapshot_.m_pState->GetCacheEntryKeys());
    return cmValue(output);
  }
  if (prop == "VARIABLES") {
    std::vector<std::string> res = this->Snapshot_.ClosureKeys();
    cm::append(res, this->Snapshot_.m_pState->GetCacheEntryKeys());
    std::sort(res.begin(), res.end());
    output = cmList::to_string(res);
    return cmValue(output);
  }
  if (prop == "INCLUDE_DIRECTORIES") {
    output = cmList::to_string(this->GetIncludeDirectoriesEntries());
    return cmValue(output);
  }
  if (prop == "COMPILE_OPTIONS") {
    output = cmList::to_string(this->GetCompileOptionsEntries());
    return cmValue(output);
  }
  if (prop == "COMPILE_DEFINITIONS") {
    output = cmList::to_string(this->GetCompileDefinitionsEntries());
    return cmValue(output);
  }
  if (prop == "LINK_OPTIONS") {
    output = cmList::to_string(this->GetLinkOptionsEntries());
    return cmValue(output);
  }
  if (prop == "LINK_DIRECTORIES") {
    output = cmList::to_string(this->GetLinkDirectoriesEntries());
    return cmValue(output);
  }

  cmValue retVal = this->DirectoryState->Properties.GetPropertyValue(prop);
  if (!retVal && chain) {
    cmStateSnapshot parentSnapshot =
      this->Snapshot_.GetBuildsystemDirectoryParent();
    if (parentSnapshot.IsValid()) {
      return parentSnapshot.GetDirectory().GetProperty(prop, chain);
    }
    return this->Snapshot_.m_pState->GetGlobalProperty(prop);
  }

  return retVal;
}

bool cmStateDirectory::GetPropertyAsBool(std::string const& prop) const
{
  return this->GetProperty(prop).IsOn();
}

std::vector<std::string> cmStateDirectory::GetPropertyKeys() const
{
  return this->DirectoryState->Properties.GetKeys();
}

void cmStateDirectory::AddNormalTargetName(std::string const& name)
{
  this->DirectoryState->NormalTargetNames.push_back(name);
}

void cmStateDirectory::AddImportedTargetName(std::string const& name)
{
  this->DirectoryState->ImportedTargetNames.emplace_back(name);
}
