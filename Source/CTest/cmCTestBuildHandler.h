/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <chrono>
#include <cstddef>
#include <deque>
#include <iosfwd>
#include <string>
#include <vector>

#include "cmsys/RegularExpression.hxx"

#include "cmCTestGenericHandler.h"
#include "cmDuration.h"
#include "cmProcessOutput.h"

class cmMakefile;
class cmStringReplaceHelper;
class cmXMLWriter;
class cmCTest;

/** \class cmCTestBuildHandler
 * \brief A class that handles ctest -S invocations
 *
 */
class cmCTestBuildHandler : public cmCTestGenericHandler
{
public:
  using Superclass = cmCTestGenericHandler;
  using Encoding = cmProcessOutput::Encoding;

  /*
   * The main entry point for this class
   */
  int ProcessHandler() override;

  cmCTestBuildHandler(cmCTest* ctest);

  void PopulateCustomVectors(cmMakefile* mf) override;

  int GetTotalErrors() const { return this->TotalErrors; }
  int GetTotalWarnings() const { return this->TotalWarnings; }

private:
  std::string GetMakeCommand();

  //! Run command specialized for make and configure. Returns process status
  // and retVal is return value or exception.
  bool RunMakeCommand(std::string const& command, int* retVal, char const* dir,
                      int timeout, std::ostream& ofs,
                      Encoding encoding = cmProcessOutput::Auto);

  enum
  {
    b_REGULAR_LINE,
    b_WARNING_LINE,
    b_ERROR_LINE
  };

  class cmCTestCompileErrorWarningRex
  {
  public:
    cmCTestCompileErrorWarningRex() {}
    int FileIndex;
    int LineIndex;
    cmsys::RegularExpression RegularExpression;
  };

  struct cmCTestBuildErrorWarning
  {
    bool Error;
    int LogLine;
    std::string Text;
    std::string SourceFile;
    std::string SourceFileTail;
    int LineNumber;
    std::string PreContext;
    std::string PostContext;
  };

  // generate the XML output
  void GenerateXMLHeader(cmXMLWriter& xml);
  void GenerateXMLLaunched(cmXMLWriter& xml);
  void GenerateXMLLogScraped(cmXMLWriter& xml);
  void GenerateInstrumentationXML(cmXMLWriter& xml);
  void GenerateXMLFooter(cmXMLWriter& xml, cmDuration elapsed_build_time);
  bool IsLaunchedErrorFile(char const* fname);
  bool IsLaunchedWarningFile(char const* fname);

  std::string StartBuild;
  std::string EndBuild;
  std::chrono::system_clock::time_point StartBuildTime;
  std::chrono::system_clock::time_point EndBuildTime;

  std::vector<std::string> CustomErrorMatches;
  std::vector<std::string> CustomErrorExceptions;
  std::vector<std::string> CustomWarningMatches;
  std::vector<std::string> CustomWarningExceptions;
  std::vector<std::string> ReallyCustomWarningMatches;
  std::vector<std::string> ReallyCustomWarningExceptions;
  std::vector<cmCTestCompileErrorWarningRex> ErrorWarningFileLineRegex;

  std::vector<cmsys::RegularExpression> ErrorMatchRegex;
  std::vector<cmsys::RegularExpression> ErrorExceptionRegex;
  std::vector<cmsys::RegularExpression> WarningMatchRegex;
  std::vector<cmsys::RegularExpression> WarningExceptionRegex;

  using t_BuildProcessingQueueType = std::deque<char>;

  void ProcessBuffer(char const* data, size_t length, size_t& tick,
                     size_t tick_len, std::ostream& ofs,
                     t_BuildProcessingQueueType* queue);
  int ProcessSingleLine(char const* data);

  t_BuildProcessingQueueType BuildProcessingQueue;
  t_BuildProcessingQueueType BuildProcessingErrorQueue;
  size_t BuildOutputLogSize = 0;
  std::vector<char> CurrentProcessingLine;

  std::string SimplifySourceDir;
  std::string SimplifyBuildDir;
  size_t OutputLineCounter = 0;
  using t_ErrorsAndWarningsVector = std::vector<cmCTestBuildErrorWarning>;
  t_ErrorsAndWarningsVector ErrorsAndWarnings;
  t_ErrorsAndWarningsVector::iterator LastErrorOrWarning;
  size_t PostContextCount = 0;
  size_t MaxPreContext = 10;
  size_t MaxPostContext = 10;
  std::deque<std::string> PreContext;

  int TotalErrors = 0;
  int TotalWarnings = 0;
  char LastTickChar = '\0';

  bool ErrorQuotaReached = false;
  bool WarningQuotaReached = false;

  int MaxErrors = 50;
  int MaxWarnings = 50;

  // Used to remove ANSI color codes before checking for errors and warnings.
  cmStringReplaceHelper* ColorRemover;

  bool UseCTestLaunch = false;
  std::string CTestLaunchDir;
  std::string LogFileName;
  class LaunchHelper;

  friend class LaunchHelper;
  class FragmentCompare;
};
