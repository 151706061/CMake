/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmLocalCommonGenerator.h"

#include <memory>
#include <utility>
#include <vector>

#include "cmGeneratorTarget.h"
#include "cmGlobalGenerator.h"
#include "cmMakefile.h"
#include "cmOutputConverter.h"
#include "cmStateDirectory.h"
#include "cmStateSnapshot.h"
#include "cmStringAlgorithms.h"
#include "cmValue.h"

cmLocalCommonGenerator::cmLocalCommonGenerator(cmGlobalGenerator* gg,
                                               cmMakefile* mf)
  : cmLocalGenerator(gg, mf)
{
  // Multi-config generators define one set of configurations at the top.
  // Single-config generators nominally define one configuration at the top,
  // but the implementation has never been strict about that, so look up the
  // per-directory config to preserve behavior.
  this->ConfigNames = (gg->IsMultiConfig() && !gg->GetMakefiles().empty()
                         ? gg->GetMakefiles().front().get()
                         : this->Makefile)
                        ->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig);
}

cmLocalCommonGenerator::~cmLocalCommonGenerator() = default;

std::string const& cmLocalCommonGenerator::GetWorkingDirectory() const
{
  return this->StateSnapshot.GetDirectory().GetCurrentBinary();
}

std::string cmLocalCommonGenerator::GetTargetFortranFlags(
  cmGeneratorTarget const* target, std::string const& config)
{
  std::string flags;

  // Enable module output if necessary.
  this->AppendFlags(
    flags, this->Makefile->GetSafeDefinition("CMAKE_Fortran_MODOUT_FLAG"));

  // Add a module output directory flag if necessary.
  std::string mod_dir =
    target->GetFortranModuleDirectory(this->GetWorkingDirectory());
  if (!mod_dir.empty()) {
    mod_dir = this->ConvertToOutputFormat(
      this->MaybeRelativeToWorkDir(mod_dir), cmOutputConverter::SHELL);
  } else {
    mod_dir =
      this->Makefile->GetSafeDefinition("CMAKE_Fortran_MODDIR_DEFAULT");
  }
  if (!mod_dir.empty()) {
    std::string modflag = cmStrCat(
      this->Makefile->GetRequiredDefinition("CMAKE_Fortran_MODDIR_FLAG"),
      mod_dir);
    this->AppendFlags(flags, modflag);
    // Some compilers do not search their own module output directory
    // for using other modules.  Add an include directory explicitly
    // for consistency with compilers that do search it.
    std::string incflag =
      this->Makefile->GetSafeDefinition("CMAKE_Fortran_MODDIR_INCLUDE_FLAG");
    if (!incflag.empty()) {
      incflag = cmStrCat(incflag, mod_dir);
      this->AppendFlags(flags, incflag);
    }
  }

  // If there is a separate module path flag then duplicate the
  // include path with it.  This compiler does not search the include
  // path for modules.
  if (cmValue modpath_flag =
        this->Makefile->GetDefinition("CMAKE_Fortran_MODPATH_FLAG")) {
    std::vector<std::string> includes;
    this->GetIncludeDirectories(includes, target, "C", config);
    for (std::string const& id : includes) {
      std::string flg =
        cmStrCat(*modpath_flag,
                 this->ConvertToOutputFormat(id, cmOutputConverter::SHELL));
      this->AppendFlags(flags, flg);
    }
  }

  return flags;
}

std::string cmLocalCommonGenerator::ComputeLongTargetDirectory(
  cmGeneratorTarget const* target) const
{
  std::string dir = target->GetName();
#if defined(__VMS)
  dir += "_dir";
#else
  dir += ".dir";
#endif
  return dir;
}

std::string cmLocalCommonGenerator::GetTargetDirectory(
  cmGeneratorTarget const* target) const
{
  if (target->GetUseShortObjectNames()) {
    return this->ComputeShortTargetDirectory(target);
  }
  return this->ComputeLongTargetDirectory(target);
}

void cmLocalCommonGenerator::ComputeObjectFilenames(
  std::map<cmSourceFile const*, std::string>& mapping,
  cmGeneratorTarget const* gt)
{
  // Determine if these object files should use a custom extension
  char const* custom_ext = gt->GetCustomObjectExtension();
  for (auto& si : mapping) {
    cmSourceFile const* sf = si.first;
    bool keptSourceExtension;
    si.second = this->GetObjectFileNameWithoutTarget(
      *sf, gt->ObjectDirectory, &keptSourceExtension, custom_ext);
  }
}
