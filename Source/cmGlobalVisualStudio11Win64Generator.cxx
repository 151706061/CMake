/*============================================================================
  CMake - Cross Platform Makefile Generator
  Copyright 2000-2011 Kitware, Inc., Insight Software Consortium

  Distributed under the OSI-approved BSD License (the "License");
  see accompanying file Copyright.txt for details.

  This software is distributed WITHOUT ANY WARRANTY; without even the
  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the License for more information.
============================================================================*/
#include "cmGlobalVisualStudio11Win64Generator.h"
#include "cmMakefile.h"
#include "cmake.h"

//----------------------------------------------------------------------------
cmGlobalVisualStudio11Win64Generator::cmGlobalVisualStudio11Win64Generator()
{
  this->ArchitectureId = "x64";
  this->AdditionalPlatformDefinition = "CMAKE_FORCE_WIN64";
}

//----------------------------------------------------------------------------
void cmGlobalVisualStudio11Win64Generator
::GetDocumentation(cmDocumentationEntry& entry)
{
  entry.Name = cmGlobalVisualStudio11Win64Generator::GetActualName();
  entry.Brief = "Generates Visual Studio 11 Win64 project files.";
  entry.Full = "";
}
