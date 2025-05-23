/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <memory>
#include <string>
#include <vector>

#include "cmCustomCommand.h"
#include "cmListFileCache.h"
#include "cmPropertyMap.h"
#include "cmSourceFileLocation.h"
#include "cmSourceFileLocationKind.h"
#include "cmValue.h"

class cmMakefile;

/** \class cmSourceFile
 * \brief Represent a class loaded from a makefile.
 *
 * cmSourceFile represents a class loaded from a makefile.
 */
class cmSourceFile
{
public:
  /**
   * Construct with the makefile storing the source and the initial name
   * referencing it. If it shall be marked as generated, this source file's
   * kind is assumed to be known, regardless of the given value.
   */
  cmSourceFile(
    cmMakefile* mf, std::string const& name, bool generated,
    cmSourceFileLocationKind kind = cmSourceFileLocationKind::Ambiguous);

  /**
   * Get the custom command for this source file
   */
  cmCustomCommand* GetCustomCommand() const;
  void SetCustomCommand(std::unique_ptr<cmCustomCommand> m_pCustomCommand);

  //! Set/Get a property of this source file
  void SetProperty(std::string const& prop, cmValue value);
  void RemoveProperty(std::string const& prop)
  {
    this->SetProperty(prop, cmValue{ nullptr });
  }
  void SetProperty(std::string const& prop, std::string const& value)
  {
    this->SetProperty(prop, cmValue(value));
  }
  void AppendProperty(std::string const& prop, std::string const& value,
                      bool asString = false);
  //! Might return a nullptr if the property is not set or invalid
  cmValue GetProperty(std::string const& prop) const;
  //! Always returns a valid pointer
  std::string const& GetSafeProperty(std::string const& prop) const;
  bool GetPropertyAsBool(std::string const& prop) const;

  /** Implement getting a property when called from a CMake language
      command like get_property or get_source_file_property.  */
  cmValue GetPropertyForUser(std::string const& prop);

  /// Marks this file as generated
  /**
   * This stores this file's path in the global table for all generated source
   * files.
   */
  void MarkAsGenerated();
  enum class CheckScope
  {
    Global,
    GlobalAndLocal
  };
  /// Determines if this source file is marked as generated.
  /**
   * This will check if this file's path is stored in the global table of all
   * generated source files. If that is not the case and checkScope is set to
   * GlobalAndLocal the value of the possibly existing local GENERATED property
   * is returned instead.
   * @param checkScope Determines if alternatively for backwards-compatibility
   * a local GENERATED property should be considered, too.
   * @return true if this source file is marked as generated, otherwise false.
   */
  bool GetIsGenerated(
    CheckScope checkScope = CheckScope::GlobalAndLocal) const;

  std::vector<BT<std::string>> const& GetCompileOptions() const
  {
    return this->CompileOptions;
  }

  std::vector<BT<std::string>> const& GetCompileDefinitions() const
  {
    return this->CompileDefinitions;
  }

  std::vector<BT<std::string>> const& GetIncludeDirectories() const
  {
    return this->IncludeDirectories;
  }

  /**
   * Resolves the full path to the file.  Attempts to locate the file on disk
   * and finalizes its location.
   */
  std::string const& ResolveFullPath(std::string* error = nullptr,
                                     std::string* cmp0115Warning = nullptr);

  /**
   * The resolved full path to the file.  The returned file name might be empty
   * if the path has not yet been resolved.
   */
  std::string const& GetFullPath() const;

  /**
   * Get the information currently known about the source file
   * location without attempting to locate the file as GetFullPath
   * would.  See cmSourceFileLocation documentation.
   */
  cmSourceFileLocation const& GetLocation() const;

  /**
   * Get the file extension of this source file.
   */
  std::string const& GetExtension() const;

  /**
   * Get the language of the compiler to use for this source file.
   */
  std::string const& GetOrDetermineLanguage();
  std::string GetLanguage() const;

  /**
   * Return the vector that holds the list of dependencies
   */
  std::vector<std::string> const& GetDepends() const { return this->Depends; }
  void AddDepend(std::string const& d) { this->Depends.push_back(d); }

  // Get the properties
  cmPropertyMap const& GetProperties() const { return this->Properties; }
  // Set the properties
  void SetProperties(cmPropertyMap properties);

  /**
   * Check whether the given source file location could refer to this
   * source.
   */
  bool Matches(cmSourceFileLocation const&);

  void SetObjectLibrary(std::string const& objlib);
  std::string GetObjectLibrary() const;

private:
  cmSourceFileLocation Location;
  cmPropertyMap Properties;
  std::unique_ptr<cmCustomCommand> CustomCommand;
  std::string Extension;
  std::string Language;
  std::string FullPath;
  std::string ObjectLibrary;
  std::vector<std::string> Depends;
  std::vector<BT<std::string>> CompileOptions;
  std::vector<BT<std::string>> CompileDefinitions;
  std::vector<BT<std::string>> IncludeDirectories;
  bool FindFullPathFailed = false;
  bool IsGenerated = false;

  bool FindFullPath(std::string* error, std::string* cmp0115Warning);
  void CheckExtension();
  void CheckLanguage(std::string const& ext);

  static std::string const propLANGUAGE;
  static std::string const propLOCATION;
  static std::string const propGENERATED;
  static std::string const propCOMPILE_DEFINITIONS;
  static std::string const propCOMPILE_OPTIONS;
  static std::string const propINCLUDE_DIRECTORIES;
};

// TODO: Factor out into platform information modules.
#define CM_HEADER_REGEX "\\.(h|hh|h\\+\\+|hm|hpp|hxx|in|txx|inl)$"

#define CM_SOURCE_REGEX                                                       \
  "\\.(C|F|M|c|c\\+\\+|cc|cpp|mpp|cxx|ixx|cppm|ccm|cxxm|c\\+\\+m|cu"          \
  "|f|f90|for|fpp|ftn|m|mm|rc|def|r|odl|idl|hpj|bat)$"

#define CM_PCH_REGEX "cmake_pch(_[^.]+)?\\.(h|hxx)$"

#define CM_RESOURCE_REGEX "\\.(pdf|plist|png|jpeg|jpg|storyboard|xcassets)$"
