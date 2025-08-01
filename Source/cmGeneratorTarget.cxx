/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmGeneratorTarget.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include <cm/memory>
#include <cm/optional>
#include <cm/string_view>
#include <cmext/algorithm>
#include <cmext/string_view>

#include "cmAlgorithms.h"
#include "cmComputeLinkInformation.h" // IWYU pragma: keep
#include "cmCryptoHash.h"
#include "cmCxxModuleUsageEffects.h"
#include "cmExperimental.h"
#include "cmFileSet.h"
#include "cmFileTimes.h"
#include "cmGeneratedFileStream.h"
#include "cmGeneratorExpression.h"
#include "cmGeneratorExpressionDAGChecker.h"
#include "cmGeneratorOptions.h"
#include "cmGlobalGenerator.h"
#include "cmList.h"
#include "cmLocalGenerator.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmOutputConverter.h"
#include "cmPropertyMap.h"
#include "cmRulePlaceholderExpander.h"
#include "cmSourceFile.h"
#include "cmSourceFileLocation.h"
#include "cmSourceFileLocationKind.h"
#include "cmStandardLevel.h"
#include "cmStandardLevelResolver.h"
#include "cmState.h"
#include "cmStateTypes.h"
#include "cmStringAlgorithms.h"
#include "cmSyntheticTargetCache.h"
#include "cmSystemTools.h"
#include "cmTarget.h"
#include "cmTargetLinkLibraryType.h"
#include "cmTargetPropertyComputer.h"
#include "cmTargetTraceDependencies.h"
#include "cmake.h"

namespace {
using UseTo = cmGeneratorTarget::UseTo;
}

template <>
cmValue cmTargetPropertyComputer::GetSources<cmGeneratorTarget>(
  cmGeneratorTarget const* tgt)
{
  return tgt->GetSourcesProperty();
}

template <>
std::string const&
cmTargetPropertyComputer::ImportedLocation<cmGeneratorTarget>(
  cmGeneratorTarget const* tgt, std::string const& config)
{
  return tgt->GetLocation(config);
}

static void CreatePropertyGeneratorExpressions(
  cmake& cmakeInstance, cmBTStringRange entries,
  std::vector<std::unique_ptr<cmGeneratorTarget::TargetPropertyEntry>>& items,
  bool evaluateForBuildsystem = false)
{
  for (auto const& entry : entries) {
    items.push_back(cmGeneratorTarget::TargetPropertyEntry::Create(
      cmakeInstance, entry, evaluateForBuildsystem));
  }
}

cmGeneratorTarget::cmGeneratorTarget(cmTarget* t, cmLocalGenerator* lg)
  : Target(t)
{
  this->Makefile = this->Target->GetMakefile();
  this->LocalGenerator = lg;
  this->GlobalGenerator = this->LocalGenerator->GetGlobalGenerator();

  this->GlobalGenerator->ComputeTargetObjectDirectory(this);

  CreatePropertyGeneratorExpressions(*lg->GetCMakeInstance(),
                                     t->GetIncludeDirectoriesEntries(),
                                     this->IncludeDirectoriesEntries);

  CreatePropertyGeneratorExpressions(*lg->GetCMakeInstance(),
                                     t->GetCompileOptionsEntries(),
                                     this->CompileOptionsEntries);

  CreatePropertyGeneratorExpressions(*lg->GetCMakeInstance(),
                                     t->GetCompileFeaturesEntries(),
                                     this->CompileFeaturesEntries);

  CreatePropertyGeneratorExpressions(*lg->GetCMakeInstance(),
                                     t->GetCompileDefinitionsEntries(),
                                     this->CompileDefinitionsEntries);

  CreatePropertyGeneratorExpressions(*lg->GetCMakeInstance(),
                                     t->GetLinkOptionsEntries(),
                                     this->LinkOptionsEntries);

  CreatePropertyGeneratorExpressions(*lg->GetCMakeInstance(),
                                     t->GetLinkDirectoriesEntries(),
                                     this->LinkDirectoriesEntries);

  CreatePropertyGeneratorExpressions(*lg->GetCMakeInstance(),
                                     t->GetPrecompileHeadersEntries(),
                                     this->PrecompileHeadersEntries);

  CreatePropertyGeneratorExpressions(
    *lg->GetCMakeInstance(), t->GetSourceEntries(), this->SourceEntries, true);

  this->PolicyMap = t->GetPolicyMap();

  // Get hard-coded linker language
  if (this->Target->GetProperty("HAS_CXX")) {
    this->LinkerLanguage = "CXX";
  } else {
    this->LinkerLanguage = this->Target->GetSafeProperty("LINKER_LANGUAGE");
  }

  auto configs =
    this->Makefile->GetGeneratorConfigs(cmMakefile::ExcludeEmptyConfig);
  std::string build_db_languages[] = { "CXX" };
  for (auto const& language : build_db_languages) {
    for (auto const& config : configs) {
      auto bdb_path = this->BuildDatabasePath(language, config);
      if (!bdb_path.empty()) {
        this->Makefile->GetOrCreateGeneratedSource(bdb_path);
        this->GetGlobalGenerator()->AddBuildDatabaseFile(language, config,
                                                         bdb_path);
      }
    }
  }
}

cmGeneratorTarget::~cmGeneratorTarget() = default;

cmValue cmGeneratorTarget::GetSourcesProperty() const
{
  std::vector<std::string> values;
  for (auto const& se : this->SourceEntries) {
    values.push_back(se->GetInput());
  }
  static std::string value;
  value = cmList::to_string(values);
  return cmValue(value);
}

cmGlobalGenerator* cmGeneratorTarget::GetGlobalGenerator() const
{
  return this->GetLocalGenerator()->GetGlobalGenerator();
}

cmLocalGenerator* cmGeneratorTarget::GetLocalGenerator() const
{
  return this->LocalGenerator;
}

cmStateEnums::TargetType cmGeneratorTarget::GetType() const
{
  return this->Target->GetType();
}

std::string const& cmGeneratorTarget::GetName() const
{
  return this->Target->GetName();
}

std::string cmGeneratorTarget::GetFamilyName() const
{
  if (!this->IsImported() && !this->IsSynthetic()) {
    return this->Target->GetTemplateName();
  }
  cmCryptoHash hasher(cmCryptoHash::AlgoSHA3_512);
  constexpr size_t HASH_TRUNCATION = 12;
  auto dirhash =
    hasher.HashString(this->GetLocalGenerator()->GetCurrentBinaryDirectory());
  auto targetIdent = hasher.HashString(cmStrCat("@d_", dirhash));
  return cmStrCat(this->Target->GetTemplateName(), '@',
                  targetIdent.substr(0, HASH_TRUNCATION));
}

std::string cmGeneratorTarget::GetExportName() const
{
  cmValue exportName = this->GetProperty("EXPORT_NAME");

  if (cmNonempty(exportName)) {
    if (!cmGeneratorExpression::IsValidTargetName(*exportName)) {
      std::ostringstream e;
      e << "EXPORT_NAME property \"" << *exportName << "\" for \""
        << this->GetName() << "\": is not valid.";
      cmSystemTools::Error(e.str());
      return "";
    }
    return *exportName;
  }
  return this->GetName();
}

std::string cmGeneratorTarget::GetFilesystemExportName() const
{
  auto fs_safe = this->GetExportName();
  // First escape any `_` characters to avoid collisions.
  cmSystemTools::ReplaceString(fs_safe, "_", "__");
  // Escape other characters that are not generally filesystem-safe.
  cmSystemTools::ReplaceString(fs_safe, ":", "_c");
  return fs_safe;
}

cmValue cmGeneratorTarget::GetProperty(std::string const& prop) const
{
  if (cmValue result =
        cmTargetPropertyComputer::GetProperty(this, prop, *this->Makefile)) {
    return result;
  }
  if (cmSystemTools::GetFatalErrorOccurred()) {
    return nullptr;
  }
  return this->Target->GetProperty(prop);
}

std::string const& cmGeneratorTarget::GetSafeProperty(
  std::string const& prop) const
{
  return this->GetProperty(prop);
}

char const* cmGeneratorTarget::GetOutputTargetType(
  cmStateEnums::ArtifactType artifact) const
{
  if (this->IsFrameworkOnApple() || this->GetGlobalGenerator()->IsXcode()) {
    // import file (i.e. .tbd file) is always in same location as library
    artifact = cmStateEnums::RuntimeBinaryArtifact;
  }

  switch (this->GetType()) {
    case cmStateEnums::SHARED_LIBRARY:
      if (this->IsDLLPlatform()) {
        switch (artifact) {
          case cmStateEnums::RuntimeBinaryArtifact:
            // A DLL shared library is treated as a runtime target.
            return "RUNTIME";
          case cmStateEnums::ImportLibraryArtifact:
            // A DLL import library is treated as an archive target.
            return "ARCHIVE";
        }
      } else {
        switch (artifact) {
          case cmStateEnums::RuntimeBinaryArtifact:
            // For non-DLL platforms shared libraries are treated as
            // library targets.
            return "LIBRARY";
          case cmStateEnums::ImportLibraryArtifact:
            // Library import libraries are treated as archive targets.
            return "ARCHIVE";
        }
      }
      break;
    case cmStateEnums::STATIC_LIBRARY:
      // Static libraries are always treated as archive targets.
      return "ARCHIVE";
    case cmStateEnums::MODULE_LIBRARY:
      switch (artifact) {
        case cmStateEnums::RuntimeBinaryArtifact:
          // Module libraries are always treated as library targets.
          return "LIBRARY";
        case cmStateEnums::ImportLibraryArtifact:
          // Module import libraries are treated as archive targets.
          return "ARCHIVE";
      }
      break;
    case cmStateEnums::OBJECT_LIBRARY:
      // Object libraries are always treated as object targets.
      return "OBJECT";
    case cmStateEnums::EXECUTABLE:
      switch (artifact) {
        case cmStateEnums::RuntimeBinaryArtifact:
          // Executables are always treated as runtime targets.
          return "RUNTIME";
        case cmStateEnums::ImportLibraryArtifact:
          // Executable import libraries are treated as archive targets.
          return "ARCHIVE";
      }
      break;
    default:
      break;
  }
  return "";
}

std::string cmGeneratorTarget::GetOutputName(
  std::string const& config, cmStateEnums::ArtifactType artifact) const
{
  // Lookup/compute/cache the output name for this configuration.
  OutputNameKey key(config, artifact);
  auto i = this->OutputNameMap.find(key);
  if (i == this->OutputNameMap.end()) {
    // Add empty name in map to detect potential recursion.
    OutputNameMapType::value_type entry(key, "");
    i = this->OutputNameMap.insert(entry).first;

    // Compute output name.
    std::vector<std::string> props;
    std::string type = this->GetOutputTargetType(artifact);
    std::string configUpper = cmSystemTools::UpperCase(config);
    if (!type.empty() && !configUpper.empty()) {
      // <ARCHIVE|LIBRARY|RUNTIME>_OUTPUT_NAME_<CONFIG>
      props.push_back(type + "_OUTPUT_NAME_" + configUpper);
    }
    if (!type.empty()) {
      // <ARCHIVE|LIBRARY|RUNTIME>_OUTPUT_NAME
      props.push_back(type + "_OUTPUT_NAME");
    }
    if (!configUpper.empty()) {
      // OUTPUT_NAME_<CONFIG>
      props.push_back("OUTPUT_NAME_" + configUpper);
      // <CONFIG>_OUTPUT_NAME
      props.push_back(configUpper + "_OUTPUT_NAME");
    }
    // OUTPUT_NAME
    props.emplace_back("OUTPUT_NAME");

    std::string outName;
    for (std::string const& p : props) {
      if (cmValue outNameProp = this->GetProperty(p)) {
        outName = *outNameProp;
        break;
      }
    }

    if (outName.empty()) {
      outName = this->GetName();
    }

    // Now evaluate genex and update the previously-prepared map entry.
    i->second =
      cmGeneratorExpression::Evaluate(outName, this->LocalGenerator, config);
  } else if (i->second.empty()) {
    // An empty map entry indicates we have been called recursively
    // from the above block.
    this->LocalGenerator->GetCMakeInstance()->IssueMessage(
      MessageType::FATAL_ERROR,
      "Target '" + this->GetName() + "' OUTPUT_NAME depends on itself.",
      this->GetBacktrace());
  }
  return i->second;
}

std::string cmGeneratorTarget::GetFilePrefix(
  std::string const& config, cmStateEnums::ArtifactType artifact) const
{
  if (this->IsImported()) {
    cmValue prefix = this->GetFilePrefixInternal(config, artifact);
    return prefix ? *prefix : std::string();
  }
  return this->GetFullNameInternalComponents(config, artifact).prefix;
}
std::string cmGeneratorTarget::GetFileSuffix(
  std::string const& config, cmStateEnums::ArtifactType artifact) const
{
  if (this->IsImported()) {
    cmValue suffix = this->GetFileSuffixInternal(config, artifact);
    return suffix ? *suffix : std::string();
  }
  return this->GetFullNameInternalComponents(config, artifact).suffix;
}

std::string cmGeneratorTarget::GetFilePostfix(std::string const& config) const
{
  cmValue postfix = nullptr;
  std::string frameworkPostfix;
  if (!config.empty()) {
    std::string configProp =
      cmStrCat(cmSystemTools::UpperCase(config), "_POSTFIX");
    postfix = this->GetProperty(configProp);

    // Mac application bundles and frameworks have no regular postfix like
    // libraries do.
    if (!this->IsImported() && postfix &&
        (this->IsAppBundleOnApple() || this->IsFrameworkOnApple())) {
      postfix = nullptr;
    }

    // Frameworks created by multi config generators can have a special
    // framework postfix.
    frameworkPostfix = this->GetFrameworkMultiConfigPostfix(config);
    if (!frameworkPostfix.empty()) {
      postfix = cmValue(frameworkPostfix);
    }
  }
  return postfix ? *postfix : std::string();
}

std::string cmGeneratorTarget::GetFrameworkMultiConfigPostfix(
  std::string const& config) const
{
  cmValue postfix = nullptr;
  if (!config.empty()) {
    std::string configProp = cmStrCat("FRAMEWORK_MULTI_CONFIG_POSTFIX_",
                                      cmSystemTools::UpperCase(config));
    postfix = this->GetProperty(configProp);

    if (!this->IsImported() && postfix &&
        (this->IsFrameworkOnApple() &&
         !this->GetGlobalGenerator()->IsMultiConfig())) {
      postfix = nullptr;
    }
  }
  return postfix ? *postfix : std::string();
}

cmValue cmGeneratorTarget::GetFilePrefixInternal(
  std::string const& config, cmStateEnums::ArtifactType artifact,
  std::string const& language) const
{
  // no prefix for non-main target types.
  if (this->GetType() != cmStateEnums::STATIC_LIBRARY &&
      this->GetType() != cmStateEnums::SHARED_LIBRARY &&
      this->GetType() != cmStateEnums::MODULE_LIBRARY &&
      this->GetType() != cmStateEnums::EXECUTABLE) {
    return nullptr;
  }

  bool const isImportedLibraryArtifact =
    (artifact == cmStateEnums::ImportLibraryArtifact);

  // Return an empty prefix for the import library if this platform
  // does not support import libraries.
  if (isImportedLibraryArtifact && !this->NeedImportLibraryName(config)) {
    return nullptr;
  }

  // The implib option is only allowed for shared libraries, module
  // libraries, and executables.
  if (this->GetType() != cmStateEnums::SHARED_LIBRARY &&
      this->GetType() != cmStateEnums::MODULE_LIBRARY &&
      this->GetType() != cmStateEnums::EXECUTABLE) {
    artifact = cmStateEnums::RuntimeBinaryArtifact;
  }

  // Compute prefix value.
  cmValue targetPrefix =
    (isImportedLibraryArtifact ? this->GetProperty("IMPORT_PREFIX")
                               : this->GetProperty("PREFIX"));

  if (!targetPrefix) {
    char const* prefixVar = this->Target->GetPrefixVariableInternal(artifact);
    if (!language.empty() && cmNonempty(prefixVar)) {
      std::string langPrefix = cmStrCat(prefixVar, '_', language);
      targetPrefix = this->Makefile->GetDefinition(langPrefix);
    }

    // if there is no prefix on the target nor specific language
    // use the cmake definition.
    if (!targetPrefix && prefixVar) {
      targetPrefix = this->Makefile->GetDefinition(prefixVar);
    }
  }

  return targetPrefix;
}

cmValue cmGeneratorTarget::GetFileSuffixInternal(
  std::string const& config, cmStateEnums::ArtifactType artifact,
  std::string const& language) const
{
  // no suffix for non-main target types.
  if (this->GetType() != cmStateEnums::STATIC_LIBRARY &&
      this->GetType() != cmStateEnums::SHARED_LIBRARY &&
      this->GetType() != cmStateEnums::MODULE_LIBRARY &&
      this->GetType() != cmStateEnums::EXECUTABLE) {
    return nullptr;
  }

  bool const isImportedLibraryArtifact =
    (artifact == cmStateEnums::ImportLibraryArtifact);

  // Return an empty suffix for the import library if this platform
  // does not support import libraries.
  if (isImportedLibraryArtifact && !this->NeedImportLibraryName(config)) {
    return nullptr;
  }

  // The implib option is only allowed for shared libraries, module
  // libraries, and executables.
  if (this->GetType() != cmStateEnums::SHARED_LIBRARY &&
      this->GetType() != cmStateEnums::MODULE_LIBRARY &&
      this->GetType() != cmStateEnums::EXECUTABLE) {
    artifact = cmStateEnums::RuntimeBinaryArtifact;
  }

  // Compute suffix value.
  cmValue targetSuffix =
    (isImportedLibraryArtifact ? this->GetProperty("IMPORT_SUFFIX")
                               : this->GetProperty("SUFFIX"));

  if (!targetSuffix) {
    char const* suffixVar = this->Target->GetSuffixVariableInternal(artifact);
    if (!language.empty() && cmNonempty(suffixVar)) {
      std::string langSuffix = cmStrCat(suffixVar, '_', language);
      targetSuffix = this->Makefile->GetDefinition(langSuffix);
    }

    // if there is no suffix on the target nor specific language
    // use the cmake definition.
    if (!targetSuffix && suffixVar) {
      targetSuffix = this->Makefile->GetDefinition(suffixVar);
    }
  }

  return targetSuffix;
}

void cmGeneratorTarget::ClearSourcesCache()
{
  this->AllConfigSources.clear();
  this->KindedSourcesMap.clear();
  this->SourcesAreContextDependent = Tribool::Indeterminate;
  this->Objects.clear();
  this->VisitedConfigsForObjects.clear();
  this->LinkImplClosureForLinkMap.clear();
  this->LinkImplClosureForUsageMap.clear();
  this->LinkImplMap.clear();
  this->LinkImplUsageRequirementsOnlyMap.clear();
  this->IncludeDirectoriesCache.clear();
  this->CompileOptionsCache.clear();
  this->CompileDefinitionsCache.clear();
  this->CustomTransitiveBuildPropertiesMap.clear();
  this->CustomTransitiveInterfacePropertiesMap.clear();
  this->PrecompileHeadersCache.clear();
  this->LinkOptionsCache.clear();
  this->LinkDirectoriesCache.clear();
  this->RuntimeBinaryFullNameCache.clear();
  this->ImportLibraryFullNameCache.clear();
}

void cmGeneratorTarget::ClearLinkInterfaceCache()
{
  this->LinkInterfaceMap.clear();
  this->LinkInterfaceUsageRequirementsOnlyMap.clear();
}

void cmGeneratorTarget::AddSourceCommon(std::string const& src, bool before)
{
  this->SourceEntries.insert(
    before ? this->SourceEntries.begin() : this->SourceEntries.end(),
    TargetPropertyEntry::Create(
      *this->LocalGenerator->GetCMakeInstance(),
      BT<std::string>(src, this->Makefile->GetBacktrace()), true));
  this->ClearSourcesCache();
}

void cmGeneratorTarget::AddSource(std::string const& src, bool before)
{
  this->Target->AddSource(src, before);
  this->AddSourceCommon(src, before);
}

void cmGeneratorTarget::AddTracedSources(std::vector<std::string> const& srcs)
{
  this->Target->AddTracedSources(srcs);
  if (!srcs.empty()) {
    this->AddSourceCommon(cmJoin(srcs, ";"));
  }
}

void cmGeneratorTarget::AddIncludeDirectory(std::string const& src,
                                            bool before)
{
  this->Target->InsertInclude(
    BT<std::string>(src, this->Makefile->GetBacktrace()), before);
  this->IncludeDirectoriesEntries.insert(
    before ? this->IncludeDirectoriesEntries.begin()
           : this->IncludeDirectoriesEntries.end(),
    TargetPropertyEntry::Create(
      *this->Makefile->GetCMakeInstance(),
      BT<std::string>(src, this->Makefile->GetBacktrace()), true));
}

void cmGeneratorTarget::AddSystemIncludeDirectory(std::string const& inc,
                                                  std::string const& lang)
{
  std::string config_upper;
  auto const& configs =
    this->Makefile->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig);

  for (auto const& config : configs) {
    std::string inc_with_config = inc;
    if (!config.empty()) {
      cmSystemTools::ReplaceString(inc_with_config, "$<CONFIG>", config);
      config_upper = cmSystemTools::UpperCase(config);
    }
    auto const& key = cmStrCat(config_upper, '/', lang);
    this->Target->AddSystemIncludeDirectories({ inc_with_config });
    if (this->SystemIncludesCache.find(key) ==
        this->SystemIncludesCache.end()) {
      this->AddSystemIncludeCacheKey(key, config, lang);
    }
    this->SystemIncludesCache[key].emplace_back(inc_with_config);

    // SystemIncludesCache should be sorted so that binary search can be used
    std::sort(this->SystemIncludesCache[key].begin(),
              this->SystemIncludesCache[key].end());
  }
}

std::vector<cmSourceFile*> const* cmGeneratorTarget::GetSourceDepends(
  cmSourceFile const* sf) const
{
  auto i = this->SourceDepends.find(sf);
  if (i != this->SourceDepends.end()) {
    return &i->second.Depends;
  }
  return nullptr;
}

namespace {
void handleSystemIncludesDep(cmLocalGenerator* lg,
                             cmGeneratorTarget const* depTgt,
                             std::string const& config,
                             cmGeneratorTarget const* headTarget,
                             cmGeneratorExpressionDAGChecker* dagChecker,
                             cmList& result, bool excludeImported,
                             std::string const& language)
{
  if (cmValue dirs =
        depTgt->GetProperty("INTERFACE_SYSTEM_INCLUDE_DIRECTORIES")) {
    result.append(cmGeneratorExpression::Evaluate(
      *dirs, lg, config, headTarget, dagChecker, depTgt, language));
  }
  if (!depTgt->GetPropertyAsBool("SYSTEM")) {
    return;
  }
  if (depTgt->IsImported()) {
    if (excludeImported) {
      return;
    }
    if (depTgt->GetPropertyAsBool("IMPORTED_NO_SYSTEM")) {
      return;
    }
  }

  if (cmValue dirs = depTgt->GetProperty("INTERFACE_INCLUDE_DIRECTORIES")) {
    result.append(cmGeneratorExpression::Evaluate(
      *dirs, lg, config, headTarget, dagChecker, depTgt, language));
  }

  if (depTgt->Target->IsFrameworkOnApple() ||
      depTgt->IsImportedFrameworkFolderOnApple(config)) {
    if (auto fwDescriptor = depTgt->GetGlobalGenerator()->SplitFrameworkPath(
          depTgt->GetLocation(config))) {
      result.push_back(fwDescriptor->Directory);
      result.push_back(fwDescriptor->GetFrameworkPath());
    }
  }
}
}

/* clang-format off */
#define IMPLEMENT_VISIT(KIND)                                                 \
  do {                                                                        \
    KindedSources const& kinded = this->GetKindedSources(config);             \
    for (SourceAndKind const& s : kinded.Sources) {                           \
      if (s.Kind == KIND) {                                                   \
        data.push_back(s.Source.Value);                                       \
      }                                                                       \
    }                                                                         \
  } while (false)
/* clang-format on */

void cmGeneratorTarget::GetObjectSources(
  std::vector<cmSourceFile const*>& data, std::string const& config) const
{
  IMPLEMENT_VISIT(SourceKindObjectSource);

  if (this->VisitedConfigsForObjects.count(config)) {
    return;
  }

  for (cmSourceFile const* it : data) {
    this->Objects[it];
  }

  this->LocalGenerator->ComputeObjectFilenames(this->Objects, this);
  this->VisitedConfigsForObjects.insert(config);
}

void cmGeneratorTarget::ComputeObjectMapping()
{
  auto const& configs =
    this->Makefile->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig);
  std::set<std::string> configSet(configs.begin(), configs.end());
  if (configSet == this->VisitedConfigsForObjects) {
    return;
  }

  for (std::string const& c : configs) {
    std::vector<cmSourceFile const*> sourceFiles;
    this->GetObjectSources(sourceFiles, c);
  }
}

cmValue cmGeneratorTarget::GetFeature(std::string const& feature,
                                      std::string const& config) const
{
  if (!config.empty()) {
    std::string featureConfig =
      cmStrCat(feature, '_', cmSystemTools::UpperCase(config));
    if (cmValue value = this->GetProperty(featureConfig)) {
      return value;
    }
  }
  if (cmValue value = this->GetProperty(feature)) {
    return value;
  }
  return this->LocalGenerator->GetFeature(feature, config);
}

std::string cmGeneratorTarget::GetLinkerTypeProperty(
  std::string const& lang, std::string const& config) const
{
  std::string propName{ "LINKER_TYPE" };
  auto linkerType = this->GetProperty(propName);
  if (!linkerType.IsEmpty()) {
    cmGeneratorExpressionDAGChecker dagChecker{
      this, propName, nullptr, nullptr, this->LocalGenerator, config,
    };
    auto ltype =
      cmGeneratorExpression::Evaluate(*linkerType, this->GetLocalGenerator(),
                                      config, this, &dagChecker, this, lang);
    if (this->IsDeviceLink()) {
      cmList list{ ltype };
      auto const DL_BEGIN = "<DEVICE_LINK>"_s;
      auto const DL_END = "</DEVICE_LINK>"_s;
      cm::erase_if(list, [&](std::string const& item) {
        return item == DL_BEGIN || item == DL_END;
      });
      return list.to_string();
    }
    return ltype;
  }
  return std::string{};
}

char const* cmGeneratorTarget::GetLinkPIEProperty(
  std::string const& config) const
{
  static std::string PICValue;

  PICValue = this->GetLinkInterfaceDependentStringAsBoolProperty(
    "POSITION_INDEPENDENT_CODE", config);

  if (PICValue == "(unset)") {
    // POSITION_INDEPENDENT_CODE is not set
    return nullptr;
  }

  auto status = this->GetPolicyStatusCMP0083();
  return (status != cmPolicies::WARN && status != cmPolicies::OLD)
    ? PICValue.c_str()
    : nullptr;
}

bool cmGeneratorTarget::IsIPOEnabled(std::string const& lang,
                                     std::string const& config) const
{
  cmValue feature = this->GetFeature("INTERPROCEDURAL_OPTIMIZATION", config);

  if (!feature.IsOn()) {
    // 'INTERPROCEDURAL_OPTIMIZATION' is off, no need to check policies
    return false;
  }

  if (lang != "C" && lang != "CXX" && lang != "CUDA" && lang != "Fortran") {
    // We do not define IPO behavior for other languages.
    return false;
  }

  if (lang == "CUDA") {
    // CUDA IPO requires both CUDA_ARCHITECTURES and CUDA_SEPARABLE_COMPILATION
    if (cmIsOff(this->GetSafeProperty("CUDA_ARCHITECTURES")) ||
        cmIsOff(this->GetSafeProperty("CUDA_SEPARABLE_COMPILATION"))) {
      return false;
    }
  }

  cmPolicies::PolicyStatus cmp0069 = this->GetPolicyStatusCMP0069();

  if (cmp0069 == cmPolicies::OLD || cmp0069 == cmPolicies::WARN) {
    if (this->Makefile->IsOn("_CMAKE_" + lang + "_IPO_LEGACY_BEHAVIOR")) {
      return true;
    }
    if (this->PolicyReportedCMP0069) {
      // problem is already reported, no need to issue a message
      return false;
    }
    bool const in_try_compile =
      this->LocalGenerator->GetCMakeInstance()->GetIsInTryCompile();
    if (cmp0069 == cmPolicies::WARN && !in_try_compile) {
      std::ostringstream w;
      w << cmPolicies::GetPolicyWarning(cmPolicies::CMP0069) << "\n";
      w << "INTERPROCEDURAL_OPTIMIZATION property will be ignored for target "
        << "'" << this->GetName() << "'.";
      this->LocalGenerator->GetCMakeInstance()->IssueMessage(
        MessageType::AUTHOR_WARNING, w.str(), this->GetBacktrace());

      this->PolicyReportedCMP0069 = true;
    }
    return false;
  }

  // Note: check consistency with messages from CheckIPOSupported
  char const* message = nullptr;
  if (!this->Makefile->IsOn("_CMAKE_" + lang + "_IPO_SUPPORTED_BY_CMAKE")) {
    message = "CMake doesn't support IPO for current compiler";
  } else if (!this->Makefile->IsOn("_CMAKE_" + lang +
                                   "_IPO_MAY_BE_SUPPORTED_BY_COMPILER")) {
    message = "Compiler doesn't support IPO";
  } else if (!this->GlobalGenerator->IsIPOSupported()) {
    message = "CMake doesn't support IPO for current generator";
  }

  if (!message) {
    // No error/warning messages
    return true;
  }

  if (this->PolicyReportedCMP0069) {
    // problem is already reported, no need to issue a message
    return false;
  }

  this->PolicyReportedCMP0069 = true;

  this->LocalGenerator->GetCMakeInstance()->IssueMessage(
    MessageType::FATAL_ERROR, message, this->GetBacktrace());
  return false;
}

std::string const& cmGeneratorTarget::GetObjectName(cmSourceFile const* file)
{
  this->ComputeObjectMapping();
  return this->Objects[file];
}

char const* cmGeneratorTarget::GetCustomObjectExtension() const
{
  struct compiler_mode
  {
    std::string variable;
    std::string extension;
  };
  static std::array<compiler_mode, 4> const modes{
    { { "CUDA_PTX_COMPILATION", ".ptx" },
      { "CUDA_CUBIN_COMPILATION", ".cubin" },
      { "CUDA_FATBIN_COMPILATION", ".fatbin" },
      { "CUDA_OPTIX_COMPILATION", ".optixir" } }
  };

  std::string const& compiler =
    this->Makefile->GetSafeDefinition("CMAKE_CUDA_COMPILER_ID");
  if (!compiler.empty()) {
    for (auto const& m : modes) {
      bool const has_extension = this->GetPropertyAsBool(m.variable);
      if (has_extension) {
        return m.extension.c_str();
      }
    }
  }
  return nullptr;
}

void cmGeneratorTarget::AddExplicitObjectName(cmSourceFile const* sf)
{
  this->ExplicitObjectName.insert(sf);
}

bool cmGeneratorTarget::HasExplicitObjectName(cmSourceFile const* file) const
{
  const_cast<cmGeneratorTarget*>(this)->ComputeObjectMapping();
  auto it = this->ExplicitObjectName.find(file);
  return it != this->ExplicitObjectName.end();
}

BTs<std::string> const* cmGeneratorTarget::GetLanguageStandardProperty(
  std::string const& lang, std::string const& config) const
{
  std::string key = cmStrCat(cmSystemTools::UpperCase(config), '-', lang);
  auto langStandardIter = this->LanguageStandardMap.find(key);
  if (langStandardIter != this->LanguageStandardMap.end()) {
    return &langStandardIter->second;
  }

  return this->Target->GetLanguageStandardProperty(
    cmStrCat(lang, "_STANDARD"));
}

cmValue cmGeneratorTarget::GetLanguageStandard(std::string const& lang,
                                               std::string const& config) const
{
  BTs<std::string> const* languageStandard =
    this->GetLanguageStandardProperty(lang, config);

  if (languageStandard) {
    return cmValue(languageStandard->Value);
  }

  return nullptr;
}

cmValue cmGeneratorTarget::GetPropertyWithPairedLanguageSupport(
  std::string const& lang, char const* suffix) const
{
  cmValue propertyValue = this->Target->GetProperty(cmStrCat(lang, suffix));
  if (!propertyValue) {
    // Check if we should use the value set by another language.
    if (lang == "OBJC") {
      propertyValue = this->GetPropertyWithPairedLanguageSupport("C", suffix);
    } else if (lang == "OBJCXX" || lang == "CUDA" || lang == "HIP") {
      propertyValue =
        this->GetPropertyWithPairedLanguageSupport("CXX", suffix);
    }
  }
  return propertyValue;
}

cmValue cmGeneratorTarget::GetLanguageExtensions(std::string const& lang) const
{
  return this->GetPropertyWithPairedLanguageSupport(lang, "_EXTENSIONS");
}

bool cmGeneratorTarget::GetLanguageStandardRequired(
  std::string const& lang) const
{
  return this->GetPropertyWithPairedLanguageSupport(lang, "_STANDARD_REQUIRED")
    .IsOn();
}

void cmGeneratorTarget::GetModuleDefinitionSources(
  std::vector<cmSourceFile const*>& data, std::string const& config) const
{
  IMPLEMENT_VISIT(SourceKindModuleDefinition);
}

void cmGeneratorTarget::GetHeaderSources(
  std::vector<cmSourceFile const*>& data, std::string const& config) const
{
  IMPLEMENT_VISIT(SourceKindHeader);
}

void cmGeneratorTarget::GetCxxModuleSources(
  std::vector<cmSourceFile const*>& data, std::string const& config) const
{
  IMPLEMENT_VISIT(SourceKindCxxModuleSource);
}

void cmGeneratorTarget::GetExtraSources(std::vector<cmSourceFile const*>& data,
                                        std::string const& config) const
{
  IMPLEMENT_VISIT(SourceKindExtra);
}

void cmGeneratorTarget::GetCustomCommands(
  std::vector<cmSourceFile const*>& data, std::string const& config) const
{
  IMPLEMENT_VISIT(SourceKindCustomCommand);
}

void cmGeneratorTarget::GetExternalObjects(
  std::vector<cmSourceFile const*>& data, std::string const& config) const
{
  IMPLEMENT_VISIT(SourceKindExternalObject);
}

void cmGeneratorTarget::GetManifests(std::vector<cmSourceFile const*>& data,
                                     std::string const& config) const
{
  IMPLEMENT_VISIT(SourceKindManifest);
}

std::set<cmLinkItem> const& cmGeneratorTarget::GetUtilityItems() const
{
  if (!this->UtilityItemsDone) {
    this->UtilityItemsDone = true;
    std::set<BT<std::pair<std::string, bool>>> const& utilities =
      this->GetUtilities();
    for (BT<std::pair<std::string, bool>> const& i : utilities) {
      if (cmGeneratorTarget* gt =
            this->LocalGenerator->FindGeneratorTargetToUse(i.Value.first)) {
        this->UtilityItems.insert(cmLinkItem(gt, i.Value.second, i.Backtrace));
      } else {
        this->UtilityItems.insert(
          cmLinkItem(i.Value.first, i.Value.second, i.Backtrace));
      }
    }
    if (cmGeneratorTarget const* reuseTarget = this->GetPchReuseTarget()) {
      this->UtilityItems.insert(
        cmLinkItem(reuseTarget, false, cmListFileBacktrace()));
    }
  }
  return this->UtilityItems;
}

std::string const& cmGeneratorTarget::GetLocation(
  std::string const& config) const
{
  static std::string location;
  if (this->IsImported()) {
    location = this->Target->ImportedGetFullPath(
      config, cmStateEnums::RuntimeBinaryArtifact);
  } else {
    location = this->GetFullPath(config, cmStateEnums::RuntimeBinaryArtifact);
  }
  return location;
}

cm::optional<std::string> cmGeneratorTarget::MaybeGetLocation(
  std::string const& config) const
{
  cm::optional<std::string> location;
  if (cmGeneratorTarget::ImportInfo const* imp = this->GetImportInfo(config)) {
    if (!imp->Location.empty()) {
      location = imp->Location;
    }
  } else {
    location = this->GetFullPath(config, cmStateEnums::RuntimeBinaryArtifact);
  }
  return location;
}

std::vector<cmCustomCommand> const& cmGeneratorTarget::GetPreBuildCommands()
  const
{
  return this->Target->GetPreBuildCommands();
}

std::vector<cmCustomCommand> const& cmGeneratorTarget::GetPreLinkCommands()
  const
{
  return this->Target->GetPreLinkCommands();
}

std::vector<cmCustomCommand> const& cmGeneratorTarget::GetPostBuildCommands()
  const
{
  return this->Target->GetPostBuildCommands();
}

void cmGeneratorTarget::AppendCustomCommandSideEffects(
  std::set<cmGeneratorTarget const*>& sideEffects) const
{
  if (!this->GetPreBuildCommands().empty() ||
      !this->GetPreLinkCommands().empty() ||
      !this->GetPostBuildCommands().empty()) {
    sideEffects.insert(this);
  } else {
    for (auto const& source : this->GetAllConfigSources()) {
      if (source.Source->GetCustomCommand()) {
        sideEffects.insert(this);
        break;
      }
    }
  }
}

void cmGeneratorTarget::AppendLanguageSideEffects(
  std::map<std::string, std::set<cmGeneratorTarget const*>>& sideEffects) const
{
  static std::set<cm::string_view> const LANGS_WITH_NO_SIDE_EFFECTS = {
    "C"_s, "CXX"_s, "OBJC"_s, "OBJCXX"_s, "ASM"_s, "CUDA"_s, "HIP"_s
  };

  for (auto const& lang : this->GetAllConfigCompileLanguages()) {
    if (!LANGS_WITH_NO_SIDE_EFFECTS.count(lang)) {
      sideEffects[lang].insert(this);
    }
  }
}

bool cmGeneratorTarget::IsInBuildSystem() const
{
  if (this->IsImported()) {
    return false;
  }
  switch (this->Target->GetType()) {
    case cmStateEnums::EXECUTABLE:
    case cmStateEnums::STATIC_LIBRARY:
    case cmStateEnums::SHARED_LIBRARY:
    case cmStateEnums::MODULE_LIBRARY:
    case cmStateEnums::OBJECT_LIBRARY:
    case cmStateEnums::UTILITY:
    case cmStateEnums::GLOBAL_TARGET:
      return true;
    case cmStateEnums::INTERFACE_LIBRARY:
      // An INTERFACE library is in the build system if it has SOURCES
      // or C++ module filesets.
      if (!this->SourceEntries.empty() ||
          !this->Target->GetHeaderSetsEntries().empty() ||
          !this->Target->GetCxxModuleSetsEntries().empty()) {
        return true;
      }
      break;
    case cmStateEnums::UNKNOWN_LIBRARY:
      break;
  }
  return false;
}

bool cmGeneratorTarget::IsNormal() const
{
  return this->Target->IsNormal();
}

bool cmGeneratorTarget::IsRuntimeBinary() const
{
  return this->Target->IsRuntimeBinary();
}

bool cmGeneratorTarget::IsSynthetic() const
{
  return this->Target->IsSynthetic();
}

bool cmGeneratorTarget::IsImported() const
{
  return this->Target->IsImported();
}

bool cmGeneratorTarget::IsImportedGloballyVisible() const
{
  return this->Target->IsImportedGloballyVisible();
}

bool cmGeneratorTarget::IsForeign() const
{
  return this->Target->IsForeign();
}

bool cmGeneratorTarget::CanCompileSources() const
{
  return this->Target->CanCompileSources();
}

bool cmGeneratorTarget::HasKnownRuntimeArtifactLocation(
  std::string const& config) const
{
  if (!this->IsRuntimeBinary()) {
    return false;
  }
  if (!this->IsImported()) {
    return true;
  }
  ImportInfo const* info = this->GetImportInfo(config);
  return info && !info->Location.empty();
}

std::string const& cmGeneratorTarget::GetLocationForBuild() const
{
  static std::string location;
  if (this->IsImported()) {
    location = this->Target->ImportedGetFullPath(
      "", cmStateEnums::RuntimeBinaryArtifact);
    return location;
  }

  // Now handle the deprecated build-time configuration location.
  std::string const noConfig;
  location = this->GetDirectory(noConfig);
  cmValue cfgid = this->Makefile->GetDefinition("CMAKE_CFG_INTDIR");
  if (cfgid && (*cfgid != ".")) {
    location += "/";
    location += *cfgid;
  }

  if (this->IsAppBundleOnApple()) {
    std::string macdir = this->BuildBundleDirectory("", "", FullLevel);
    if (!macdir.empty()) {
      location += "/";
      location += macdir;
    }
  }
  location += "/";
  location += this->GetFullName("", cmStateEnums::RuntimeBinaryArtifact);
  return location;
}

void cmGeneratorTarget::AddSystemIncludeCacheKey(
  std::string const& key, std::string const& config,
  std::string const& language) const
{
  cmGeneratorExpressionDAGChecker dagChecker{
    this,    "SYSTEM_INCLUDE_DIRECTORIES", nullptr,
    nullptr, this->LocalGenerator,         config,
  };

  bool excludeImported = this->GetPropertyAsBool("NO_SYSTEM_FROM_IMPORTED");

  cmList result;
  for (std::string const& it : this->Target->GetSystemIncludeDirectories()) {
    result.append(cmGeneratorExpression::Evaluate(
      it, this->LocalGenerator, config, this, &dagChecker, nullptr, language));
  }

  std::vector<cmGeneratorTarget const*> const& deps =
    this->GetLinkImplementationClosure(config, UseTo::Compile);
  for (cmGeneratorTarget const* dep : deps) {
    handleSystemIncludesDep(this->LocalGenerator, dep, config, this,
                            &dagChecker, result, excludeImported, language);
  }

  cmLinkImplementation const* impl =
    this->GetLinkImplementation(config, UseTo::Compile);
  if (impl) {
    auto runtimeEntries = impl->LanguageRuntimeLibraries.find(language);
    if (runtimeEntries != impl->LanguageRuntimeLibraries.end()) {
      for (auto const& lib : runtimeEntries->second) {
        if (lib.Target) {
          handleSystemIncludesDep(this->LocalGenerator, lib.Target, config,
                                  this, &dagChecker, result, excludeImported,
                                  language);
        }
      }
    }
  }

  std::for_each(result.begin(), result.end(),
                cmSystemTools::ConvertToUnixSlashes);
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  SystemIncludesCache.emplace(key, result);
}

bool cmGeneratorTarget::IsSystemIncludeDirectory(
  std::string const& dir, std::string const& config,
  std::string const& language) const
{
  std::string config_upper;
  if (!config.empty()) {
    config_upper = cmSystemTools::UpperCase(config);
  }

  std::string key = cmStrCat(config_upper, '/', language);
  auto iter = this->SystemIncludesCache.find(key);

  if (iter == this->SystemIncludesCache.end()) {
    this->AddSystemIncludeCacheKey(key, config, language);
    iter = this->SystemIncludesCache.find(key);
  }

  return std::binary_search(iter->second.begin(), iter->second.end(), dir);
}

bool cmGeneratorTarget::GetPropertyAsBool(std::string const& prop) const
{
  return this->Target->GetPropertyAsBool(prop);
}

std::string cmGeneratorTarget::GetCompilePDBName(
  std::string const& config) const
{
  if (cmGeneratorTarget const* reuseTarget = this->GetPchReuseTarget()) {
    if (reuseTarget != this) {
      return reuseTarget->GetCompilePDBName(config);
    }
  }

  // Check for a per-configuration output directory target property.
  std::string configUpper = cmSystemTools::UpperCase(config);
  std::string configProp = cmStrCat("COMPILE_PDB_NAME_", configUpper);
  cmValue config_name = this->GetProperty(configProp);
  if (cmNonempty(config_name)) {
    std::string pdbName = cmGeneratorExpression::Evaluate(
      *config_name, this->LocalGenerator, config, this);
    NameComponents const& components = GetFullNameInternalComponents(
      config, cmStateEnums::RuntimeBinaryArtifact);
    return components.prefix + pdbName + ".pdb";
  }

  cmValue name = this->GetProperty("COMPILE_PDB_NAME");
  if (cmNonempty(name)) {
    std::string pdbName = cmGeneratorExpression::Evaluate(
      *name, this->LocalGenerator, config, this);
    NameComponents const& components = GetFullNameInternalComponents(
      config, cmStateEnums::RuntimeBinaryArtifact);
    return components.prefix + pdbName + ".pdb";
  }

  // If the target is PCH-reused, we need a stable name for the PDB file so
  // that reusing targets can construct a stable name for it.
  if (this->PchReused) {
    NameComponents const& components = GetFullNameInternalComponents(
      config, cmStateEnums::RuntimeBinaryArtifact);
    return cmStrCat(components.prefix, this->GetName(), ".pdb");
  }

  return "";
}

std::string cmGeneratorTarget::GetCompilePDBPath(
  std::string const& config) const
{
  std::string dir = this->GetCompilePDBDirectory(config);
  std::string name = this->GetCompilePDBName(config);
  if (dir.empty() && !name.empty() && this->HaveWellDefinedOutputFiles()) {
    dir = this->GetPDBDirectory(config);
  }
  if (!dir.empty()) {
    dir += "/";
  }
  return dir + name;
}

bool cmGeneratorTarget::HasSOName(std::string const& config) const
{
  // soname is supported only for shared libraries and modules,
  // and then only when the platform supports an soname flag.
  return ((this->GetType() == cmStateEnums::SHARED_LIBRARY) &&
          !this->GetPropertyAsBool("NO_SONAME") &&
          (this->Makefile->GetSONameFlag(this->GetLinkerLanguage(config)) ||
           this->IsArchivedAIXSharedLibrary()));
}

bool cmGeneratorTarget::NeedRelinkBeforeInstall(
  std::string const& config) const
{
  // Only executables and shared libraries can have an rpath and may
  // need relinking.
  if (this->GetType() != cmStateEnums::EXECUTABLE &&
      this->GetType() != cmStateEnums::SHARED_LIBRARY &&
      this->GetType() != cmStateEnums::MODULE_LIBRARY) {
    return false;
  }

  // If there is no install location this target will not be installed
  // and therefore does not need relinking.
  if (!this->Target->GetHaveInstallRule()) {
    return false;
  }

  // If skipping all rpaths completely then no relinking is needed.
  if (this->Makefile->IsOn("CMAKE_SKIP_RPATH")) {
    return false;
  }

  // If building with the install-tree rpath no relinking is needed.
  if (this->GetPropertyAsBool("BUILD_WITH_INSTALL_RPATH")) {
    return false;
  }

  // If chrpath is going to be used no relinking is needed.
  if (this->IsChrpathUsed(config)) {
    return false;
  }

  // Check for rpath support on this platform.
  std::string ll = this->GetLinkerLanguage(config);
  if (!ll.empty()) {
    std::string flagVar =
      cmStrCat("CMAKE_SHARED_LIBRARY_RUNTIME_", ll, "_FLAG");
    if (!this->Makefile->IsSet(flagVar)) {
      // There is no rpath support on this platform so nothing needs
      // relinking.
      return false;
    }
  } else {
    // No linker language is known.  This error will be reported by
    // other code.
    return false;
  }

  // If either a build or install tree rpath is set then the rpath
  // will likely change between the build tree and install tree and
  // this target must be relinked.
  bool have_rpath =
    this->HaveBuildTreeRPATH(config) || this->HaveInstallTreeRPATH(config);
  bool is_ninja = this->LocalGenerator->GetGlobalGenerator()->IsNinja();

  if (have_rpath && is_ninja) {
    std::ostringstream w;
    /* clang-format off */
    w <<
      "The install of the " << this->GetName() << " target requires changing "
      "an RPATH from the build tree, but this is not supported with the Ninja "
      "generator unless on an ELF-based or XCOFF-based platform.  "
      "The CMAKE_BUILD_WITH_INSTALL_RPATH variable may be set to avoid this "
      "relinking step."
      ;
    /* clang-format on */

    cmake* cm = this->LocalGenerator->GetCMakeInstance();
    cm->IssueMessage(MessageType::FATAL_ERROR, w.str(), this->GetBacktrace());
  }

  return have_rpath;
}

bool cmGeneratorTarget::IsChrpathUsed(std::string const& config) const
{
  // Only certain target types have an rpath.
  if (!(this->GetType() == cmStateEnums::SHARED_LIBRARY ||
        this->GetType() == cmStateEnums::MODULE_LIBRARY ||
        this->GetType() == cmStateEnums::EXECUTABLE)) {
    return false;
  }

  // If the target will not be installed we do not need to change its
  // rpath.
  if (!this->Target->GetHaveInstallRule()) {
    return false;
  }

  // Skip chrpath if skipping rpath altogether.
  if (this->Makefile->IsOn("CMAKE_SKIP_RPATH")) {
    return false;
  }

  // Skip chrpath if it does not need to be changed at install time.
  if (this->GetPropertyAsBool("BUILD_WITH_INSTALL_RPATH")) {
    return false;
  }

  // Allow the user to disable builtin chrpath explicitly.
  if (this->Makefile->IsOn("CMAKE_NO_BUILTIN_CHRPATH")) {
    return false;
  }

  if (this->Makefile->IsOn("CMAKE_PLATFORM_HAS_INSTALLNAME")) {
    return true;
  }

  // Enable if the rpath flag uses a separator and the target uses
  // binaries we know how to edit.
  std::string ll = this->GetLinkerLanguage(config);
  if (!ll.empty()) {
    std::string sepVar =
      cmStrCat("CMAKE_SHARED_LIBRARY_RUNTIME_", ll, "_FLAG_SEP");
    cmValue sep = this->Makefile->GetDefinition(sepVar);
    if (cmNonempty(sep)) {
      // TODO: Add binary format check to ABI detection and get rid of
      // CMAKE_EXECUTABLE_FORMAT.
      if (cmValue fmt =
            this->Makefile->GetDefinition("CMAKE_EXECUTABLE_FORMAT")) {
        if (*fmt == "ELF") {
          return true;
        }
#if defined(CMake_USE_XCOFF_PARSER)
        if (*fmt == "XCOFF") {
          return true;
        }
#endif
      }
    }
  }
  return false;
}

bool cmGeneratorTarget::IsImportedSharedLibWithoutSOName(
  std::string const& config) const
{
  if (this->IsImported() && this->GetType() == cmStateEnums::SHARED_LIBRARY) {
    if (cmGeneratorTarget::ImportInfo const* info =
          this->GetImportInfo(config)) {
      return info->NoSOName;
    }
  }
  return false;
}

bool cmGeneratorTarget::HasMacOSXRpathInstallNameDir(
  std::string const& config) const
{
  TargetPtrToBoolMap& cache = this->MacOSXRpathInstallNameDirCache[config];
  auto const lookup = cache.find(this->Target);

  if (lookup != cache.cend()) {
    return lookup->second;
  }

  bool const result = this->DetermineHasMacOSXRpathInstallNameDir(config);
  cache[this->Target] = result;
  return result;
}

bool cmGeneratorTarget::DetermineHasMacOSXRpathInstallNameDir(
  std::string const& config) const
{
  bool install_name_is_rpath = false;
  bool macosx_rpath = false;

  if (!this->IsImported()) {
    if (this->GetType() != cmStateEnums::SHARED_LIBRARY) {
      return false;
    }
    cmValue install_name = this->GetProperty("INSTALL_NAME_DIR");
    bool use_install_name = this->MacOSXUseInstallNameDir();
    if (install_name && use_install_name && *install_name == "@rpath") {
      install_name_is_rpath = true;
    } else if (install_name && use_install_name) {
      return false;
    }
    if (!install_name_is_rpath) {
      macosx_rpath = this->MacOSXRpathInstallNameDirDefault();
    }
  } else {
    // Lookup the imported soname.
    if (cmGeneratorTarget::ImportInfo const* info =
          this->GetImportInfo(config)) {
      if (!info->NoSOName && !info->SOName.empty()) {
        if (cmHasLiteralPrefix(info->SOName, "@rpath/")) {
          install_name_is_rpath = true;
        }
      } else {
        std::string install_name;
        cmSystemTools::GuessLibraryInstallName(info->Location, install_name);
        if (install_name.find("@rpath") != std::string::npos) {
          install_name_is_rpath = true;
        }
      }
    }
  }

  if (!install_name_is_rpath && !macosx_rpath) {
    return false;
  }

  if (!this->Makefile->IsSet("CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG")) {
    std::ostringstream w;
    w << "Attempting to use ";
    if (macosx_rpath) {
      w << "MACOSX_RPATH";
    } else {
      w << "@rpath";
    }
    w << " without CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG being set.";
    w << "  This could be because you are using a Mac OS X version";
    w << " less than 10.5 or because CMake's platform configuration is";
    w << " corrupt.";
    cmake* cm = this->LocalGenerator->GetCMakeInstance();
    cm->IssueMessage(MessageType::FATAL_ERROR, w.str(), this->GetBacktrace());
  }

  return true;
}

bool cmGeneratorTarget::MacOSXRpathInstallNameDirDefault() const
{
  // we can't do rpaths when unsupported
  if (!this->Makefile->IsSet("CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG")) {
    return false;
  }

  cmValue macosx_rpath_str = this->GetProperty("MACOSX_RPATH");
  if (macosx_rpath_str) {
    return this->GetPropertyAsBool("MACOSX_RPATH");
  }

  return true;
}

bool cmGeneratorTarget::MacOSXUseInstallNameDir() const
{
  cmValue build_with_install_name =
    this->GetProperty("BUILD_WITH_INSTALL_NAME_DIR");
  if (build_with_install_name) {
    return build_with_install_name.IsOn();
  }

  cmPolicies::PolicyStatus cmp0068 = this->GetPolicyStatusCMP0068();
  if (cmp0068 == cmPolicies::NEW) {
    return false;
  }

  bool use_install_name = this->GetPropertyAsBool("BUILD_WITH_INSTALL_RPATH");

  if (use_install_name && cmp0068 == cmPolicies::WARN) {
    this->LocalGenerator->GetGlobalGenerator()->AddCMP0068WarnTarget(
      this->GetName());
  }

  return use_install_name;
}

bool cmGeneratorTarget::CanGenerateInstallNameDir(
  InstallNameType name_type) const
{
  cmPolicies::PolicyStatus cmp0068 = this->GetPolicyStatusCMP0068();

  if (cmp0068 == cmPolicies::NEW) {
    return true;
  }

  bool skip = this->Makefile->IsOn("CMAKE_SKIP_RPATH");
  if (name_type == INSTALL_NAME_FOR_INSTALL) {
    skip |= this->Makefile->IsOn("CMAKE_SKIP_INSTALL_RPATH");
  } else {
    skip |= this->GetPropertyAsBool("SKIP_BUILD_RPATH");
  }

  if (skip && cmp0068 == cmPolicies::WARN) {
    this->LocalGenerator->GetGlobalGenerator()->AddCMP0068WarnTarget(
      this->GetName());
  }

  return !skip;
}

std::string cmGeneratorTarget::GetSOName(
  std::string const& config, cmStateEnums::ArtifactType artifact) const
{
  if (this->IsImported()) {
    // Lookup the imported soname.
    if (cmGeneratorTarget::ImportInfo const* info =
          this->GetImportInfo(config)) {
      if (info->NoSOName) {
        // The imported library has no builtin soname so the name
        // searched at runtime will be just the filename.
        return cmSystemTools::GetFilenameName(info->Location);
      }
      // Use the soname given if any.
      if (this->IsFrameworkOnApple()) {
        auto fwDescriptor = this->GetGlobalGenerator()->SplitFrameworkPath(
          info->SOName, cmGlobalGenerator::FrameworkFormat::Strict);
        if (fwDescriptor) {
          return fwDescriptor->GetVersionedName();
        }
      }
      if (cmHasLiteralPrefix(info->SOName, "@rpath/")) {
        return info->SOName.substr(cmStrLen("@rpath/"));
      }
      return info->SOName;
    }
    return "";
  }
  // Compute the soname that will be built.
  return artifact == cmStateEnums::RuntimeBinaryArtifact
    ? this->GetLibraryNames(config).SharedObject
    : this->GetLibraryNames(config).ImportLibrary;
}

namespace {
bool shouldAddFullLevel(cmGeneratorTarget::BundleDirectoryLevel level)
{
  return level == cmGeneratorTarget::FullLevel;
}

bool shouldAddContentLevel(cmGeneratorTarget::BundleDirectoryLevel level)
{
  return level == cmGeneratorTarget::ContentLevel || shouldAddFullLevel(level);
}
}

std::string cmGeneratorTarget::GetAppBundleDirectory(
  std::string const& config, BundleDirectoryLevel level) const
{
  std::string fpath = cmStrCat(
    this->GetFullName(config, cmStateEnums::RuntimeBinaryArtifact), '.');
  cmValue ext = this->GetProperty("BUNDLE_EXTENSION");
  fpath += (ext ? *ext : "app");
  if (shouldAddContentLevel(level) &&
      !this->Makefile->PlatformIsAppleEmbedded()) {
    fpath += "/Contents";
    if (shouldAddFullLevel(level)) {
      fpath += "/MacOS";
    }
  }
  return fpath;
}

bool cmGeneratorTarget::IsBundleOnApple() const
{
  return this->IsFrameworkOnApple() || this->IsAppBundleOnApple() ||
    this->IsCFBundleOnApple();
}

bool cmGeneratorTarget::IsWin32Executable(std::string const& config) const
{
  return cmIsOn(cmGeneratorExpression::Evaluate(
    this->GetSafeProperty("WIN32_EXECUTABLE"), this->LocalGenerator, config));
}

std::string cmGeneratorTarget::GetCFBundleDirectory(
  std::string const& config, BundleDirectoryLevel level) const
{
  std::string fpath = cmStrCat(
    this->GetOutputName(config, cmStateEnums::RuntimeBinaryArtifact), '.');
  std::string ext;
  if (cmValue p = this->GetProperty("BUNDLE_EXTENSION")) {
    ext = *p;
  } else {
    if (this->IsXCTestOnApple()) {
      ext = "xctest";
    } else {
      ext = "bundle";
    }
  }
  fpath += ext;
  if (shouldAddContentLevel(level) &&
      !this->Makefile->PlatformIsAppleEmbedded()) {
    fpath += "/Contents";
    if (shouldAddFullLevel(level)) {
      fpath += "/MacOS";
    }
  }
  return fpath;
}

std::string cmGeneratorTarget::GetFrameworkDirectory(
  std::string const& config, BundleDirectoryLevel level) const
{
  std::string fpath = cmStrCat(
    this->GetOutputName(config, cmStateEnums::RuntimeBinaryArtifact), '.');
  cmValue ext = this->GetProperty("BUNDLE_EXTENSION");
  fpath += (ext ? *ext : "framework");
  if (shouldAddFullLevel(level) &&
      !this->Makefile->PlatformIsAppleEmbedded()) {
    fpath += "/Versions/";
    fpath += this->GetFrameworkVersion();
  }
  return fpath;
}

std::string cmGeneratorTarget::GetFullName(
  std::string const& config, cmStateEnums::ArtifactType artifact) const
{
  if (this->IsImported()) {
    return this->GetFullNameImported(config, artifact);
  }
  return this->GetFullNameInternal(config, artifact);
}

std::string cmGeneratorTarget::GetInstallNameDirForBuildTree(
  std::string const& config) const
{
  if (this->Makefile->IsOn("CMAKE_PLATFORM_HAS_INSTALLNAME")) {

    // If building directly for installation then the build tree install_name
    // is the same as the install tree.
    if (this->MacOSXUseInstallNameDir()) {
      std::string installPrefix =
        this->Makefile->GetSafeDefinition("CMAKE_INSTALL_PREFIX");
      return this->GetInstallNameDirForInstallTree(config, installPrefix);
    }

    // Use the build tree directory for the target.
    if (this->CanGenerateInstallNameDir(INSTALL_NAME_FOR_BUILD)) {
      std::string dir;
      if (this->MacOSXRpathInstallNameDirDefault()) {
        dir = "@rpath";
      } else {
        dir = this->GetDirectory(config);
      }
      dir += "/";
      return dir;
    }
  }
  return "";
}

std::string cmGeneratorTarget::GetInstallNameDirForInstallTree(
  std::string const& config, std::string const& installPrefix) const
{
  if (this->Makefile->IsOn("CMAKE_PLATFORM_HAS_INSTALLNAME")) {
    std::string dir;
    cmValue install_name_dir = this->GetProperty("INSTALL_NAME_DIR");

    if (this->CanGenerateInstallNameDir(INSTALL_NAME_FOR_INSTALL)) {
      if (cmNonempty(install_name_dir)) {
        dir = *install_name_dir;
        cmGeneratorExpression::ReplaceInstallPrefix(dir, installPrefix);
        dir =
          cmGeneratorExpression::Evaluate(dir, this->LocalGenerator, config);
        if (!dir.empty()) {
          dir = cmStrCat(dir, '/');
        }
      }
    }
    if (!install_name_dir) {
      if (this->MacOSXRpathInstallNameDirDefault()) {
        dir = "@rpath/";
      }
    }
    return dir;
  }
  return "";
}

cmListFileBacktrace cmGeneratorTarget::GetBacktrace() const
{
  return this->Target->GetBacktrace();
}

std::set<BT<std::pair<std::string, bool>>> const&
cmGeneratorTarget::GetUtilities() const
{
  return this->Target->GetUtilities();
}

bool cmGeneratorTarget::HaveWellDefinedOutputFiles() const
{
  return this->GetType() == cmStateEnums::STATIC_LIBRARY ||
    this->GetType() == cmStateEnums::SHARED_LIBRARY ||
    this->GetType() == cmStateEnums::MODULE_LIBRARY ||
    this->GetType() == cmStateEnums::OBJECT_LIBRARY ||
    this->GetType() == cmStateEnums::EXECUTABLE;
}

std::string const* cmGeneratorTarget::GetExportMacro() const
{
  // Define the symbol for targets that export symbols.
  if (this->GetType() == cmStateEnums::SHARED_LIBRARY ||
      this->GetType() == cmStateEnums::MODULE_LIBRARY ||
      this->IsExecutableWithExports()) {
    if (cmValue custom_export_name = this->GetProperty("DEFINE_SYMBOL")) {
      this->ExportMacro = *custom_export_name;
    } else {
      std::string in = cmStrCat(this->GetName(), "_EXPORTS");
      this->ExportMacro = cmSystemTools::MakeCidentifier(in);
    }
    return &this->ExportMacro;
  }
  return nullptr;
}

cmGeneratorTarget::NameComponents const&
cmGeneratorTarget::GetFullNameComponents(
  std::string const& config, cmStateEnums::ArtifactType artifact) const
{
  return this->GetFullNameInternalComponents(config, artifact);
}

std::string cmGeneratorTarget::BuildBundleDirectory(
  std::string const& base, std::string const& config,
  BundleDirectoryLevel level) const
{
  std::string fpath = base;
  if (this->IsAppBundleOnApple()) {
    fpath += this->GetAppBundleDirectory(config, level);
  }
  if (this->IsFrameworkOnApple()) {
    fpath += this->GetFrameworkDirectory(config, level);
  }
  if (this->IsCFBundleOnApple()) {
    fpath += this->GetCFBundleDirectory(config, level);
  }
  return fpath;
}

std::string cmGeneratorTarget::GetMacContentDirectory(
  std::string const& config, cmStateEnums::ArtifactType artifact) const
{
  // Start with the output directory for the target.
  std::string fpath = cmStrCat(this->GetDirectory(config, artifact), '/');
  BundleDirectoryLevel level = ContentLevel;
  if (this->IsFrameworkOnApple()) {
    // additional files with a framework go into the version specific
    // directory
    level = FullLevel;
  }
  fpath = this->BuildBundleDirectory(fpath, config, level);
  return fpath;
}

std::string cmGeneratorTarget::GetEffectiveFolderName() const
{
  std::string effectiveFolder;

  if (!this->GlobalGenerator->UseFolderProperty()) {
    return effectiveFolder;
  }

  cmValue targetFolder = this->GetProperty("FOLDER");
  if (targetFolder) {
    effectiveFolder += *targetFolder;
  }

  return effectiveFolder;
}

cmGeneratorTarget::CompileInfo const* cmGeneratorTarget::GetCompileInfo(
  std::string const& config) const
{
  // There is no compile information for imported targets.
  if (this->IsImported()) {
    return nullptr;
  }

  if (this->GetType() > cmStateEnums::OBJECT_LIBRARY) {
    std::string msg = cmStrCat("cmTarget::GetCompileInfo called for ",
                               this->GetName(), " which has type ",
                               cmState::GetTargetTypeName(this->GetType()));
    this->LocalGenerator->IssueMessage(MessageType::INTERNAL_ERROR, msg);
    return nullptr;
  }

  // Lookup/compute/cache the compile information for this configuration.
  std::string config_upper;
  if (!config.empty()) {
    config_upper = cmSystemTools::UpperCase(config);
  }
  auto i = this->CompileInfoMap.find(config_upper);
  if (i == this->CompileInfoMap.end()) {
    CompileInfo info;
    this->ComputePDBOutputDir("COMPILE_PDB", config, info.CompilePdbDir);
    CompileInfoMapType::value_type entry(config_upper, info);
    i = this->CompileInfoMap.insert(entry).first;
  }
  return &i->second;
}

cmGeneratorTarget::ModuleDefinitionInfo const*
cmGeneratorTarget::GetModuleDefinitionInfo(std::string const& config) const
{
  // A module definition file only makes sense on certain target types.
  if (this->GetType() != cmStateEnums::SHARED_LIBRARY &&
      this->GetType() != cmStateEnums::MODULE_LIBRARY &&
      !this->IsExecutableWithExports()) {
    return nullptr;
  }

  // Lookup/compute/cache the compile information for this configuration.
  std::string config_upper;
  if (!config.empty()) {
    config_upper = cmSystemTools::UpperCase(config);
  }
  auto i = this->ModuleDefinitionInfoMap.find(config_upper);
  if (i == this->ModuleDefinitionInfoMap.end()) {
    ModuleDefinitionInfo info;
    this->ComputeModuleDefinitionInfo(config, info);
    ModuleDefinitionInfoMapType::value_type entry(config_upper, info);
    i = this->ModuleDefinitionInfoMap.insert(entry).first;
  }
  return &i->second;
}

void cmGeneratorTarget::ComputeModuleDefinitionInfo(
  std::string const& config, ModuleDefinitionInfo& info) const
{
  this->GetModuleDefinitionSources(info.Sources, config);
  info.WindowsExportAllSymbols =
    this->Makefile->IsOn("CMAKE_SUPPORT_WINDOWS_EXPORT_ALL_SYMBOLS") &&
    this->GetPropertyAsBool("WINDOWS_EXPORT_ALL_SYMBOLS");
#if !defined(CMAKE_BOOTSTRAP)
  info.DefFileGenerated =
    info.WindowsExportAllSymbols || info.Sources.size() > 1;
#else
  // Our __create_def helper is not available during CMake bootstrap.
  info.DefFileGenerated = false;
#endif
  if (info.DefFileGenerated) {
    info.DefFile =
      this->GetObjectDirectory(config) /* has slash */ + "exports.def";
  } else if (!info.Sources.empty()) {
    info.DefFile = info.Sources.front()->GetFullPath();
  }
}

bool cmGeneratorTarget::IsAIX() const
{
  return this->Target->IsAIX();
}

bool cmGeneratorTarget::IsApple() const
{
  return this->Target->IsApple();
}

bool cmGeneratorTarget::IsDLLPlatform() const
{
  return this->Target->IsDLLPlatform();
}

void cmGeneratorTarget::GetAutoUicOptions(std::vector<std::string>& result,
                                          std::string const& config) const
{
  char const* prop =
    this->GetLinkInterfaceDependentStringProperty("AUTOUIC_OPTIONS", config);
  if (!prop) {
    return;
  }

  cmGeneratorExpressionDAGChecker dagChecker{
    this, "AUTOUIC_OPTIONS", nullptr, nullptr, this->LocalGenerator, config,
  };
  cmExpandList(cmGeneratorExpression::Evaluate(prop, this->LocalGenerator,
                                               config, this, &dagChecker),
               result);
}

void cmGeneratorTarget::TraceDependencies()
{
  // CMake-generated targets have no dependencies to trace.  Normally tracing
  // would find nothing anyway, but when building CMake itself the "install"
  // target command ends up referencing the "cmake" target but we do not
  // really want the dependency because "install" depend on "all" anyway.
  if (this->GetType() == cmStateEnums::GLOBAL_TARGET) {
    return;
  }

  // Use a helper object to trace the dependencies.
  cmTargetTraceDependencies tracer(this);
  tracer.Trace();
}

std::string cmGeneratorTarget::GetCompilePDBDirectory(
  std::string const& config) const
{
  if (CompileInfo const* info = this->GetCompileInfo(config)) {
    return info->CompilePdbDir;
  }
  return "";
}

std::vector<std::string> cmGeneratorTarget::GetAppleArchs(
  std::string const& config, cm::optional<std::string> lang) const
{
  cmList archList;
  if (!this->IsApple()) {
    return std::move(archList.data());
  }
  cmValue archs = nullptr;
  if (!config.empty()) {
    std::string defVarName =
      cmStrCat("OSX_ARCHITECTURES_", cmSystemTools::UpperCase(config));
    archs = this->GetProperty(defVarName);
  }
  if (!archs) {
    archs = this->GetProperty("OSX_ARCHITECTURES");
  }
  if (archs) {
    archList.assign(*archs);
  }
  if (archList.empty() &&
      // Fall back to a default architecture if no compiler target is set.
      (!lang ||
       this->Makefile
         ->GetDefinition(cmStrCat("CMAKE_", *lang, "_COMPILER_TARGET"))
         .IsEmpty())) {
    archList.assign(
      this->Makefile->GetDefinition("_CMAKE_APPLE_ARCHS_DEFAULT"));
  }
  return std::move(archList.data());
}

std::string const& cmGeneratorTarget::GetTargetLabelsString()
{
  this->targetLabelsString = this->GetSafeProperty("LABELS");
  std::replace(this->targetLabelsString.begin(),
               this->targetLabelsString.end(), ';', ',');
  return this->targetLabelsString;
}

namespace {

bool IsSupportedClassifiedFlagsLanguage(std::string const& lang)
{
  return lang == "CXX"_s;
}

bool CanUseCompilerLauncher(std::string const& lang)
{
  // Also found in `cmCommonTargetGenerator::GetCompilerLauncher`.
  return lang == "C"_s || lang == "CXX"_s || lang == "Fortran"_s ||
    lang == "CUDA"_s || lang == "HIP"_s || lang == "ISPC"_s ||
    lang == "OBJC"_s || lang == "OBJCXX"_s;
}

// FIXME: return a vector of `cm::string_view` instead to avoid lots of tiny
// allocations.
std::vector<std::string> SplitFlags(std::string const& flags)
{
  std::vector<std::string> options;

#ifdef _WIN32
  cmSystemTools::ParseWindowsCommandLine(flags.c_str(), options);
#else
  cmSystemTools::ParseUnixCommandLine(flags.c_str(), options);
#endif

  return options;
}

}

cmGeneratorTarget::ClassifiedFlags
cmGeneratorTarget::GetClassifiedFlagsForSource(cmSourceFile const* sf,
                                               std::string const& config)
{
  auto& sourceFlagsCache = this->Configs[config].SourceFlags;
  auto cacheEntry = sourceFlagsCache.lower_bound(sf);
  if (cacheEntry != sourceFlagsCache.end() && cacheEntry->first == sf) {
    return cacheEntry->second;
  }

  ClassifiedFlags flags;
  std::string const& lang = sf->GetLanguage();

  if (!IsSupportedClassifiedFlagsLanguage(lang)) {
    return flags;
  }

  auto* const lg = this->GetLocalGenerator();
  auto const* const mf = this->Makefile;

  // Compute the compiler launcher flags.
  if (CanUseCompilerLauncher(lang)) {
    // Compiler launchers are all execution flags and should not be relevant to
    // the actual compilation.
    FlagClassification cls = FlagClassification::ExecutionFlag;
    FlagKind kind = FlagKind::NotAFlag;

    std::string const clauncher_prop = cmStrCat(lang, "_COMPILER_LAUNCHER");
    cmValue clauncher = this->GetProperty(clauncher_prop);
    std::string const evaluatedClauncher = cmGeneratorExpression::Evaluate(
      *clauncher, lg, config, this, nullptr, this, lang);

    for (auto const& flag : SplitFlags(evaluatedClauncher)) {
      flags.emplace_back(cls, kind, flag);
    }
  }

  ClassifiedFlags define_flags;
  ClassifiedFlags include_flags;
  ClassifiedFlags compile_flags;

  SourceVariables sfVars = this->GetSourceVariables(sf, config);

  // Compute language flags.
  {
    FlagClassification cls = FlagClassification::BaselineFlag;
    FlagKind kind = FlagKind::Compile;

    std::string mfFlags;
    // Explicitly add the explicit language flag before any other flag
    // so user flags can override it.
    this->AddExplicitLanguageFlags(mfFlags, *sf);

    for (auto const& flag : SplitFlags(mfFlags)) {
      flags.emplace_back(cls, kind, flag);
    }
  }

  std::unordered_map<std::string, std::string> pchSources;
  std::string filterArch;

  {
    std::vector<std::string> pchArchs = this->GetPchArchs(config, lang);

    for (std::string const& arch : pchArchs) {
      std::string const pchSource = this->GetPchSource(config, lang, arch);
      if (pchSource == sf->GetFullPath()) {
        filterArch = arch;
      }
      if (!pchSource.empty()) {
        pchSources.insert(std::make_pair(pchSource, arch));
      }
    }
  }

  // Compute target-wide flags.
  {
    FlagClassification cls = FlagClassification::BaselineFlag;

    // Compile flags
    {
      FlagKind kind = FlagKind::Compile;
      std::string targetFlags;

      lg->GetTargetCompileFlags(this, config, lang, targetFlags, filterArch);

      for (auto&& flag : SplitFlags(targetFlags)) {
        compile_flags.emplace_back(cls, kind, std::move(flag));
      }
    }

    // Define flags
    {
      FlagKind kind = FlagKind::Definition;
      std::set<std::string> defines;

      lg->GetTargetDefines(this, config, lang, defines);

      std::string defineFlags;
      lg->JoinDefines(defines, defineFlags, lang);

      for (auto&& flag : SplitFlags(defineFlags)) {
        define_flags.emplace_back(cls, kind, std::move(flag));
      }
    }

    // Include flags
    {
      FlagKind kind = FlagKind::Include;
      std::vector<std::string> includes;

      lg->GetIncludeDirectories(includes, this, lang, config);
      auto includeFlags =
        lg->GetIncludeFlags(includes, this, lang, config, false);

      for (auto&& flag : SplitFlags(includeFlags)) {
        include_flags.emplace_back(cls, kind, std::move(flag));
      }
    }
  }

  std::string const COMPILE_FLAGS("COMPILE_FLAGS");
  std::string const COMPILE_OPTIONS("COMPILE_OPTIONS");

  cmGeneratorExpressionInterpreter genexInterpreter(lg, config, this, lang);

  // Source-specific flags.
  {
    FlagClassification cls = FlagClassification::PrivateFlag;
    FlagKind kind = FlagKind::Compile;

    std::string sourceFlags;

    if (cmValue cflags = sf->GetProperty(COMPILE_FLAGS)) {
      lg->AppendFlags(sourceFlags,
                      genexInterpreter.Evaluate(*cflags, COMPILE_FLAGS));
    }

    if (cmValue coptions = sf->GetProperty(COMPILE_OPTIONS)) {
      lg->AppendCompileOptions(
        sourceFlags, genexInterpreter.Evaluate(*coptions, COMPILE_OPTIONS));
    }

    for (auto&& flag : SplitFlags(sourceFlags)) {
      compile_flags.emplace_back(cls, kind, std::move(flag));
    }

    // Dependency tracking flags.
    {
      if (!sfVars.DependencyFlags.empty()) {
        cmRulePlaceholderExpander::RuleVariables vars;
        auto rulePlaceholderExpander = lg->CreateRulePlaceholderExpander();

        vars.DependencyFile = sfVars.DependencyFile.c_str();
        vars.DependencyTarget = sfVars.DependencyTarget.c_str();

        std::string depfileFlags = sfVars.DependencyFlags;
        rulePlaceholderExpander->ExpandRuleVariables(lg, depfileFlags, vars);
        for (auto&& flag : SplitFlags(depfileFlags)) {
          compile_flags.emplace_back(FlagClassification::LocationFlag,
                                     FlagKind::BuildSystem, std::move(flag));
        }
      }
    }
  }

  // Precompiled headers.
  {
    FlagClassification cls = FlagClassification::PrivateFlag;
    FlagKind kind = FlagKind::Compile;

    std::string pchFlags;

    // Add precompile headers compile options.
    if (!sf->GetProperty("SKIP_PRECOMPILE_HEADERS")) {
      if (!pchSources.empty()) {
        std::string pchOptions;
        auto pchIt = pchSources.find(sf->GetFullPath());
        if (pchIt != pchSources.end()) {
          pchOptions =
            this->GetPchCreateCompileOptions(config, lang, pchIt->second);
        } else {
          pchOptions = this->GetPchUseCompileOptions(config, lang);
        }

        this->LocalGenerator->AppendCompileOptions(
          pchFlags, genexInterpreter.Evaluate(pchOptions, COMPILE_OPTIONS));
      }
    }

    for (auto&& flag : SplitFlags(pchFlags)) {
      compile_flags.emplace_back(cls, kind, std::move(flag));
    }
  }

  // C++ module flags.
  if (lang == "CXX"_s) {
    FlagClassification cls = FlagClassification::LocationFlag;
    FlagKind kind = FlagKind::BuildSystem;

    std::string bmiFlags;

    auto const* fs = this->GetFileSetForSource(config, sf);
    if (fs && fs->GetType() == "CXX_MODULES"_s) {
      if (lang != "CXX"_s) {
        mf->IssueMessage(
          MessageType::FATAL_ERROR,
          cmStrCat(
            "Target \"", this->Target->GetName(), "\" contains the source\n  ",
            sf->GetFullPath(), "\nin a file set of type \"", fs->GetType(),
            R"(" but the source is not classified as a "CXX" source.)"));
      }

      if (!this->Target->IsNormal()) {
        auto flag = mf->GetSafeDefinition("CMAKE_CXX_MODULE_BMI_ONLY_FLAG");
        cmRulePlaceholderExpander::RuleVariables compileObjectVars;
        compileObjectVars.Object = sfVars.ObjectFileDir.c_str();
        auto rulePlaceholderExpander = lg->CreateRulePlaceholderExpander();
        rulePlaceholderExpander->ExpandRuleVariables(lg, flag,
                                                     compileObjectVars);
        lg->AppendCompileOptions(bmiFlags, flag);
      }
    }

    for (auto&& flag : SplitFlags(bmiFlags)) {
      compile_flags.emplace_back(cls, kind, std::move(flag));
    }
  }

  cmRulePlaceholderExpander::RuleVariables vars;
  vars.CMTargetName = this->GetName().c_str();
  vars.CMTargetType = cmState::GetTargetTypeName(this->GetType()).c_str();
  vars.CMTargetLabels = this->GetTargetLabelsString().c_str();
  vars.Language = lang.c_str();

  auto const sfPath = this->LocalGenerator->ConvertToOutputFormat(
    sf->GetFullPath(), cmOutputConverter::SHELL);

  // Compute the base compiler command line. We'll find placeholders and
  // replace them with arguments later in this function.
  {
    FlagClassification cls = FlagClassification::ExecutionFlag;
    FlagKind kind = FlagKind::NotAFlag;

    std::string const cmdVar = cmStrCat("CMAKE_", lang, "_COMPILE_OBJECT");
    std::string const& compileCmd = mf->GetRequiredDefinition(cmdVar);
    cmList compileCmds(compileCmd); // FIXME: which command to use?
    std::string& cmd = compileCmds[0];
    auto rulePlaceholderExpander = lg->CreateRulePlaceholderExpander();

    static std::string const PlaceholderDefines = "__CMAKE_DEFINES";
    static std::string const PlaceholderIncludes = "__CMAKE_INCLUDES";
    static std::string const PlaceholderFlags = "__CMAKE_FLAGS";
    static std::string const PlaceholderSource = "__CMAKE_SOURCE";

    vars.Defines = PlaceholderDefines.c_str();
    vars.Includes = PlaceholderIncludes.c_str();
    vars.TargetPDB = sfVars.TargetPDB.c_str();
    vars.TargetCompilePDB = sfVars.TargetCompilePDB.c_str();
    vars.Object = sfVars.ObjectFileDir.c_str();
    vars.ObjectDir = sfVars.ObjectDir.c_str();
    vars.ObjectFileDir = sfVars.ObjectFileDir.c_str();
    vars.TargetSupportDir = sfVars.TargetSupportDir.c_str();
    vars.Flags = PlaceholderFlags.c_str();
    vars.DependencyFile = sfVars.DependencyFile.c_str();
    vars.DependencyTarget = sfVars.DependencyTarget.c_str();
    vars.Source = PlaceholderSource.c_str();

    rulePlaceholderExpander->ExpandRuleVariables(lg, cmd, vars);
    for (auto&& flag : SplitFlags(cmd)) {
      if (flag == PlaceholderDefines) {
        flags.insert(flags.end(), define_flags.begin(), define_flags.end());
      } else if (flag == PlaceholderIncludes) {
        flags.insert(flags.end(), include_flags.begin(), include_flags.end());
      } else if (flag == PlaceholderFlags) {
        flags.insert(flags.end(), compile_flags.begin(), compile_flags.end());
      } else if (flag == PlaceholderSource) {
        flags.emplace_back(FlagClassification::LocationFlag,
                           FlagKind::BuildSystem, sfPath);
      } else {
        flags.emplace_back(cls, kind, std::move(flag));
        // All remaining flags here are build system flags.
        kind = FlagKind::BuildSystem;
      }
    }
  }

  cacheEntry = sourceFlagsCache.emplace_hint(cacheEntry, sf, std::move(flags));
  return cacheEntry->second;
}

cmGeneratorTarget::SourceVariables cmGeneratorTarget::GetSourceVariables(
  cmSourceFile const* sf, std::string const& config)
{
  SourceVariables vars;
  auto const language = sf->GetLanguage();
  auto const targetType = this->GetType();
  auto const* const lg = this->GetLocalGenerator();
  auto const* const gg = this->GetGlobalGenerator();
  auto const* const mf = this->Makefile;

  // PDB settings.
  {
    if (mf->GetDefinition("MSVC_C_ARCHITECTURE_ID") ||
        mf->GetDefinition("MSVC_CXX_ARCHITECTURE_ID") ||
        mf->GetDefinition("MSVC_CUDA_ARCHITECTURE_ID")) {
      std::string pdbPath;
      std::string compilePdbPath;
      if (targetType <= cmStateEnums::OBJECT_LIBRARY) {
        compilePdbPath = this->GetCompilePDBPath(config);
        if (compilePdbPath.empty()) {
          // Match VS default: `$(IntDir)vc$(PlatformToolsetVersion).pdb`.
          // A trailing slash tells the toolchain to add its default file name.
          compilePdbPath = this->GetSupportDirectory();
          if (gg->IsMultiConfig()) {
            compilePdbPath = cmStrCat(compilePdbPath, '/', config);
          }
          compilePdbPath += '/';
          if (targetType == cmStateEnums::STATIC_LIBRARY) {
            // Match VS default for static libs: `$(IntDir)$(ProjectName).pdb`.
            compilePdbPath = cmStrCat(compilePdbPath, this->GetName(), ".pdb");
          }
        }
      }

      if (targetType == cmStateEnums::EXECUTABLE ||
          targetType == cmStateEnums::STATIC_LIBRARY ||
          targetType == cmStateEnums::SHARED_LIBRARY ||
          targetType == cmStateEnums::MODULE_LIBRARY) {
        pdbPath = cmStrCat(this->GetPDBDirectory(config), '/',
                           this->GetPDBName(config));
      }

      vars.TargetPDB = lg->ConvertToOutputFormat(
        gg->ConvertToOutputPath(std::move(pdbPath)), cmOutputConverter::SHELL);
      vars.TargetCompilePDB = lg->ConvertToOutputFormat(
        gg->ConvertToOutputPath(std::move(compilePdbPath)),
        cmOutputConverter::SHELL);
    }
  }

  // Object settings.
  {
    std::string const targetSupportDir = lg->MaybeRelativeToTopBinDir(
      gg->ConvertToOutputPath(this->GetCMFSupportDirectory()));
    std::string const objectDir = gg->ConvertToOutputPath(
      cmStrCat(this->GetSupportDirectory(), gg->GetConfigDirectory(config)));
    std::string const objectFileName = this->GetObjectName(sf);
    std::string const objectFilePath =
      cmStrCat(objectDir, '/', objectFileName);

    vars.TargetSupportDir =
      lg->ConvertToOutputFormat(targetSupportDir, cmOutputConverter::SHELL);
    vars.ObjectDir =
      lg->ConvertToOutputFormat(objectDir, cmOutputConverter::SHELL);
    vars.ObjectFileDir =
      lg->ConvertToOutputFormat(objectFilePath, cmOutputConverter::SHELL);

    // Dependency settings.
    {
      std::string const depfileFormatName =
        cmStrCat("CMAKE_", language, "_DEPFILE_FORMAT");
      std::string const depfileFormat =
        mf->GetSafeDefinition(depfileFormatName);
      if (depfileFormat != "msvc"_s) {
        std::string const flagsName =
          cmStrCat("CMAKE_DEPFILE_FLAGS_", language);
        std::string const depfileFlags = mf->GetSafeDefinition(flagsName);
        if (!depfileFlags.empty()) {
          bool replaceExt = false;
          if (!language.empty()) {
            std::string const repVar =
              cmStrCat("CMAKE_", language, "_DEPFILE_EXTENSION_REPLACE");
            replaceExt = mf->IsOn(repVar);
          }
          std::string const depfilePath = cmStrCat(
            objectDir, '/',
            replaceExt
              ? cmSystemTools::GetFilenameWithoutLastExtension(objectFileName)
              : objectFileName,
            ".d");

          vars.DependencyFlags = depfileFlags;
          vars.DependencyTarget = vars.ObjectFileDir;
          vars.DependencyFile =
            lg->ConvertToOutputFormat(depfilePath, cmOutputConverter::SHELL);
        }
      }
    }
  }

  return vars;
}

void cmGeneratorTarget::AddExplicitLanguageFlags(std::string& flags,
                                                 cmSourceFile const& sf) const
{
  cmValue lang = sf.GetProperty("LANGUAGE");
  if (!lang) {
    return;
  }

  switch (this->GetPolicyStatusCMP0119()) {
    case cmPolicies::WARN:
      CM_FALLTHROUGH;
    case cmPolicies::OLD:
      // The OLD behavior is to not add explicit language flags.
      return;
    case cmPolicies::NEW:
      // The NEW behavior is to add explicit language flags.
      break;
  }

  this->LocalGenerator->AppendFeatureOptions(flags, *lang,
                                             "EXPLICIT_LANGUAGE");
}

void cmGeneratorTarget::AddCUDAArchitectureFlags(cmBuildStep compileOrLink,
                                                 std::string const& config,
                                                 std::string& flags) const
{
  std::string arch = this->GetSafeProperty("CUDA_ARCHITECTURES");

  if (arch.empty()) {
    switch (this->GetPolicyStatusCMP0104()) {
      case cmPolicies::WARN:
        if (!this->LocalGenerator->GetCMakeInstance()->GetIsInTryCompile()) {
          this->Makefile->IssueMessage(
            MessageType::AUTHOR_WARNING,
            cmPolicies::GetPolicyWarning(cmPolicies::CMP0104) +
              "\nCUDA_ARCHITECTURES is empty for target \"" + this->GetName() +
              "\".");
        }
        CM_FALLTHROUGH;
      case cmPolicies::OLD:
        break;
      default:
        this->Makefile->IssueMessage(
          MessageType::FATAL_ERROR,
          "CUDA_ARCHITECTURES is empty for target \"" + this->GetName() +
            "\".");
    }
  }

  // If CUDA_ARCHITECTURES is false we don't add any architectures.
  if (cmIsOff(arch)) {
    return;
  }

  this->AddCUDAArchitectureFlagsImpl(compileOrLink, config, "CUDA",
                                     std::move(arch), flags);
}

void cmGeneratorTarget::AddCUDAArchitectureFlagsImpl(cmBuildStep compileOrLink,
                                                     std::string const& config,
                                                     std::string const& lang,
                                                     std::string arch,
                                                     std::string& flags) const
{
  std::string const& compiler = this->Makefile->GetSafeDefinition(
    cmStrCat("CMAKE_", lang, "_COMPILER_ID"));
  bool const ipoEnabled = this->IsIPOEnabled(lang, config);

  // Check for special modes: `all`, `all-major`.
  if (arch == "all" || arch == "all-major") {
    if (compiler == "NVIDIA" &&
        cmSystemTools::VersionCompare(cmSystemTools::OP_GREATER_EQUAL,
                                      this->Makefile->GetDefinition(cmStrCat(
                                        "CMAKE_", lang, "_COMPILER_VERSION")),
                                      "11.5")) {
      flags = cmStrCat(flags, " -arch=", arch);
      return;
    }
    if (arch == "all") {
      arch = *this->Makefile->GetDefinition(
        cmStrCat("CMAKE_", lang, "_ARCHITECTURES_ALL"));
    } else if (arch == "all-major") {
      arch = *this->Makefile->GetDefinition(
        cmStrCat("CMAKE_", lang, "_ARCHITECTURES_ALL_MAJOR"));
    }
  } else if (arch == "native") {
    cmValue native = this->Makefile->GetDefinition(
      cmStrCat("CMAKE_", lang, "_ARCHITECTURES_NATIVE"));
    if (native.IsEmpty()) {
      this->Makefile->IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat(lang,
                 "_ARCHITECTURES is set to \"native\", but no NVIDIA GPU was "
                 "detected."));
    }
    if (compiler == "NVIDIA" &&
        cmSystemTools::VersionCompare(cmSystemTools::OP_GREATER_EQUAL,
                                      this->Makefile->GetDefinition(cmStrCat(
                                        "CMAKE_", lang, "_COMPILER_VERSION")),
                                      "11.6")) {
      flags = cmStrCat(flags, " -arch=", arch);
      return;
    }
    arch = *native;
  }

  struct CudaArchitecture
  {
    std::string name;
    bool real{ true };
    bool virtual_{ true };
  };
  std::vector<CudaArchitecture> architectures;

  {
    cmList options(arch);

    for (auto& option : options) {
      CudaArchitecture architecture;

      // Architecture name is up to the first specifier.
      std::size_t pos = option.find_first_of('-');
      architecture.name = option.substr(0, pos);

      if (pos != std::string::npos) {
        cm::string_view specifier{ option.c_str() + pos + 1,
                                   option.length() - pos - 1 };

        if (specifier == "real") {
          architecture.real = true;
          architecture.virtual_ = false;
        } else if (specifier == "virtual") {
          architecture.real = false;
          architecture.virtual_ = true;
        } else {
          this->Makefile->IssueMessage(
            MessageType::FATAL_ERROR,
            "Unknown CUDA architecture specifier \"" + std::string(specifier) +
              "\".");
        }
      }

      architectures.emplace_back(architecture);
    }
  }

  if (compiler == "NVIDIA") {
    if (ipoEnabled && compileOrLink == cmBuildStep::Link) {
      if (cmValue cudaIPOFlags = this->Makefile->GetDefinition(
            cmStrCat("CMAKE_", lang, "_LINK_OPTIONS_IPO"))) {
        flags += *cudaIPOFlags;
      }
    }

    for (CudaArchitecture& architecture : architectures) {
      flags +=
        " \"--generate-code=arch=compute_" + architecture.name + ",code=[";

      if (architecture.virtual_) {
        flags += "compute_" + architecture.name;

        if (ipoEnabled || architecture.real) {
          flags += ",";
        }
      }

      if (ipoEnabled) {
        if (compileOrLink == cmBuildStep::Compile) {
          flags += "lto_" + architecture.name;
        } else if (compileOrLink == cmBuildStep::Link) {
          flags += "sm_" + architecture.name;
        }
      } else if (architecture.real) {
        flags += "sm_" + architecture.name;
      }

      flags += "]\"";
    }
  } else if (compiler == "Clang" && compileOrLink == cmBuildStep::Compile) {
    for (CudaArchitecture& architecture : architectures) {
      flags += " --cuda-gpu-arch=sm_" + architecture.name;

      if (!architecture.real) {
        this->Makefile->IssueMessage(
          MessageType::WARNING,
          "Clang doesn't support disabling CUDA real code generation.");
      }

      if (!architecture.virtual_) {
        flags += " --no-cuda-include-ptx=sm_" + architecture.name;
      }
    }
  }
}

void cmGeneratorTarget::AddISPCTargetFlags(std::string& flags) const
{
  std::string const& arch = this->GetSafeProperty("ISPC_INSTRUCTION_SETS");

  // If ISPC_TARGET is false we don't add any architectures.
  if (cmIsOff(arch)) {
    return;
  }

  std::string const& compiler =
    this->Makefile->GetSafeDefinition("CMAKE_ISPC_COMPILER_ID");

  if (compiler == "Intel") {
    cmList targets(arch);
    if (!targets.empty()) {
      flags += cmStrCat(" --target=", cmWrap("", targets, "", ","));
    }
  }
}

void cmGeneratorTarget::AddHIPArchitectureFlags(cmBuildStep compileOrLink,
                                                std::string const& config,
                                                std::string& flags) const
{
  std::string arch = this->GetSafeProperty("HIP_ARCHITECTURES");

  if (arch.empty()) {
    this->Makefile->IssueMessage(MessageType::FATAL_ERROR,
                                 "HIP_ARCHITECTURES is empty for target \"" +
                                   this->GetName() + "\".");
  }

  // If HIP_ARCHITECTURES is false we don't add any architectures.
  if (cmIsOff(arch)) {
    return;
  }

  if (this->Makefile->GetSafeDefinition("CMAKE_HIP_PLATFORM") == "nvidia") {
    this->AddCUDAArchitectureFlagsImpl(compileOrLink, config, "HIP",
                                       std::move(arch), flags);
    return;
  }

  cmList options(arch);

  for (std::string& option : options) {
    flags += " --offload-arch=" + option;
  }
}

void cmGeneratorTarget::AddCUDAToolkitFlags(std::string& flags) const
{
  std::string const& compiler =
    this->Makefile->GetSafeDefinition("CMAKE_CUDA_COMPILER_ID");

  if (compiler == "Clang") {
    // Pass CUDA toolkit explicitly to Clang.
    // Clang's searching for the system CUDA toolkit isn't very good and it's
    // expected the user will explicitly pass the toolkit path.
    // This also avoids Clang having to search for the toolkit on every
    // invocation.
    std::string toolkitRoot =
      this->Makefile->GetSafeDefinition("CMAKE_CUDA_COMPILER_LIBRARY_ROOT");

    if (!toolkitRoot.empty()) {
      flags += " --cuda-path=" +
        this->LocalGenerator->ConvertToOutputFormat(toolkitRoot,
                                                    cmOutputConverter::SHELL);
    }
  }
}

//----------------------------------------------------------------------------
std::string cmGeneratorTarget::GetFeatureSpecificLinkRuleVariable(
  std::string const& var, std::string const& lang,
  std::string const& config) const
{
  if (this->IsIPOEnabled(lang, config)) {
    std::string varIPO = var + "_IPO";
    if (this->Makefile->IsDefinitionSet(varIPO)) {
      return varIPO;
    }
  }

  return var;
}

//----------------------------------------------------------------------------
std::string cmGeneratorTarget::GetCreateRuleVariable(
  std::string const& lang, std::string const& config) const
{
  switch (this->GetType()) {
    case cmStateEnums::STATIC_LIBRARY: {
      std::string var = "CMAKE_" + lang + "_CREATE_STATIC_LIBRARY";
      return this->GetFeatureSpecificLinkRuleVariable(var, lang, config);
    }
    case cmStateEnums::SHARED_LIBRARY:
      if (this->IsArchivedAIXSharedLibrary()) {
        return "CMAKE_" + lang + "_CREATE_SHARED_LIBRARY_ARCHIVE";
      }
      return "CMAKE_" + lang + "_CREATE_SHARED_LIBRARY";
    case cmStateEnums::MODULE_LIBRARY:
      return "CMAKE_" + lang + "_CREATE_SHARED_MODULE";
    case cmStateEnums::EXECUTABLE:
      if (this->IsExecutableWithExports()) {
        std::string linkExeWithExports =
          "CMAKE_" + lang + "_LINK_EXECUTABLE_WITH_EXPORTS";
        if (this->Makefile->IsDefinitionSet(linkExeWithExports)) {
          return linkExeWithExports;
        }
      }
      return "CMAKE_" + lang + "_LINK_EXECUTABLE";
    default:
      break;
  }
  return "";
}

//----------------------------------------------------------------------------
std::string cmGeneratorTarget::GetClangTidyExportFixesDirectory(
  std::string const& lang) const
{
  cmValue val =
    this->GetProperty(cmStrCat(lang, "_CLANG_TIDY_EXPORT_FIXES_DIR"));
  if (!cmNonempty(val)) {
    return {};
  }

  std::string path = *val;
  if (!cmSystemTools::FileIsFullPath(path)) {
    path =
      cmStrCat(this->LocalGenerator->GetCurrentBinaryDirectory(), '/', path);
  }
  return cmSystemTools::CollapseFullPath(path);
}

struct CycleWatcher
{
  CycleWatcher(bool& flag)
    : Flag(flag)
  {
    this->Flag = true;
  }
  ~CycleWatcher() { this->Flag = false; }
  bool& Flag;
};

cmGeneratorTarget const* cmGeneratorTarget::GetPchReuseTarget() const
{
  if (this->ComputingPchReuse) {
    // TODO: Get the full cycle.
    if (!this->PchReuseCycleDetected) {
      this->Makefile->IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat("Circular PCH reuse target involving '", this->GetName(),
                 '\''));
    }
    this->PchReuseCycleDetected = true;
    return nullptr;
  }
  CycleWatcher watch(this->ComputingPchReuse);
  (void)watch;
  cmValue pchReuseFrom = this->GetProperty("PRECOMPILE_HEADERS_REUSE_FROM");
  if (!pchReuseFrom) {
    return nullptr;
  }
  cmGeneratorTarget const* generatorTarget =
    this->GetGlobalGenerator()->FindGeneratorTarget(*pchReuseFrom);
  if (!generatorTarget) {
    this->Makefile->IssueMessage(
      MessageType::FATAL_ERROR,
      cmStrCat(
        "Target \"", *pchReuseFrom, "\" for the \"", this->GetName(),
        R"(" target's "PRECOMPILE_HEADERS_REUSE_FROM" property does not exist.)"));
  }
  if (this->GetProperty("PRECOMPILE_HEADERS").IsOn()) {
    this->Makefile->IssueMessage(
      MessageType::FATAL_ERROR,
      cmStrCat("PRECOMPILE_HEADERS property is already set on target (\"",
               this->GetName(), "\")\n"));
  }

  if (generatorTarget) {
    if (generatorTarget->GetPropertyAsBool("DISABLE_PRECOMPILE_HEADERS")) {
      this->Makefile->IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat(
          "Target \"", *pchReuseFrom, "\" for the \"", this->GetName(),
          R"(" target's "PRECOMPILE_HEADERS_REUSE_FROM" property has set "DISABLE_PRECOMPILE_HEADERS".)"));
      return nullptr;
    }

    if (auto const* recurseReuseTarget =
          generatorTarget->GetPchReuseTarget()) {
      return recurseReuseTarget;
    }
  }
  return generatorTarget;
}

cmGeneratorTarget* cmGeneratorTarget::GetPchReuseTarget()
{
  if (this->ComputingPchReuse) {
    // TODO: Get the full cycle.
    if (!this->PchReuseCycleDetected) {
      this->Makefile->IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat("Circular PCH reuse target involving '", this->GetName(),
                 '\''));
    }
    this->PchReuseCycleDetected = true;
    return nullptr;
  }
  CycleWatcher watch(this->ComputingPchReuse);
  (void)watch;
  cmValue pchReuseFrom = this->GetProperty("PRECOMPILE_HEADERS_REUSE_FROM");
  if (!pchReuseFrom) {
    return nullptr;
  }
  cmGeneratorTarget* generatorTarget =
    this->GetGlobalGenerator()->FindGeneratorTarget(*pchReuseFrom);
  if (!generatorTarget) {
    this->Makefile->IssueMessage(
      MessageType::FATAL_ERROR,
      cmStrCat(
        "Target \"", *pchReuseFrom, "\" for the \"", this->GetName(),
        R"(" target's "PRECOMPILE_HEADERS_REUSE_FROM" property does not exist.)"));
  }
  if (this->GetProperty("PRECOMPILE_HEADERS").IsOn()) {
    this->Makefile->IssueMessage(
      MessageType::FATAL_ERROR,
      cmStrCat("PRECOMPILE_HEADERS property is already set on target (\"",
               this->GetName(), "\")\n"));
  }

  if (generatorTarget) {
    if (generatorTarget->GetPropertyAsBool("DISABLE_PRECOMPILE_HEADERS")) {
      this->Makefile->IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat(
          "Target \"", *pchReuseFrom, "\" for the \"", this->GetName(),
          R"(" target's "PRECOMPILE_HEADERS_REUSE_FROM" property has set "DISABLE_PRECOMPILE_HEADERS".)"));
      return nullptr;
    }

    if (auto* recurseReuseTarget = generatorTarget->GetPchReuseTarget()) {
      return recurseReuseTarget;
    }
  }
  return generatorTarget;
}

std::vector<std::string> cmGeneratorTarget::GetPchArchs(
  std::string const& config, std::string const& lang) const
{
  std::vector<std::string> pchArchs;
  if (!this->GetGlobalGenerator()->IsXcode()) {
    pchArchs = this->GetAppleArchs(config, lang);
  }
  if (pchArchs.size() < 2) {
    // We do not need per-arch PCH files when building for one architecture.
    pchArchs = { {} };
  }
  return pchArchs;
}

std::string cmGeneratorTarget::GetPchHeader(std::string const& config,
                                            std::string const& language,
                                            std::string const& arch) const
{
  if (language != "C" && language != "CXX" && language != "OBJC" &&
      language != "OBJCXX") {
    return std::string();
  }

  if (this->GetPropertyAsBool("DISABLE_PRECOMPILE_HEADERS")) {
    return std::string();
  }
  cmGeneratorTarget const* generatorTarget = this;
  cmGeneratorTarget const* reuseTarget = this->GetPchReuseTarget();
  bool const haveReuseTarget = reuseTarget && reuseTarget != this;
  if (reuseTarget) {
    generatorTarget = reuseTarget;
  }

  auto const inserted =
    this->PchHeaders.insert(std::make_pair(language + config + arch, ""));
  if (inserted.second) {
    std::vector<BT<std::string>> const headers =
      this->GetPrecompileHeaders(config, language);
    if (headers.empty() && !haveReuseTarget) {
      return std::string();
    }
    std::string& filename = inserted.first->second;

    std::map<std::string, std::string> const languageToExtension = {
      { "C", ".h" },
      { "CXX", ".hxx" },
      { "OBJC", ".objc.h" },
      { "OBJCXX", ".objcxx.hxx" }
    };

    filename = generatorTarget->GetCMFSupportDirectory();

    if (this->GetGlobalGenerator()->IsMultiConfig()) {
      filename = cmStrCat(filename, '/', config);
    }

    // This is acceptable as its the source file, won't have a rename/hash
    filename =
      cmStrCat(filename, "/cmake_pch", arch.empty() ? "" : cmStrCat('_', arch),
               languageToExtension.at(language));

    std::string const filename_tmp = cmStrCat(filename, ".tmp");
    if (!haveReuseTarget) {
      cmValue pchPrologue =
        this->Makefile->GetDefinition("CMAKE_PCH_PROLOGUE");
      cmValue pchEpilogue =
        this->Makefile->GetDefinition("CMAKE_PCH_EPILOGUE");

      std::string firstHeaderOnDisk;
      {
        cmGeneratedFileStream file(
          filename_tmp, false,
          this->GetGlobalGenerator()->GetMakefileEncoding());
        file << "/* generated by CMake */\n\n";
        if (pchPrologue) {
          file << *pchPrologue << "\n";
        }
        if (this->GetGlobalGenerator()->IsXcode()) {
          file << "#ifndef CMAKE_SKIP_PRECOMPILE_HEADERS\n";
        }
        if (language == "CXX" && !this->GetGlobalGenerator()->IsXcode()) {
          file << "#ifdef __cplusplus\n";
        }
        for (auto const& header_bt : headers) {
          if (header_bt.Value.empty()) {
            continue;
          }
          if (header_bt.Value[0] == '<' || header_bt.Value[0] == '\"') {
            file << "#include " << header_bt.Value << "\n";
          } else {
            file << "#include \"" << header_bt.Value << "\"\n";
          }

          if (cmSystemTools::FileExists(header_bt.Value) &&
              firstHeaderOnDisk.empty()) {
            firstHeaderOnDisk = header_bt.Value;
          }
        }
        if (language == "CXX" && !this->GetGlobalGenerator()->IsXcode()) {
          file << "#endif // __cplusplus\n";
        }
        if (this->GetGlobalGenerator()->IsXcode()) {
          file << "#endif // CMAKE_SKIP_PRECOMPILE_HEADERS\n";
        }
        if (pchEpilogue) {
          file << *pchEpilogue << "\n";
        }
      }

      if (!firstHeaderOnDisk.empty()) {
        cmFileTimes::Copy(firstHeaderOnDisk, filename_tmp);
      }

      cmSystemTools::MoveFileIfDifferent(filename_tmp, filename);
    }
  }
  return inserted.first->second;
}

std::string cmGeneratorTarget::GetPchSource(std::string const& config,
                                            std::string const& language,
                                            std::string const& arch) const
{
  if (language != "C" && language != "CXX" && language != "OBJC" &&
      language != "OBJCXX") {
    return std::string();
  }
  auto const inserted =
    this->PchSources.insert(std::make_pair(language + config + arch, ""));
  if (inserted.second) {
    std::string const pchHeader = this->GetPchHeader(config, language, arch);
    if (pchHeader.empty()) {
      return std::string();
    }
    std::string& filename = inserted.first->second;

    cmGeneratorTarget const* generatorTarget = this;
    cmGeneratorTarget const* reuseTarget = this->GetPchReuseTarget();
    bool const haveReuseTarget = reuseTarget && reuseTarget != this;
    if (reuseTarget) {
      generatorTarget = reuseTarget;
    }

    filename =
      cmStrCat(generatorTarget->GetCMFSupportDirectory(), "/cmake_pch");

    // For GCC the source extension will be transformed into .h[xx].gch
    if (!this->Makefile->IsOn("CMAKE_LINK_PCH")) {
      std::map<std::string, std::string> const languageToExtension = {
        { "C", ".h.c" },
        { "CXX", ".hxx.cxx" },
        { "OBJC", ".objc.h.m" },
        { "OBJCXX", ".objcxx.hxx.mm" }
      };

      filename = cmStrCat(filename, arch.empty() ? "" : cmStrCat('_', arch),
                          languageToExtension.at(language));
    } else {
      std::map<std::string, std::string> const languageToExtension = {
        { "C", ".c" }, { "CXX", ".cxx" }, { "OBJC", ".m" }, { "OBJCXX", ".mm" }
      };

      filename = cmStrCat(filename, arch.empty() ? "" : cmStrCat('_', arch),
                          languageToExtension.at(language));
    }

    std::string const filename_tmp = cmStrCat(filename, ".tmp");
    if (!haveReuseTarget) {
      {
        cmGeneratedFileStream file(filename_tmp);
        file << "/* generated by CMake */\n";
      }
      cmFileTimes::Copy(pchHeader, filename_tmp);
      cmSystemTools::MoveFileIfDifferent(filename_tmp, filename);
    }
  }
  return inserted.first->second;
}

std::string cmGeneratorTarget::GetPchFileObject(std::string const& config,
                                                std::string const& language,
                                                std::string const& arch)
{
  if (language != "C" && language != "CXX" && language != "OBJC" &&
      language != "OBJCXX") {
    return std::string();
  }
  auto const inserted =
    this->PchObjectFiles.insert(std::make_pair(language + config + arch, ""));
  if (inserted.second) {
    std::string const pchSource = this->GetPchSource(config, language, arch);
    if (pchSource.empty()) {
      return std::string();
    }
    std::string& filename = inserted.first->second;

    auto* pchSf = this->Makefile->GetOrCreateSource(
      pchSource, false, cmSourceFileLocationKind::Known);
    pchSf->ResolveFullPath();
    filename = cmStrCat(this->GetObjectDirectory(config), '/',
                        this->GetObjectName(pchSf));
  }
  return inserted.first->second;
}

std::string cmGeneratorTarget::GetPchFile(std::string const& config,
                                          std::string const& language,
                                          std::string const& arch)
{
  auto const inserted =
    this->PchFiles.insert(std::make_pair(language + config + arch, ""));
  if (inserted.second) {
    std::string& pchFile = inserted.first->second;

    std::string const pchExtension =
      this->Makefile->GetSafeDefinition("CMAKE_PCH_EXTENSION");

    if (this->Makefile->IsOn("CMAKE_LINK_PCH")) {
      auto replaceExtension = [](std::string const& str,
                                 std::string const& ext) -> std::string {
        auto dot_pos = str.rfind('.');
        std::string result;
        if (dot_pos != std::string::npos) {
          result = str.substr(0, dot_pos);
        }
        result += ext;
        return result;
      };

      cmGeneratorTarget* generatorTarget = this;
      cmGeneratorTarget* reuseTarget = this->GetPchReuseTarget();
      if (reuseTarget) {
        generatorTarget = reuseTarget;
      }

      std::string const pchFileObject =
        generatorTarget->GetPchFileObject(config, language, arch);
      if (!pchExtension.empty()) {
        pchFile = replaceExtension(pchFileObject, pchExtension);
      }
    } else {
      if (this->GetUseShortObjectNames()) {
        auto pchSource = this->GetPchSource(config, language, arch);
        auto* pchSf = this->Makefile->GetOrCreateSource(
          pchSource, false, cmSourceFileLocationKind::Known);
        pchSf->ResolveFullPath();
        pchFile = cmStrCat(
          this->GetSupportDirectory(), '/',
          this->LocalGenerator->GetShortObjectFileName(*pchSf), pchExtension);
      } else {
        pchFile =
          cmStrCat(this->GetPchHeader(config, language, arch), pchExtension);
      }
    }
  }
  return inserted.first->second;
}

std::string cmGeneratorTarget::GetPchCreateCompileOptions(
  std::string const& config, std::string const& language,
  std::string const& arch)
{
  auto const inserted = this->PchCreateCompileOptions.insert(
    std::make_pair(language + config + arch, ""));
  if (inserted.second) {
    std::string& createOptionList = inserted.first->second;

    if (this->GetPropertyAsBool("PCH_WARN_INVALID")) {
      createOptionList = this->Makefile->GetSafeDefinition(
        cmStrCat("CMAKE_", language, "_COMPILE_OPTIONS_INVALID_PCH"));
    }

    if (this->GetPropertyAsBool("PCH_INSTANTIATE_TEMPLATES")) {
      std::string varName = cmStrCat(
        "CMAKE_", language, "_COMPILE_OPTIONS_INSTANTIATE_TEMPLATES_PCH");
      std::string instantiateOption =
        this->Makefile->GetSafeDefinition(varName);
      if (!instantiateOption.empty()) {
        createOptionList = cmStrCat(createOptionList, ';', instantiateOption);
      }
    }

    std::string const createOptVar =
      cmStrCat("CMAKE_", language, "_COMPILE_OPTIONS_CREATE_PCH");

    createOptionList = cmStrCat(
      createOptionList, ';', this->Makefile->GetSafeDefinition(createOptVar));

    std::string const pchHeader = this->GetPchHeader(config, language, arch);
    std::string const pchFile = this->GetPchFile(config, language, arch);

    cmSystemTools::ReplaceString(createOptionList, "<PCH_HEADER>", pchHeader);
    cmSystemTools::ReplaceString(createOptionList, "<PCH_FILE>", pchFile);
  }
  return inserted.first->second;
}

std::string cmGeneratorTarget::GetPchUseCompileOptions(
  std::string const& config, std::string const& language,
  std::string const& arch)
{
  auto const inserted = this->PchUseCompileOptions.insert(
    std::make_pair(language + config + arch, ""));
  if (inserted.second) {
    std::string& useOptionList = inserted.first->second;

    if (this->GetPropertyAsBool("PCH_WARN_INVALID")) {
      useOptionList = this->Makefile->GetSafeDefinition(
        cmStrCat("CMAKE_", language, "_COMPILE_OPTIONS_INVALID_PCH"));
    }

    std::string const useOptVar =
      cmStrCat(language, "_COMPILE_OPTIONS_USE_PCH");

    std::string const& useOptionListProperty =
      this->GetSafeProperty(useOptVar);

    useOptionList = cmStrCat(
      useOptionList, ';',
      useOptionListProperty.empty()
        ? this->Makefile->GetSafeDefinition(cmStrCat("CMAKE_", useOptVar))
        : useOptionListProperty);

    std::string const pchHeader = this->GetPchHeader(config, language, arch);
    std::string const pchFile = this->GetPchFile(config, language, arch);

    cmSystemTools::ReplaceString(useOptionList, "<PCH_HEADER>", pchHeader);
    cmSystemTools::ReplaceString(useOptionList, "<PCH_FILE>", pchFile);
  }
  return inserted.first->second;
}

void cmGeneratorTarget::AddSourceFileToUnityBatch(
  std::string const& sourceFilename)
{
  this->UnityBatchedSourceFiles.insert(sourceFilename);
}

bool cmGeneratorTarget::IsSourceFilePartOfUnityBatch(
  std::string const& sourceFilename) const
{
  if (!this->GetPropertyAsBool("UNITY_BUILD")) {
    return false;
  }

  return this->UnityBatchedSourceFiles.find(sourceFilename) !=
    this->UnityBatchedSourceFiles.end();
}

void cmGeneratorTarget::ComputeTargetManifest(std::string const& config) const
{
  if (this->IsImported()) {
    return;
  }
  cmGlobalGenerator* gg = this->LocalGenerator->GetGlobalGenerator();

  // Get the names.
  cmGeneratorTarget::Names targetNames;
  if (this->GetType() == cmStateEnums::EXECUTABLE) {
    targetNames = this->GetExecutableNames(config);
  } else if (this->GetType() == cmStateEnums::STATIC_LIBRARY ||
             this->GetType() == cmStateEnums::SHARED_LIBRARY ||
             this->GetType() == cmStateEnums::MODULE_LIBRARY) {
    targetNames = this->GetLibraryNames(config);
  } else {
    return;
  }

  // Get the directory.
  std::string dir =
    this->GetDirectory(config, cmStateEnums::RuntimeBinaryArtifact);

  // Add each name.
  std::string f;
  if (!targetNames.Output.empty()) {
    f = cmStrCat(dir, '/', targetNames.Output);
    gg->AddToManifest(f);
  }
  if (!targetNames.SharedObject.empty()) {
    f = cmStrCat(dir, '/', targetNames.SharedObject);
    gg->AddToManifest(f);
  }
  if (!targetNames.Real.empty()) {
    f = cmStrCat(dir, '/', targetNames.Real);
    gg->AddToManifest(f);
  }
  if (!targetNames.PDB.empty()) {
    f = cmStrCat(dir, '/', targetNames.PDB);
    gg->AddToManifest(f);
  }

  dir = this->GetDirectory(config, cmStateEnums::ImportLibraryArtifact);
  if (!targetNames.ImportOutput.empty()) {
    f = cmStrCat(dir, '/', targetNames.ImportOutput);
    gg->AddToManifest(f);
  }
  if (!targetNames.ImportLibrary.empty()) {
    f = cmStrCat(dir, '/', targetNames.ImportLibrary);
    gg->AddToManifest(f);
  }
  if (!targetNames.ImportReal.empty()) {
    f = cmStrCat(dir, '/', targetNames.ImportReal);
    gg->AddToManifest(f);
  }
}

cm::optional<cmStandardLevel> cmGeneratorTarget::GetExplicitStandardLevel(
  std::string const& lang, std::string const& config) const
{
  cm::optional<cmStandardLevel> level;
  std::string key = cmStrCat(cmSystemTools::UpperCase(config), '-', lang);
  auto i = this->ExplicitStandardLevel.find(key);
  if (i != this->ExplicitStandardLevel.end()) {
    level = i->second;
  }
  return level;
}

void cmGeneratorTarget::UpdateExplicitStandardLevel(std::string const& lang,
                                                    std::string const& config,
                                                    cmStandardLevel level)
{
  auto e = this->ExplicitStandardLevel.emplace(
    cmStrCat(cmSystemTools::UpperCase(config), '-', lang), level);
  if (!e.second && e.first->second < level) {
    e.first->second = level;
  }
}

bool cmGeneratorTarget::ComputeCompileFeatures(std::string const& config)
{
  cmStandardLevelResolver standardResolver(this->Makefile);

  for (std::string const& lang :
       this->Makefile->GetState()->GetEnabledLanguages()) {
    if (cmValue languageStd = this->GetLanguageStandard(lang, config)) {
      if (cm::optional<cmStandardLevel> langLevel =
            standardResolver.LanguageStandardLevel(lang, *languageStd)) {
        this->UpdateExplicitStandardLevel(lang, config, *langLevel);
      }
    }
  }

  // Compute the language standard based on the compile features.
  std::vector<BT<std::string>> features = this->GetCompileFeatures(config);
  for (BT<std::string> const& f : features) {
    std::string lang;
    if (!standardResolver.CompileFeatureKnown(this->Target->GetName(), f.Value,
                                              lang, nullptr)) {
      return false;
    }

    std::string key = cmStrCat(cmSystemTools::UpperCase(config), '-', lang);
    cmValue currentLanguageStandard = this->GetLanguageStandard(lang, config);

    cm::optional<cmStandardLevel> featureLevel;
    std::string newRequiredStandard;
    if (!standardResolver.GetNewRequiredStandard(
          this->Target->GetName(), f.Value, currentLanguageStandard,
          featureLevel, newRequiredStandard)) {
      return false;
    }

    if (featureLevel) {
      this->UpdateExplicitStandardLevel(lang, config, *featureLevel);
    }

    if (!newRequiredStandard.empty()) {
      BTs<std::string>& languageStandardProperty =
        this->LanguageStandardMap[key];
      if (languageStandardProperty.Value != newRequiredStandard) {
        languageStandardProperty.Value = newRequiredStandard;
        languageStandardProperty.Backtraces.clear();
      }
      languageStandardProperty.Backtraces.emplace_back(f.Backtrace);
    }
  }

  return true;
}

bool cmGeneratorTarget::ComputeCompileFeatures(
  std::string const& config, std::set<LanguagePair> const& languagePairs)
{
  for (auto const& language : languagePairs) {
    BTs<std::string> const* generatorTargetLanguageStandard =
      this->GetLanguageStandardProperty(language.first, config);
    if (!generatorTargetLanguageStandard) {
      // If the standard isn't explicitly set we copy it over from the
      // specified paired language.
      std::string key =
        cmStrCat(cmSystemTools::UpperCase(config), '-', language.first);
      BTs<std::string> const* standardToCopy =
        this->GetLanguageStandardProperty(language.second, config);
      if (standardToCopy) {
        this->LanguageStandardMap[key] = *standardToCopy;
        generatorTargetLanguageStandard = &this->LanguageStandardMap[key];
      } else {
        cmValue defaultStandard = this->Makefile->GetDefinition(
          cmStrCat("CMAKE_", language.second, "_STANDARD_DEFAULT"));
        if (defaultStandard) {
          this->LanguageStandardMap[key] = BTs<std::string>(*defaultStandard);
          generatorTargetLanguageStandard = &this->LanguageStandardMap[key];
        }
      }

      // Custom updates for the CUDA standard.
      if (generatorTargetLanguageStandard && (language.first == "CUDA")) {
        if (generatorTargetLanguageStandard->Value == "98") {
          this->LanguageStandardMap[key].Value = "03";
        }
      }
    }
  }

  return true;
}

std::string cmGeneratorTarget::GetImportedLibName(
  std::string const& config) const
{
  if (cmGeneratorTarget::ImportInfo const* info =
        this->GetImportInfo(config)) {
    return info->LibName;
  }
  return std::string();
}

std::string cmGeneratorTarget::GetFullPath(std::string const& config,
                                           cmStateEnums::ArtifactType artifact,
                                           bool realname) const
{
  if (this->IsImported()) {
    return this->Target->ImportedGetFullPath(config, artifact);
  }
  return this->NormalGetFullPath(config, artifact, realname);
}

std::string cmGeneratorTarget::NormalGetFullPath(
  std::string const& config, cmStateEnums::ArtifactType artifact,
  bool realname) const
{
  std::string fpath = cmStrCat(this->GetDirectory(config, artifact), '/');
  if (this->IsAppBundleOnApple()) {
    fpath =
      cmStrCat(this->BuildBundleDirectory(fpath, config, FullLevel), '/');
  }

  // Add the full name of the target.
  switch (artifact) {
    case cmStateEnums::RuntimeBinaryArtifact:
      if (realname) {
        fpath += this->NormalGetRealName(config);
      } else {
        fpath +=
          this->GetFullName(config, cmStateEnums::RuntimeBinaryArtifact);
      }
      break;
    case cmStateEnums::ImportLibraryArtifact:
      if (realname) {
        fpath +=
          this->NormalGetRealName(config, cmStateEnums::ImportLibraryArtifact);
      } else {
        fpath +=
          this->GetFullName(config, cmStateEnums::ImportLibraryArtifact);
      }
      break;
  }
  return fpath;
}

std::string cmGeneratorTarget::NormalGetRealName(
  std::string const& config, cmStateEnums::ArtifactType artifact) const
{
  // This should not be called for imported targets.
  // TODO: Split cmTarget into a class hierarchy to get compile-time
  // enforcement of the limited imported target API.
  if (this->IsImported()) {
    std::string msg = cmStrCat("NormalGetRealName called on imported target: ",
                               this->GetName());
    this->LocalGenerator->IssueMessage(MessageType::INTERNAL_ERROR, msg);
  }

  Names names = this->GetType() == cmStateEnums::EXECUTABLE
    ? this->GetExecutableNames(config)
    : this->GetLibraryNames(config);

  // Compute the real name that will be built.
  return artifact == cmStateEnums::RuntimeBinaryArtifact ? names.Real
                                                         : names.ImportReal;
}

cmGeneratorTarget::Names cmGeneratorTarget::GetLibraryNames(
  std::string const& config) const
{
  cmGeneratorTarget::Names targetNames;

  // This should not be called for imported targets.
  // TODO: Split cmTarget into a class hierarchy to get compile-time
  // enforcement of the limited imported target API.
  if (this->IsImported()) {
    std::string msg =
      cmStrCat("GetLibraryNames called on imported target: ", this->GetName());
    this->LocalGenerator->IssueMessage(MessageType::INTERNAL_ERROR, msg);
  }

  // Check for library version properties.
  cmValue version = this->GetProperty("VERSION");
  cmValue soversion = this->GetProperty("SOVERSION");
  if (!this->HasSOName(config) ||
      this->Makefile->IsOn("CMAKE_PLATFORM_NO_VERSIONED_SONAME") ||
      this->IsFrameworkOnApple()) {
    // Versioning is supported only for shared libraries and modules,
    // and then only when the platform supports an soname flag.
    version = nullptr;
    soversion = nullptr;
  }
  if (version && !soversion) {
    // The soversion must be set if the library version is set.  Use
    // the library version as the soversion.
    soversion = version;
  }
  if (!version && soversion) {
    // Use the soversion as the library version.
    version = soversion;
  }

  // Get the components of the library name.
  NameComponents const& components = this->GetFullNameInternalComponents(
    config, cmStateEnums::RuntimeBinaryArtifact);

  // The library name.
  targetNames.Base = components.base;
  targetNames.Output =
    cmStrCat(components.prefix, targetNames.Base, components.suffix);

  if (this->IsFrameworkOnApple()) {
    targetNames.Real = components.prefix;
    if (!this->Makefile->PlatformIsAppleEmbedded()) {
      targetNames.Real +=
        cmStrCat("Versions/", this->GetFrameworkVersion(), '/');
    }
    targetNames.Real += cmStrCat(targetNames.Base, components.suffix);
    targetNames.SharedObject = targetNames.Real;
  } else if (this->IsArchivedAIXSharedLibrary()) {
    targetNames.SharedObject =
      cmStrCat(components.prefix, targetNames.Base, ".so");
    if (soversion) {
      targetNames.SharedObject += ".";
      targetNames.SharedObject += *soversion;
    }
    targetNames.Real = targetNames.Output;
  } else {
    // The library's soname.
    targetNames.SharedObject = this->ComputeVersionedName(
      components.prefix, targetNames.Base, components.suffix,
      targetNames.Output, soversion);

    // The library's real name on disk.
    targetNames.Real = this->ComputeVersionedName(
      components.prefix, targetNames.Base, components.suffix,
      targetNames.Output, version);
  }

  // The import library names.
  if (this->GetType() == cmStateEnums::SHARED_LIBRARY ||
      this->GetType() == cmStateEnums::MODULE_LIBRARY) {
    NameComponents const& importComponents =
      this->GetFullNameInternalComponents(config,
                                          cmStateEnums::ImportLibraryArtifact);
    targetNames.ImportOutput = cmStrCat(
      importComponents.prefix, importComponents.base, importComponents.suffix);

    if (this->IsFrameworkOnApple() && this->IsSharedLibraryWithExports()) {
      targetNames.ImportReal = components.prefix;
      if (!this->Makefile->PlatformIsAppleEmbedded()) {
        targetNames.ImportReal +=
          cmStrCat("Versions/", this->GetFrameworkVersion(), '/');
      }
      targetNames.ImportReal +=
        cmStrCat(importComponents.base, importComponents.suffix);
      targetNames.ImportLibrary = targetNames.ImportOutput;
    } else {
      // The import library's soname.
      targetNames.ImportLibrary = this->ComputeVersionedName(
        importComponents.prefix, importComponents.base,
        importComponents.suffix, targetNames.ImportOutput, soversion);

      // The import library's real name on disk.
      targetNames.ImportReal = this->ComputeVersionedName(
        importComponents.prefix, importComponents.base,
        importComponents.suffix, targetNames.ImportOutput, version);
    }
  }

  // The program database file name.
  targetNames.PDB = this->GetPDBName(config);

  return targetNames;
}

cmGeneratorTarget::Names cmGeneratorTarget::GetExecutableNames(
  std::string const& config) const
{
  cmGeneratorTarget::Names targetNames;

  // This should not be called for imported targets.
  // TODO: Split cmTarget into a class hierarchy to get compile-time
  // enforcement of the limited imported target API.
  if (this->IsImported()) {
    std::string msg = cmStrCat(
      "GetExecutableNames called on imported target: ", this->GetName());
    this->LocalGenerator->IssueMessage(MessageType::INTERNAL_ERROR, msg);
  }

// This versioning is supported only for executables and then only
// when the platform supports symbolic links.
#if defined(_WIN32) && !defined(__CYGWIN__)
  cmValue version;
#else
  // Check for executable version properties.
  cmValue version = this->GetProperty("VERSION");
  if (this->GetType() != cmStateEnums::EXECUTABLE ||
      this->Makefile->IsOn("XCODE")) {
    version = nullptr;
  }
#endif

  // Get the components of the executable name.
  NameComponents const& components = this->GetFullNameInternalComponents(
    config, cmStateEnums::RuntimeBinaryArtifact);

  // The executable name.
  targetNames.Base = components.base;

  if (this->IsArchivedAIXSharedLibrary()) {
    targetNames.Output = components.prefix + targetNames.Base;
  } else {
    targetNames.Output =
      components.prefix + targetNames.Base + components.suffix;
  }

// The executable's real name on disk.
#if defined(__CYGWIN__)
  targetNames.Real = components.prefix + targetNames.Base;
#else
  targetNames.Real = targetNames.Output;
#endif
  if (version) {
    targetNames.Real += "-";
    targetNames.Real += *version;
  }
#if defined(__CYGWIN__)
  targetNames.Real += components.suffix;
#endif

  // The import library name.
  targetNames.ImportLibrary =
    this->GetFullNameInternal(config, cmStateEnums::ImportLibraryArtifact);
  targetNames.ImportReal = targetNames.ImportLibrary;
  targetNames.ImportOutput = targetNames.ImportLibrary;

  // The program database file name.
  targetNames.PDB = this->GetPDBName(config);

  return targetNames;
}

std::string cmGeneratorTarget::GetFullNameInternal(
  std::string const& config, cmStateEnums::ArtifactType artifact) const
{
  NameComponents const& components =
    this->GetFullNameInternalComponents(config, artifact);
  return components.prefix + components.base + components.suffix;
}

std::string cmGeneratorTarget::ImportedGetLocation(
  std::string const& config) const
{
  assert(this->IsImported());
  return this->Target->ImportedGetFullPath(
    config, cmStateEnums::RuntimeBinaryArtifact);
}

std::string cmGeneratorTarget::GetFullNameImported(
  std::string const& config, cmStateEnums::ArtifactType artifact) const
{
  return cmSystemTools::GetFilenameName(
    this->Target->ImportedGetFullPath(config, artifact));
}

cmGeneratorTarget::NameComponents const&
cmGeneratorTarget::GetFullNameInternalComponents(
  std::string const& config, cmStateEnums::ArtifactType artifact) const
{
  assert(artifact == cmStateEnums::RuntimeBinaryArtifact ||
         artifact == cmStateEnums::ImportLibraryArtifact);
  FullNameCache& cache = artifact == cmStateEnums::RuntimeBinaryArtifact
    ? RuntimeBinaryFullNameCache
    : ImportLibraryFullNameCache;
  auto search = cache.find(config);
  if (search != cache.end()) {
    return search->second;
  }
  // Use just the target name for non-main target types.
  if (this->GetType() != cmStateEnums::STATIC_LIBRARY &&
      this->GetType() != cmStateEnums::SHARED_LIBRARY &&
      this->GetType() != cmStateEnums::MODULE_LIBRARY &&
      this->GetType() != cmStateEnums::EXECUTABLE) {
    NameComponents components;
    components.base = this->GetName();
    return cache.emplace(config, std::move(components)).first->second;
  }

  bool const isImportedLibraryArtifact =
    (artifact == cmStateEnums::ImportLibraryArtifact);

  // Return an empty name for the import library if this platform
  // does not support import libraries.
  if (isImportedLibraryArtifact && !this->NeedImportLibraryName(config)) {
    return cache.emplace(config, NameComponents()).first->second;
  }

  NameComponents parts;
  std::string& outPrefix = parts.prefix;
  std::string& outBase = parts.base;
  std::string& outSuffix = parts.suffix;

  // retrieve prefix and suffix
  std::string ll = this->GetLinkerLanguage(config);
  cmValue targetPrefix = this->GetFilePrefixInternal(config, artifact, ll);
  cmValue targetSuffix = this->GetFileSuffixInternal(config, artifact, ll);

  // The implib option is only allowed for shared libraries, module
  // libraries, and executables.
  if (this->GetType() != cmStateEnums::SHARED_LIBRARY &&
      this->GetType() != cmStateEnums::MODULE_LIBRARY &&
      this->GetType() != cmStateEnums::EXECUTABLE) {
    artifact = cmStateEnums::RuntimeBinaryArtifact;
  }

  // Compute the full name for main target types.
  std::string configPostfix = this->GetFilePostfix(config);

  // frameworks have directory prefix
  std::string fw_prefix;
  if (this->IsFrameworkOnApple()) {
    fw_prefix =
      cmStrCat(this->GetFrameworkDirectory(config, ContentLevel), '/');
    targetPrefix = cmValue(fw_prefix);
    if (!isImportedLibraryArtifact) {
      // no suffix
      targetSuffix = nullptr;
    }
  }

  if (this->IsCFBundleOnApple()) {
    fw_prefix = cmStrCat(this->GetCFBundleDirectory(config, FullLevel), '/');
    targetPrefix = cmValue(fw_prefix);
    targetSuffix = nullptr;
  }

  // Begin the final name with the prefix.
  outPrefix = targetPrefix ? *targetPrefix : "";

  // Append the target name or property-specified name.
  outBase += this->GetOutputName(config, artifact);

  // Append the per-configuration postfix.
  // When using Xcode, the postfix should be part of the suffix rather than
  // the base, because the suffix ends up being used in Xcode's
  // EXECUTABLE_SUFFIX attribute.
  if (this->IsFrameworkOnApple() && this->GetGlobalGenerator()->IsXcode()) {
    configPostfix += *targetSuffix;
    targetSuffix = cmValue(configPostfix);
  } else {
    outBase += configPostfix;
  }

  // Name shared libraries with their version number on some platforms.
  if (cmValue soversion = this->GetProperty("SOVERSION")) {
    cmValue dllProp;
    if (this->IsDLLPlatform()) {
      dllProp = this->GetProperty("DLL_NAME_WITH_SOVERSION");
    }
    if (this->GetType() == cmStateEnums::SHARED_LIBRARY &&
        !isImportedLibraryArtifact &&
        (dllProp.IsOn() ||
         (!dllProp.IsSet() &&
          this->Makefile->IsOn("CMAKE_SHARED_LIBRARY_NAME_WITH_VERSION")))) {
      outBase += "-";
      outBase += *soversion;
    }
  }

  // Append the suffix.
  outSuffix = targetSuffix ? *targetSuffix : "";

  return cache.emplace(config, std::move(parts)).first->second;
}

std::string cmGeneratorTarget::GetLinkerLanguage(
  std::string const& config) const
{
  return this->GetLinkClosure(config)->LinkerLanguage;
}

std::string cmGeneratorTarget::GetLinkerTool(std::string const& config) const
{
  return this->GetLinkerTool(this->GetLinkerLanguage(config), config);
}

std::string cmGeneratorTarget::GetLinkerTool(std::string const& lang,
                                             std::string const& config) const
{
  auto linkMode = cmStrCat(
    "CMAKE_", lang, this->IsDeviceLink() ? "_DEVICE_" : "_", "LINK_MODE");
  auto mode = this->Makefile->GetDefinition(linkMode);
  if (!mode || mode != "LINKER"_s) {
    return this->Makefile->GetDefinition("CMAKE_LINKER");
  }

  auto linkerType = this->GetLinkerTypeProperty(lang, config);
  if (linkerType.empty()) {
    linkerType = "DEFAULT";
  }
  auto usingLinker =
    cmStrCat("CMAKE_", lang, "_USING_", this->IsDeviceLink() ? "DEVICE_" : "",
             "LINKER_", linkerType);
  auto linkerTool = this->Makefile->GetDefinition(usingLinker);

  if (!linkerTool) {
    if (this->GetGlobalGenerator()->IsVisualStudio() &&
        linkerType == "DEFAULT"_s) {
      return std::string{};
    }

    // fall-back to generic definition
    linkerTool = this->Makefile->GetDefinition("CMAKE_LINKER");

    if (linkerType != "DEFAULT"_s) {
      auto isCMakeLinkerType = [](std::string const& type) -> bool {
        return std::all_of(type.cbegin(), type.cend(),
                           [](char c) { return std::isupper(c); });
      };
      if (isCMakeLinkerType(linkerType)) {
        this->LocalGenerator->IssueMessage(
          MessageType::FATAL_ERROR,
          cmStrCat("LINKER_TYPE '", linkerType,
                   "' is unknown or not supported by this toolchain."));
      } else {
        this->LocalGenerator->IssueMessage(
          MessageType::FATAL_ERROR,
          cmStrCat("LINKER_TYPE '", linkerType,
                   "' is unknown. Did you forget to define the '", usingLinker,
                   "' variable?"));
      }
    }
  }

  return linkerTool;
}

bool cmGeneratorTarget::LinkerEnforcesNoAllowShLibUndefined(
  std::string const& config) const
{
  // FIXME(#25486): Account for the LINKER_TYPE target property.
  // Also factor out the hard-coded list below into a platform
  // information table based on the linker id.
  std::string ll = this->GetLinkerLanguage(config);
  std::string linkerIdVar = cmStrCat("CMAKE_", ll, "_COMPILER_LINKER_ID");
  cmValue linkerId = this->Makefile->GetDefinition(linkerIdVar);
  // The GNU bfd-based linker may enforce '--no-allow-shlib-undefined'
  // recursively by default.  The Solaris linker has similar behavior.
  return linkerId && (*linkerId == "GNU" || *linkerId == "Solaris");
}

std::string cmGeneratorTarget::GetPDBOutputName(
  std::string const& config) const
{
  // Lookup/compute/cache the pdb output name for this configuration.
  auto i = this->PdbOutputNameMap.find(config);
  if (i == this->PdbOutputNameMap.end()) {
    // Add empty name in map to detect potential recursion.
    PdbOutputNameMapType::value_type entry(config, "");
    i = this->PdbOutputNameMap.insert(entry).first;

    // Compute output name.
    std::vector<std::string> props;
    std::string configUpper = cmSystemTools::UpperCase(config);
    if (!configUpper.empty()) {
      // PDB_NAME_<CONFIG>
      props.push_back("PDB_NAME_" + configUpper);
    }

    // PDB_NAME
    props.emplace_back("PDB_NAME");

    std::string outName;
    for (std::string const& p : props) {
      if (cmValue outNameProp = this->GetProperty(p)) {
        outName = *outNameProp;
        break;
      }
    }

    // Now evaluate genex and update the previously-prepared map entry.
    if (outName.empty()) {
      i->second =
        this->GetOutputName(config, cmStateEnums::RuntimeBinaryArtifact) +
        this->GetFilePostfix(config);
    } else {
      i->second =
        cmGeneratorExpression::Evaluate(outName, this->LocalGenerator, config);
    }
  } else if (i->second.empty()) {
    // An empty map entry indicates we have been called recursively
    // from the above block.
    this->LocalGenerator->GetCMakeInstance()->IssueMessage(
      MessageType::FATAL_ERROR,
      "Target '" + this->GetName() + "' PDB_NAME depends on itself.",
      this->GetBacktrace());
  }
  return i->second;
}

std::string cmGeneratorTarget::GetPDBName(std::string const& config) const
{
  NameComponents const& parts = this->GetFullNameInternalComponents(
    config, cmStateEnums::RuntimeBinaryArtifact);

  std::string base = this->GetPDBOutputName(config);

  return parts.prefix + base + ".pdb";
}

std::string cmGeneratorTarget::GetObjectDirectory(
  std::string const& config) const
{
  std::string obj_dir =
    this->GlobalGenerator->ExpandCFGIntDir(this->ObjectDirectory, config);
#if defined(__APPLE__)
  // Replace Xcode's placeholder for the object file directory since
  // installation and export scripts need to know the real directory.
  // Xcode has build-time settings (e.g. for sanitizers) that affect this,
  // but we use the default here.  Users that want to enable sanitizers
  // will do so at the cost of object library installation and export.
  cmSystemTools::ReplaceString(obj_dir, "$(OBJECT_FILE_DIR_normal:base)",
                               "Objects-normal");
#endif
  return obj_dir;
}

void cmGeneratorTarget::GetTargetObjectNames(
  std::string const& config, std::vector<std::string>& objects) const
{
  std::vector<cmSourceFile const*> objectSources;
  this->GetObjectSources(objectSources, config);
  std::map<cmSourceFile const*, std::string> mapping;

  for (cmSourceFile const* sf : objectSources) {
    mapping[sf];
  }

  this->LocalGenerator->ComputeObjectFilenames(mapping, this);

  for (cmSourceFile const* src : objectSources) {
    // Find the object file name corresponding to this source file.
    auto map_it = mapping.find(src);
    // It must exist because we populated the mapping just above.
    assert(!map_it->second.empty());
    objects.push_back(map_it->second);
  }

  // We need to compute the relative path from the root of
  // of the object directory to handle subdirectory paths
  std::string rootObjectDir = this->GetObjectDirectory(config);
  rootObjectDir = cmSystemTools::CollapseFullPath(rootObjectDir);
  auto ispcObjects = this->GetGeneratedISPCObjects(config);
  for (std::string const& output : ispcObjects) {
    auto relativePathFromObjectDir = output.substr(rootObjectDir.size());
    objects.push_back(relativePathFromObjectDir);
  }
}

bool cmGeneratorTarget::StrictTargetComparison::operator()(
  cmGeneratorTarget const* t1, cmGeneratorTarget const* t2) const
{
  int nameResult = strcmp(t1->GetName().c_str(), t2->GetName().c_str());
  if (nameResult == 0) {
    return strcmp(
             t1->GetLocalGenerator()->GetCurrentBinaryDirectory().c_str(),
             t2->GetLocalGenerator()->GetCurrentBinaryDirectory().c_str()) < 0;
  }
  return nameResult < 0;
}

struct cmGeneratorTarget::SourceFileFlags
cmGeneratorTarget::GetTargetSourceFileFlags(cmSourceFile const* sf) const
{
  struct SourceFileFlags flags;
  this->ConstructSourceFileFlags();
  auto si = this->SourceFlagsMap.find(sf);
  if (si != this->SourceFlagsMap.end()) {
    flags = si->second;
  } else {
    // Handle the MACOSX_PACKAGE_LOCATION property on source files that
    // were not listed in one of the other lists.
    if (cmValue location = sf->GetProperty("MACOSX_PACKAGE_LOCATION")) {
      flags.MacFolder = location->c_str();
      bool const stripResources =
        this->GlobalGenerator->ShouldStripResourcePath(this->Makefile);
      if (*location == "Resources") {
        flags.Type = cmGeneratorTarget::SourceFileTypeResource;
        if (stripResources) {
          flags.MacFolder = "";
        }
      } else if (cmHasLiteralPrefix(*location, "Resources/")) {
        flags.Type = cmGeneratorTarget::SourceFileTypeDeepResource;
        if (stripResources) {
          flags.MacFolder += cmStrLen("Resources/");
        }
      } else {
        flags.Type = cmGeneratorTarget::SourceFileTypeMacContent;
      }
    }
  }
  return flags;
}

void cmGeneratorTarget::ConstructSourceFileFlags() const
{
  if (this->SourceFileFlagsConstructed) {
    return;
  }
  this->SourceFileFlagsConstructed = true;

  // Process public headers to mark the source files.
  if (cmValue files = this->GetProperty("PUBLIC_HEADER")) {
    cmList relFiles{ *files };
    for (auto const& relFile : relFiles) {
      if (cmSourceFile* sf = this->Makefile->GetSource(relFile)) {
        SourceFileFlags& flags = this->SourceFlagsMap[sf];
        flags.MacFolder = "Headers";
        flags.Type = cmGeneratorTarget::SourceFileTypePublicHeader;
      }
    }
  }

  // Process private headers after public headers so that they take
  // precedence if a file is listed in both.
  if (cmValue files = this->GetProperty("PRIVATE_HEADER")) {
    cmList relFiles{ *files };
    for (auto const& relFile : relFiles) {
      if (cmSourceFile* sf = this->Makefile->GetSource(relFile)) {
        SourceFileFlags& flags = this->SourceFlagsMap[sf];
        flags.MacFolder = "PrivateHeaders";
        flags.Type = cmGeneratorTarget::SourceFileTypePrivateHeader;
      }
    }
  }

  // Mark sources listed as resources.
  if (cmValue files = this->GetProperty("RESOURCE")) {
    cmList relFiles{ *files };
    for (auto const& relFile : relFiles) {
      if (cmSourceFile* sf = this->Makefile->GetSource(relFile)) {
        SourceFileFlags& flags = this->SourceFlagsMap[sf];
        flags.MacFolder = "";
        if (!this->GlobalGenerator->ShouldStripResourcePath(this->Makefile)) {
          flags.MacFolder = "Resources";
        }
        flags.Type = cmGeneratorTarget::SourceFileTypeResource;
      }
    }
  }
}

bool cmGeneratorTarget::SetDeviceLink(bool deviceLink)
{
  bool previous = this->DeviceLink;
  this->DeviceLink = deviceLink;
  return previous;
}

void cmGeneratorTarget::GetTargetVersion(int& major, int& minor) const
{
  int patch;
  this->GetTargetVersion("VERSION", major, minor, patch);
}

void cmGeneratorTarget::GetTargetVersionFallback(
  std::string const& property, std::string const& fallback_property,
  int& major, int& minor, int& patch) const
{
  if (this->GetProperty(property)) {
    this->GetTargetVersion(property, major, minor, patch);
  } else {
    this->GetTargetVersion(fallback_property, major, minor, patch);
  }
}

void cmGeneratorTarget::GetTargetVersion(std::string const& property,
                                         int& major, int& minor,
                                         int& patch) const
{
  // Set the default values.
  major = 0;
  minor = 0;
  patch = 0;

  assert(this->GetType() != cmStateEnums::INTERFACE_LIBRARY);

  if (cmValue version = this->GetProperty(property)) {
    // Try to parse the version number and store the results that were
    // successfully parsed.
    int parsed_major;
    int parsed_minor;
    int parsed_patch;
    switch (sscanf(version->c_str(), "%d.%d.%d", &parsed_major, &parsed_minor,
                   &parsed_patch)) {
      case 3:
        patch = parsed_patch;
        CM_FALLTHROUGH;
      case 2:
        minor = parsed_minor;
        CM_FALLTHROUGH;
      case 1:
        major = parsed_major;
        CM_FALLTHROUGH;
      default:
        break;
    }
  }
}

std::string cmGeneratorTarget::GetRuntimeLinkLibrary(
  std::string const& lang, std::string const& config) const
{
  // This is activated by the presence of a default selection whether or
  // not it is overridden by a property.
  cmValue runtimeLibraryDefault = this->Makefile->GetDefinition(
    cmStrCat("CMAKE_", lang, "_RUNTIME_LIBRARY_DEFAULT"));
  if (!cmNonempty(runtimeLibraryDefault)) {
    return std::string();
  }
  cmValue runtimeLibraryValue =
    this->Target->GetProperty(cmStrCat(lang, "_RUNTIME_LIBRARY"));
  if (!runtimeLibraryValue) {
    runtimeLibraryValue = runtimeLibraryDefault;
  }
  return cmSystemTools::UpperCase(cmGeneratorExpression::Evaluate(
    *runtimeLibraryValue, this->LocalGenerator, config, this));
}

std::string cmGeneratorTarget::GetFortranModuleDirectory(
  std::string const& working_dir) const
{
  if (!this->FortranModuleDirectoryCreated) {
    this->FortranModuleDirectory =
      this->CreateFortranModuleDirectory(working_dir);
    this->FortranModuleDirectoryCreated = true;
  }

  return this->FortranModuleDirectory;
}

bool cmGeneratorTarget::IsFortranBuildingIntrinsicModules() const
{
  // ATTENTION Before 4.0 the property name was misspelled.
  // Check the correct name first and than the old name.
  if (cmValue prop = this->GetProperty("Fortran_BUILDING_INTRINSIC_MODULES")) {
    return prop.IsOn();
  }
  if (cmValue prop =
        this->GetProperty("Fortran_BUILDING_INSTRINSIC_MODULES")) {
    return prop.IsOn();
  }
  return false;
}

std::string cmGeneratorTarget::CreateFortranModuleDirectory(
  std::string const& working_dir) const
{
  std::string mod_dir;
  std::string target_mod_dir;
  if (cmValue prop = this->GetProperty("Fortran_MODULE_DIRECTORY")) {
    target_mod_dir = *prop;
  } else {
    std::string const& default_mod_dir =
      this->LocalGenerator->GetCurrentBinaryDirectory();
    if (default_mod_dir != working_dir) {
      target_mod_dir = default_mod_dir;
    }
  }
  cmValue moddir_flag =
    this->Makefile->GetDefinition("CMAKE_Fortran_MODDIR_FLAG");
  if (!target_mod_dir.empty() && moddir_flag) {
    // Compute the full path to the module directory.
    if (cmSystemTools::FileIsFullPath(target_mod_dir)) {
      // Already a full path.
      mod_dir = target_mod_dir;
    } else {
      // Interpret relative to the current output directory.
      mod_dir = cmStrCat(this->LocalGenerator->GetCurrentBinaryDirectory(),
                         '/', target_mod_dir);
    }

    // Make sure the module output directory exists.
    cmSystemTools::MakeDirectory(mod_dir);
  }
  return mod_dir;
}

void cmGeneratorTarget::AddISPCGeneratedHeader(std::string const& header,
                                               std::string const& config)
{
  std::string config_upper;
  if (!config.empty()) {
    config_upper = cmSystemTools::UpperCase(config);
  }
  auto iter = this->ISPCGeneratedHeaders.find(config_upper);
  if (iter == this->ISPCGeneratedHeaders.end()) {
    std::vector<std::string> headers;
    headers.emplace_back(header);
    this->ISPCGeneratedHeaders.insert({ config_upper, headers });
  } else {
    iter->second.emplace_back(header);
  }
}

std::vector<std::string> cmGeneratorTarget::GetGeneratedISPCHeaders(
  std::string const& config) const
{
  std::string config_upper;
  if (!config.empty()) {
    config_upper = cmSystemTools::UpperCase(config);
  }
  auto iter = this->ISPCGeneratedHeaders.find(config_upper);
  if (iter == this->ISPCGeneratedHeaders.end()) {
    return std::vector<std::string>{};
  }
  return iter->second;
}

void cmGeneratorTarget::AddISPCGeneratedObject(std::vector<std::string>&& objs,
                                               std::string const& config)
{
  std::string config_upper;
  if (!config.empty()) {
    config_upper = cmSystemTools::UpperCase(config);
  }
  auto iter = this->ISPCGeneratedObjects.find(config_upper);
  if (iter == this->ISPCGeneratedObjects.end()) {
    this->ISPCGeneratedObjects.insert({ config_upper, objs });
  } else {
    iter->second.insert(iter->second.end(), objs.begin(), objs.end());
  }
}

std::vector<std::string> cmGeneratorTarget::GetGeneratedISPCObjects(
  std::string const& config) const
{
  std::string config_upper;
  if (!config.empty()) {
    config_upper = cmSystemTools::UpperCase(config);
  }
  auto iter = this->ISPCGeneratedObjects.find(config_upper);
  if (iter == this->ISPCGeneratedObjects.end()) {
    return std::vector<std::string>{};
  }
  return iter->second;
}

std::string cmGeneratorTarget::GetFrameworkVersion() const
{
  assert(this->GetType() != cmStateEnums::INTERFACE_LIBRARY);

  if (cmValue fversion = this->GetProperty("FRAMEWORK_VERSION")) {
    return *fversion;
  }
  if (cmValue tversion = this->GetProperty("VERSION")) {
    return *tversion;
  }
  return "A";
}

std::string cmGeneratorTarget::ComputeVersionedName(std::string const& prefix,
                                                    std::string const& base,
                                                    std::string const& suffix,
                                                    std::string const& name,
                                                    cmValue version) const
{
  std::string vName = this->IsApple() ? (prefix + base) : name;
  if (version) {
    vName += ".";
    vName += *version;
  }
  vName += this->IsApple() ? suffix : std::string();
  return vName;
}

std::vector<std::string> cmGeneratorTarget::GetPropertyKeys() const
{
  return this->Target->GetProperties().GetKeys();
}

void cmGeneratorTarget::ReportPropertyOrigin(
  std::string const& p, std::string const& result, std::string const& report,
  std::string const& compatibilityType) const
{
  cmList debugProperties{ this->Target->GetMakefile()->GetDefinition(
    "CMAKE_DEBUG_TARGET_PROPERTIES") };
  bool debugOrigin = !this->DebugCompatiblePropertiesDone[p] &&
    cm::contains(debugProperties, p);

  this->DebugCompatiblePropertiesDone[p] = true;
  if (!debugOrigin) {
    return;
  }

  std::string areport =
    cmStrCat(compatibilityType, " of property \"", p, "\" for target \"",
             this->GetName(), "\" (result: \"", result, "\"):\n", report);

  this->LocalGenerator->GetCMakeInstance()->IssueMessage(MessageType::LOG,
                                                         areport);
}

std::string cmGeneratorTarget::GetDirectory(
  std::string const& config, cmStateEnums::ArtifactType artifact) const
{
  if (this->IsImported()) {
    auto fullPath = this->Target->ImportedGetFullPath(config, artifact);
    if (this->IsFrameworkOnApple()) {
      auto fwDescriptor = this->GetGlobalGenerator()->SplitFrameworkPath(
        fullPath, cmGlobalGenerator::FrameworkFormat::Strict);
      if (fwDescriptor) {
        return fwDescriptor->Directory;
      }
    }
    // Return the directory from which the target is imported.
    return cmSystemTools::GetFilenamePath(fullPath);
  }
  if (OutputInfo const* info = this->GetOutputInfo(config)) {
    // Return the directory in which the target will be built.
    switch (artifact) {
      case cmStateEnums::RuntimeBinaryArtifact:
        return info->OutDir;
      case cmStateEnums::ImportLibraryArtifact:
        return info->ImpDir;
    }
  }
  return "";
}

bool cmGeneratorTarget::UsesDefaultOutputDir(
  std::string const& config, cmStateEnums::ArtifactType artifact) const
{
  std::string dir;
  return this->ComputeOutputDir(config, artifact, dir);
}

cmGeneratorTarget::OutputInfo const* cmGeneratorTarget::GetOutputInfo(
  std::string const& config) const
{
  // There is no output information for imported targets.
  if (this->IsImported()) {
    return nullptr;
  }

  // Synthetic targets don't have output.
  if (this->IsSynthetic()) {
    return nullptr;
  }

  // Only libraries and executables have well-defined output files.
  if (!this->HaveWellDefinedOutputFiles()) {
    std::string msg = cmStrCat("cmGeneratorTarget::GetOutputInfo called for ",
                               this->GetName(), " which has type ",
                               cmState::GetTargetTypeName(this->GetType()));
    this->LocalGenerator->IssueMessage(MessageType::INTERNAL_ERROR, msg);
    return nullptr;
  }

  // Lookup/compute/cache the output information for this configuration.
  std::string config_upper;
  if (!config.empty()) {
    config_upper = cmSystemTools::UpperCase(config);
  }
  auto i = this->OutputInfoMap.find(config_upper);
  if (i == this->OutputInfoMap.end()) {
    // Add empty info in map to detect potential recursion.
    OutputInfo info;
    OutputInfoMapType::value_type entry(config_upper, info);
    i = this->OutputInfoMap.insert(entry).first;

    // Compute output directories.
    this->ComputeOutputDir(config, cmStateEnums::RuntimeBinaryArtifact,
                           info.OutDir);
    this->ComputeOutputDir(config, cmStateEnums::ImportLibraryArtifact,
                           info.ImpDir);
    if (!this->ComputePDBOutputDir("PDB", config, info.PdbDir)) {
      info.PdbDir = info.OutDir;
    }

    // Now update the previously-prepared map entry.
    i->second = info;
  } else if (i->second.empty()) {
    // An empty map entry indicates we have been called recursively
    // from the above block.
    this->LocalGenerator->GetCMakeInstance()->IssueMessage(
      MessageType::FATAL_ERROR,
      "Target '" + this->GetName() + "' OUTPUT_DIRECTORY depends on itself.",
      this->GetBacktrace());
    return nullptr;
  }
  return &i->second;
}

bool cmGeneratorTarget::ComputeOutputDir(std::string const& config,
                                         cmStateEnums::ArtifactType artifact,
                                         std::string& out) const
{
  bool usesDefaultOutputDir = false;
  std::string conf = config;

  // Look for a target property defining the target output directory
  // based on the target type.
  std::string targetTypeName = this->GetOutputTargetType(artifact);
  std::string propertyName;
  if (!targetTypeName.empty()) {
    propertyName = cmStrCat(targetTypeName, "_OUTPUT_DIRECTORY");
  }

  // Check for a per-configuration output directory target property.
  std::string configUpper = cmSystemTools::UpperCase(conf);
  std::string configProp;
  if (!targetTypeName.empty()) {
    configProp = cmStrCat(targetTypeName, "_OUTPUT_DIRECTORY_", configUpper);
  }

  // Select an output directory.
  if (cmValue config_outdir = this->GetProperty(configProp)) {
    // Use the user-specified per-configuration output directory.
    out = cmGeneratorExpression::Evaluate(*config_outdir, this->LocalGenerator,
                                          config, this);

    // Skip per-configuration subdirectory.
    conf.clear();
  } else if (cmValue outdir = this->GetProperty(propertyName)) {
    // Use the user-specified output directory.
    out = cmGeneratorExpression::Evaluate(*outdir, this->LocalGenerator,
                                          config, this);
    // Skip per-configuration subdirectory if the value contained a
    // generator expression.
    if (out != *outdir) {
      conf.clear();
    }
  } else if (this->GetType() == cmStateEnums::EXECUTABLE) {
    // Lookup the output path for executables.
    out = this->Makefile->GetSafeDefinition("EXECUTABLE_OUTPUT_PATH");
  } else if (this->GetType() == cmStateEnums::STATIC_LIBRARY ||
             this->GetType() == cmStateEnums::SHARED_LIBRARY ||
             this->GetType() == cmStateEnums::MODULE_LIBRARY) {
    // Lookup the output path for libraries.
    out = this->Makefile->GetSafeDefinition("LIBRARY_OUTPUT_PATH");
  }
  if (out.empty()) {
    // Default to the current output directory.
    usesDefaultOutputDir = true;
    out = ".";
  }

  // Convert the output path to a full path in case it is
  // specified as a relative path.  Treat a relative path as
  // relative to the current output directory for this makefile.
  out = (cmSystemTools::CollapseFullPath(
    out, this->LocalGenerator->GetCurrentBinaryDirectory()));

  // The generator may add the configuration's subdirectory.
  if (!conf.empty()) {
    bool useEPN =
      this->GlobalGenerator->UseEffectivePlatformName(this->Makefile);
    std::string suffix =
      usesDefaultOutputDir && useEPN ? "${EFFECTIVE_PLATFORM_NAME}" : "";
    this->LocalGenerator->GetGlobalGenerator()->AppendDirectoryForConfig(
      "/", conf, suffix, out);
  }

  return usesDefaultOutputDir;
}

bool cmGeneratorTarget::ComputePDBOutputDir(std::string const& kind,
                                            std::string const& config,
                                            std::string& out) const
{
  // Look for a target property defining the target output directory
  // based on the target type.
  std::string propertyName;
  if (!kind.empty()) {
    propertyName = cmStrCat(kind, "_OUTPUT_DIRECTORY");
  }
  std::string conf = config;

  // Check for a per-configuration output directory target property.
  std::string configUpper = cmSystemTools::UpperCase(conf);
  std::string configProp;
  if (!kind.empty()) {
    configProp = cmStrCat(kind, "_OUTPUT_DIRECTORY_", configUpper);
  }

  // Select an output directory.
  if (cmValue config_outdir = this->GetProperty(configProp)) {
    // Use the user-specified per-configuration output directory.
    out = cmGeneratorExpression::Evaluate(*config_outdir, this->LocalGenerator,
                                          config);

    // Skip per-configuration subdirectory.
    conf.clear();
  } else if (cmValue outdir = this->GetProperty(propertyName)) {
    // Use the user-specified output directory.
    out =
      cmGeneratorExpression::Evaluate(*outdir, this->LocalGenerator, config);

    // Skip per-configuration subdirectory if the value contained a
    // generator expression.
    if (out != *outdir) {
      conf.clear();
    }
  }
  if (out.empty()) {
    // Compile output should always have a path.
    if (kind == "COMPILE_PDB"_s) {
      out = this->GetSupportDirectory();
    } else {
      return false;
    }
  }

  // Convert the output path to a full path in case it is
  // specified as a relative path.  Treat a relative path as
  // relative to the current output directory for this makefile.
  out = (cmSystemTools::CollapseFullPath(
    out, this->LocalGenerator->GetCurrentBinaryDirectory()));

  // The generator may add the configuration's subdirectory.
  if (!conf.empty()) {
    this->LocalGenerator->GetGlobalGenerator()->AppendDirectoryForConfig(
      "/", conf, "", out);
  }
  return true;
}

bool cmGeneratorTarget::HaveInstallTreeRPATH(std::string const& config) const
{
  std::string install_rpath;
  this->GetInstallRPATH(config, install_rpath);
  return !install_rpath.empty() &&
    !this->Makefile->IsOn("CMAKE_SKIP_INSTALL_RPATH");
}

bool cmGeneratorTarget::GetBuildRPATH(std::string const& config,
                                      std::string& rpath) const
{
  return this->GetRPATH(config, "BUILD_RPATH", rpath);
}

bool cmGeneratorTarget::GetInstallRPATH(std::string const& config,
                                        std::string& rpath) const
{
  return this->GetRPATH(config, "INSTALL_RPATH", rpath);
}

bool cmGeneratorTarget::GetRPATH(std::string const& config,
                                 std::string const& prop,
                                 std::string& rpath) const
{
  cmValue value = this->GetProperty(prop);
  if (!value) {
    return false;
  }

  rpath =
    cmGeneratorExpression::Evaluate(*value, this->LocalGenerator, config);

  return true;
}

cmGeneratorTarget::ImportInfo const* cmGeneratorTarget::GetImportInfo(
  std::string const& config) const
{
  // There is no imported information for non-imported targets.
  if (!this->IsImported()) {
    return nullptr;
  }

  // Lookup/compute/cache the import information for this
  // configuration.
  std::string config_upper;
  if (!config.empty()) {
    config_upper = cmSystemTools::UpperCase(config);
  } else {
    config_upper = "NOCONFIG";
  }

  auto i = this->ImportInfoMap.find(config_upper);
  if (i == this->ImportInfoMap.end()) {
    ImportInfo info;
    this->ComputeImportInfo(config_upper, info);
    ImportInfoMapType::value_type entry(config_upper, info);
    i = this->ImportInfoMap.insert(entry).first;
  }

  if (this->GetType() == cmStateEnums::INTERFACE_LIBRARY) {
    return &i->second;
  }
  // If the location is empty then the target is not available for
  // this configuration.
  if (i->second.Location.empty() && i->second.ImportLibrary.empty()) {
    return nullptr;
  }

  // Return the import information.
  return &i->second;
}

void cmGeneratorTarget::ComputeImportInfo(std::string const& desired_config,
                                          ImportInfo& info) const
{
  // This method finds information about an imported target from its
  // properties.  The "IMPORTED_" namespace is reserved for properties
  // defined by the project exporting the target.

  // Initialize members.
  info.NoSOName = false;

  cmValue loc = nullptr;
  cmValue imp = nullptr;
  std::string suffix;
  if (!this->Target->GetMappedConfig(desired_config, loc, imp, suffix)) {
    return;
  }

  // Get the link interface.
  {
    // Use the INTERFACE_LINK_LIBRARIES special representation directly
    // to get backtraces.
    cmBTStringRange entries = this->Target->GetLinkInterfaceEntries();
    if (!entries.empty()) {
      info.LibrariesProp = "INTERFACE_LINK_LIBRARIES";
      for (BT<std::string> const& entry : entries) {
        info.Libraries.emplace_back(entry);
      }
    } else if (this->GetType() != cmStateEnums::INTERFACE_LIBRARY) {
      std::string linkProp =
        cmStrCat("IMPORTED_LINK_INTERFACE_LIBRARIES", suffix);
      cmValue propertyLibs = this->GetProperty(linkProp);
      if (!propertyLibs) {
        linkProp = "IMPORTED_LINK_INTERFACE_LIBRARIES";
        propertyLibs = this->GetProperty(linkProp);
      }
      if (propertyLibs) {
        info.LibrariesProp = linkProp;
        info.Libraries.emplace_back(*propertyLibs);
      }
    }
  }
  for (BT<std::string> const& entry :
       this->Target->GetLinkInterfaceDirectEntries()) {
    info.LibrariesHeadInclude.emplace_back(entry);
  }
  for (BT<std::string> const& entry :
       this->Target->GetLinkInterfaceDirectExcludeEntries()) {
    info.LibrariesHeadExclude.emplace_back(entry);
  }
  if (this->GetType() == cmStateEnums::INTERFACE_LIBRARY) {
    if (loc) {
      info.LibName = *loc;
    }
    return;
  }

  // A provided configuration has been chosen.  Load the
  // configuration's properties.

  // Get the location.
  if (loc) {
    info.Location = *loc;
  } else {
    std::string impProp = cmStrCat("IMPORTED_LOCATION", suffix);
    if (cmValue config_location = this->GetProperty(impProp)) {
      info.Location = *config_location;
    } else if (cmValue location = this->GetProperty("IMPORTED_LOCATION")) {
      info.Location = *location;
    }
  }

  // Get the soname.
  if (this->GetType() == cmStateEnums::SHARED_LIBRARY) {
    std::string soProp = cmStrCat("IMPORTED_SONAME", suffix);
    if (cmValue config_soname = this->GetProperty(soProp)) {
      info.SOName = *config_soname;
    } else if (cmValue soname = this->GetProperty("IMPORTED_SONAME")) {
      info.SOName = *soname;
    }
  }

  // Get the "no-soname" mark.
  if (this->GetType() == cmStateEnums::SHARED_LIBRARY) {
    std::string soProp = cmStrCat("IMPORTED_NO_SONAME", suffix);
    if (cmValue config_no_soname = this->GetProperty(soProp)) {
      info.NoSOName = config_no_soname.IsOn();
    } else if (cmValue no_soname = this->GetProperty("IMPORTED_NO_SONAME")) {
      info.NoSOName = no_soname.IsOn();
    }
  }

  // Get the import library.
  if (imp) {
    info.ImportLibrary = *imp;
  } else if (this->GetType() == cmStateEnums::SHARED_LIBRARY ||
             this->IsExecutableWithExports()) {
    std::string impProp = cmStrCat("IMPORTED_IMPLIB", suffix);
    if (cmValue config_implib = this->GetProperty(impProp)) {
      info.ImportLibrary = *config_implib;
    } else if (cmValue implib = this->GetProperty("IMPORTED_IMPLIB")) {
      info.ImportLibrary = *implib;
    }
  }

  // Get the link dependencies.
  {
    std::string linkProp =
      cmStrCat("IMPORTED_LINK_DEPENDENT_LIBRARIES", suffix);
    if (cmValue config_libs = this->GetProperty(linkProp)) {
      info.SharedDeps = *config_libs;
    } else if (cmValue libs =
                 this->GetProperty("IMPORTED_LINK_DEPENDENT_LIBRARIES")) {
      info.SharedDeps = *libs;
    }
  }

  // Get the link languages.
  if (this->LinkLanguagePropagatesToDependents()) {
    std::string linkProp =
      cmStrCat("IMPORTED_LINK_INTERFACE_LANGUAGES", suffix);
    if (cmValue config_libs = this->GetProperty(linkProp)) {
      info.Languages = *config_libs;
    } else if (cmValue libs =
                 this->GetProperty("IMPORTED_LINK_INTERFACE_LANGUAGES")) {
      info.Languages = *libs;
    }
  }

  // Get information if target is managed assembly.
  {
    std::string linkProp = "IMPORTED_COMMON_LANGUAGE_RUNTIME";
    if (cmValue pc = this->GetProperty(linkProp + suffix)) {
      info.Managed = this->CheckManagedType(*pc);
    } else if (cmValue p = this->GetProperty(linkProp)) {
      info.Managed = this->CheckManagedType(*p);
    }
  }

  // Get the cyclic repetition count.
  if (this->GetType() == cmStateEnums::STATIC_LIBRARY) {
    std::string linkProp =
      cmStrCat("IMPORTED_LINK_INTERFACE_MULTIPLICITY", suffix);
    if (cmValue config_reps = this->GetProperty(linkProp)) {
      sscanf(config_reps->c_str(), "%u", &info.Multiplicity);
    } else if (cmValue reps =
                 this->GetProperty("IMPORTED_LINK_INTERFACE_MULTIPLICITY")) {
      sscanf(reps->c_str(), "%u", &info.Multiplicity);
    }
  }
}

bool cmGeneratorTarget::GetConfigCommonSourceFilesForXcode(
  std::vector<cmSourceFile*>& files) const
{
  std::vector<std::string> const& configs =
    this->Makefile->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig);

  auto it = configs.begin();
  std::string const& firstConfig = *it;
  this->GetSourceFilesWithoutObjectLibraries(files, firstConfig);

  for (; it != configs.end(); ++it) {
    std::vector<cmSourceFile*> configFiles;
    this->GetSourceFilesWithoutObjectLibraries(configFiles, *it);
    if (configFiles != files) {
      std::string firstConfigFiles;
      char const* sep = "";
      for (cmSourceFile* f : files) {
        firstConfigFiles += sep;
        firstConfigFiles += f->ResolveFullPath();
        sep = "\n  ";
      }

      std::string thisConfigFiles;
      sep = "";
      for (cmSourceFile* f : configFiles) {
        thisConfigFiles += sep;
        thisConfigFiles += f->ResolveFullPath();
        sep = "\n  ";
      }
      std::ostringstream e;
      /* clang-format off */
      e << "Target \"" << this->GetName()
        << "\" has source files which vary by "
        "configuration. This is not supported by the \""
        << this->GlobalGenerator->GetName()
        << "\" generator.\n"
          "Config \"" << firstConfig << "\":\n"
          "  " << firstConfigFiles << "\n"
          "Config \"" << *it << "\":\n"
          "  " << thisConfigFiles << "\n";
      /* clang-format on */
      this->LocalGenerator->IssueMessage(MessageType::FATAL_ERROR, e.str());
      return false;
    }
  }
  return true;
}

void cmGeneratorTarget::GetObjectLibrariesInSources(
  std::vector<cmGeneratorTarget*>& objlibs) const
{
  // FIXME: This searches SOURCES for TARGET_OBJECTS for backwards
  // compatibility with the OLD behavior of CMP0026 since this
  // could be called at configure time.  CMP0026 has been removed,
  // so this should now be called only at generate time.
  // Therefore we should be able to improve the implementation
  // with generate-time information.
  cmBTStringRange rng = this->Target->GetSourceEntries();
  for (auto const& entry : rng) {
    cmList files{ entry.Value };
    for (auto const& li : files) {
      if (cmHasLiteralPrefix(li, "$<TARGET_OBJECTS:") && li.back() == '>') {
        std::string objLibName = li.substr(17, li.size() - 18);

        if (cmGeneratorExpression::Find(objLibName) != std::string::npos) {
          continue;
        }
        cmGeneratorTarget* objLib =
          this->LocalGenerator->FindGeneratorTargetToUse(objLibName);
        if (objLib) {
          objlibs.push_back(objLib);
        }
      }
    }
  }
}

std::string cmGeneratorTarget::CheckCMP0004(std::string const& item) const
{
  // Strip whitespace off the library names because we used to do this
  // in case variables were expanded at generate time.  We no longer
  // do the expansion but users link to libraries like " ${VAR} ".
  std::string lib = item;
  std::string::size_type pos = lib.find_first_not_of(" \t\r\n");
  if (pos != std::string::npos) {
    lib = lib.substr(pos);
  }
  pos = lib.find_last_not_of(" \t\r\n");
  if (pos != std::string::npos) {
    lib = lib.substr(0, pos + 1);
  }
  if (lib != item) {
    cmake* cm = this->LocalGenerator->GetCMakeInstance();
    std::ostringstream e;
    e << "Target \"" << this->GetName() << "\" links to item \"" << item
      << "\" which has leading or trailing whitespace.  "
      << "This is now an error according to policy CMP0004.";
    cm->IssueMessage(MessageType::FATAL_ERROR, e.str(), this->GetBacktrace());
  }
  return lib;
}

bool cmGeneratorTarget::IsDeprecated() const
{
  cmValue deprecation = this->GetProperty("DEPRECATION");
  return cmNonempty(deprecation);
}

std::string cmGeneratorTarget::GetDeprecation() const
{
  // find DEPRECATION property
  if (cmValue deprecation = this->GetProperty("DEPRECATION")) {
    return *deprecation;
  }
  return std::string();
}

void cmGeneratorTarget::GetLanguages(std::set<std::string>& languages,
                                     std::string const& config) const
{
  // Targets that do not compile anything have no languages.
  if (!this->CanCompileSources()) {
    return;
  }

  std::vector<cmSourceFile*> sourceFiles;
  this->GetSourceFiles(sourceFiles, config);
  for (cmSourceFile* src : sourceFiles) {
    std::string const& lang = src->GetOrDetermineLanguage();
    if (!lang.empty()) {
      languages.insert(lang);
    }
  }

  std::set<cmGeneratorTarget const*> objectLibraries =
    this->GetSourceObjectLibraries(config);
  for (cmGeneratorTarget const* objLib : objectLibraries) {
    objLib->GetLanguages(languages, config);
  }
}

std::set<cmGeneratorTarget const*> cmGeneratorTarget::GetSourceObjectLibraries(
  std::string const& config) const
{
  std::set<cmGeneratorTarget const*> objectLibraries;
  std::vector<cmSourceFile const*> externalObjects;
  this->GetExternalObjects(externalObjects, config);
  for (cmSourceFile const* extObj : externalObjects) {
    std::string objLib = extObj->GetObjectLibrary();
    if (cmGeneratorTarget* tgt =
          this->LocalGenerator->FindGeneratorTargetToUse(objLib)) {
      objectLibraries.insert(tgt);
    }
  }

  return objectLibraries;
}

bool cmGeneratorTarget::IsLanguageUsed(std::string const& language,
                                       std::string const& config) const
{
  std::set<std::string> languages;
  this->GetLanguages(languages, config);
  return languages.count(language);
}

bool cmGeneratorTarget::IsCSharpOnly() const
{
  // Only certain target types may compile CSharp.
  if (this->GetType() != cmStateEnums::SHARED_LIBRARY &&
      this->GetType() != cmStateEnums::STATIC_LIBRARY &&
      this->GetType() != cmStateEnums::EXECUTABLE) {
    return false;
  }
  std::set<std::string> languages = this->GetAllConfigCompileLanguages();
  // Consider an explicit linker language property, but *not* the
  // computed linker language that may depend on linked targets.
  cmValue linkLang = this->GetProperty("LINKER_LANGUAGE");
  if (cmNonempty(linkLang)) {
    languages.insert(*linkLang);
  }
  return languages.size() == 1 && languages.count("CSharp") > 0;
}

bool cmGeneratorTarget::IsDotNetSdkTarget() const
{
  return !this->GetProperty("DOTNET_SDK").IsEmpty();
}

void cmGeneratorTarget::ComputeLinkImplementationLanguages(
  std::string const& config, cmOptionalLinkImplementation& impl) const
{
  // This target needs runtime libraries for its source languages.
  std::set<std::string> languages;
  // Get languages used in our source files.
  this->GetLanguages(languages, config);
  // Copy the set of languages to the link implementation.
  impl.Languages.insert(impl.Languages.begin(), languages.begin(),
                        languages.end());
}

bool cmGeneratorTarget::HaveBuildTreeRPATH(std::string const& config) const
{
  if (this->GetPropertyAsBool("SKIP_BUILD_RPATH")) {
    return false;
  }
  std::string build_rpath;
  if (this->GetBuildRPATH(config, build_rpath)) {
    return true;
  }
  if (cmLinkImplementationLibraries const* impl =
        this->GetLinkImplementationLibraries(config, UseTo::Link)) {
    return !impl->Libraries.empty();
  }
  return false;
}

bool cmGeneratorTarget::IsNullImpliedByLinkLibraries(
  std::string const& p) const
{
  return cm::contains(this->LinkImplicitNullProperties, p);
}

bool cmGeneratorTarget::ApplyCXXStdTargets()
{
  cmStandardLevelResolver standardResolver(this->Makefile);
  cmStandardLevel const cxxStd23 =
    *standardResolver.LanguageStandardLevel("CXX", "23");
  std::vector<std::string> const& configs =
    this->Makefile->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig);
  auto std_prop = this->GetProperty("CXX_MODULE_STD");
  if (!std_prop) {
    // TODO(cxxmodules): Add a target policy to flip the default here. Set
    // `std_prop` based on it.
    return true;
  }

  std::string std_prop_value;
  if (std_prop) {
    // Evaluate generator expressions.
    cmGeneratorExpression ge(*this->LocalGenerator->GetCMakeInstance());
    auto cge = ge.Parse(*std_prop);
    if (!cge) {
      this->Makefile->IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat(R"(The "CXX_MODULE_STD" property on the target ")",
                 this->GetName(), "\" is not a valid generator expression."));
      return false;
    }
    // But do not allow context-sensitive queries. Whether a target uses
    // `import std` should not depend on configuration or properties of the
    // consumer (head target). The link language also shouldn't matter, so ban
    // it as well.
    if (cge->GetHadHeadSensitiveCondition()) {
      // Not reachable; all target-sensitive genexes actually fail to parse.
      this->Makefile->IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat(R"(The "CXX_MODULE_STD" property on the target ")",
                 this->GetName(),
                 "\" contains a condition that queries the "
                 "consuming target which is not supported."));
      return false;
    }
    if (cge->GetHadLinkLanguageSensitiveCondition()) {
      // Not reachable; all link language genexes actually fail to parse.
      this->Makefile->IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat(R"(The "CXX_MODULE_STD" property on the target ")",
                 this->GetName(),
                 "\" contains a condition that queries the "
                 "link language which is not supported."));
      return false;
    }
    std_prop_value = cge->Evaluate(this->LocalGenerator, "");
    if (cge->GetHadContextSensitiveCondition()) {
      this->Makefile->IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat(R"(The "CXX_MODULE_STD" property on the target ")",
                 this->GetName(),
                 "\" contains a context-sensitive condition "
                 "that is not supported."));
      return false;
    }
  }
  auto use_std = cmIsOn(std_prop_value);

  // If we have a value and it is not true, there's nothing to do.
  if (std_prop && !use_std) {
    return true;
  }

  for (auto const& config : configs) {
    if (this->HaveCxxModuleSupport(config) != Cxx20SupportLevel::Supported) {
      continue;
    }

    cm::optional<cmStandardLevel> explicitLevel =
      this->GetExplicitStandardLevel("CXX", config);
    if (!explicitLevel || *explicitLevel < cxxStd23) {
      continue;
    }

    auto const stdLevel =
      standardResolver.GetLevelString("CXX", *explicitLevel);
    auto const targetName = cmStrCat("__CMAKE::CXX", stdLevel);
    if (!this->Makefile->FindTargetToUse(targetName)) {
      auto basicReason = this->Makefile->GetDefinition(cmStrCat(
        "CMAKE_CXX", stdLevel, "_COMPILER_IMPORT_STD_NOT_FOUND_MESSAGE"));
      std::string reason;
      if (!basicReason.IsEmpty()) {
        reason = cmStrCat("  Reason:\n  ", basicReason);
      }
      this->Makefile->IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat(R"(The "CXX_MODULE_STD" property on the target ")",
                 this->GetName(), "\" requires that the \"", targetName,
                 "\" target exist, but it was not provided by the toolchain.",
                 reason));
      break;
    }

    // Check the experimental feature here as well. A toolchain may have
    // provided the target and skipped the check in the toolchain preparation
    // logic.
    if (!cmExperimental::HasSupportEnabled(
          *this->Makefile, cmExperimental::Feature::CxxImportStd)) {
      break;
    }

    this->Target->AppendProperty(
      "LINK_LIBRARIES",
      cmStrCat("$<BUILD_LOCAL_INTERFACE:$<$<CONFIG:", config, ">:", targetName,
               ">>"));
  }

  return true;
}

bool cmGeneratorTarget::DiscoverSyntheticTargets(cmSyntheticTargetCache& cache,
                                                 std::string const& config)
{
  std::vector<std::string> allConfigs =
    this->Makefile->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig);
  cmOptionalLinkImplementation impl;
  this->ComputeLinkImplementationLibraries(config, impl, UseTo::Link);

  cmCxxModuleUsageEffects usage(this);

  auto& SyntheticDeps = this->Configs[config].SyntheticDeps;

  for (auto const& entry : impl.Libraries) {
    auto const* gt = entry.Target;
    if (!gt || !gt->IsImported()) {
      continue;
    }

    if (gt->HaveCxx20ModuleSources()) {
      cmCryptoHash hasher(cmCryptoHash::AlgoSHA3_512);
      constexpr size_t HASH_TRUNCATION = 12;
      auto dirhash = hasher.HashString(
        gt->GetLocalGenerator()->GetCurrentBinaryDirectory());
      std::string safeName = gt->GetName();
      cmSystemTools::ReplaceString(safeName, ":", "_");
      auto targetIdent =
        hasher.HashString(cmStrCat("@d_", dirhash, "@u_", usage.GetHash()));
      std::string targetName =
        cmStrCat(safeName, "@synth_", targetIdent.substr(0, HASH_TRUNCATION));

      // Check the cache to see if this instance of the imported target has
      // already been created.
      auto cached = cache.CxxModuleTargets.find(targetName);
      cmGeneratorTarget const* synthDep = nullptr;
      if (cached == cache.CxxModuleTargets.end()) {
        auto const* model = gt->Target;
        auto* mf = gt->Makefile;
        auto* lg = gt->GetLocalGenerator();
        auto* tgt = mf->AddSynthesizedTarget(cmStateEnums::INTERFACE_LIBRARY,
                                             targetName);

        // Copy relevant information from the existing IMPORTED target.

        // Copy policies to the target.
        tgt->CopyPolicyStatuses(model);

        // Copy file sets.
        {
          auto fsNames = model->GetAllFileSetNames();
          for (auto const& fsName : fsNames) {
            auto const* fs = model->GetFileSet(fsName);
            if (!fs) {
              mf->IssueMessage(MessageType::INTERNAL_ERROR,
                               cmStrCat("Failed to find file set named '",
                                        fsName, "' on target '",
                                        tgt->GetName(), '\''));
              continue;
            }
            auto* newFs = tgt
                            ->GetOrCreateFileSet(fs->GetName(), fs->GetType(),
                                                 fs->GetVisibility())
                            .first;
            newFs->CopyEntries(fs);
          }
        }

        // Copy imported C++ module properties.
        tgt->CopyImportedCxxModulesEntries(model);

        // Copy other properties which may affect the C++ module BMI
        // generation.
        tgt->CopyImportedCxxModulesProperties(model);

        tgt->AddLinkLibrary(*mf,
                            cmStrCat("$<COMPILE_ONLY:", model->GetName(), '>'),
                            GENERAL_LibraryType);

        // Apply usage requirements to the target.
        usage.ApplyToTarget(tgt);

        // Create the generator target and attach it to the local generator.
        auto gtp = cm::make_unique<cmGeneratorTarget>(tgt, lg);

        synthDep = gtp.get();
        cache.CxxModuleTargets[targetName] = synthDep;

        // See `localGen->ComputeTargetCompileFeatures()` call in
        // `cmGlobalGenerator::Compute` for where non-synthetic targets resolve
        // this.
        for (auto const& innerConfig : allConfigs) {
          gtp->ComputeCompileFeatures(innerConfig);
        }
        // See `cmGlobalGenerator::ApplyCXXStdTargets` in
        // `cmGlobalGenerator::Compute` for non-synthetic target resolutions.
        gtp->ApplyCXXStdTargets();

        gtp->DiscoverSyntheticTargets(cache, config);

        lg->AddGeneratorTarget(std::move(gtp));
      } else {
        synthDep = cached->second;
      }

      SyntheticDeps[gt].push_back(synthDep);
    }
  }

  return true;
}

bool cmGeneratorTarget::HasPackageReferences() const
{
  return this->IsInBuildSystem() &&
    !this->GetProperty("VS_PACKAGE_REFERENCES")->empty();
}

std::vector<std::string> cmGeneratorTarget::GetPackageReferences() const
{
  cmList packageReferences;

  if (this->IsInBuildSystem()) {
    if (cmValue vsPackageReferences =
          this->GetProperty("VS_PACKAGE_REFERENCES")) {
      packageReferences.assign(*vsPackageReferences);
    }
  }

  return std::move(packageReferences.data());
}

std::string cmGeneratorTarget::GetPDBDirectory(std::string const& config) const
{
  if (OutputInfo const* info = this->GetOutputInfo(config)) {
    // Return the directory in which the target will be built.
    return info->PdbDir;
  }
  return "";
}

bool cmGeneratorTarget::HasImplibGNUtoMS(std::string const& config) const
{
  return this->HasImportLibrary(config) && this->GetPropertyAsBool("GNUtoMS");
}

bool cmGeneratorTarget::GetImplibGNUtoMS(std::string const& config,
                                         std::string const& gnuName,
                                         std::string& out,
                                         char const* newExt) const
{
  if (this->HasImplibGNUtoMS(config) && gnuName.size() > 6 &&
      gnuName.substr(gnuName.size() - 6) == ".dll.a") {
    out = cmStrCat(cm::string_view(gnuName).substr(0, gnuName.size() - 6),
                   newExt ? newExt : ".lib");
    return true;
  }
  return false;
}

bool cmGeneratorTarget::HasContextDependentSources() const
{
  return this->SourcesAreContextDependent == Tribool::True;
}

bool cmGeneratorTarget::IsExecutableWithExports() const
{
  return this->Target->IsExecutableWithExports();
}

bool cmGeneratorTarget::IsSharedLibraryWithExports() const
{
  return this->Target->IsSharedLibraryWithExports();
}

bool cmGeneratorTarget::HasImportLibrary(std::string const& config) const
{
  bool generate_Stubs = true;
  if (this->GetGlobalGenerator()->IsXcode()) {
    // take care of CMAKE_XCODE_ATTRIBUTE_GENERATE_TEXT_BASED_STUBS variable
    // as well as XCODE_ATTRIBUTE_GENERATE_TEXT_BASED_STUBS property
    if (cmValue propGenStubs =
          this->GetProperty("XCODE_ATTRIBUTE_GENERATE_TEXT_BASED_STUBS")) {
      generate_Stubs = propGenStubs == "YES";
    } else if (cmValue varGenStubs = this->Makefile->GetDefinition(
                 "CMAKE_XCODE_ATTRIBUTE_GENERATE_TEXT_BASED_STUBS")) {
      generate_Stubs = varGenStubs == "YES";
    }
  }

  return (this->IsDLLPlatform() &&
          (this->GetType() == cmStateEnums::SHARED_LIBRARY ||
           this->IsExecutableWithExports()) &&
          // Assemblies which have only managed code do not have
          // import libraries.
          this->GetManagedType(config) != ManagedType::Managed) ||
    (this->IsAIX() && this->IsExecutableWithExports()) ||
    (this->Makefile->PlatformSupportsAppleTextStubs() &&
     this->IsSharedLibraryWithExports() && generate_Stubs);
}

bool cmGeneratorTarget::NeedImportLibraryName(std::string const& config) const
{
  return this->HasImportLibrary(config) ||
    // On DLL platforms we always generate the import library name
    // just in case the sources have export markup.
    (this->IsDLLPlatform() &&
     (this->GetType() == cmStateEnums::EXECUTABLE ||
      this->GetType() == cmStateEnums::MODULE_LIBRARY));
}

bool cmGeneratorTarget::GetUseShortObjectNames(
  cmStateEnums::IntermediateDirKind kind) const
{
  return this->LocalGenerator->UseShortObjectNames(kind);
}

std::string cmGeneratorTarget::GetSupportDirectory(
  cmStateEnums::IntermediateDirKind kind) const
{
  cmLocalGenerator* lg = this->GetLocalGenerator();
  return cmStrCat(lg->GetObjectOutputRoot(kind), '/',
                  lg->GetTargetDirectory(this));
}

std::string cmGeneratorTarget::GetCMFSupportDirectory(
  cmStateEnums::IntermediateDirKind kind) const
{
  cmLocalGenerator* lg = this->GetLocalGenerator();
  if (!lg->AlwaysUsesCMFPaths()) {
    return cmStrCat(lg->GetCurrentBinaryDirectory(), "/CMakeFiles/",
                    lg->GetTargetDirectory(this));
  }
  return cmStrCat(lg->GetObjectOutputRoot(kind), '/',
                  lg->GetTargetDirectory(this));
}

bool cmGeneratorTarget::IsLinkable() const
{
  return (this->GetType() == cmStateEnums::STATIC_LIBRARY ||
          this->GetType() == cmStateEnums::SHARED_LIBRARY ||
          this->GetType() == cmStateEnums::MODULE_LIBRARY ||
          this->GetType() == cmStateEnums::UNKNOWN_LIBRARY ||
          this->GetType() == cmStateEnums::OBJECT_LIBRARY ||
          this->GetType() == cmStateEnums::INTERFACE_LIBRARY ||
          this->IsExecutableWithExports());
}

bool cmGeneratorTarget::HasLinkDependencyFile(std::string const& config) const
{
  if (this->GetType() != cmStateEnums::EXECUTABLE &&
      this->GetType() != cmStateEnums::SHARED_LIBRARY &&
      this->GetType() != cmStateEnums::MODULE_LIBRARY) {
    return false;
  }

  if (this->Target->GetProperty("LINK_DEPENDS_NO_SHARED").IsOn()) {
    // Do not use the linker dependency file because it includes shared
    // libraries as well
    return false;
  }

  std::string const depsUseLinker{ "CMAKE_LINK_DEPENDS_USE_LINKER" };
  auto linkLanguage = this->GetLinkerLanguage(config);
  std::string const langDepsUseLinker{ cmStrCat("CMAKE_", linkLanguage,
                                                "_LINK_DEPENDS_USE_LINKER") };

  return (!this->Makefile->IsDefinitionSet(depsUseLinker) ||
          this->Makefile->IsOn(depsUseLinker)) &&
    this->Makefile->IsOn(langDepsUseLinker);
}

bool cmGeneratorTarget::IsFrameworkOnApple() const
{
  return this->Target->IsFrameworkOnApple();
}

bool cmGeneratorTarget::IsArchivedAIXSharedLibrary() const
{
  return this->Target->IsArchivedAIXSharedLibrary();
}

bool cmGeneratorTarget::IsImportedFrameworkFolderOnApple(
  std::string const& config) const
{
  if (this->IsApple() && this->IsImported() &&
      (this->GetType() == cmStateEnums::STATIC_LIBRARY ||
       this->GetType() == cmStateEnums::SHARED_LIBRARY ||
       this->GetType() == cmStateEnums::UNKNOWN_LIBRARY)) {
    std::string cfg = config;
    if (cfg.empty() && this->GetGlobalGenerator()->IsXcode()) {
      // FIXME(#25515): Remove the need for this workaround.
      // The Xcode generator queries include directories without any
      // specific configuration.  Pick one in case this target does
      // not set either IMPORTED_LOCATION or IMPORTED_CONFIGURATIONS.
      cfg =
        this->Makefile->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig)[0];
    }
    return cmSystemTools::IsPathToFramework(this->GetLocation(cfg));
  }

  return false;
}

bool cmGeneratorTarget::IsAppBundleOnApple() const
{
  return this->Target->IsAppBundleOnApple();
}

bool cmGeneratorTarget::IsXCTestOnApple() const
{
  return (this->IsCFBundleOnApple() && this->GetPropertyAsBool("XCTEST"));
}

bool cmGeneratorTarget::IsCFBundleOnApple() const
{
  return (this->GetType() == cmStateEnums::MODULE_LIBRARY && this->IsApple() &&
          this->GetPropertyAsBool("BUNDLE"));
}

cmGeneratorTarget::ManagedType cmGeneratorTarget::CheckManagedType(
  std::string const& propval) const
{
  // The type of the managed assembly (mixed unmanaged C++ and C++/CLI,
  // or only C++/CLI) does only depend on whether the property is an empty
  // string or contains any value at all. In Visual Studio generators
  // this propval is prepended with /clr[:] which results in:
  //
  // 1. propval does not exist: no /clr flag, unmanaged target, has import
  //                            lib
  // 2. empty propval:          add /clr as flag, mixed unmanaged/managed
  //                            target, has import lib
  // 3. netcore propval:        add /clr:netcore as flag, mixed
  //                            unmanaged/managed target, has import lib.
  // 4. any value (safe,pure):  add /clr:[propval] as flag, target with
  //                            managed code only, no import lib
  if (propval.empty() || propval == "netcore") {
    return ManagedType::Mixed;
  }
  return ManagedType::Managed;
}

cmGeneratorTarget::ManagedType cmGeneratorTarget::GetManagedType(
  std::string const& config) const
{
  // Only libraries and executables can be managed targets.
  if (this->GetType() > cmStateEnums::SHARED_LIBRARY) {
    return ManagedType::Undefined;
  }

  if (this->GetType() == cmStateEnums::STATIC_LIBRARY) {
    return ManagedType::Native;
  }

  // Check imported target.
  if (this->IsImported()) {
    if (cmGeneratorTarget::ImportInfo const* info =
          this->GetImportInfo(config)) {
      return info->Managed;
    }
    return ManagedType::Undefined;
  }

  // Check for explicitly set clr target property.
  if (cmValue clr = this->GetProperty("COMMON_LANGUAGE_RUNTIME")) {
    return this->CheckManagedType(*clr);
  }

  // C# targets are always managed. This language specific check
  // is added to avoid that the COMMON_LANGUAGE_RUNTIME target property
  // has to be set manually for C# targets.
  return this->IsCSharpOnly() ? ManagedType::Managed : ManagedType::Native;
}

bool cmGeneratorTarget::AddHeaderSetVerification()
{
  if (!this->GetPropertyAsBool("VERIFY_INTERFACE_HEADER_SETS")) {
    return true;
  }

  if (this->GetType() != cmStateEnums::STATIC_LIBRARY &&
      this->GetType() != cmStateEnums::SHARED_LIBRARY &&
      this->GetType() != cmStateEnums::UNKNOWN_LIBRARY &&
      this->GetType() != cmStateEnums::OBJECT_LIBRARY &&
      this->GetType() != cmStateEnums::INTERFACE_LIBRARY &&
      !this->IsExecutableWithExports()) {
    return true;
  }

  auto verifyValue = this->GetProperty("INTERFACE_HEADER_SETS_TO_VERIFY");
  bool const all = verifyValue.IsEmpty();
  std::set<std::string> verifySet;
  if (!all) {
    cmList verifyList{ verifyValue };
    verifySet.insert(verifyList.begin(), verifyList.end());
  }

  cmTarget* verifyTarget = nullptr;
  cmTarget* allVerifyTarget =
    this->GlobalGenerator->GetMakefiles().front()->FindTargetToUse(
      "all_verify_interface_header_sets",
      { cmStateEnums::TargetDomain::NATIVE });

  auto interfaceFileSetEntries = this->Target->GetInterfaceHeaderSetsEntries();

  std::set<cmFileSet*> fileSets;
  for (auto const& entry : interfaceFileSetEntries) {
    for (auto const& name : cmList{ entry.Value }) {
      if (all || verifySet.count(name)) {
        fileSets.insert(this->Target->GetFileSet(name));
        verifySet.erase(name);
      }
    }
  }
  if (!verifySet.empty()) {
    this->Makefile->IssueMessage(
      MessageType::FATAL_ERROR,
      cmStrCat("Property INTERFACE_HEADER_SETS_TO_VERIFY of target \"",
               this->GetName(),
               "\" contained the following header sets that are nonexistent "
               "or not INTERFACE:\n  ",
               cmJoin(verifySet, "\n  ")));
    return false;
  }

  cm::optional<std::set<std::string>> languages;
  for (auto* fileSet : fileSets) {
    auto dirCges = fileSet->CompileDirectoryEntries();
    auto fileCges = fileSet->CompileFileEntries();

    static auto const contextSensitive =
      [](std::unique_ptr<cmCompiledGeneratorExpression> const& cge) {
        return cge->GetHadContextSensitiveCondition();
      };
    bool dirCgesContextSensitive = false;
    bool fileCgesContextSensitive = false;

    std::vector<std::string> dirs;
    std::map<std::string, std::vector<std::string>> filesPerDir;
    bool first = true;
    for (auto const& config : this->Makefile->GetGeneratorConfigs(
           cmMakefile::GeneratorConfigQuery::IncludeEmptyConfig)) {
      if (first || dirCgesContextSensitive) {
        dirs = fileSet->EvaluateDirectoryEntries(dirCges, this->LocalGenerator,
                                                 config, this);
        dirCgesContextSensitive =
          std::any_of(dirCges.begin(), dirCges.end(), contextSensitive);
      }
      if (first || fileCgesContextSensitive) {
        filesPerDir.clear();
        for (auto const& fileCge : fileCges) {
          fileSet->EvaluateFileEntry(dirs, filesPerDir, fileCge,
                                     this->LocalGenerator, config, this);
          if (fileCge->GetHadContextSensitiveCondition()) {
            fileCgesContextSensitive = true;
          }
        }
      }

      for (auto const& files : filesPerDir) {
        for (auto const& file : files.second) {
          std::string filename = this->GenerateHeaderSetVerificationFile(
            *this->Makefile->GetOrCreateSource(file), files.first, languages);
          if (filename.empty()) {
            continue;
          }

          if (!verifyTarget) {
            {
              cmMakefile::PolicyPushPop polScope(this->Makefile);
              this->Makefile->SetPolicy(cmPolicies::CMP0119, cmPolicies::NEW);
              verifyTarget = this->Makefile->AddLibrary(
                cmStrCat(this->GetName(), "_verify_interface_header_sets"),
                cmStateEnums::OBJECT_LIBRARY, {}, true);
            }

            verifyTarget->AddLinkLibrary(
              *this->Makefile, this->GetName(),
              cmTargetLinkLibraryType::GENERAL_LibraryType);
            verifyTarget->SetProperty("AUTOMOC", "OFF");
            verifyTarget->SetProperty("AUTORCC", "OFF");
            verifyTarget->SetProperty("AUTOUIC", "OFF");
            verifyTarget->SetProperty("DISABLE_PRECOMPILE_HEADERS", "ON");
            verifyTarget->SetProperty("UNITY_BUILD", "OFF");
            verifyTarget->SetProperty("CXX_SCAN_FOR_MODULES", "OFF");
            verifyTarget->FinalizeTargetConfiguration(
              this->Makefile->GetCompileDefinitionsEntries());

            if (!allVerifyTarget) {
              allVerifyTarget = this->GlobalGenerator->GetMakefiles()
                                  .front()
                                  ->AddNewUtilityTarget(
                                    "all_verify_interface_header_sets", true);
            }

            allVerifyTarget->AddUtility(verifyTarget->GetName(), false);
          }

          if (fileCgesContextSensitive) {
            filename = cmStrCat("$<$<CONFIG:", config, ">:", filename, '>');
          }
          verifyTarget->AddSource(filename);
        }
      }

      if (!dirCgesContextSensitive && !fileCgesContextSensitive) {
        break;
      }
      first = false;
    }
  }

  if (verifyTarget) {
    this->LocalGenerator->AddGeneratorTarget(
      cm::make_unique<cmGeneratorTarget>(verifyTarget, this->LocalGenerator));
  }

  return true;
}

std::string cmGeneratorTarget::GenerateHeaderSetVerificationFile(
  cmSourceFile& source, std::string const& dir,
  cm::optional<std::set<std::string>>& languages) const
{
  std::string extension;
  std::string language = source.GetOrDetermineLanguage();

  if (source.GetPropertyAsBool("SKIP_LINTING")) {
    return std::string{};
  }

  if (language.empty()) {
    if (!languages) {
      languages.emplace();
      for (auto const& tgtSource : this->GetAllConfigSources()) {
        auto const& tgtSourceLanguage =
          tgtSource.Source->GetOrDetermineLanguage();
        if (tgtSourceLanguage == "CXX") {
          languages->insert("CXX");
          break; // C++ overrides everything else, so we don't need to keep
                 // checking.
        }
        if (tgtSourceLanguage == "C") {
          languages->insert("C");
        }
      }

      if (languages->empty()) {
        std::vector<std::string> languagesVector;
        this->GlobalGenerator->GetEnabledLanguages(languagesVector);
        languages->insert(languagesVector.begin(), languagesVector.end());
      }
    }

    if (languages->count("CXX")) {
      language = "CXX";
    } else if (languages->count("C")) {
      language = "C";
    }
  }

  if (language == "C") {
    extension = ".c";
  } else if (language == "CXX") {
    extension = ".cxx";
  } else {
    return "";
  }

  std::string headerFilename = dir;
  if (!headerFilename.empty()) {
    headerFilename += '/';
  }
  headerFilename += source.GetLocation().GetName();

  auto filename = cmStrCat(
    this->LocalGenerator->GetCurrentBinaryDirectory(), '/', this->GetName(),
    "_verify_interface_header_sets/", headerFilename, extension);
  auto* verificationSource = this->Makefile->GetOrCreateSource(filename);
  verificationSource->SetProperty("LANGUAGE", language);

  cmSystemTools::MakeDirectory(cmSystemTools::GetFilenamePath(filename));

  cmGeneratedFileStream fout(filename);
  fout.SetCopyIfDifferent(true);
  // The IWYU "associated" pragma tells include-what-you-use to
  // consider the headerFile as part of the entire language
  // unit within include-what-you-use and as a result allows
  // one to get IWYU advice for headers.
  // Also suppress clang-tidy include checks in generated code.
  fout
    << "/* NOLINTNEXTLINE(misc-header-include-cycle,misc-include-cleaner) */\n"
    << "#include <" << headerFilename << "> /* IWYU pragma: associated */\n";
  fout.close();

  return filename;
}

std::string cmGeneratorTarget::GetImportedXcFrameworkPath(
  std::string const& config) const
{
  if (!(this->IsApple() && this->IsImported() &&
        (this->GetType() == cmStateEnums::SHARED_LIBRARY ||
         this->GetType() == cmStateEnums::STATIC_LIBRARY ||
         this->GetType() == cmStateEnums::UNKNOWN_LIBRARY))) {
    return {};
  }

  std::string desiredConfig = config;
  if (config.empty()) {
    desiredConfig = "NOCONFIG";
  }

  std::string result;

  cmValue loc = nullptr;
  cmValue imp = nullptr;
  std::string suffix;

  if (this->Target->GetMappedConfig(desiredConfig, loc, imp, suffix)) {
    if (loc) {
      result = *loc;
    } else {
      std::string impProp = cmStrCat("IMPORTED_LOCATION", suffix);
      if (cmValue configLocation = this->GetProperty(impProp)) {
        result = *configLocation;
      } else if (cmValue location = this->GetProperty("IMPORTED_LOCATION")) {
        result = *location;
      }
    }

    if (cmSystemTools::IsPathToXcFramework(result)) {
      return result;
    }
  }

  return {};
}

bool cmGeneratorTarget::HaveFortranSources(std::string const& config) const
{
  auto sources = this->GetSourceFiles(config);
  bool const have_direct = std::any_of(
    sources.begin(), sources.end(), [](BT<cmSourceFile*> const& sf) -> bool {
      return sf.Value->GetLanguage() == "Fortran"_s;
    });
  bool have_via_target_objects = false;
  if (!have_direct) {
    auto const sourceObjectLibraries = this->GetSourceObjectLibraries(config);
    have_via_target_objects =
      std::any_of(sourceObjectLibraries.begin(), sourceObjectLibraries.end(),
                  [&config](cmGeneratorTarget const* tgt) -> bool {
                    return tgt->HaveFortranSources(config);
                  });
  }
  return have_direct || have_via_target_objects;
}

bool cmGeneratorTarget::HaveFortranSources() const
{
  auto sources = this->GetAllConfigSources();
  bool const have_direct = std::any_of(
    sources.begin(), sources.end(), [](AllConfigSource const& sf) -> bool {
      return sf.Source->GetLanguage() == "Fortran"_s;
    });
  bool have_via_target_objects = false;
  if (!have_direct) {
    std::vector<std::string> configs =
      this->Makefile->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig);
    for (auto const& config : configs) {
      auto const sourceObjectLibraries =
        this->GetSourceObjectLibraries(config);
      have_via_target_objects =
        std::any_of(sourceObjectLibraries.begin(), sourceObjectLibraries.end(),
                    [&config](cmGeneratorTarget const* tgt) -> bool {
                      return tgt->HaveFortranSources(config);
                    });
      if (have_via_target_objects) {
        break;
      }
    }
  }
  return have_direct || have_via_target_objects;
}

bool cmGeneratorTarget::HaveCxx20ModuleSources(std::string* errorMessage) const
{
  auto const& fs_names = this->Target->GetAllFileSetNames();
  return std::any_of(
    fs_names.begin(), fs_names.end(),
    [this, errorMessage](std::string const& name) -> bool {
      auto const* file_set = this->Target->GetFileSet(name);
      if (!file_set) {
        auto message = cmStrCat("Target \"", this->Target->GetName(),
                                "\" is tracked to have file set \"", name,
                                "\", but it was not found.");
        if (errorMessage) {
          *errorMessage = std::move(message);
        } else {
          this->Makefile->IssueMessage(MessageType::INTERNAL_ERROR, message);
        }
        return false;
      }

      auto const& fs_type = file_set->GetType();
      return fs_type == "CXX_MODULES"_s;
    });
}

cmGeneratorTarget::Cxx20SupportLevel cmGeneratorTarget::HaveCxxModuleSupport(
  std::string const& config) const
{
  auto const* state = this->Makefile->GetState();
  if (!state->GetLanguageEnabled("CXX")) {
    return Cxx20SupportLevel::MissingCxx;
  }

  cmValue standardDefault =
    this->Makefile->GetDefinition("CMAKE_CXX_STANDARD_DEFAULT");
  if (!standardDefault || standardDefault->empty()) {
    // We do not know any meaningful C++ standard levels for this compiler.
    return Cxx20SupportLevel::NoCxx20;
  }

  cmStandardLevelResolver standardResolver(this->Makefile);
  cmStandardLevel const cxxStd20 =
    *standardResolver.LanguageStandardLevel("CXX", "20");
  cm::optional<cmStandardLevel> explicitLevel =
    this->GetExplicitStandardLevel("CXX", config);
  if (!explicitLevel || *explicitLevel < cxxStd20) {
    return Cxx20SupportLevel::NoCxx20;
  }

  cmValue scandepRule =
    this->Makefile->GetDefinition("CMAKE_CXX_SCANDEP_SOURCE");
  if (!scandepRule) {
    return Cxx20SupportLevel::MissingRule;
  }
  return Cxx20SupportLevel::Supported;
}

void cmGeneratorTarget::CheckCxxModuleStatus(std::string const& config) const
{
  bool haveScannableSources = false;

  // Check for `CXX_MODULE*` file sets and a lack of support.
  if (this->HaveCxx20ModuleSources()) {
    haveScannableSources = true;
  }

  if (!haveScannableSources) {
    // Check to see if there are regular sources that have requested scanning.
    auto sources = this->GetSourceFiles(config);
    for (auto const& source : sources) {
      auto const* sf = source.Value;
      auto const& lang = sf->GetLanguage();
      if (lang != "CXX"_s) {
        continue;
      }
      // Ignore sources which do not need dyndep.
      if (this->NeedDyndepForSource(lang, config, sf)) {
        haveScannableSources = true;
      }
    }
  }

  // If there isn't anything scannable, ignore it.
  if (!haveScannableSources) {
    return;
  }

  // If the generator doesn't support modules at all, error that we have
  // sources that require the support.
  if (!this->GetGlobalGenerator()->CheckCxxModuleSupport(
        cmGlobalGenerator::CxxModuleSupportQuery::Expected)) {
    this->Makefile->IssueMessage(
      MessageType::FATAL_ERROR,
      cmStrCat("The target named \"", this->GetName(),
               "\" has C++ sources that may use modules, but modules are not "
               "supported by this generator:\n  ",
               this->GetGlobalGenerator()->GetName(),
               "\n"
               "Modules are supported only by Ninja, Ninja Multi-Config, "
               "and Visual Studio generators for VS 17.4 and newer.  "
               "See the cmake-cxxmodules(7) manual for details.  "
               "Use the CMAKE_CXX_SCAN_FOR_MODULES variable to enable or "
               "disable scanning."));
    return;
  }

  switch (this->HaveCxxModuleSupport(config)) {
    case cmGeneratorTarget::Cxx20SupportLevel::MissingCxx:
      this->Makefile->IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat("The target named \"", this->GetName(),
                 "\" has C++ sources that use modules, but the \"CXX\" "
                 "language has not been enabled."));
      break;
    case cmGeneratorTarget::Cxx20SupportLevel::NoCxx20: {
      cmStandardLevelResolver standardResolver(this->Makefile);
      auto effStandard =
        standardResolver.GetEffectiveStandard(this, "CXX", config);
      if (effStandard.empty()) {
        effStandard = "; no C++ standard found";
      } else {
        effStandard = cmStrCat("; found \"cxx_std_", effStandard, '"');
      }
      this->Makefile->IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat(
          "The target named \"", this->GetName(),
          "\" has C++ sources that use modules, but does not include "
          "\"cxx_std_20\" (or newer) among its `target_compile_features`",
          effStandard, '.'));
    } break;
    case cmGeneratorTarget::Cxx20SupportLevel::MissingRule: {
      this->Makefile->IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat("The target named \"", this->GetName(),
                 "\" has C++ sources that may use modules, but the compiler "
                 "does not provide a way to discover the import graph "
                 "dependencies.  See the cmake-cxxmodules(7) manual for "
                 "details.  Use the CMAKE_CXX_SCAN_FOR_MODULES variable to "
                 "enable or disable scanning."));
    } break;
    case cmGeneratorTarget::Cxx20SupportLevel::Supported:
      // All is well.
      break;
  }
}

bool cmGeneratorTarget::NeedCxxModuleSupport(std::string const& lang,
                                             std::string const& config) const
{
  if (lang != "CXX"_s) {
    return false;
  }
  return this->HaveCxxModuleSupport(config) == Cxx20SupportLevel::Supported &&
    this->GetGlobalGenerator()->CheckCxxModuleSupport(
      cmGlobalGenerator::CxxModuleSupportQuery::Inspect);
}

bool cmGeneratorTarget::NeedDyndep(std::string const& lang,
                                   std::string const& config) const
{
  return lang == "Fortran"_s || this->NeedCxxModuleSupport(lang, config);
}

cmFileSet const* cmGeneratorTarget::GetFileSetForSource(
  std::string const& config, cmSourceFile const* sf) const
{
  this->BuildFileSetInfoCache(config);

  auto const& path = sf->GetFullPath();
  auto const& per_config = this->Configs[config];

  auto const fsit = per_config.FileSetCache.find(path);
  if (fsit == per_config.FileSetCache.end()) {
    return nullptr;
  }
  return fsit->second;
}

bool cmGeneratorTarget::NeedDyndepForSource(std::string const& lang,
                                            std::string const& config,
                                            cmSourceFile const* sf) const
{
  // Fortran always needs to be scanned.
  if (lang == "Fortran"_s) {
    return true;
  }
  // Only C++ code needs scanned otherwise.
  if (lang != "CXX"_s) {
    return false;
  }

  // Any file in `CXX_MODULES` file sets need scanned (it being `CXX` is
  // enforced elsewhere).
  auto const* fs = this->GetFileSetForSource(config, sf);
  if (fs && fs->GetType() == "CXX_MODULES"_s) {
    return true;
  }

  auto targetDyndep = this->NeedCxxDyndep(config);
  if (targetDyndep == CxxModuleSupport::Unavailable) {
    return false;
  }
  auto const sfProp = sf->GetProperty("CXX_SCAN_FOR_MODULES");
  if (sfProp.IsSet()) {
    return sfProp.IsOn();
  }
  return targetDyndep == CxxModuleSupport::Enabled;
}

cmGeneratorTarget::CxxModuleSupport cmGeneratorTarget::NeedCxxDyndep(
  std::string const& config) const
{
  bool haveRule = false;
  switch (this->HaveCxxModuleSupport(config)) {
    case Cxx20SupportLevel::MissingCxx:
    case Cxx20SupportLevel::NoCxx20:
      return CxxModuleSupport::Unavailable;
    case Cxx20SupportLevel::MissingRule:
      break;
    case Cxx20SupportLevel::Supported:
      haveRule = true;
      break;
  }
  bool haveGeneratorSupport =
    this->GetGlobalGenerator()->CheckCxxModuleSupport(
      cmGlobalGenerator::CxxModuleSupportQuery::Inspect);
  auto const tgtProp = this->GetProperty("CXX_SCAN_FOR_MODULES");
  if (tgtProp.IsSet()) {
    return tgtProp.IsOn() ? CxxModuleSupport::Enabled
                          : CxxModuleSupport::Disabled;
  }

  CxxModuleSupport policyAnswer = CxxModuleSupport::Unavailable;
  switch (this->GetPolicyStatusCMP0155()) {
    case cmPolicies::WARN:
    case cmPolicies::OLD:
      // The OLD behavior is to not scan the source.
      policyAnswer = CxxModuleSupport::Disabled;
      break;
    case cmPolicies::NEW:
      // The NEW behavior is to scan the source if the compiler supports
      // scanning and the generator supports it.
      if (haveRule && haveGeneratorSupport) {
        policyAnswer = CxxModuleSupport::Enabled;
      } else {
        policyAnswer = CxxModuleSupport::Disabled;
      }
      break;
  }
  return policyAnswer;
}

std::string cmGeneratorTarget::BuildDatabasePath(
  std::string const& lang, std::string const& config) const
{
  // Check to see if the target wants it.
  if (!this->GetPropertyAsBool("EXPORT_BUILD_DATABASE")) {
    return {};
  }
  if (!cmExperimental::HasSupportEnabled(
        *this->Makefile, cmExperimental::Feature::ExportBuildDatabase)) {
    return {};
  }
  // Check to see if the generator supports it.
  if (!this->GetGlobalGenerator()->SupportsBuildDatabase()) {
    return {};
  }

  if (this->GetGlobalGenerator()->IsMultiConfig()) {
    return cmStrCat(this->GetSupportDirectory(), '/', config, '/', lang,
                    "_build_database.json");
  }

  return cmStrCat(this->GetSupportDirectory(), '/', lang,
                  "_build_database.json");
}

void cmGeneratorTarget::BuildFileSetInfoCache(std::string const& config) const
{
  auto& per_config = this->Configs[config];

  if (per_config.BuiltFileSetCache) {
    return;
  }

  auto const* tgt = this->Target;

  for (auto const& name : tgt->GetAllFileSetNames()) {
    auto const* file_set = tgt->GetFileSet(name);
    if (!file_set) {
      tgt->GetMakefile()->IssueMessage(
        MessageType::INTERNAL_ERROR,
        cmStrCat("Target \"", tgt->GetName(),
                 "\" is tracked to have file set \"", name,
                 "\", but it was not found."));
      continue;
    }

    auto fileEntries = file_set->CompileFileEntries();
    auto directoryEntries = file_set->CompileDirectoryEntries();
    auto directories = file_set->EvaluateDirectoryEntries(
      directoryEntries, this->LocalGenerator, config, this);

    std::map<std::string, std::vector<std::string>> files;
    for (auto const& entry : fileEntries) {
      file_set->EvaluateFileEntry(directories, files, entry,
                                  this->LocalGenerator, config, this);
    }

    for (auto const& it : files) {
      for (auto const& filename : it.second) {
        auto collapsedFile = cmSystemTools::CollapseFullPath(filename);
        per_config.FileSetCache[collapsedFile] = file_set;
      }
    }
  }

  per_config.BuiltFileSetCache = true;
}

std::string cmGeneratorTarget::GetSwiftModuleName() const
{
  return this->GetPropertyOrDefault("Swift_MODULE_NAME", this->GetName());
}

std::string cmGeneratorTarget::GetSwiftModuleFileName() const
{
  std::string moduleFilename = this->GetPropertyOrDefault(
    "Swift_MODULE", this->GetSwiftModuleName() + ".swiftmodule");
  if (this->GetPolicyStatusCMP0195() == cmPolicies::NEW) {
    if (cmValue moduleTriple =
          this->Makefile->GetDefinition("CMAKE_Swift_MODULE_TRIPLE")) {
      moduleFilename += "/" + *moduleTriple + ".swiftmodule";
    }
  }
  return moduleFilename;
}

std::string cmGeneratorTarget::GetSwiftModuleDirectory(
  std::string const& config) const
{
  // This is like the *_OUTPUT_DIRECTORY properties except that we don't have a
  // separate per-configuration target property.
  //
  // The property expands generator expressions. Multi-config generators append
  // a per-configuration subdirectory to the specified directory unless a
  // generator expression is used.
  bool appendConfigDir = true;
  std::string moduleDirectory;

  if (cmValue value = this->GetProperty("Swift_MODULE_DIRECTORY")) {
    moduleDirectory = cmGeneratorExpression::Evaluate(
      *value, this->LocalGenerator, config, this);
    appendConfigDir = *value == moduleDirectory;
  }
  if (moduleDirectory.empty()) {
    moduleDirectory = this->LocalGenerator->GetCurrentBinaryDirectory();
  }
  if (appendConfigDir) {
    this->LocalGenerator->GetGlobalGenerator()->AppendDirectoryForConfig(
      "/", config, "", moduleDirectory);
  }
  return moduleDirectory;
}

std::string cmGeneratorTarget::GetSwiftModulePath(
  std::string const& config) const
{
  return this->GetSwiftModuleDirectory(config) + "/" +
    this->GetSwiftModuleFileName();
}

std::string cmGeneratorTarget::GetPropertyOrDefault(
  std::string const& property, std::string defaultValue) const
{
  if (cmValue name = this->GetProperty(property)) {
    return *name;
  }
  return defaultValue;
}
