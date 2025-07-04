/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmFindPathCommand.h"

#include <utility>

#include <cm/memory>

#include "cmsys/Glob.hxx"

#include "cmFindCommon.h"
#include "cmStateTypes.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"

class cmExecutionStatus;

cmFindPathCommand::cmFindPathCommand(std::string findCommandName,
                                     cmExecutionStatus& status)
  : cmFindBase(std::move(findCommandName), status)
{
  this->EnvironmentPath = "INCLUDE";
  this->IncludeFileInPath = false;
  this->VariableDocumentation = "Path to a file.";
  this->VariableType = cmStateEnums::PATH;
}
cmFindPathCommand::cmFindPathCommand(cmExecutionStatus& status)
  : cmFindPathCommand("find_path", status)
{
}

// cmFindPathCommand
bool cmFindPathCommand::InitialPass(std::vector<std::string> const& argsIn)
{
  this->CMakePathName = "INCLUDE";

  if (!this->ParseArguments(argsIn)) {
    return false;
  }

  this->FullDebugMode = this->ComputeIfDebugModeWanted(this->VariableName);
  if (this->FullDebugMode || !this->ComputeIfImplicitDebugModeSuppressed()) {
    this->DebugState = cm::make_unique<cmFindBaseDebugState>(this);
  }

  if (this->IsFound()) {
    this->NormalizeFindResult();
    return true;
  }

  std::string result = this->FindHeader();
  this->StoreFindResult(result);
  return true;
}

std::string cmFindPathCommand::FindHeader()
{
  std::string header;
  if (this->SearchFrameworkFirst || this->SearchFrameworkOnly) {
    header = this->FindFrameworkHeader();
  }
  if (header.empty() && !this->SearchFrameworkOnly) {
    header = this->FindNormalHeader();
  }
  if (header.empty() && this->SearchFrameworkLast) {
    header = this->FindFrameworkHeader();
  }

  return header;
}

std::string cmFindPathCommand::FindHeaderInFramework(
  std::string const& file, std::string const& dir) const
{
  std::string fileName = file;
  std::string frameWorkName;
  std::string::size_type pos = fileName.find('/');
  // if there is a / in the name try to find the header as a framework
  // For example bar/foo.h would look for:
  // bar.framework/Headers/foo.h
  if (pos != std::string::npos) {
    // remove the name from the slash;
    fileName = fileName.substr(pos + 1);
    frameWorkName = file;
    frameWorkName =
      frameWorkName.substr(0, frameWorkName.size() - fileName.size() - 1);
    // if the framework has a path in it then just use the filename
    if (frameWorkName.find('/') != std::string::npos) {
      fileName = file;
      frameWorkName.clear();
    }
    if (!frameWorkName.empty()) {
      std::string fpath = cmStrCat(dir, frameWorkName, ".framework");
      std::string intPath = cmStrCat(fpath, "/Headers/", fileName);
      if (cmSystemTools::FileExists(intPath) &&
          this->Validate(this->IncludeFileInPath ? intPath : fpath)) {
        if (this->DebugState) {
          this->DebugState->FoundAt(intPath);
        }
        if (this->IncludeFileInPath) {
          return intPath;
        }
        return fpath;
      }
      if (this->DebugState) {
        this->DebugState->FailedAt(intPath);
      }
    }
  }
  // if it is not found yet or not a framework header, then do a glob search
  // for all frameworks in the directory: dir/*.framework/Headers/<file>
  std::string glob = cmStrCat(dir, "*.framework/Headers/", file);
  cmsys::Glob globIt;
  globIt.FindFiles(glob);
  std::vector<std::string> files = globIt.GetFiles();
  if (!files.empty()) {
    std::string fheader = cmSystemTools::ToNormalizedPathOnDisk(files[0]);
    if (this->DebugState) {
      this->DebugState->FoundAt(fheader);
    }
    if (this->IncludeFileInPath) {
      return fheader;
    }
    fheader.resize(fheader.size() - file.size());
    return fheader;
  }

  // No frameworks matched the glob, so nothing more to add to debug.FailedAt()
  return "";
}

std::string cmFindPathCommand::FindNormalHeader()
{
  std::string tryPath;
  for (std::string const& n : this->Names) {
    for (std::string const& sp : this->SearchPaths) {
      tryPath = cmStrCat(sp, n);
      if (cmSystemTools::FileExists(tryPath) &&
          this->Validate(this->IncludeFileInPath ? tryPath : sp)) {
        if (this->DebugState) {
          this->DebugState->FoundAt(tryPath);
        }
        if (this->IncludeFileInPath) {
          return tryPath;
        }
        return sp;
      }
      if (this->DebugState) {
        this->DebugState->FailedAt(tryPath);
      }
    }
  }
  return "";
}

std::string cmFindPathCommand::FindFrameworkHeader()
{
  for (std::string const& n : this->Names) {
    for (std::string const& sp : this->SearchPaths) {
      std::string fwPath = this->FindHeaderInFramework(n, sp);
      if (!fwPath.empty()) {
        return fwPath;
      }
    }
  }
  return "";
}

bool cmFindPath(std::vector<std::string> const& args,
                cmExecutionStatus& status)
{
  return cmFindPathCommand(status).InitialPass(args);
}
