/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmExtraKateGenerator.h"

#include <cstring>
#include <memory>
#include <ostream>
#include <set>
#include <vector>

#include "cmCMakePath.h"
#include "cmGeneratedFileStream.h"
#include "cmGeneratorTarget.h"
#include "cmGlobalGenerator.h"
#include "cmLocalGenerator.h"
#include "cmMakefile.h"
#include "cmSourceFile.h"
#include "cmStateTypes.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmValue.h"

cmExtraKateGenerator::cmExtraKateGenerator() = default;

cmExternalMakefileProjectGeneratorFactory* cmExtraKateGenerator::GetFactory()
{
  static cmExternalMakefileProjectGeneratorSimpleFactory<cmExtraKateGenerator>
    factory("Kate", "Generates Kate project files (deprecated).");

  if (factory.GetSupportedGlobalGenerators().empty()) {
#if defined(_WIN32)
    factory.AddSupportedGlobalGenerator("MinGW Makefiles");
    factory.AddSupportedGlobalGenerator("NMake Makefiles");
// disable until somebody actually tests it:
// factory.AddSupportedGlobalGenerator("MSYS Makefiles");
#endif
    factory.AddSupportedGlobalGenerator("Ninja");
    factory.AddSupportedGlobalGenerator("Ninja Multi-Config");
    factory.AddSupportedGlobalGenerator("Unix Makefiles");
  }

  return &factory;
}

void cmExtraKateGenerator::Generate()
{
  auto const& lg = this->m_pGlobalGenerator->GetLocalGenerators()[0];
  cmMakefile const* mf = lg->GetMakefile();
  this->ProjectName = this->GenerateProjectName(
    lg->GetProjectName(), mf->GetSafeDefinition("CMAKE_BUILD_TYPE"),
    this->GetPathBasename(lg->GetBinaryDirectory()));
  this->UseNinja =
    ((this->m_pGlobalGenerator->GetName() == "Ninja") ||
     (this->m_pGlobalGenerator->GetName() == "Ninja Multi-Config"));

  this->CreateKateProjectFile(*lg);
  this->CreateDummyKateProjectFile(*lg);
}

void cmExtraKateGenerator::CreateKateProjectFile(
  cmLocalGenerator const& lg) const
{
  std::string filename = cmStrCat(lg.GetBinaryDirectory(), "/.kateproject");
  cmGeneratedFileStream fout(filename);
  if (!fout) {
    return;
  }

  /* clang-format off */
  fout <<
    "{\n"
    "\t\"name\": \"" << this->ProjectName << "\",\n"
    "\t\"directory\": \"" << lg.GetSourceDirectory() << "\",\n"
    "\t\"files\": [ { " << this->GenerateFilesString(lg) << "} ],\n";
  /* clang-format on */
  this->WriteTargets(lg, fout);
  fout << "}\n";
}

void cmExtraKateGenerator::WriteTargets(cmLocalGenerator const& lg,
                                        cmGeneratedFileStream& fout) const
{
  cmMakefile const* mf = lg.GetMakefile();
  std::string const& make = mf->GetRequiredDefinition("CMAKE_MAKE_PROGRAM");
  std::string const& makeArgs =
    mf->GetSafeDefinition("CMAKE_KATE_MAKE_ARGUMENTS");
  std::string const& homeOutputDir = lg.GetBinaryDirectory();
  auto const configs = mf->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig);

  /* clang-format off */
  fout <<
  "\t\"build\": {\n"
  "\t\t\"directory\": \"" << homeOutputDir << "\",\n"
  "\t\t\"default_target\": \"all\",\n"
  "\t\t\"clean_target\": \"clean\",\n";
  /* clang-format on */

  // build, clean and quick are for the build plugin kate <= 4.12:
  fout << "\t\t\"build\": \"" << make << " -C \\\"" << homeOutputDir << "\\\" "
       << makeArgs << " "
       << "all\",\n";
  fout << "\t\t\"clean\": \"" << make << " -C \\\"" << homeOutputDir << "\\\" "
       << makeArgs << " "
       << "clean\",\n";
  fout << "\t\t\"quick\": \"" << make << " -C \\\"" << homeOutputDir << "\\\" "
       << makeArgs << " "
       << "install\",\n";

  // this is for kate >= 4.13:
  fout << "\t\t\"targets\":[\n";

  this->AppendTarget(fout, "all", configs, make, makeArgs, homeOutputDir,
                     homeOutputDir);
  this->AppendTarget(fout, "clean", configs, make, makeArgs, homeOutputDir,
                     homeOutputDir);

  // add all executable and library targets and some of the GLOBAL
  // and UTILITY targets
  for (auto const& localGen : this->m_pGlobalGenerator->GetLocalGenerators()) {
    auto const& targets = localGen->GetGeneratorTargets();
    std::string const currentDir = localGen->GetCurrentBinaryDirectory();
    bool topLevel = (currentDir == localGen->GetBinaryDirectory());

    for (auto const& target : targets) {
      std::string const& targetName = target->GetName();
      switch (target->GetType()) {
        case cmStateEnums::GLOBAL_TARGET: {
          bool insertTarget = false;
          // Only add the global targets from CMAKE_BINARY_DIR,
          // not from the subdirs
          if (topLevel) {
            insertTarget = true;
            // only add the "edit_cache" target if it's not ccmake, because
            // this will not work within the IDE
            if (targetName == "edit_cache") {
              cmValue editCommand =
                localGen->GetMakefile()->GetDefinition("CMAKE_EDIT_COMMAND");
              if (!editCommand ||
                  strstr(editCommand->c_str(), "ccmake") != nullptr) {
                insertTarget = false;
              }
            }
          }
          if (insertTarget) {
            this->AppendTarget(fout, targetName, configs, make, makeArgs,
                               currentDir, homeOutputDir);
          }
        } break;
        case cmStateEnums::UTILITY:
          // Add all utility targets, except the Nightly/Continuous/
          // Experimental-"sub"targets as e.g. NightlyStart
          if ((cmHasLiteralPrefix(targetName, "Nightly") &&
               (targetName != "Nightly")) ||
              (cmHasLiteralPrefix(targetName, "Continuous") &&
               (targetName != "Continuous")) ||
              (cmHasLiteralPrefix(targetName, "Experimental") &&
               (targetName != "Experimental"))) {
            break;
          }

          this->AppendTarget(fout, targetName, configs, make, makeArgs,
                             currentDir, homeOutputDir);
          break;
        case cmStateEnums::EXECUTABLE:
        case cmStateEnums::STATIC_LIBRARY:
        case cmStateEnums::SHARED_LIBRARY:
        case cmStateEnums::MODULE_LIBRARY:
        case cmStateEnums::OBJECT_LIBRARY: {
          this->AppendTarget(fout, targetName, configs, make, makeArgs,
                             currentDir, homeOutputDir);
          if (!this->UseNinja) {
            std::string fastTarget = cmStrCat(targetName, "/fast");
            this->AppendTarget(fout, fastTarget, configs, make, makeArgs,
                               currentDir, homeOutputDir);
          }

        } break;
        default:
          break;
      }
    }

    // insert rules for compiling, preprocessing and assembling individual
    // files
    std::vector<std::string> objectFileTargets;
    localGen->GetIndividualFileTargets(objectFileTargets);
    for (std::string const& f : objectFileTargets) {
      this->AppendTarget(fout, f, configs, make, makeArgs, currentDir,
                         homeOutputDir);
    }
  }

  fout << "\t] }\n";
}

void cmExtraKateGenerator::AppendTarget(
  cmGeneratedFileStream& fout, std::string const& target,
  std::vector<std::string> const& configs, std::string const& make,
  std::string const& makeArgs, std::string const& path,
  std::string const& homeOutputDir) const
{
  static char JsonSep = ' ';

  for (std::string const& conf : configs) {
    fout << "\t\t\t" << JsonSep << R"({"name":")" << target
         << ((configs.size() > 1) ? (std::string(":") + conf) : std::string())
         << "\", "
            "\"build_cmd\":\""
         << make << " -C \\\"" << (this->UseNinja ? homeOutputDir : path)
         << "\\\" "
         << ((this->UseNinja && configs.size() > 1)
               ? std::string(" -f build-") + conf + ".ninja"
               : std::string())
         << makeArgs << " " << target << "\"}\n";

    JsonSep = ',';
  }
}

void cmExtraKateGenerator::CreateDummyKateProjectFile(
  cmLocalGenerator const& lg) const
{
  std::string filename =
    cmStrCat(lg.GetBinaryDirectory(), '/', this->ProjectName, ".kateproject");
  cmGeneratedFileStream fout(filename);
  if (!fout) {
    return;
  }

  fout << "#Generated by " << cmSystemTools::GetCMakeCommand()
       << ", do not edit.\n";
}

std::string cmExtraKateGenerator::GenerateFilesString(
  cmLocalGenerator const& lg) const
{
  cmMakefile const* mf = lg.GetMakefile();
  std::string mode =
    cmSystemTools::UpperCase(mf->GetSafeDefinition("CMAKE_KATE_FILES_MODE"));
  static std::string const gitString = "\"git\": 1 ";
  static std::string const svnString = "\"svn\": 1 ";
  static std::string const hgString = "\"hg\": 1 ";
  static std::string const fossilString = "\"fossil\": 1 ";

  if (mode == "SVN") {
    return svnString;
  }
  if (mode == "GIT") {
    return gitString;
  }
  if (mode == "HG") {
    return hgString;
  }
  if (mode == "FOSSIL") {
    return fossilString;
  }

  // check for the VCS files except when "forced" to "FILES" mode:
  if (mode != "LIST") {
    cmCMakePath startDir(lg.GetSourceDirectory(), cmCMakePath::auto_format);
    // move the directories up to the root directory to see whether we are in
    // a subdir of a svn, git, hg or fossil checkout
    for (;;) {
      std::string s = startDir.String() + "/.git";
      if (cmSystemTools::FileExists(s)) {
        return gitString;
      }

      s = startDir.String() + "/.svn";
      if (cmSystemTools::FileExists(s)) {
        return svnString;
      }

      s = startDir.String() + "/.hg";
      if (cmSystemTools::FileExists(s)) {
        return hgString;
      }
      s = startDir.String() + "/.fslckout";
      if (cmSystemTools::FileExists(s)) {
        return fossilString;
      }

      if (!startDir.HasRelativePath()) { // have we reached the root dir ?
        break;
      }
      startDir = startDir.GetParentPath();
    }
  }

  std::set<std::string> files;
  std::string tmp;
  auto const& lgs = this->m_pGlobalGenerator->GetLocalGenerators();

  for (auto const& lgen : lgs) {
    cmMakefile* makefile = lgen->GetMakefile();
    std::vector<std::string> const& listFiles = makefile->GetListFiles();
    for (std::string const& listFile : listFiles) {
      if (listFile.find("/CMakeFiles/") == std::string::npos) {
        files.insert(listFile);
      }
    }

    for (auto const& sf : makefile->GetSourceFiles()) {
      if (sf->GetIsGenerated()) {
        continue;
      }

      tmp = sf->ResolveFullPath();
      files.insert(tmp);
    }
  }

  char const* sep = "";
  tmp = "\"list\": [";
  for (std::string const& f : files) {
    tmp += sep;
    tmp += " \"";
    tmp += f;
    tmp += "\"";
    sep = ",";
  }
  tmp += "] ";

  return tmp;
}

std::string cmExtraKateGenerator::GenerateProjectName(
  std::string const& name, std::string const& type,
  std::string const& path) const
{
  return name + (type.empty() ? "" : "-") + type + '@' + path;
}

std::string cmExtraKateGenerator::GetPathBasename(
  std::string const& path) const
{
  std::string outputBasename = path;
  while (!outputBasename.empty() &&
         (outputBasename.back() == '/' || outputBasename.back() == '\\')) {
    outputBasename.resize(outputBasename.size() - 1);
  }
  std::string::size_type loc = outputBasename.find_last_of("/\\");
  if (loc != std::string::npos) {
    outputBasename = outputBasename.substr(loc + 1);
  }

  return outputBasename;
}
