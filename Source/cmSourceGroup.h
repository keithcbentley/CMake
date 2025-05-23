/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "cmsys/RegularExpression.hxx"

class cmSourceFile;
class cmSourceGroupInternals;

/** \class cmSourceGroup
 * \brief Hold a group of sources as specified by a SOURCE_GROUP command.
 *
 * cmSourceGroup holds a regular expression and a list of files.  When
 * local generators are about to generate the rules for a target's
 * files, the set of source groups is consulted to group files
 * together.  A file is placed into the last source group that lists
 * the file by name.  If no group lists the file, it is placed into
 * the last group whose regex matches it.
 */
class cmSourceGroup
{
public:
  cmSourceGroup(std::string name, char const* regex,
                char const* parentName = nullptr);
  cmSourceGroup(cmSourceGroup const& r);
  ~cmSourceGroup();
  cmSourceGroup& operator=(cmSourceGroup const&);

  /**
   * Set the regular expression for this group.
   */
  void SetGroupRegex(char const* regex);

  /**
   * Add a file name to the explicit list of files for this group.
   */
  void AddGroupFile(std::string const& name);

  /**
   * Add child to this sourcegroup
   */
  void AddChild(cmSourceGroup const& child);

  /**
   * Looks up child and returns it
   */
  cmSourceGroup* LookupChild(std::string const& name);

  /**
   * Get the name of this group.
   */
  std::string const& GetName() const;

  /**
   * Get the full path name for group.
   */
  std::string const& GetFullName() const;

  /**
   * Check if the given name matches this group's regex.
   */
  bool MatchesRegex(std::string const& name);

  /**
   * Check if the given name matches this group's explicit file list.
   */
  bool MatchesFiles(std::string const& name) const;

  /**
   * Check if the given name matches this group's explicit file list
   * in children.
   */
  cmSourceGroup* MatchChildrenFiles(std::string const& name);

  /**
   * Check if the given name matches this group's explicit file list
   * in children.
   */
  cmSourceGroup const* MatchChildrenFiles(std::string const& name) const;

  /**
   * Check if the given name matches this group's regex in children.
   */
  cmSourceGroup* MatchChildrenRegex(std::string const& name);

  /**
   * Assign the given source file to this group.  Used only by
   * generators.
   */
  void AssignSource(cmSourceFile const* sf);

  /**
   * Get the list of the source files that have been assigned to this
   * source group.
   */
  std::vector<cmSourceFile const*> const& GetSourceFiles() const;

  std::vector<cmSourceGroup> const& GetGroupChildren() const;

private:
  /**
   * The name of the source group.
   */
  std::string m_name;
  // Full path to group
  std::string FullName;

  /**
   * The regular expression matching the files in the group.
   */
  cmsys::RegularExpression GroupRegex;

  /**
   * Set of file names explicitly added to this group.
   */
  std::set<std::string> GroupFiles;

  /**
   * Vector of all source files that have been assigned to
   * this group.
   */
  std::vector<cmSourceFile const*> m_sourceFiles;

  std::unique_ptr<cmSourceGroupInternals> Internal;
};
