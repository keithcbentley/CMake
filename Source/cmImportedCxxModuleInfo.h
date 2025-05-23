/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <map>
#include <string>
#include <vector>

struct ImportedCxxModuleInfo
{
  std::string const m_name;
  std::vector<std::string> const AvailableBmis;
};

struct ImportedCxxModuleGeneratorInfo
{
  std::string const BmiName;
};

struct ImportedCxxModuleLookup
{
  ImportedCxxModuleLookup() = default;
  ~ImportedCxxModuleLookup() = default;

  bool Initialized() const;
  void Initialize(std::string const& importedModules);

  std::string BmiNameForSource(std::string const& path);

private:
  bool DoneInit = false;
  std::map<std::string, ImportedCxxModuleInfo> ImportedInfo;
  std::map<std::string, ImportedCxxModuleGeneratorInfo> GeneratorInfo;
};
