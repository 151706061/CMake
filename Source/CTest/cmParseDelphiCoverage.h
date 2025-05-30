/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <string>
#include <vector>

class cmCTest;
class cmCTestCoverageHandlerContainer;

/** \class cmParseDelphiCoverage
 * \brief Parse Delphi coverage information
 *
 * This class is used to parse Delphi(Pascal) coverage information
 * generated by the Delphi-Code-Coverage tool
 *
 * https://code.google.com/p/delphi-code-coverage/
 */

class cmParseDelphiCoverage
{
public:
  cmParseDelphiCoverage(cmCTestCoverageHandlerContainer& cont, cmCTest* ctest);
  bool LoadCoverageData(std::vector<std::string> const& files);
  bool ReadDelphiHTML(char const* file);
  // Read a single HTML file from output
  bool ReadHTMLFile(char const* f);

protected:
  class HTMLParser;

  cmCTestCoverageHandlerContainer& Coverage;
  cmCTest* CTest;
};
