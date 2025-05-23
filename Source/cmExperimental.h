/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */

#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <string>
#include <vector>

#include <cm/optional>

class cmMakefile;

class cmExperimental
{
public:
  enum class Feature
  {
    ExportPackageDependencies,
    WindowsKernelModeDriver,
    CxxImportStd,
    ImportPackageInfo,
    ExportPackageInfo,
    ExportBuildDatabase,
    m_pInstrumentation,

    Sentinel,
  };

  enum class TryCompileCondition
  {
    Always,
    SkipCompilerChecks,
    Never,
  };

  struct FeatureData
  {
    std::string const m_name;
    std::string const Uuid;
    std::string const Variable;
    std::string const Description;
    std::vector<std::string> const TryCompileVariables;
    TryCompileCondition const ForwardThroughTryCompile;
    bool Warned;
  };

  static FeatureData const& DataForFeature(Feature f);
  static cm::optional<Feature> FeatureByName(std::string const& name);
  static bool HasSupportEnabled(cmMakefile const& mf, Feature f);
};
