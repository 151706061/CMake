/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */

#include "cmBinUtilsLinuxELFGetRuntimeDependenciesTool.h"

#include "cmRuntimeDependencyArchive.h"

cmBinUtilsLinuxELFGetRuntimeDependenciesTool::
  cmBinUtilsLinuxELFGetRuntimeDependenciesTool(
    cmRuntimeDependencyArchive* archive)
  : Archive(archive)
{
}

void cmBinUtilsLinuxELFGetRuntimeDependenciesTool::SetError(
  std::string const& error)
{
  this->Archive->SetError(error);
}
