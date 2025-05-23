/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <iosfwd>
#include <map>
#include <string>
#include <vector>

#include "cmCTestGlobalVC.h"

class cmCTest;
class cmMakefile;

/** \class cmCTestP4
 * \brief Interaction with the Perforce command-line tool
 *
 */
class cmCTestP4 : public cmCTestGlobalVC
{
public:
  /** Construct with a CTest instance and update log stream.  */
  cmCTestP4(cmCTest* ctest, cmMakefile* mf, std::ostream& log);

  ~cmCTestP4() override;

private:
  std::vector<std::string> ChangeLists;

  struct User
  {
    std::string UserName;
    std::string m_name;
    std::string EMail;
    std::string AccessTime;
  };
  std::map<std::string, User> Users;
  std::vector<std::string> P4Options;

  User GetUserData(std::string const& username);
  void SetP4Options(std::vector<std::string>& options);

  std::string GetWorkingRevision();
  bool NoteOldRevision() override;
  bool NoteNewRevision() override;
  bool UpdateImpl() override;
  bool UpdateCustom(std::string const& custom);

  bool LoadRevisions() override;
  bool LoadModifications() override;

  class ChangesParser;
  class DescribeParser;
  class DiffParser;
  // Parsing helper classes.
  class IdentifyParser;
  class UserParser;

  friend class IdentifyParser;
  friend class ChangesParser;
  friend class UserParser;
  friend class DescribeParser;
  friend class DiffParser;
};
