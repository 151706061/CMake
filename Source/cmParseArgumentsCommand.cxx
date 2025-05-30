/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmParseArgumentsCommand.h"

#include <map>
#include <set>
#include <utility>

#include <cm/string_view>

#include "cmArgumentParser.h"
#include "cmArgumentParserTypes.h"
#include "cmExecutionStatus.h"
#include "cmList.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmPolicies.h"
#include "cmRange.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmValue.h"

namespace {

std::string EscapeArg(std::string const& arg)
{
  // replace ";" with "\;" so output argument lists will split correctly
  std::string escapedArg;
  for (char i : arg) {
    if (i == ';') {
      escapedArg += '\\';
    }
    escapedArg += i;
  }
  return escapedArg;
}

std::string JoinList(std::vector<std::string> const& arg, bool escape)
{
  return escape ? cmList::to_string(cmMakeRange(arg).transform(EscapeArg))
                : cmList::to_string(cmMakeRange(arg));
}

using options_map = std::map<std::string, bool>;
using single_map = std::map<std::string, std::string>;
using multi_map =
  std::map<std::string, ArgumentParser::NonEmpty<std::vector<std::string>>>;
using options_set = std::set<cm::string_view>;

struct UserArgumentParser : public cmArgumentParser<void>
{
  void BindKeywordsMissingValue(std::vector<cm::string_view>& ref)
  {
    this->cmArgumentParser<void>::BindKeywordMissingValue(
      [&ref](Instance&, cm::string_view arg) { ref.emplace_back(arg); });
  }

  template <typename T, typename H>
  void Bind(std::vector<std::string> const& names,
            std::map<std::string, T>& ref, H duplicateKey)
  {
    for (std::string const& key : names) {
      auto const it = ref.emplace(key, T{}).first;
      bool const inserted = this->cmArgumentParser<void>::Bind(
        cm::string_view(it->first), it->second);
      if (!inserted) {
        duplicateKey(key);
      }
    }
  }
};

} // namespace

static void PassParsedArguments(
  std::string const& prefix, cmMakefile& makefile, options_map const& options,
  single_map const& singleValArgs, multi_map const& multiValArgs,
  std::vector<std::string> const& unparsed, options_set const& keywordsSeen,
  options_set const& keywordsMissingValues, bool parseFromArgV)
{
  for (auto const& iter : options) {
    makefile.AddDefinition(cmStrCat(prefix, iter.first),
                           iter.second ? "TRUE" : "FALSE");
  }

  cmPolicies::PolicyStatus const cmp0174 =
    makefile.GetPolicyStatus(cmPolicies::CMP0174);
  for (auto const& iter : singleValArgs) {
    if (keywordsSeen.find(iter.first) == keywordsSeen.end()) {
      makefile.RemoveDefinition(cmStrCat(prefix, iter.first));
    } else if ((parseFromArgV && cmp0174 == cmPolicies::NEW) ||
               !iter.second.empty()) {
      makefile.AddDefinition(cmStrCat(prefix, iter.first), iter.second);
    } else {
      // The OLD policy behavior doesn't define a variable for an empty or
      // missing value, and we can't differentiate between those two cases.
      if (parseFromArgV && (cmp0174 == cmPolicies::WARN)) {
        makefile.IssueMessage(
          MessageType::AUTHOR_WARNING,
          cmStrCat("The ", iter.first,
                   " keyword was followed by an empty string or no value at "
                   "all. Policy CMP0174 is not set, so "
                   "cmake_parse_arguments() will unset the ",
                   prefix, iter.first,
                   " variable rather than setting it to an empty string."));
      }
      makefile.RemoveDefinition(cmStrCat(prefix, iter.first));
    }
  }

  for (auto const& iter : multiValArgs) {
    if (!iter.second.empty()) {
      makefile.AddDefinition(cmStrCat(prefix, iter.first),
                             JoinList(iter.second, parseFromArgV));
    } else {
      makefile.RemoveDefinition(cmStrCat(prefix, iter.first));
    }
  }

  if (!unparsed.empty()) {
    makefile.AddDefinition(cmStrCat(prefix, "UNPARSED_ARGUMENTS"),
                           JoinList(unparsed, parseFromArgV));
  } else {
    makefile.RemoveDefinition(cmStrCat(prefix, "UNPARSED_ARGUMENTS"));
  }

  if (!keywordsMissingValues.empty()) {
    makefile.AddDefinition(
      cmStrCat(prefix, "KEYWORDS_MISSING_VALUES"),
      cmList::to_string(cmMakeRange(keywordsMissingValues)));
  } else {
    makefile.RemoveDefinition(cmStrCat(prefix, "KEYWORDS_MISSING_VALUES"));
  }
}

bool cmParseArgumentsCommand(std::vector<std::string> const& args,
                             cmExecutionStatus& status)
{
  // cmake_parse_arguments(prefix options single multi <ARGN>)
  //                         1       2      3      4
  // or
  // cmake_parse_arguments(PARSE_ARGV N prefix options single multi)
  if (args.size() < 4) {
    status.SetError("must be called with at least 4 arguments.");
    return false;
  }

  auto argIter = args.begin();
  auto argEnd = args.end();
  bool parseFromArgV = false;
  unsigned long argvStart = 0;
  if (*argIter == "PARSE_ARGV") {
    if (args.size() != 6) {
      status.GetMakefile().IssueMessage(
        MessageType::FATAL_ERROR,
        "PARSE_ARGV must be called with exactly 6 arguments.");
      cmSystemTools::SetFatalErrorOccurred();
      return true;
    }
    parseFromArgV = true;
    argIter++; // move past PARSE_ARGV
    if (!cmStrToULong(*argIter, &argvStart)) {
      status.GetMakefile().IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat("PARSE_ARGV index '", *argIter,
                 "' is not an unsigned integer"));
      cmSystemTools::SetFatalErrorOccurred();
      return true;
    }
    argIter++; // move past N
  }
  // the first argument is the prefix
  std::string const prefix = (*argIter++) + "_";

  UserArgumentParser parser;

  // define the result maps holding key/value pairs for
  // options, single values and multi values
  options_map options;
  single_map singleValArgs;
  multi_map multiValArgs;

  // anything else is put into a vector of unparsed strings
  std::vector<std::string> unparsed;

  auto const duplicateKey = [&status](std::string const& key) {
    status.GetMakefile().IssueMessage(
      MessageType::WARNING, cmStrCat("keyword defined more than once: ", key));
  };

  // the second argument is a (cmake) list of options without argument
  cmList list{ *argIter++ };
  parser.Bind(list, options, duplicateKey);

  // the third argument is a (cmake) list of single argument options
  list.assign(*argIter++);
  parser.Bind(list, singleValArgs, duplicateKey);

  // the fourth argument is a (cmake) list of multi argument options
  list.assign(*argIter++);
  parser.Bind(list, multiValArgs, duplicateKey);

  list.clear();
  if (!parseFromArgV) {
    // Flatten ;-lists in the arguments into a single list as was done
    // by the original function(CMAKE_PARSE_ARGUMENTS).
    for (; argIter != argEnd; ++argIter) {
      list.append(*argIter);
    }
  } else {
    // in the PARSE_ARGV move read the arguments from ARGC and ARGV#
    std::string argc = status.GetMakefile().GetSafeDefinition("ARGC");
    unsigned long count;
    if (!cmStrToULong(argc, &count)) {
      status.GetMakefile().IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat("PARSE_ARGV called with ARGC='", argc,
                 "' that is not an unsigned integer"));
      cmSystemTools::SetFatalErrorOccurred();
      return true;
    }
    for (unsigned long i = argvStart; i < count; ++i) {
      std::string const argName{ cmStrCat("ARGV", i) };
      cmValue arg = status.GetMakefile().GetDefinition(argName);
      if (!arg) {
        status.GetMakefile().IssueMessage(
          MessageType::FATAL_ERROR,
          cmStrCat("PARSE_ARGV called with ", argName, " not set"));
        cmSystemTools::SetFatalErrorOccurred();
        return true;
      }
      list.emplace_back(*arg);
    }
  }

  std::vector<cm::string_view> keywordsSeen;
  parser.BindParsedKeywords(keywordsSeen);

  // For single-value keywords, only the last instance matters, since it
  // determines the value stored. But if a keyword is repeated, it will be
  // added to this vector if _any_ instance is missing a value. If one of the
  // earlier instances is missing a value but the last one isn't, its presence
  // in this vector will be misleading.
  std::vector<cm::string_view> keywordsMissingValues;
  parser.BindKeywordsMissingValue(keywordsMissingValues);

  parser.Parse(list, &unparsed);

  PassParsedArguments(
    prefix, status.GetMakefile(), options, singleValArgs, multiValArgs,
    unparsed, options_set(keywordsSeen.begin(), keywordsSeen.end()),
    options_set(keywordsMissingValues.begin(), keywordsMissingValues.end()),
    parseFromArgV);

  return true;
}
