
#include <algorithm>
#include <iterator>
#include <set>
#include <unordered_set>
#include <utility>

#include <cm/memory>
#include <cmext/string_view>

#include "cmCustomCommand.h"
#include "cmCustomCommandLines.h"
#include "cmCustomCommandTypes.h"
#include "cmExecutionStatus.h"
#include "cmGeneratorExpression.h"
#include "cmGlobalGenerator.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmPolicies.h"
#include "cmState.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmTest.h"
#include "cmTestGenerator.h"
#include "cmValue.h"

bool cmAddCompileDefinitionsCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status)
{
  cmMakefile& mf = status.GetMakefile();
  for (std::string const& i : args) {
    mf.AddCompileDefinition(i);
  }
  return true;
}

bool cmAddCompileOptionsCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status)
{
  cmMakefile& mf = status.GetMakefile();
  for (std::string const& i : args) {
    mf.AddCompileOption(i);
  }
  return true;
}

bool cmAddCustomCommandCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status)
{
  /* Let's complain at the end of this function about the lack of a particular
     arg. For the moment, let's say that COMMAND, and either TARGET or SOURCE
     are required.
  */
  if (args.size() < 4) {
    status.SetError("called with wrong number of arguments.");
    return false;
  }

  cmMakefile& mf = status.GetMakefile();
  std::string source;
  std::string target;
  std::string main_dependency;
  std::string working;
  std::string depfile;
  std::string job_pool;
  std::string job_server_aware;
  std::string comment_buffer;
  char const* comment = nullptr;
  std::vector<std::string> depends;
  std::vector<std::string> outputs;
  std::vector<std::string> output;
  std::vector<std::string> byproducts;
  bool verbatim = false;
  bool append = false;
  bool uses_terminal = false;
  bool command_expand_lists = false;
  bool depends_explicit_only = mf.IsOn("CMAKE_ADD_CUSTOM_COMMAND_DEPENDS_EXPLICIT_ONLY");
  bool codegen = false;
  std::string implicit_depends_lang;
  cmImplicitDependsList implicit_depends;

  // Accumulate one command line at a time.
  cmCustomCommandLine currentLine;

  // Save all command lines.
  cmCustomCommandLines commandLines;

  cmCustomCommandType cctype = cmCustomCommandType::POST_BUILD;

  enum tdoing
  {
    doing_source,
    doing_command,
    doing_target,
    doing_depends,
    doing_implicit_depends_lang,
    doing_implicit_depends_file,
    doing_main_dependency,
    doing_output,
    doing_outputs,
    doing_byproducts,
    doing_comment,
    doing_working_directory,
    doing_depfile,
    doing_job_pool,
    doing_job_server_aware,
    doing_nothing
  };

  tdoing doing = doing_nothing;

#define MAKE_STATIC_KEYWORD(KEYWORD) static const std::string key##KEYWORD = #KEYWORD
  MAKE_STATIC_KEYWORD(APPEND);
  MAKE_STATIC_KEYWORD(ARGS);
  MAKE_STATIC_KEYWORD(BYPRODUCTS);
  MAKE_STATIC_KEYWORD(COMMAND);
  MAKE_STATIC_KEYWORD(COMMAND_EXPAND_LISTS);
  MAKE_STATIC_KEYWORD(COMMENT);
  MAKE_STATIC_KEYWORD(DEPENDS);
  MAKE_STATIC_KEYWORD(DEPFILE);
  MAKE_STATIC_KEYWORD(IMPLICIT_DEPENDS);
  MAKE_STATIC_KEYWORD(JOB_POOL);
  MAKE_STATIC_KEYWORD(JOB_SERVER_AWARE);
  MAKE_STATIC_KEYWORD(MAIN_DEPENDENCY);
  MAKE_STATIC_KEYWORD(OUTPUT);
  MAKE_STATIC_KEYWORD(OUTPUTS);
  MAKE_STATIC_KEYWORD(POST_BUILD);
  MAKE_STATIC_KEYWORD(PRE_BUILD);
  MAKE_STATIC_KEYWORD(PRE_LINK);
  MAKE_STATIC_KEYWORD(SOURCE);
  MAKE_STATIC_KEYWORD(TARGET);
  MAKE_STATIC_KEYWORD(USES_TERMINAL);
  MAKE_STATIC_KEYWORD(VERBATIM);
  MAKE_STATIC_KEYWORD(WORKING_DIRECTORY);
  MAKE_STATIC_KEYWORD(DEPENDS_EXPLICIT_ONLY);
  MAKE_STATIC_KEYWORD(CODEGEN);
#undef MAKE_STATIC_KEYWORD
  static std::unordered_set<std::string> const keywords{ keyAPPEND,
                                                         keyARGS,
                                                         keyBYPRODUCTS,
                                                         keyCOMMAND,
                                                         keyCOMMAND_EXPAND_LISTS,
                                                         keyCOMMENT,
                                                         keyDEPENDS,
                                                         keyDEPFILE,
                                                         keyIMPLICIT_DEPENDS,
                                                         keyJOB_POOL,
                                                         keyMAIN_DEPENDENCY,
                                                         keyOUTPUT,
                                                         keyOUTPUTS,
                                                         keyPOST_BUILD,
                                                         keyPRE_BUILD,
                                                         keyPRE_LINK,
                                                         keySOURCE,
                                                         keyJOB_SERVER_AWARE,
                                                         keyTARGET,
                                                         keyUSES_TERMINAL,
                                                         keyVERBATIM,
                                                         keyWORKING_DIRECTORY,
                                                         keyDEPENDS_EXPLICIT_ONLY,
                                                         keyCODEGEN };
  /* clang-format off */
  static std::set<std::string> const supportedTargetKeywords{
    keyARGS,
    keyBYPRODUCTS,
    keyCOMMAND,
    keyCOMMAND_EXPAND_LISTS,
    keyCOMMENT,
    keyPOST_BUILD,
    keyPRE_BUILD,
    keyPRE_LINK,
    keyTARGET,
    keyUSES_TERMINAL,
    keyVERBATIM,
    keyWORKING_DIRECTORY
  };
  /* clang-format on */
  static std::set<std::string> const supportedOutputKeywords{ keyAPPEND,
                                                              keyARGS,
                                                              keyBYPRODUCTS,
                                                              keyCODEGEN,
                                                              keyCOMMAND,
                                                              keyCOMMAND_EXPAND_LISTS,
                                                              keyCOMMENT,
                                                              keyDEPENDS,
                                                              keyDEPENDS_EXPLICIT_ONLY,
                                                              keyDEPFILE,
                                                              keyIMPLICIT_DEPENDS,
                                                              keyJOB_POOL,
                                                              keyJOB_SERVER_AWARE,
                                                              keyMAIN_DEPENDENCY,
                                                              keyOUTPUT,
                                                              keyUSES_TERMINAL,
                                                              keyVERBATIM,
                                                              keyWORKING_DIRECTORY };
  /* clang-format off */
  static std::set<std::string> const supportedAppendKeywords{
    keyAPPEND,
    keyARGS,
    keyCOMMAND,
    keyCOMMENT,           // Allowed but ignored
    keyDEPENDS,
    keyIMPLICIT_DEPENDS,
    keyMAIN_DEPENDENCY,   // Allowed but ignored
    keyOUTPUT,
    keyWORKING_DIRECTORY  // Allowed but ignored
  };
  /* clang-format on */
  std::set<std::string> keywordsSeen;
  std::string const* keywordExpectingValue = nullptr;
  auto const cmp0175 = mf.GetPolicyStatus(cmPolicies::CMP0175);

  for (std::string const& copy : args) {
    if (keywords.count(copy)) {
      // Check if a preceding keyword expected a value but there wasn't one
      if (keywordExpectingValue) {
        std::string const msg = cmStrCat("Keyword ", *keywordExpectingValue, " requires a value, but none was given.");
        if (cmp0175 == cmPolicies::NEW) {
          mf.IssueMessage(MessageType::FATAL_ERROR, msg);
          return false;
        }
        if (cmp0175 == cmPolicies::WARN) {
          mf.IssueMessage(
            MessageType::AUTHOR_WARNING, cmStrCat(msg, '\n', cmPolicies::GetPolicyWarning(cmPolicies::CMP0175)));
        }
      }
      keywordExpectingValue = nullptr;
      keywordsSeen.insert(copy);

      if (copy == keySOURCE) {
        doing = doing_source;
        keywordExpectingValue = &keySOURCE;
      } else if (copy == keyCOMMAND) {
        doing = doing_command;

        // Save the current command before starting the next command.
        if (!currentLine.empty()) {
          commandLines.push_back(currentLine);
          currentLine.clear();
        }
      } else if (copy == keyPRE_BUILD) {
        cctype = cmCustomCommandType::PRE_BUILD;
      } else if (copy == keyPRE_LINK) {
        cctype = cmCustomCommandType::PRE_LINK;
      } else if (copy == keyPOST_BUILD) {
        cctype = cmCustomCommandType::POST_BUILD;
      } else if (copy == keyVERBATIM) {
        verbatim = true;
      } else if (copy == keyAPPEND) {
        append = true;
      } else if (copy == keyUSES_TERMINAL) {
        uses_terminal = true;
      } else if (copy == keyCOMMAND_EXPAND_LISTS) {
        command_expand_lists = true;
      } else if (copy == keyDEPENDS_EXPLICIT_ONLY) {
        depends_explicit_only = true;
      } else if (copy == keyCODEGEN) {
        codegen = true;
      } else if (copy == keyTARGET) {
        doing = doing_target;
        keywordExpectingValue = &keyTARGET;
      } else if (copy == keyARGS) {
        // Ignore this old keyword.
      } else if (copy == keyDEPENDS) {
        doing = doing_depends;
      } else if (copy == keyOUTPUTS) {
        doing = doing_outputs;
      } else if (copy == keyOUTPUT) {
        doing = doing_output;
        keywordExpectingValue = &keyOUTPUT;
      } else if (copy == keyBYPRODUCTS) {
        doing = doing_byproducts;
      } else if (copy == keyWORKING_DIRECTORY) {
        doing = doing_working_directory;
        keywordExpectingValue = &keyWORKING_DIRECTORY;
      } else if (copy == keyMAIN_DEPENDENCY) {
        doing = doing_main_dependency;
        keywordExpectingValue = &keyMAIN_DEPENDENCY;
      } else if (copy == keyIMPLICIT_DEPENDS) {
        doing = doing_implicit_depends_lang;
      } else if (copy == keyCOMMENT) {
        doing = doing_comment;
        keywordExpectingValue = &keyCOMMENT;
      } else if (copy == keyDEPFILE) {
        doing = doing_depfile;
        if (!mf.GetGlobalGenerator()->SupportsCustomCommandDepfile()) {
          status.SetError(cmStrCat("Option DEPFILE not supported by ", mf.GetGlobalGenerator()->GetName()));
          return false;
        }
        keywordExpectingValue = &keyDEPFILE;
      } else if (copy == keyJOB_POOL) {
        doing = doing_job_pool;
        keywordExpectingValue = &keyJOB_POOL;
      } else if (copy == keyJOB_SERVER_AWARE) {
        doing = doing_job_server_aware;
        keywordExpectingValue = &keyJOB_SERVER_AWARE;
      }
    } else {
      keywordExpectingValue = nullptr; // Value is being processed now
      std::string filename;
      switch (doing) {
        case doing_output:
        case doing_outputs:
        case doing_byproducts:
          if (!cmSystemTools::FileIsFullPath(copy) && cmGeneratorExpression::Find(copy) != 0) {
            // This is an output to be generated, so it should be
            // under the build tree.
            filename = cmStrCat(mf.GetCurrentBinaryDirectory(), '/');
          }
          filename += copy;
          cmSystemTools::ConvertToUnixSlashes(filename);
          break;
        case doing_source:
        // We do not want to convert the argument to SOURCE because
        // that option is only available for backward compatibility.
        // Old-style use of this command may use the SOURCE==TARGET
        // trick which we must preserve.  If we convert the source
        // to a full path then it will no longer equal the target.
        default:
          break;
      }

      if (cmSystemTools::FileIsFullPath(filename)) {
        filename = cmSystemTools::CollapseFullPath(filename);
      }
      switch (doing) {
        case doing_depfile:
          depfile = copy;
          break;
        case doing_job_pool:
          job_pool = copy;
          break;
        case doing_job_server_aware:
          job_server_aware = copy;
          break;
        case doing_working_directory:
          working = copy;
          break;
        case doing_source:
          source = copy;
          break;
        case doing_output:
          output.push_back(filename);
          break;
        case doing_main_dependency:
          main_dependency = copy;
          break;
        case doing_implicit_depends_lang:
          implicit_depends_lang = copy;
          doing = doing_implicit_depends_file;
          break;
        case doing_implicit_depends_file: {
          // An implicit dependency starting point is also an
          // explicit dependency.
          std::string dep = copy;
          // Upfront path conversion is correct because Genex
          // are not supported.
          cmSystemTools::ConvertToUnixSlashes(dep);
          depends.push_back(dep);

          // Add the implicit dependency language and file.
          implicit_depends.emplace_back(implicit_depends_lang, dep);

          // Switch back to looking for a language.
          doing = doing_implicit_depends_lang;
        } break;
        case doing_command:
          currentLine.push_back(copy);
          break;
        case doing_target:
          target = copy;
          break;
        case doing_depends:
          depends.push_back(copy);
          break;
        case doing_outputs:
          outputs.push_back(filename);
          break;
        case doing_byproducts:
          byproducts.push_back(filename);
          break;
        case doing_comment:
          if (!comment_buffer.empty()) {
            std::string const msg = "COMMENT requires exactly one argument, but multiple values "
                                    "or COMMENT keywords have been given.";
            if (cmp0175 == cmPolicies::NEW) {
              mf.IssueMessage(MessageType::FATAL_ERROR, msg);
              return false;
            }
            if (cmp0175 == cmPolicies::WARN) {
              mf.IssueMessage(
                MessageType::AUTHOR_WARNING, cmStrCat(msg, '\n', cmPolicies::GetPolicyWarning(cmPolicies::CMP0175)));
            }
          }
          comment_buffer = copy;
          comment = comment_buffer.c_str();
          break;
        default:
          status.SetError("Wrong syntax. Unknown type of argument.");
          return false;
      }
    }
  }

  // Store the last command line finished.
  if (!currentLine.empty()) {
    commandLines.push_back(currentLine);
    currentLine.clear();
  }

  // At this point we could complain about the lack of arguments.  For
  // the moment, let's say that COMMAND, TARGET are always required.
  if (output.empty() && target.empty()) {
    status.SetError("Wrong syntax. A TARGET or OUTPUT must be specified.");
    return false;
  }

  if (source.empty() && !target.empty() && !output.empty()) {
    status.SetError("Wrong syntax. A TARGET and OUTPUT can not both be specified.");
    return false;
  }
  if (append && output.empty()) {
    status.SetError("given APPEND option with no OUTPUT.");
    return false;
  }
  if (!implicit_depends.empty() && !depfile.empty() && mf.GetGlobalGenerator()->GetName() != "Ninja") {
    // Makefiles generators does not support both at the same time
    status.SetError("IMPLICIT_DEPENDS and DEPFILE can not both be specified.");
    return false;
  }

  if (codegen) {
    if (output.empty()) {
      status.SetError("CODEGEN requires at least 1 OUTPUT.");
      return false;
    }

    if (append) {
      status.SetError("CODEGEN may not be used with APPEND.");
      return false;
    }

    if (!implicit_depends.empty()) {
      status.SetError("CODEGEN is not compatible with IMPLICIT_DEPENDS.");
      return false;
    }

    if (mf.GetPolicyStatus(cmPolicies::CMP0171) != cmPolicies::NEW) {
      status.SetError("CODEGEN option requires policy CMP0171 be set to NEW!");
      return false;
    }
  }

  // Check for an append request.
  if (append) {
    std::vector<std::string> unsupportedKeywordsUsed;
    std::set_difference(
      keywordsSeen.begin(), keywordsSeen.end(), supportedAppendKeywords.begin(), supportedAppendKeywords.end(),
      std::back_inserter(unsupportedKeywordsUsed));
    if (!unsupportedKeywordsUsed.empty()) {
      std::string const msg = cmJoin(
        unsupportedKeywordsUsed, ", "_s,
        "The following keywords are not supported when using "
        "APPEND with add_custom_command(OUTPUT): "_s);
      if (cmp0175 == cmPolicies::NEW) {
        mf.IssueMessage(MessageType::FATAL_ERROR, msg);
        return false;
      }
      if (cmp0175 == cmPolicies::WARN) {
        mf.IssueMessage(
          MessageType::AUTHOR_WARNING, cmStrCat(msg, ".\n", cmPolicies::GetPolicyWarning(cmPolicies::CMP0175)));
      }
    }
    mf.AppendCustomCommandToOutput(output[0], depends, implicit_depends, commandLines);
    return true;
  }

  if (uses_terminal && !job_pool.empty()) {
    status.SetError("JOB_POOL is shadowed by USES_TERMINAL.");
    return false;
  }

  // Choose which mode of the command to use.
  auto m_pCustomCommand = cm::make_unique<cmCustomCommand>();
  m_pCustomCommand->SetByproducts(byproducts);
  m_pCustomCommand->SetCommandLines(commandLines);
  m_pCustomCommand->SetComment(comment);
  m_pCustomCommand->SetWorkingDirectory(working.c_str());
  m_pCustomCommand->SetEscapeOldStyle(!verbatim);
  m_pCustomCommand->SetUsesTerminal(uses_terminal);
  m_pCustomCommand->SetDepfile(depfile);
  m_pCustomCommand->SetJobPool(job_pool);
  m_pCustomCommand->SetJobserverAware(cmIsOn(job_server_aware));
  m_pCustomCommand->SetCommandExpandLists(command_expand_lists);
  m_pCustomCommand->SetDependsExplicitOnly(depends_explicit_only);
  if (source.empty() && output.empty()) {
    // Source is empty, use the target.
    if (commandLines.empty()) {
      std::string const msg = "At least one COMMAND must be given.";
      if (cmp0175 == cmPolicies::NEW) {
        mf.IssueMessage(MessageType::FATAL_ERROR, msg);
        return false;
      }
      if (cmp0175 == cmPolicies::WARN) {
        mf.IssueMessage(
          MessageType::AUTHOR_WARNING, cmStrCat(msg, '\n', cmPolicies::GetPolicyWarning(cmPolicies::CMP0175)));
      }
    }

    std::vector<std::string> unsupportedKeywordsUsed;
    std::set_difference(
      keywordsSeen.begin(), keywordsSeen.end(), supportedTargetKeywords.begin(), supportedTargetKeywords.end(),
      std::back_inserter(unsupportedKeywordsUsed));
    if (!unsupportedKeywordsUsed.empty()) {
      std::string const msg = cmJoin(
        unsupportedKeywordsUsed, ", "_s,
        "The following keywords are not supported when using "
        "add_custom_command(TARGET): "_s);
      if (cmp0175 == cmPolicies::NEW) {
        mf.IssueMessage(MessageType::FATAL_ERROR, msg);
        return false;
      }
      if (cmp0175 == cmPolicies::WARN) {
        mf.IssueMessage(
          MessageType::AUTHOR_WARNING, cmStrCat(msg, ".\n", cmPolicies::GetPolicyWarning(cmPolicies::CMP0175)));
      }
    }
    auto const prePostCount =
      keywordsSeen.count(keyPRE_BUILD) + keywordsSeen.count(keyPRE_LINK) + keywordsSeen.count(keyPOST_BUILD);
    if (prePostCount != 1) {
      std::string msg = "Exactly one of PRE_BUILD, PRE_LINK, or POST_BUILD must be given.";
      if (cmp0175 == cmPolicies::NEW) {
        mf.IssueMessage(MessageType::FATAL_ERROR, msg);
        return false;
      }
      if (cmp0175 == cmPolicies::WARN) {
        msg += " Assuming ";
        switch (cctype) {
          case cmCustomCommandType::PRE_BUILD:
            msg += "PRE_BUILD";
            break;
          case cmCustomCommandType::PRE_LINK:
            msg += "PRE_LINK";
            break;
          case cmCustomCommandType::POST_BUILD:
            msg += "POST_BUILD";
        }
        mf.IssueMessage(
          MessageType::AUTHOR_WARNING,
          cmStrCat(msg, " to preserve backward compatibility.\n", cmPolicies::GetPolicyWarning(cmPolicies::CMP0175)));
      }
    }
    mf.AddCustomCommandToTarget(target, cctype, std::move(m_pCustomCommand));
  } else if (target.empty()) {
    // Target is empty, use the output.
    std::vector<std::string> unsupportedKeywordsUsed;
    std::set_difference(
      keywordsSeen.begin(), keywordsSeen.end(), supportedOutputKeywords.begin(), supportedOutputKeywords.end(),
      std::back_inserter(unsupportedKeywordsUsed));
    if (!unsupportedKeywordsUsed.empty()) {
      std::string const msg = cmJoin(
        unsupportedKeywordsUsed, ", "_s,
        "The following keywords are not supported when using "
        "add_custom_command(OUTPUT): "_s);
      if (cmp0175 == cmPolicies::NEW) {
        mf.IssueMessage(MessageType::FATAL_ERROR, msg);
        return false;
      }
      if (cmp0175 == cmPolicies::WARN) {
        mf.IssueMessage(
          MessageType::AUTHOR_WARNING, cmStrCat(msg, ".\n", cmPolicies::GetPolicyWarning(cmPolicies::CMP0175)));
      }
    }
    m_pCustomCommand->SetOutputs(output);
    m_pCustomCommand->SetMainDependency(main_dependency);
    m_pCustomCommand->SetDepends(depends);
    m_pCustomCommand->SetCodegen(codegen);
    m_pCustomCommand->SetImplicitDepends(implicit_depends);
    mf.AddCustomCommandToOutput(std::move(m_pCustomCommand));
  } else {
    mf.IssueMessage(MessageType::FATAL_ERROR, "The SOURCE signatures of add_custom_command are no longer supported.");
    return false;
  }

  return true;
}

bool cmAddCustomTargetCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status)
{
  if (args.empty()) {
    status.SetError("called with incorrect number of arguments");
    return false;
  }

  cmMakefile& mf = status.GetMakefile();
  std::string const& targetName = args[0];

  // Check the target name.
  if (targetName.find_first_of("/\\") != std::string::npos) {
    status.SetError(cmStrCat(
      "called with invalid target name \"", targetName,
      "\".  Target names may not contain a slash.  "
      "Use ADD_CUSTOM_COMMAND to generate files."));
    return false;
  }

  // Accumulate one command line at a time.
  cmCustomCommandLine currentLine;

  // Save all command lines.
  cmCustomCommandLines commandLines;

  // Accumulate dependencies.
  std::vector<std::string> depends;
  std::vector<std::string> byproducts;
  std::string working_directory;
  bool verbatim = false;
  bool uses_terminal = false;
  bool command_expand_lists = false;
  std::string comment_buffer;
  char const* comment = nullptr;
  std::vector<std::string> sources;
  std::string job_pool;
  std::string job_server_aware;

  // Keep track of parser state.
  enum tdoing
  {
    doing_command,
    doing_depends,
    doing_byproducts,
    doing_working_directory,
    doing_comment,
    doing_source,
    doing_job_pool,
    doing_job_server_aware,
    doing_nothing
  };
  tdoing doing = doing_command;

  // Look for the ALL option.
  bool excludeFromAll = true;
  unsigned int start = 1;
  if (args.size() > 1) {
    if (args[1] == "ALL") {
      excludeFromAll = false;
      start = 2;
    }
  }

  // Parse the rest of the arguments.
  for (unsigned int j = start; j < args.size(); ++j) {
    std::string const& copy = args[j];

    if (copy == "DEPENDS") {
      doing = doing_depends;
    } else if (copy == "BYPRODUCTS") {
      doing = doing_byproducts;
    } else if (copy == "WORKING_DIRECTORY") {
      doing = doing_working_directory;
    } else if (copy == "VERBATIM") {
      doing = doing_nothing;
      verbatim = true;
    } else if (copy == "USES_TERMINAL") {
      doing = doing_nothing;
      uses_terminal = true;
    } else if (copy == "COMMAND_EXPAND_LISTS") {
      doing = doing_nothing;
      command_expand_lists = true;
    } else if (copy == "COMMENT") {
      doing = doing_comment;
    } else if (copy == "JOB_POOL") {
      doing = doing_job_pool;
    } else if (copy == "JOB_SERVER_AWARE") {
      doing = doing_job_server_aware;
    } else if (copy == "COMMAND") {
      doing = doing_command;

      // Save the current command before starting the next command.
      if (!currentLine.empty()) {
        commandLines.push_back(currentLine);
        currentLine.clear();
      }
    } else if (copy == "SOURCES") {
      doing = doing_source;
    } else {
      switch (doing) {
        case doing_working_directory:
          working_directory = copy;
          break;
        case doing_command:
          currentLine.push_back(copy);
          break;
        case doing_byproducts: {
          std::string filename;
          if (!cmSystemTools::FileIsFullPath(copy) && cmGeneratorExpression::Find(copy) != 0) {
            filename = cmStrCat(mf.GetCurrentBinaryDirectory(), '/');
          }
          filename += copy;
          cmSystemTools::ConvertToUnixSlashes(filename);
          if (cmSystemTools::FileIsFullPath(filename)) {
            filename = cmSystemTools::CollapseFullPath(filename);
          }
          byproducts.push_back(filename);
        } break;
        case doing_depends: {
          std::string dep = copy;
          cmSystemTools::ConvertToUnixSlashes(dep);
          depends.push_back(std::move(dep));
        } break;
        case doing_comment:
          comment_buffer = copy;
          comment = comment_buffer.c_str();
          break;
        case doing_source:
          sources.push_back(copy);
          break;
        case doing_job_pool:
          job_pool = copy;
          break;
        case doing_job_server_aware:
          job_server_aware = copy;
          break;
        default:
          status.SetError("Wrong syntax. Unknown type of argument.");
          return false;
      }
    }
  }

  bool nameOk = cmGeneratorExpression::IsValidTargetName(targetName) &&
    !cmGlobalGenerator::IsReservedTarget(targetName) && targetName.find(':') == std::string::npos;
  if (!nameOk) {
    mf.IssueInvalidTargetNameError(targetName);
    return false;
  }

  // Store the last command line finished.
  if (!currentLine.empty()) {
    commandLines.push_back(currentLine);
    currentLine.clear();
  }

  // Enforce name uniqueness.
  {
    std::string msg;
    if (!mf.EnforceUniqueName(targetName, msg, true)) {
      status.SetError(msg);
      return false;
    }
  }

  if (commandLines.empty() && !byproducts.empty()) {
    mf.IssueMessage(MessageType::FATAL_ERROR, "BYPRODUCTS may not be specified without any COMMAND");
    return true;
  }
  if (commandLines.empty() && uses_terminal) {
    mf.IssueMessage(MessageType::FATAL_ERROR, "USES_TERMINAL may not be specified without any COMMAND");
    return true;
  }
  if (commandLines.empty() && command_expand_lists) {
    mf.IssueMessage(MessageType::FATAL_ERROR, "COMMAND_EXPAND_LISTS may not be specified without any COMMAND");
    return true;
  }

  if (uses_terminal && !job_pool.empty()) {
    status.SetError("JOB_POOL is shadowed by USES_TERMINAL.");
    return false;
  }

  // Add the utility target to the makefile.
  auto m_pCustomCommand = cm::make_unique<cmCustomCommand>();
  m_pCustomCommand->SetWorkingDirectory(working_directory.c_str());
  m_pCustomCommand->SetByproducts(byproducts);
  m_pCustomCommand->SetDepends(depends);
  m_pCustomCommand->SetCommandLines(commandLines);
  m_pCustomCommand->SetEscapeOldStyle(!verbatim);
  m_pCustomCommand->SetComment(comment);
  m_pCustomCommand->SetUsesTerminal(uses_terminal);
  m_pCustomCommand->SetCommandExpandLists(command_expand_lists);
  m_pCustomCommand->SetJobPool(job_pool);
  m_pCustomCommand->SetJobserverAware(cmIsOn(job_server_aware));
  cmTarget* target = mf.AddUtilityCommand(targetName, excludeFromAll, std::move(m_pCustomCommand));

  // Add additional user-specified source files to the target.
  target->AddSources(sources);

  return true;
}

bool cmAddDefinitionsCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status)
{
  cmMakefile& mf = status.GetMakefile();
  for (std::string const& i : args) {
    mf.AddDefineFlag(i);
  }
  return true;
}

bool cmAddDependenciesCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status)
{
  if (args.empty()) {
    status.SetError("called with incorrect number of arguments");
    return false;
  }

  cmMakefile& mf = status.GetMakefile();
  std::string const& target_name = args[0];
  if (mf.IsAlias(target_name)) {
    mf.IssueMessage(
      MessageType::FATAL_ERROR,
      cmStrCat("Cannot add target-level dependencies to alias target \"", target_name, "\".\n"));
  }
  if (cmTarget* target = mf.FindTargetToUse(target_name)) {

    // skip over target_name
    for (std::string const& arg : cmMakeRange(args).advance(1)) {
      target->AddUtility(arg, false, &mf);
      target->AddCodegenDependency(arg);
    }
  } else {
    mf.IssueMessage(
      MessageType::FATAL_ERROR,
      cmStrCat(
        "Cannot add target-level dependencies to non-existent "
        "target \"",
        target_name,
        "\".\nThe add_dependencies works for "
        "top-level logical targets created by the add_executable, "
        "add_library, or add_custom_target commands.  If you want to add "
        "file-level dependencies see the DEPENDS option of the "
        "add_custom_target and add_custom_command commands."));
  }

  return true;
}

bool cmAddExecutableCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status)
{
  if (args.empty()) {
    status.SetError("called with incorrect number of arguments");
    return false;
  }

  cmMakefile& mf = status.GetMakefile();
  auto s = args.begin();

  std::string const& exename = *s;

  ++s;
  bool use_win32 = false;
  bool use_macbundle = false;
  bool excludeFromAll = false;
  bool importTarget = false;
  bool importGlobal = false;
  bool isAlias = false;
  while (s != args.end()) {
    if (*s == "WIN32") {
      ++s;
      use_win32 = true;
    } else if (*s == "MACOSX_BUNDLE") {
      ++s;
      use_macbundle = true;
    } else if (*s == "EXCLUDE_FROM_ALL") {
      ++s;
      excludeFromAll = true;
    } else if (*s == "IMPORTED") {
      ++s;
      importTarget = true;
    } else if (importTarget && *s == "GLOBAL") {
      ++s;
      importGlobal = true;
    } else if (*s == "ALIAS") {
      ++s;
      isAlias = true;
    } else {
      break;
    }
  }

  if (importTarget && !importGlobal) {
    importGlobal = mf.IsImportedTargetGlobalScope();
  }

  bool nameOk = cmGeneratorExpression::IsValidTargetName(exename) && !cmGlobalGenerator::IsReservedTarget(exename);

  if (nameOk && !importTarget && !isAlias) {
    nameOk = exename.find(':') == std::string::npos;
  }
  if (!nameOk) {
    mf.IssueInvalidTargetNameError(exename);
    return false;
  }

  // Special modifiers are not allowed with IMPORTED signature.
  if (importTarget && (use_win32 || use_macbundle || excludeFromAll)) {
    if (use_win32) {
      status.SetError("may not be given WIN32 for an IMPORTED target.");
    } else if (use_macbundle) {
      status.SetError("may not be given MACOSX_BUNDLE for an IMPORTED target.");
    } else // if(excludeFromAll)
    {
      status.SetError("may not be given EXCLUDE_FROM_ALL for an IMPORTED target.");
    }
    return false;
  }
  if (isAlias) {
    if (!cmGeneratorExpression::IsValidTargetName(exename)) {
      status.SetError("Invalid name for ALIAS: " + exename);
      return false;
    }
    if (excludeFromAll) {
      status.SetError("EXCLUDE_FROM_ALL with ALIAS makes no sense.");
      return false;
    }
    if (importTarget || importGlobal) {
      status.SetError("IMPORTED with ALIAS is not allowed.");
      return false;
    }
    if (args.size() != 3) {
      status.SetError("ALIAS requires exactly one target argument.");
      return false;
    }

    std::string const& aliasedName = *s;
    if (mf.IsAlias(aliasedName)) {
      status.SetError(cmStrCat(
        "cannot create ALIAS target \"", exename, "\" because target \"", aliasedName, "\" is itself an ALIAS."));
      return false;
    }
    cmTarget* aliasedTarget = mf.FindTargetToUse(aliasedName, { cmStateEnums::TargetDomain::NATIVE });
    if (!aliasedTarget) {
      status.SetError(cmStrCat(
        "cannot create ALIAS target \"", exename, "\" because target \"", aliasedName, "\" does not already exist."));
      return false;
    }
    cmStateEnums::TargetType type = aliasedTarget->GetType();
    if (type != cmStateEnums::EXECUTABLE) {
      status.SetError(cmStrCat(
        "cannot create ALIAS target \"", exename, "\" because target \"", aliasedName, "\" is not an executable."));
      return false;
    }
    mf.AddAlias(exename, aliasedName, !aliasedTarget->IsImported() || aliasedTarget->IsImportedGloballyVisible());
    return true;
  }

  // Handle imported target creation.
  if (importTarget) {
    // Make sure the target does not already exist.
    if (mf.FindTargetToUse(exename)) {
      status.SetError(cmStrCat(
        "cannot create imported target \"", exename, "\" because another target with the same name already exists."));
      return false;
    }

    // Create the imported target.
    mf.AddImportedTarget(exename, cmStateEnums::EXECUTABLE, importGlobal);
    return true;
  }

  // Enforce name uniqueness.
  {
    std::string msg;
    if (!mf.EnforceUniqueName(exename, msg)) {
      status.SetError(msg);
      return false;
    }
  }

  std::vector<std::string> srclists(s, args.end());
  cmTarget* tgt = mf.AddExecutable(exename, srclists, excludeFromAll);
  if (use_win32) {
    tgt->SetProperty("WIN32_EXECUTABLE", "ON");
  }
  if (use_macbundle) {
    tgt->SetProperty("MACOSX_BUNDLE", "ON");
  }

  return true;
}

bool cmAddLibraryCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status)
{
  if (args.empty()) {
    status.SetError("called with incorrect number of arguments");
    return false;
  }

  cmMakefile& mf = status.GetMakefile();
  // Library type defaults to value of BUILD_SHARED_LIBS, if it exists,
  // otherwise it defaults to static library.
  cmStateEnums::TargetType type = cmStateEnums::SHARED_LIBRARY;
  if (mf.GetDefinition("BUILD_SHARED_LIBS").IsOff()) {
    type = cmStateEnums::STATIC_LIBRARY;
  }
  bool excludeFromAll = false;
  bool importTarget = false;
  bool importGlobal = false;

  auto s = args.begin();

  std::string const& libName = *s;

  ++s;

  // If the second argument is "SHARED" or "STATIC", then it controls
  // the type of library.  Otherwise, it is treated as a source or
  // source list name. There may be two keyword arguments, check for them
  bool haveSpecifiedType = false;
  bool isAlias = false;
  while (s != args.end()) {
    std::string libType = *s;
    if (libType == "STATIC") {
      if (type == cmStateEnums::INTERFACE_LIBRARY) {
        status.SetError("INTERFACE library specified with conflicting STATIC type.");
        return false;
      }
      ++s;
      type = cmStateEnums::STATIC_LIBRARY;
      haveSpecifiedType = true;
    } else if (libType == "SHARED") {
      if (type == cmStateEnums::INTERFACE_LIBRARY) {
        status.SetError("INTERFACE library specified with conflicting SHARED type.");
        return false;
      }
      ++s;
      type = cmStateEnums::SHARED_LIBRARY;
      haveSpecifiedType = true;
    } else if (libType == "MODULE") {
      if (type == cmStateEnums::INTERFACE_LIBRARY) {
        status.SetError("INTERFACE library specified with conflicting MODULE type.");
        return false;
      }
      ++s;
      type = cmStateEnums::MODULE_LIBRARY;
      haveSpecifiedType = true;
    } else if (libType == "OBJECT") {
      if (type == cmStateEnums::INTERFACE_LIBRARY) {
        status.SetError("INTERFACE library specified with conflicting OBJECT type.");
        return false;
      }
      ++s;
      type = cmStateEnums::OBJECT_LIBRARY;
      haveSpecifiedType = true;
    } else if (libType == "UNKNOWN") {
      if (type == cmStateEnums::INTERFACE_LIBRARY) {
        status.SetError("INTERFACE library specified with conflicting UNKNOWN type.");
        return false;
      }
      ++s;
      type = cmStateEnums::UNKNOWN_LIBRARY;
      haveSpecifiedType = true;
    } else if (libType == "ALIAS") {
      if (type == cmStateEnums::INTERFACE_LIBRARY) {
        status.SetError("INTERFACE library specified with conflicting ALIAS type.");
        return false;
      }
      ++s;
      isAlias = true;
    } else if (libType == "INTERFACE") {
      if (haveSpecifiedType) {
        status.SetError("INTERFACE library specified with conflicting/multiple types.");
        return false;
      }
      if (isAlias) {
        status.SetError("INTERFACE library specified with conflicting ALIAS type.");
        return false;
      }
      ++s;
      type = cmStateEnums::INTERFACE_LIBRARY;
      haveSpecifiedType = true;
    } else if (*s == "EXCLUDE_FROM_ALL") {
      ++s;
      excludeFromAll = true;
    } else if (*s == "IMPORTED") {
      ++s;
      importTarget = true;
    } else if (importTarget && *s == "GLOBAL") {
      ++s;
      importGlobal = true;
    } else if (type == cmStateEnums::INTERFACE_LIBRARY && *s == "GLOBAL") {
      status.SetError("GLOBAL option may only be used with IMPORTED libraries.");
      return false;
    } else {
      break;
    }
  }

  if (importTarget && !importGlobal) {
    importGlobal = mf.IsImportedTargetGlobalScope();
  }

  if (type == cmStateEnums::INTERFACE_LIBRARY) {
    if (importGlobal && !importTarget) {
      status.SetError("INTERFACE library specified as GLOBAL, but not as IMPORTED.");
      return false;
    }
  }

  bool nameOk = cmGeneratorExpression::IsValidTargetName(libName) && !cmGlobalGenerator::IsReservedTarget(libName);

  if (nameOk && !importTarget && !isAlias) {
    nameOk = libName.find(':') == std::string::npos;
  }
  if (!nameOk) {
    mf.IssueInvalidTargetNameError(libName);
    return false;
  }

  if (isAlias) {
    if (!cmGeneratorExpression::IsValidTargetName(libName)) {
      status.SetError("Invalid name for ALIAS: " + libName);
      return false;
    }
    if (excludeFromAll) {
      status.SetError("EXCLUDE_FROM_ALL with ALIAS makes no sense.");
      return false;
    }
    if (importTarget || importGlobal) {
      status.SetError("IMPORTED with ALIAS is not allowed.");
      return false;
    }
    if (args.size() != 3) {
      status.SetError("ALIAS requires exactly one target argument.");
      return false;
    }

    if (mf.GetPolicyStatus(cmPolicies::CMP0107) == cmPolicies::NEW) {
      // Make sure the target does not already exist.
      if (mf.FindTargetToUse(libName)) {
        status.SetError(cmStrCat(
          "cannot create ALIAS target \"", libName, "\" because another target with the same name already exists."));
        return false;
      }
    }

    std::string const& aliasedName = *s;
    if (mf.IsAlias(aliasedName)) {
      status.SetError(cmStrCat(
        "cannot create ALIAS target \"", libName, "\" because target \"", aliasedName, "\" is itself an ALIAS."));
      return false;
    }
    cmTarget* aliasedTarget = mf.FindTargetToUse(aliasedName, { cmStateEnums::TargetDomain::NATIVE });
    if (!aliasedTarget) {
      status.SetError(cmStrCat(
        "cannot create ALIAS target \"", libName, "\" because target \"", aliasedName, "\" does not already exist."));
      return false;
    }
    cmStateEnums::TargetType aliasedType = aliasedTarget->GetType();
    if (
      aliasedType != cmStateEnums::SHARED_LIBRARY && aliasedType != cmStateEnums::STATIC_LIBRARY &&
      aliasedType != cmStateEnums::MODULE_LIBRARY && aliasedType != cmStateEnums::OBJECT_LIBRARY &&
      aliasedType != cmStateEnums::INTERFACE_LIBRARY &&
      !(aliasedType == cmStateEnums::UNKNOWN_LIBRARY && aliasedTarget->IsImported())) {
      status.SetError(cmStrCat(
        "cannot create ALIAS target \"", libName, "\" because target \"", aliasedName, "\" is not a library."));
      return false;
    }
    mf.AddAlias(libName, aliasedName, !aliasedTarget->IsImported() || aliasedTarget->IsImportedGloballyVisible());
    return true;
  }

  if (importTarget && excludeFromAll) {
    status.SetError("excludeFromAll with IMPORTED target makes no sense.");
    return false;
  }

  /* ideally we should check whether for the linker language of the target
    CMAKE_${LANG}_CREATE_SHARED_LIBRARY is defined and if not default to
    STATIC. But at this point we know only the name of the target, but not
    yet its linker language. */
  if (
    (type == cmStateEnums::SHARED_LIBRARY || type == cmStateEnums::MODULE_LIBRARY) &&
    !mf.GetState()->GetGlobalPropertyAsBool("TARGET_SUPPORTS_SHARED_LIBS")) {
    switch (status.GetMakefile().GetPolicyStatus(cmPolicies::CMP0164)) {
      case cmPolicies::WARN:
        mf.IssueMessage(
          MessageType::AUTHOR_WARNING,
          cmStrCat(
            "ADD_LIBRARY called with ", (type == cmStateEnums::SHARED_LIBRARY ? "SHARED" : "MODULE"),
            " option but the target platform does not support dynamic "
            "linking. ",
            "Building a STATIC library instead. This may lead to problems."));
        CM_FALLTHROUGH;
      case cmPolicies::OLD:
        type = cmStateEnums::STATIC_LIBRARY;
        break;
      case cmPolicies::NEW:
        mf.IssueMessage(
          MessageType::FATAL_ERROR,
          cmStrCat(
            "ADD_LIBRARY called with ", (type == cmStateEnums::SHARED_LIBRARY ? "SHARED" : "MODULE"),
            " option but the target platform does not support dynamic "
            "linking."));
        cmSystemTools::SetFatalErrorOccurred();
        return false;
      default:
        break;
    }
  }

  // Handle imported target creation.
  if (importTarget) {
    // The IMPORTED signature requires a type to be specified explicitly.
    if (!haveSpecifiedType) {
      status.SetError("called with IMPORTED argument but no library type.");
      return false;
    }
    if (type == cmStateEnums::INTERFACE_LIBRARY) {
      if (!cmGeneratorExpression::IsValidTargetName(libName)) {
        status.SetError(cmStrCat("Invalid name for IMPORTED INTERFACE library target: ", libName));
        return false;
      }
    }

    // Make sure the target does not already exist.
    if (mf.FindTargetToUse(libName)) {
      status.SetError(cmStrCat(
        "cannot create imported target \"", libName, "\" because another target with the same name already exists."));
      return false;
    }

    // Create the imported target.
    mf.AddImportedTarget(libName, type, importGlobal);
    return true;
  }

  // A non-imported target may not have UNKNOWN type.
  if (type == cmStateEnums::UNKNOWN_LIBRARY) {
    mf.IssueMessage(MessageType::FATAL_ERROR, "The UNKNOWN library type may be used only for IMPORTED libraries.");
    return true;
  }

  // Enforce name uniqueness.
  {
    std::string msg;
    if (!mf.EnforceUniqueName(libName, msg)) {
      status.SetError(msg);
      return false;
    }
  }

  if (type == cmStateEnums::INTERFACE_LIBRARY) {
    if (!cmGeneratorExpression::IsValidTargetName(libName) || libName.find("::") != std::string::npos) {
      status.SetError(cmStrCat("Invalid name for INTERFACE library target: ", libName));
      return false;
    }
  }

  std::vector<std::string> srcs(s, args.end());
  mf.AddLibrary(libName, type, srcs, excludeFromAll);

  return true;
}

bool cmAddLinkOptionsCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status)
{
  cmMakefile& mf = status.GetMakefile();
  for (std::string const& i : args) {
    mf.AddLinkOption(i);
  }
  return true;
}

bool cmAddSubDirectoryCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status)
{
  if (args.empty()) {
    status.SetError("called with incorrect number of arguments");
    return false;
  }

  cmMakefile& mf = status.GetMakefile();
  // store the binpath
  std::string const& srcArg = args.front();
  std::string binArg;

  bool excludeFromAll = false;
  bool system = false;

  // process the rest of the arguments looking for optional args
  for (std::string const& arg : cmMakeRange(args).advance(1)) {
    if (arg == "EXCLUDE_FROM_ALL") {
      excludeFromAll = true;
      continue;
    }
    if (arg == "SYSTEM") {
      system = true;
      continue;
    }
    if (binArg.empty()) {
      binArg = arg;
    } else {
      status.SetError("called with incorrect number of arguments");
      return false;
    }
  }
  // "SYSTEM" directory property should also affects targets in nested
  // subdirectories.
  if (mf.GetPropertyAsBool("SYSTEM")) {
    system = true;
  }

  // Compute the full path to the specified source directory.
  // Interpret a relative path with respect to the current source directory.
  std::string srcPath;
  if (cmSystemTools::FileIsFullPath(srcArg)) {
    srcPath = srcArg;
  } else {
    srcPath = cmStrCat(mf.GetCurrentSourceDirectory(), '/', srcArg);
  }
  if (!cmSystemTools::FileIsDirectory(srcPath)) {
    std::string error = cmStrCat("given source \"", srcArg, "\" which is not an existing directory.");
    status.SetError(error);
    return false;
  }
  srcPath = cmSystemTools::CollapseFullPath(srcPath, mf.GetHomeOutputDirectory());

  // Compute the full path to the binary directory.
  std::string binPath;
  if (binArg.empty()) {
    // No binary directory was specified.  If the source directory is
    // not a subdirectory of the current directory then it is an
    // error.
    if (!cmSystemTools::IsSubDirectory(srcPath, mf.GetCurrentSourceDirectory())) {
      status.SetError(cmStrCat(
        "not given a binary directory but the given source ", "directory \"", srcPath, "\" is not a subdirectory of \"",
        mf.GetCurrentSourceDirectory(),
        "\".  When specifying an "
        "out-of-tree source a binary directory must be explicitly "
        "specified."));
      return false;
    }

    // Remove the CurrentDirectory from the srcPath and replace it
    // with the CurrentOutputDirectory.
    std::string const& src = mf.GetCurrentSourceDirectory();
    std::string const& bin = mf.GetCurrentBinaryDirectory();
    size_t srcLen = src.length();
    size_t binLen = bin.length();
    if (srcLen > 0 && src.back() == '/') {
      --srcLen;
    }
    if (binLen > 0 && bin.back() == '/') {
      --binLen;
    }
    binPath = cmStrCat(cm::string_view(bin).substr(0, binLen), cm::string_view(srcPath).substr(srcLen));
  } else {
    // Use the binary directory specified.
    // Interpret a relative path with respect to the current binary directory.
    if (cmSystemTools::FileIsFullPath(binArg)) {
      binPath = binArg;
    } else {
      binPath = cmStrCat(mf.GetCurrentBinaryDirectory(), '/', binArg);
    }
  }
  binPath = cmSystemTools::CollapseFullPath(binPath);

  // Add the subdirectory using the computed full paths.
  mf.AddSubDirectory(srcPath, binPath, excludeFromAll, true, system);

  return true;
}

static std::string const keywordCMP0178 = "__CMP0178";

static bool cmAddTestCommandHandleNameMode(
  std::vector<std::string> const& args,
  cmExecutionStatus& status);

bool cmAddTestCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status)
{
  if (!args.empty() && args[0] == "NAME") {
    return cmAddTestCommandHandleNameMode(args, status);
  }

  // First argument is the name of the test Second argument is the name of
  // the executable to run (a target or external program) Remaining arguments
  // are the arguments to pass to the executable
  if (args.size() < 2) {
    status.SetError("called with incorrect number of arguments");
    return false;
  }

  cmMakefile& mf = status.GetMakefile();
  cmPolicies::PolicyStatus cmp0178;

  // If the __CMP0178 keyword is present, it is always at the end
  auto endOfCommandIter = std::find(args.begin() + 2, args.end(), keywordCMP0178);
  if (endOfCommandIter != args.end()) {
    auto cmp0178Iter = endOfCommandIter + 1;
    if (cmp0178Iter == args.end()) {
      status.SetError(cmStrCat(keywordCMP0178, " keyword missing value"));
      return false;
    }
    if (*cmp0178Iter == "NEW") {
      cmp0178 = cmPolicies::PolicyStatus::NEW;
    } else if (*cmp0178Iter == "OLD") {
      cmp0178 = cmPolicies::PolicyStatus::OLD;
    } else {
      cmp0178 = cmPolicies::PolicyStatus::WARN;
    }
  } else {
    cmp0178 = mf.GetPolicyStatus(cmPolicies::CMP0178);
  }

  // Collect the command with arguments.
  std::vector<std::string> command(args.begin() + 1, endOfCommandIter);

  // Create the test but add a generator only the first time it is
  // seen.  This preserves behavior from before test generators.
  cmTest* test = mf.GetTest(args[0]);
  if (test) {
    // If the test was already added by a new-style signature do not
    // allow it to be duplicated.
    if (!test->GetOldStyle()) {
      status.SetError(cmStrCat(" given test name \"", args[0], "\" which already exists in this directory."));
      return false;
    }
  } else {
    test = mf.CreateTest(args[0]);
    test->SetOldStyle(true);
    test->SetCMP0178(cmp0178);
    mf.AddTestGenerator(cm::make_unique<cmTestGenerator>(test));
  }
  test->SetCommand(command);

  return true;
}

bool cmAddTestCommandHandleNameMode(
  std::vector<std::string> const& args,
  cmExecutionStatus& status)
{
  cmMakefile& mf = status.GetMakefile();

  std::string name;
  std::vector<std::string> configurations;
  std::string working_directory;
  std::vector<std::string> command;
  bool command_expand_lists = false;
  cmPolicies::PolicyStatus cmp0178 = mf.GetPolicyStatus(cmPolicies::CMP0178);

  // Read the arguments.
  enum Doing
  {
    DoingName,
    DoingCommand,
    DoingConfigs,
    DoingWorkingDirectory,
    DoingCmp0178,
    DoingNone
  };
  Doing doing = DoingName;
  for (unsigned int i = 1; i < args.size(); ++i) {
    if (args[i] == "COMMAND") {
      if (!command.empty()) {
        status.SetError(" may be given at most one COMMAND.");
        return false;
      }
      doing = DoingCommand;
    } else if (args[i] == "CONFIGURATIONS") {
      if (!configurations.empty()) {
        status.SetError(" may be given at most one set of CONFIGURATIONS.");
        return false;
      }
      doing = DoingConfigs;
    } else if (args[i] == "WORKING_DIRECTORY") {
      if (!working_directory.empty()) {
        status.SetError(" may be given at most one WORKING_DIRECTORY.");
        return false;
      }
      doing = DoingWorkingDirectory;
    } else if (args[i] == keywordCMP0178) {
      doing = DoingCmp0178;
    } else if (args[i] == "COMMAND_EXPAND_LISTS") {
      if (command_expand_lists) {
        status.SetError(" may be given at most one COMMAND_EXPAND_LISTS.");
        return false;
      }
      command_expand_lists = true;
      doing = DoingNone;
    } else if (doing == DoingName) {
      name = args[i];
      doing = DoingNone;
    } else if (doing == DoingCommand) {
      command.push_back(args[i]);
    } else if (doing == DoingConfigs) {
      configurations.push_back(args[i]);
    } else if (doing == DoingWorkingDirectory) {
      working_directory = args[i];
      doing = DoingNone;
    } else if (doing == DoingCmp0178) {
      if (args[i] == "NEW") {
        cmp0178 = cmPolicies::PolicyStatus::NEW;
      } else if (args[i] == "OLD") {
        cmp0178 = cmPolicies::PolicyStatus::OLD;
      } else {
        cmp0178 = cmPolicies::PolicyStatus::WARN;
      }
      doing = DoingNone;
    } else {
      status.SetError(cmStrCat(" given unknown argument:\n  ", args[i], "\n"));
      return false;
    }
  }

  // Require a test name.
  if (name.empty()) {
    status.SetError(" must be given non-empty NAME.");
    return false;
  }

  // Require a command.
  if (command.empty()) {
    status.SetError(" must be given non-empty COMMAND.");
    return false;
  }

  // Require a unique test name within the directory.
  if (mf.GetTest(name)) {
    status.SetError(cmStrCat(" given test NAME \"", name, "\" which already exists in this directory."));
    return false;
  }

  // Add the test.
  cmTest* test = mf.CreateTest(name);
  test->SetOldStyle(false);
  test->SetCMP0178(cmp0178);
  test->SetCommand(command);
  if (!working_directory.empty()) {
    test->SetProperty("WORKING_DIRECTORY", working_directory);
  }
  test->SetCommandExpandLists(command_expand_lists);
  mf.AddTestGenerator(cm::make_unique<cmTestGenerator>(test, configurations));

  return true;
}
