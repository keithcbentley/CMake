/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmSourceGroup.h"

#include <utility>

#include <cm/memory>

#include "cmStringAlgorithms.h"

class cmSourceGroupInternals
{
public:
  std::vector<cmSourceGroup> GroupChildren;
};

cmSourceGroup::cmSourceGroup(std::string name, char const* regex,
                             char const* parentName)
  : m_name(std::move(name))
{
  this->Internal = cm::make_unique<cmSourceGroupInternals>();
  this->SetGroupRegex(regex);
  if (parentName) {
    this->FullName = cmStrCat(parentName, '\\');
  }
  this->FullName += this->m_name;
}

cmSourceGroup::~cmSourceGroup() = default;

cmSourceGroup::cmSourceGroup(cmSourceGroup const& r)
{
  this->m_name = r.m_name;
  this->FullName = r.FullName;
  this->GroupRegex = r.GroupRegex;
  this->GroupFiles = r.GroupFiles;
  this->m_sourceFiles = r.m_sourceFiles;
  this->Internal = cm::make_unique<cmSourceGroupInternals>(*r.Internal);
}

cmSourceGroup& cmSourceGroup::operator=(cmSourceGroup const& r)
{
  this->m_name = r.m_name;
  this->GroupRegex = r.GroupRegex;
  this->GroupFiles = r.GroupFiles;
  this->m_sourceFiles = r.m_sourceFiles;
  *(this->Internal) = *(r.Internal);
  return *this;
}

void cmSourceGroup::SetGroupRegex(char const* regex)
{
  if (regex) {
    this->GroupRegex.compile(regex);
  } else {
    this->GroupRegex.compile("^$");
  }
}

void cmSourceGroup::AddGroupFile(std::string const& name)
{
  this->GroupFiles.insert(name);
}

std::string const& cmSourceGroup::GetName() const
{
  return this->m_name;
}

std::string const& cmSourceGroup::GetFullName() const
{
  return this->FullName;
}

bool cmSourceGroup::MatchesRegex(std::string const& name)
{
  return this->GroupRegex.find(name);
}

bool cmSourceGroup::MatchesFiles(std::string const& name) const
{
  return this->GroupFiles.find(name) != this->GroupFiles.cend();
}

void cmSourceGroup::AssignSource(cmSourceFile const* sf)
{
  this->m_sourceFiles.push_back(sf);
}

std::vector<cmSourceFile const*> const& cmSourceGroup::GetSourceFiles() const
{
  return this->m_sourceFiles;
}

void cmSourceGroup::AddChild(cmSourceGroup const& child)
{
  this->Internal->GroupChildren.push_back(child);
}

cmSourceGroup* cmSourceGroup::LookupChild(std::string const& name)
{
  for (cmSourceGroup& group : this->Internal->GroupChildren) {
    // look if descenened is the one were looking for
    if (group.GetName() == name) {
      return (&group); // if it so return it
    }
  }

  // if no child with this name was found return NULL
  return nullptr;
}

cmSourceGroup* cmSourceGroup::MatchChildrenFiles(std::string const& name)
{
  if (this->MatchesFiles(name)) {
    return this;
  }
  for (cmSourceGroup& group : this->Internal->GroupChildren) {
    cmSourceGroup* result = group.MatchChildrenFiles(name);
    if (result) {
      return result;
    }
  }
  return nullptr;
}

cmSourceGroup const* cmSourceGroup::MatchChildrenFiles(
  std::string const& name) const
{
  if (this->MatchesFiles(name)) {
    return this;
  }
  for (cmSourceGroup const& group : this->Internal->GroupChildren) {
    cmSourceGroup const* result = group.MatchChildrenFiles(name);
    if (result) {
      return result;
    }
  }
  return nullptr;
}

cmSourceGroup* cmSourceGroup::MatchChildrenRegex(std::string const& name)
{
  for (cmSourceGroup& group : this->Internal->GroupChildren) {
    cmSourceGroup* result = group.MatchChildrenRegex(name);
    if (result) {
      return result;
    }
  }
  if (this->MatchesRegex(name)) {
    return this;
  }

  return nullptr;
}

std::vector<cmSourceGroup> const& cmSourceGroup::GetGroupChildren() const
{
  return this->Internal->GroupChildren;
}
