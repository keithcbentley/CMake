/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <iosfwd>
#include <map>
#include <set>
#include <string>
#include <vector>

class cmFileTimeCache;
class cmLocalUnixMakefileGenerator3;

/** \class cmDepends
 * \brief Dependency scanner superclass.
 *
 * This class is responsible for maintaining a .depends.make file in
 * the build tree corresponding to an object file.  Subclasses help it
 * maintain dependencies for particular languages.
 */
class cmDepends
{
public:
  using DependencyMap = std::map<std::string, std::vector<std::string>>;

  /** Instances need to know the build directory name and the relative
      path from the build directory to the target file.  */
  cmDepends(cmLocalUnixMakefileGenerator3* lg = nullptr,
            std::string targetDir = "");

  cmDepends(cmDepends const&) = delete;
  cmDepends& operator=(cmDepends const&) = delete;

  /** Set the local generator for the directory in which we are
      scanning dependencies.  This is not a full local generator; it
      has been setup to do relative path conversions for the current
      directory.  */
  void SetLocalGenerator(cmLocalUnixMakefileGenerator3* lg)
  {
    this->LocalGenerator = lg;
  }

  /** Set the specific language to be scanned.  */
  void SetLanguage(std::string const& lang) { this->Language = lang; }

  /** Set the target build directory.  */
  void SetTargetDirectory(std::string const& dir)
  {
    this->TargetDirectory = dir;
  }

  /** should this be verbose in its output */
  void SetVerbose(bool verb) { this->Verbose = verb; }

  /** Virtual destructor to cleanup subclasses properly.  */
  virtual ~cmDepends();

  /** Write dependencies for the target file.  */
  bool Write(std::ostream& makeDepends, std::ostream& internalDepends);

  /** Check dependencies for the target file.  Returns true if
      dependencies are okay and false if they must be generated.  If
      they must be generated Clear has already been called to wipe out
      the old dependencies.
      Dependencies which are still valid will be stored in validDeps. */
  bool Check(std::string const& makeFile, std::string const& internalFile,
             DependencyMap& validDeps);

  /** Clear dependencies for the target file so they will be regenerated.  */
  void Clear(std::string const& file) const;

  /** Set the file comparison object */
  void SetFileTimeCache(cmFileTimeCache* fc) { this->m_fileTimeCache = fc; }

protected:
  // Write dependencies for the target file to the given stream.
  // Return true for success and false for failure.
  virtual bool WriteDependencies(std::set<std::string> const& sources,
                                 std::string const& obj,
                                 std::ostream& makeDepends,
                                 std::ostream& internalDepends);

  // Check dependencies for the target file in the given stream.
  // Return false if dependencies must be regenerated and true
  // otherwise.
  virtual bool CheckDependencies(std::istream& internalDepends,
                                 std::string const& internalDependsFileName,
                                 DependencyMap& validDeps);

  // Finalize the dependency information for the target.
  virtual bool Finalize(std::ostream& makeDepends,
                        std::ostream& internalDepends);

  // The local generator.
  cmLocalUnixMakefileGenerator3* LocalGenerator;

  // Flag for verbose output.
  bool Verbose = false;
  cmFileTimeCache* m_fileTimeCache = nullptr;

  std::string Language;

  // The full path to the target's build directory.
  std::string TargetDirectory;

  // The include file search path.
  std::vector<std::string> IncludePath;

  void SetIncludePathFromLanguage(std::string const& lang);
};
