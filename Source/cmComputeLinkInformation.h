/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cmsys/RegularExpression.hxx"

#include "cmComputeLinkDepends.h"
#include "cmListFileCache.h"
#include "cmValue.h"

class cmGeneratorTarget;
class cmGlobalGenerator;
class cmMakefile;
class cmOrderDirectories;
class cmSourceFile;
class CMake;

/** \class cmComputeLinkInformation
 * \brief Compute link information for a target in one configuration.
 */
class cmComputeLinkInformation
{
private:
  class FeatureDescriptor;

public:
  cmComputeLinkInformation(cmGeneratorTarget const* target,
                           std::string const& config);
  cmComputeLinkInformation(cmComputeLinkInformation const&) = delete;
  cmComputeLinkInformation& operator=(cmComputeLinkInformation const&) =
    delete;
  ~cmComputeLinkInformation();
  bool Compute();

  enum class ItemIsPath
  {
    No,
    Yes,
  };

  struct Item
  {
    Item(BT<std::string> v, ItemIsPath isPath,
         cmGeneratorTarget const* target = nullptr,
         cmSourceFile const* objectSource = nullptr,
         FeatureDescriptor const* feature = nullptr)
      : Value(std::move(v))
      , IsPath(isPath)
      , Target(target)
      , ObjectSource(objectSource)
      , Feature(feature)
    {
    }
    BT<std::string> Value;
    ItemIsPath IsPath = ItemIsPath::No;
    cmGeneratorTarget const* Target = nullptr;
    // The source file representing the external object (used when linking
    // `$<TARGET_OBJECTS>`)
    cmSourceFile const* ObjectSource = nullptr;

    bool HasFeature() const { return this->Feature != nullptr; }
    std::string const& GetFeatureName() const
    {
      return HasFeature() ? this->Feature->m_name
                          : cmComputeLinkDepends::LinkEntry::DEFAULT;
    }

    BT<std::string> GetFormattedItem(std::string const& path) const
    {
      return { this->Feature
                 ? this->Feature->GetDecoratedItem(path, this->IsPath)
                 : path,
               Value.m_backtrace };
    }

  private:
    FeatureDescriptor const* Feature = nullptr;
  };
  using ItemVector = std::vector<Item>;
  void AppendValues(std::string& result, std::vector<BT<std::string>>& values);
  ItemVector const& GetItems() const;
  std::vector<std::string> const& GetDirectories() const;
  std::vector<BT<std::string>> GetDirectoriesWithBacktraces();
  std::vector<std::string> const& GetDepends() const;
  std::vector<std::string> const& GetFrameworkPaths() const;
  std::set<std::string> const& GetFrameworkPathsEmitted() const;
  std::vector<std::string> const& GetXcFrameworkHeaderPaths() const;
  std::string GetLinkLanguage() const { return this->LinkLanguage; }
  std::vector<std::string> const& GetRuntimeSearchPath() const;
  std::string const& GetRuntimeFlag() const { return this->RuntimeFlag; }
  std::string const& GetRuntimeSep() const { return this->RuntimeSep; }
  void GetRPath(std::vector<std::string>& runtimeDirs, bool for_install) const;
  std::string GetRPathString(bool for_install) const;
  std::string GetChrpathString() const;
  std::set<cmGeneratorTarget const*> const& GetSharedLibrariesLinked() const;
  std::vector<cmGeneratorTarget const*> const& GetExternalObjectTargets()
    const;
  std::vector<cmGeneratorTarget const*> const& GetRuntimeDLLs() const
  {
    return this->RuntimeDLLs;
  }

  std::string const& GetLibLinkFileFlag() const
  {
    return this->LibLinkFileFlag;
  }

  std::string const& GetObjLinkFileFlag() const
  {
    return this->ObjLinkFileFlag;
  }

  std::string const& GetRPathLinkFlag() const { return this->RPathLinkFlag; }
  std::string GetRPathLinkString() const;

  std::string GetConfig() const { return this->Config; }

  cmGeneratorTarget const* GetTarget() { return this->Target; }

private:
  using LinkEntry = cmComputeLinkDepends::LinkEntry;

  void AddItem(LinkEntry const& entry);
  void AddSharedDepItem(LinkEntry const& entry);
  void AddRuntimeDLL(cmGeneratorTarget const* tgt);

  // Output information.
  ItemVector Items;
  std::vector<std::string> Directories;
  std::vector<std::string> Depends;
  std::vector<std::string> FrameworkPaths;
  std::vector<std::string> XcFrameworkHeaderPaths;
  std::vector<std::string> RuntimeSearchPath;
  std::set<cmGeneratorTarget const*> SharedLibrariesLinked;
  std::vector<cmGeneratorTarget const*> ExternalObjectTargets;
  std::vector<cmGeneratorTarget const*> RuntimeDLLs;

  // Context information.
  cmGeneratorTarget const* const Target;
  cmMakefile* const m_pMakefile;
  cmGlobalGenerator* const m_pGlobalGenerator;
  CMake* const CMakeInstance;

  // Configuration information.
  std::string const Config;
  std::string LinkLanguage;

  // Modes for dealing with dependent shared libraries.
  enum SharedDepMode
  {
    SharedDepModeNone,   // Drop
    SharedDepModeDir,    // List dir in -rpath-link flag
    SharedDepModeLibDir, // List dir in linker search path
    SharedDepModeLink    // List file on link line
  };

  cmValue LoaderFlag;
  std::string LibLinkFlag;
  std::string LibLinkFileFlag;
  std::string ObjLinkFileFlag;
  std::string LibLinkSuffix;
  std::string RuntimeFlag;
  std::string RuntimeSep;
  std::string RuntimeAlways;
  std::string RPathLinkFlag;
  SharedDepMode SharedDependencyMode;

  enum LinkType
  {
    LinkUnknown,
    LinkStatic,
    LinkShared
  };
  void SetCurrentLinkType(LinkType lt);

  // Link type adjustment.
  void ComputeLinkTypeInfo();
  LinkType StartLinkType;
  LinkType CurrentLinkType;
  std::string StaticLinkTypeFlag;
  std::string SharedLinkTypeFlag;

  // Link item parsing.
  void ComputeItemParserInfo();
  std::vector<std::string> StaticLinkExtensions;
  std::vector<std::string> SharedLinkExtensions;
  std::vector<std::string> LinkExtensions;
  std::set<std::string> LinkPrefixes;
  cmsys::RegularExpression ExtractStaticLibraryName;
  cmsys::RegularExpression ExtractSharedLibraryName;
  cmsys::RegularExpression ExtractAnyLibraryName;
  std::string SharedRegexString;
  void AddLinkPrefix(std::string const& p);
  void AddLinkExtension(std::string const& e, LinkType type);
  std::string CreateExtensionRegex(std::vector<std::string> const& exts,
                                   LinkType type);
  std::string NoCaseExpression(std::string const& str);

  // Handling of link items.
  void AddTargetItem(LinkEntry const& entry);
  void AddFullItem(LinkEntry const& entry);
  bool CheckImplicitDirItem(LinkEntry const& entry);
  void AddUserItem(LinkEntry const& entry);
  void AddFrameworkItem(LinkEntry const& entry);
  void AddXcFrameworkItem(LinkEntry const& entry);
  void DropDirectoryItem(BT<std::string> const& item);
  bool CheckSharedLibNoSOName(LinkEntry const& entry);
  void AddSharedLibNoSOName(LinkEntry const& entry);

  // Framework info.
  void ComputeFrameworkInfo();
  void AddFrameworkPath(std::string const& p);
  std::set<std::string> FrameworkPathsEmitted;

  void AddXcFrameworkHeaderPath(std::string const& p);

  // Linker search path computation.
  std::unique_ptr<cmOrderDirectories> OrderLinkerSearchPath;

  void AddExternalObjectTargets();

  // Implicit link libraries and directories for linker language.
  void LoadImplicitLinkInfo();
  void AddImplicitLinkInfo();
  void AddImplicitLinkInfo(std::string const& lang);
  void AddRuntimeLinkLibrary(std::string const& lang);
  std::set<std::string> ImplicitLinkDirs;
  std::set<std::string> ImplicitLinkLibs;

  // Additional paths configured by the runtime linker
  std::vector<std::string> RuntimeLinkDirs;

  // Dependent library path computation.
  std::unique_ptr<cmOrderDirectories> OrderDependentRPath;
  // Runtime path computation.
  std::unique_ptr<cmOrderDirectories> OrderRuntimeSearchPath;

  bool IsOpenBSD;
  bool LinkDependsNoShared;
  bool RuntimeUseChrpath;
  bool NoSONameUsesPath;
  bool LinkWithRuntimePath;
  bool LinkTypeEnabled;
  bool ArchivesMayBeShared;

  void AddLibraryRuntimeInfo(std::string const& fullPath,
                             cmGeneratorTarget const* target);
  void AddLibraryRuntimeInfo(std::string const& fullPath);

  class FeatureDescriptor
  {
  public:
    FeatureDescriptor() = default;

    std::string const m_name;
    bool const Supported = false;
    std::string const Prefix;
    std::string const Suffix;
    std::string GetDecoratedItem(std::string const& library,
                                 ItemIsPath isPath) const;
    std::string GetDecoratedItem(std::string const& library,
                                 std::string const& linkItem,
                                 std::string const& defaultValue,
                                 ItemIsPath isPath) const;

  protected:
    FeatureDescriptor(std::string name, std::string itemFormat);
    FeatureDescriptor(std::string name, std::string itemPathFormat,
                      std::string itemNameFormat);
    FeatureDescriptor(std::string name, std::string prefix,
                      std::string itemPathFormat, std::string itemNameFormat,
                      std::string suffix);

    FeatureDescriptor(std::string name, std::string prefix, std::string suffix,
                      bool isGroup);

  private:
    std::string ItemPathFormat;
    std::string ItemNameFormat;
  };

  class LibraryFeatureDescriptor : public FeatureDescriptor
  {
  public:
    LibraryFeatureDescriptor(std::string name, std::string itemFormat);
    LibraryFeatureDescriptor(std::string name, std::string itemPathFormat,
                             std::string itemNameFormat);
    LibraryFeatureDescriptor(std::string name, std::string prefix,
                             std::string itemPathFormat,
                             std::string itemNameFormat, std::string suffix);
  };
  std::map<std::string, FeatureDescriptor> LibraryFeatureDescriptors;
  bool AddLibraryFeature(std::string const& feature);
  FeatureDescriptor const& GetLibraryFeature(std::string const& feature) const;
  FeatureDescriptor const* FindLibraryFeature(
    std::string const& feature) const;

  class GroupFeatureDescriptor : public FeatureDescriptor
  {
  public:
    GroupFeatureDescriptor(std::string name, std::string prefix,
                           std::string suffix);
  };
  std::map<std::string, FeatureDescriptor> GroupFeatureDescriptors;
  FeatureDescriptor const& GetGroupFeature(std::string const& feature);
};
