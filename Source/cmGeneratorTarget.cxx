/*============================================================================
  CMake - Cross Platform Makefile Generator
  Copyright 2000-2012 Kitware, Inc., Insight Software Consortium

  Distributed under the OSI-approved BSD License (the "License");
  see accompanying file Copyright.txt for details.

  This software is distributed WITHOUT ANY WARRANTY; without even the
  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the License for more information.
============================================================================*/
#include "cmGeneratorTarget.h"

#include "cmTarget.h"
#include "cmMakefile.h"
#include "cmLocalGenerator.h"
#include "cmComputeLinkInformation.h"
#include "cmGlobalGenerator.h"
#include "cmSourceFile.h"

#include <assert.h>

//----------------------------------------------------------------------------
cmGeneratorTarget::cmGeneratorTarget(cmTarget* t): Target(t)
{
  this->Makefile = this->Target->GetMakefile();
  this->LocalGenerator = this->Makefile->GetLocalGenerator();
  this->GlobalGenerator = this->LocalGenerator->GetGlobalGenerator();
  this->ClassifySources();
  this->LookupObjectLibraries();
}

cmGeneratorTarget::~cmGeneratorTarget()
{
  for(std::map<cmStdString, cmComputeLinkInformation*>::iterator i
                  = LinkInformation.begin(); i != LinkInformation.end(); ++i)
    {
    delete i->second;
    }
}

//----------------------------------------------------------------------------
int cmGeneratorTarget::GetType() const
{
  return this->Target->GetType();
}

//----------------------------------------------------------------------------
const char *cmGeneratorTarget::GetName() const
{
  return this->Target->GetName();
}

//----------------------------------------------------------------------------
const char *cmGeneratorTarget::GetProperty(const char *prop)
{
  return this->Target->GetProperty(prop);
}

//----------------------------------------------------------------------------
bool cmGeneratorTarget::GetPropertyAsBool(const char *prop)
{
  return this->Target->GetPropertyAsBool(prop);
}

//----------------------------------------------------------------------------
std::vector<cmSourceFile*> const& cmGeneratorTarget::GetSourceFiles()
{
  return this->Target->GetSourceFiles();
}

//----------------------------------------------------------------------------
void cmGeneratorTarget::ClassifySources()
{
  cmsys::RegularExpression header(CM_HEADER_REGEX);
  bool isObjLib = this->Target->GetType() == cmTarget::OBJECT_LIBRARY;
  std::vector<cmSourceFile*> badObjLib;
  std::vector<cmSourceFile*> const& sources = this->Target->GetSourceFiles();
  for(std::vector<cmSourceFile*>::const_iterator si = sources.begin();
      si != sources.end(); ++si)
    {
    cmSourceFile* sf = *si;
    std::string ext = cmSystemTools::LowerCase(sf->GetExtension());
    if(sf->GetCustomCommand())
      {
      this->CustomCommands.push_back(sf);
      }
    else if(sf->GetPropertyAsBool("HEADER_FILE_ONLY"))
      {
      this->HeaderSources.push_back(sf);
      }
    else if(sf->GetPropertyAsBool("EXTERNAL_OBJECT"))
      {
      this->ExternalObjects.push_back(sf);
      if(isObjLib) { badObjLib.push_back(sf); }
      }
    else if(sf->GetLanguage())
      {
      this->ObjectSources.push_back(sf);
      }
    else if(ext == "def")
      {
      this->ModuleDefinitionFile = sf->GetFullPath();
      if(isObjLib) { badObjLib.push_back(sf); }
      }
    else if(ext == "idl")
      {
      this->IDLSources.push_back(sf);
      if(isObjLib) { badObjLib.push_back(sf); }
      }
    else if(header.find(sf->GetFullPath().c_str()))
      {
      this->HeaderSources.push_back(sf);
      }
    else if(this->GlobalGenerator->IgnoreFile(sf->GetExtension().c_str()))
      {
      // We only get here if a source file is not an external object
      // and has an extension that is listed as an ignored file type.
      // No message or diagnosis should be given.
      this->ExtraSources.push_back(sf);
      }
    else
      {
      this->ExtraSources.push_back(sf);
      if(isObjLib && ext != "txt")
        {
        badObjLib.push_back(sf);
        }
      }
    }

  if(!badObjLib.empty())
    {
    cmOStringStream e;
    e << "OBJECT library \"" << this->Target->GetName() << "\" contains:\n";
    for(std::vector<cmSourceFile*>::iterator i = badObjLib.begin();
        i != badObjLib.end(); ++i)
      {
      e << "  " << (*i)->GetLocation().GetName() << "\n";
      }
    e << "but may contain only headers and sources that compile.";
    this->GlobalGenerator->GetCMakeInstance()
      ->IssueMessage(cmake::FATAL_ERROR, e.str(),
                     this->Target->GetBacktrace());
    }
}

//----------------------------------------------------------------------------
void cmGeneratorTarget::LookupObjectLibraries()
{
  std::vector<std::string> const& objLibs =
    this->Target->GetObjectLibraries();
  for(std::vector<std::string>::const_iterator oli = objLibs.begin();
      oli != objLibs.end(); ++oli)
    {
    std::string const& objLibName = *oli;
    if(cmTarget* objLib = this->Makefile->FindTargetToUse(objLibName.c_str()))
      {
      if(objLib->GetType() == cmTarget::OBJECT_LIBRARY)
        {
        if(this->Target->GetType() != cmTarget::EXECUTABLE &&
           this->Target->GetType() != cmTarget::STATIC_LIBRARY &&
           this->Target->GetType() != cmTarget::SHARED_LIBRARY &&
           this->Target->GetType() != cmTarget::MODULE_LIBRARY)
          {
          this->GlobalGenerator->GetCMakeInstance()
            ->IssueMessage(cmake::FATAL_ERROR,
                           "Only executables and non-OBJECT libraries may "
                           "reference target objects.",
                           this->Target->GetBacktrace());
          return;
          }
        this->Target->AddUtility(objLib->GetName());
        this->ObjectLibraries.push_back(objLib);
        }
      else
        {
        cmOStringStream e;
        e << "Objects of target \"" << objLibName
          << "\" referenced but is not an OBJECT library.";
        this->GlobalGenerator->GetCMakeInstance()
          ->IssueMessage(cmake::FATAL_ERROR, e.str(),
                         this->Target->GetBacktrace());
        return;
        }
      }
    else
      {
      cmOStringStream e;
      e << "Objects of target \"" << objLibName
        << "\" referenced but no such target exists.";
      this->GlobalGenerator->GetCMakeInstance()
        ->IssueMessage(cmake::FATAL_ERROR, e.str(),
                       this->Target->GetBacktrace());
      return;
      }
    }
}

//----------------------------------------------------------------------------
void cmGeneratorTarget::UseObjectLibraries(std::vector<std::string>& objs)
{
  for(std::vector<cmTarget*>::const_iterator
        ti = this->ObjectLibraries.begin();
      ti != this->ObjectLibraries.end(); ++ti)
    {
    cmTarget* objLib = *ti;
    cmGeneratorTarget* ogt =
      this->GlobalGenerator->GetGeneratorTarget(objLib);
    for(std::vector<cmSourceFile*>::const_iterator
          si = ogt->ObjectSources.begin();
        si != ogt->ObjectSources.end(); ++si)
      {
      std::string obj = ogt->ObjectDirectory;
      obj += ogt->Objects[*si];
      objs.push_back(obj);
      }
    }
}

//----------------------------------------------------------------------------
void cmGeneratorTarget::GenerateTargetManifest(const char* config)
{
  cmMakefile* mf = this->Target->GetMakefile();
  cmLocalGenerator* lg = mf->GetLocalGenerator();
  cmGlobalGenerator* gg = lg->GetGlobalGenerator();

  // Get the names.
  std::string name;
  std::string soName;
  std::string realName;
  std::string impName;
  std::string pdbName;
  if(this->GetType() == cmTarget::EXECUTABLE)
    {
    this->Target->GetExecutableNames(name, realName, impName, pdbName,
                                     config);
    }
  else if(this->GetType() == cmTarget::STATIC_LIBRARY ||
          this->GetType() == cmTarget::SHARED_LIBRARY ||
          this->GetType() == cmTarget::MODULE_LIBRARY)
    {
    this->Target->GetLibraryNames(name, soName, realName, impName, pdbName,
                                  config);
    }
  else
    {
    return;
    }

  // Get the directory.
  std::string dir = this->Target->GetDirectory(config, false);

  // Add each name.
  std::string f;
  if(!name.empty())
    {
    f = dir;
    f += "/";
    f += name;
    gg->AddToManifest(config? config:"", f);
    }
  if(!soName.empty())
    {
    f = dir;
    f += "/";
    f += soName;
    gg->AddToManifest(config? config:"", f);
    }
  if(!realName.empty())
    {
    f = dir;
    f += "/";
    f += realName;
    gg->AddToManifest(config? config:"", f);
    }
  if(!pdbName.empty())
    {
    f = dir;
    f += "/";
    f += pdbName;
    gg->AddToManifest(config? config:"", f);
    }
  if(!impName.empty())
    {
    f = this->Target->GetDirectory(config, true);
    f += "/";
    f += impName;
    gg->AddToManifest(config? config:"", f);
    }
}

//----------------------------------------------------------------------------
cmComputeLinkInformation*
cmGeneratorTarget::GetLinkInformation(const char* config)
{
  // Lookup any existing information for this configuration.
  std::map<cmStdString, cmComputeLinkInformation*>::iterator
    i = this->LinkInformation.find(config?config:"");
  if(i == this->LinkInformation.end())
    {
    // Compute information for this configuration.
    cmComputeLinkInformation* info =
      new cmComputeLinkInformation(this->Target, config);
    if(!info || !info->Compute())
      {
      delete info;
      info = 0;
      }

    // Store the information for this configuration.
    std::map<cmStdString, cmComputeLinkInformation*>::value_type
      entry(config?config:"", info);
    i = this->LinkInformation.insert(entry).first;
    }
  return i->second;
}

//----------------------------------------------------------------------------
void cmGeneratorTarget::GetAppleArchs(const char* config,
                             std::vector<std::string>& archVec)
{
  const char* archs = 0;
  if(config && *config)
    {
    std::string defVarName = "OSX_ARCHITECTURES_";
    defVarName += cmSystemTools::UpperCase(config);
    archs = this->Target->GetProperty(defVarName.c_str());
    }
  if(!archs)
    {
    archs = this->Target->GetProperty("OSX_ARCHITECTURES");
    }
  if(archs)
    {
    cmSystemTools::ExpandListArgument(std::string(archs), archVec);
    }
}

//----------------------------------------------------------------------------
const char* cmGeneratorTarget::GetCreateRuleVariable()
{
  switch(this->GetType())
    {
    case cmTarget::STATIC_LIBRARY:
      return "_CREATE_STATIC_LIBRARY";
    case cmTarget::SHARED_LIBRARY:
      return "_CREATE_SHARED_LIBRARY";
    case cmTarget::MODULE_LIBRARY:
      return "_CREATE_SHARED_MODULE";
    case cmTarget::EXECUTABLE:
      return "_LINK_EXECUTABLE";
    default:
      break;
    }
  return "";
}

//----------------------------------------------------------------------------
std::vector<std::string> cmGeneratorTarget::GetIncludeDirectories()
{
  std::vector<std::string> includes;
  const char *prop = this->Target->GetProperty("INCLUDE_DIRECTORIES");
  if(prop)
    {
    cmSystemTools::ExpandListArgument(prop, includes);
    }

  std::set<std::string> uniqueIncludes;
  std::vector<std::string> orderedAndUniqueIncludes;
  for(std::vector<std::string>::const_iterator
      li = includes.begin(); li != includes.end(); ++li)
    {
    if(uniqueIncludes.insert(*li).second)
      {
      orderedAndUniqueIncludes.push_back(*li);
      }
    }

  return orderedAndUniqueIncludes;
}

//----------------------------------------------------------------------------
const char *cmGeneratorTarget::GetCompileDefinitions(const char *config)
{
  if (!config)
    {
    return this->Target->GetProperty("COMPILE_DEFINITIONS");
    }
  std::string defPropName = "COMPILE_DEFINITIONS_";
  defPropName +=
    cmSystemTools::UpperCase(config);

  return this->Target->GetProperty(defPropName.c_str());
}
