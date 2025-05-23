/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing#kwsys for details.  */
#include "kwsysPrivate.h"
#include KWSYS_HEADER(CommandLineArguments.hxx)

#include KWSYS_HEADER(Configure.hxx)

// Work-around CMake dependency scanning limitation.  This must
// duplicate the above list of headers.
#if 0
#  include "CommandLineArguments.hxx.in"
#  include "Configure.hxx.in"
#endif

#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _MSC_VER
#  pragma warning(disable : 4786)
#endif

#if defined(__sgi) && !defined(__GNUC__)
#  pragma set woff 1375 /* base class destructor not virtual */
#endif

#if 0
#  define CommandLineArguments_DEBUG(x)                                       \
    std::cout << __LINE__ << " CLA: " << x << std::endl
#else
#  define CommandLineArguments_DEBUG(x)
#endif

namespace KWSYS_NAMESPACE {

struct CommandLineArgumentsCallbackStructure
{
  char const* m_argument;
  int ArgumentType;
  CommandLineArguments::CallbackType Callback;
  void* CallData;
  void* Variable;
  int VariableType;
  char const* Help;
};

class CommandLineArgumentsVectorOfStrings : public std::vector<std::string>
{
};
class CommandLineArgumentsSetOfStrings : public std::set<std::string>
{
};
class CommandLineArgumentsMapOfStrucs
  : public std::map<std::string, CommandLineArgumentsCallbackStructure>
{
};

class CommandLineArgumentsInternal
{
public:
  CommandLineArgumentsInternal() = default;

  using VectorOfStrings = CommandLineArgumentsVectorOfStrings;
  using CallbacksMap = CommandLineArgumentsMapOfStrucs;
  using String = std::string;
  using SetOfStrings = CommandLineArgumentsSetOfStrings;

  VectorOfStrings Argv;
  String Argv0;
  CallbacksMap Callbacks;

  CommandLineArguments::ErrorCallbackType UnknownArgumentCallback{ nullptr };
  void* ClientData{ nullptr };

  VectorOfStrings::size_type LastArgument{ 0 };

  VectorOfStrings UnusedArguments;
};

CommandLineArguments::CommandLineArguments()
{
  this->Internals = new CommandLineArguments::Internal;
  this->Help = "";
  this->LineLength = 80;
  this->StoreUnusedArgumentsFlag = false;
}

CommandLineArguments::~CommandLineArguments()
{
  delete this->Internals;
}

void CommandLineArguments::Initialize(int argc, char const* const argv[])
{
  int m_pCustomCommand;

  this->Initialize();
  this->Internals->Argv0 = argv[0];
  for (m_pCustomCommand = 1; m_pCustomCommand < argc; m_pCustomCommand++) {
    this->ProcessArgument(argv[m_pCustomCommand]);
  }
}

void CommandLineArguments::Initialize(int argc, char* argv[])
{
  this->Initialize(argc, static_cast<char const* const*>(argv));
}

void CommandLineArguments::Initialize()
{
  this->Internals->Argv.clear();
  this->Internals->LastArgument = 0;
}

void CommandLineArguments::ProcessArgument(char const* arg)
{
  this->Internals->Argv.push_back(arg);
}

bool CommandLineArguments::GetMatchedArguments(
  std::vector<std::string>* matches, std::string const& arg)
{
  matches->clear();
  CommandLineArguments::Internal::CallbacksMap::iterator it;

  // Does the argument match to any we know about?
  for (it = this->Internals->Callbacks.begin();
       it != this->Internals->Callbacks.end(); ++it) {
    CommandLineArguments::Internal::String const& parg = it->first;
    CommandLineArgumentsCallbackStructure* cs = &it->second;
    if (cs->ArgumentType == CommandLineArguments::NO_ARGUMENT ||
        cs->ArgumentType == CommandLineArguments::SPACE_ARGUMENT) {
      if (arg == parg) {
        matches->push_back(parg);
      }
    } else if (arg.find(parg) == 0) {
      matches->push_back(parg);
    }
  }
  return !matches->empty();
}

int CommandLineArguments::Parse()
{
  std::vector<std::string>::size_type m_pCustomCommand;
  std::vector<std::string> matches;
  if (this->StoreUnusedArgumentsFlag) {
    this->Internals->UnusedArguments.clear();
  }
  for (m_pCustomCommand = 0; m_pCustomCommand < this->Internals->Argv.size(); m_pCustomCommand++) {
    std::string const& arg = this->Internals->Argv[m_pCustomCommand];
    CommandLineArguments_DEBUG("Process argument: " << arg);
    this->Internals->LastArgument = m_pCustomCommand;
    if (this->GetMatchedArguments(&matches, arg)) {
      // Ok, we found one or more arguments that match what user specified.
      // Let's find the longest one.
      CommandLineArguments::Internal::VectorOfStrings::size_type kk;
      CommandLineArguments::Internal::VectorOfStrings::size_type maxidx = 0;
      CommandLineArguments::Internal::String::size_type maxlen = 0;
      for (kk = 0; kk < matches.size(); kk++) {
        if (matches[kk].size() > maxlen) {
          maxlen = matches[kk].size();
          maxidx = kk;
        }
      }
      // So, the longest one is probably the right one. Now see if it has any
      // additional value
      CommandLineArgumentsCallbackStructure* cs =
        &this->Internals->Callbacks[matches[maxidx]];
      std::string const& sarg = matches[maxidx];
      if (cs->m_argument != sarg) {
        abort();
      }
      switch (cs->ArgumentType) {
        case NO_ARGUMENT:
          // No value
          if (!this->PopulateVariable(cs, nullptr)) {
            return 0;
          }
          break;
        case SPACE_ARGUMENT:
          if (m_pCustomCommand == this->Internals->Argv.size() - 1) {
            this->Internals->LastArgument--;
            return 0;
          }
          CommandLineArguments_DEBUG("This is a space argument: "
                                     << arg << " value: "
                                     << this->Internals->Argv[m_pCustomCommand + 1]);
          // Value is the next argument
          if (!this->PopulateVariable(cs,
                                      this->Internals->Argv[m_pCustomCommand + 1].c_str())) {
            return 0;
          }
          m_pCustomCommand++;
          break;
        case EQUAL_ARGUMENT:
          if (arg.size() == sarg.size() || arg.at(sarg.size()) != '=') {
            this->Internals->LastArgument--;
            return 0;
          }
          // Value is everythng followed the '=' sign
          if (!this->PopulateVariable(cs, arg.c_str() + sarg.size() + 1)) {
            return 0;
          }
          break;
        case CONCAT_ARGUMENT:
          // Value is whatever follows the argument
          if (!this->PopulateVariable(cs, arg.c_str() + sarg.size())) {
            return 0;
          }
          break;
        case MULTI_ARGUMENT:
          // Suck in all the rest of the arguments
          CommandLineArguments_DEBUG("This is a multi argument: " << arg);
          for (m_pCustomCommand++; m_pCustomCommand < this->Internals->Argv.size(); ++m_pCustomCommand) {
            std::string const& marg = this->Internals->Argv[m_pCustomCommand];
            CommandLineArguments_DEBUG(
              " check multi argument value: " << marg);
            if (this->GetMatchedArguments(&matches, marg)) {
              CommandLineArguments_DEBUG("End of multi argument "
                                         << arg << " with value: " << marg);
              break;
            }
            CommandLineArguments_DEBUG(
              " populate multi argument value: " << marg);
            if (!this->PopulateVariable(cs, marg.c_str())) {
              return 0;
            }
          }
          if (m_pCustomCommand != this->Internals->Argv.size()) {
            CommandLineArguments_DEBUG("Again End of multi argument " << arg);
            m_pCustomCommand--;
            continue;
          }
          break;
        default:
          std::cerr << "Got unknown argument type: \"" << cs->ArgumentType
                    << "\"" << std::endl;
          this->Internals->LastArgument--;
          return 0;
      }
    } else {
      // Handle unknown arguments
      if (this->Internals->UnknownArgumentCallback) {
        if (!this->Internals->UnknownArgumentCallback(
              arg.c_str(), this->Internals->ClientData)) {
          this->Internals->LastArgument--;
          return 0;
        }
        return 1;
      } else if (this->StoreUnusedArgumentsFlag) {
        CommandLineArguments_DEBUG("Store unused argument " << arg);
        this->Internals->UnusedArguments.push_back(arg);
      } else {
        std::cerr << "Got unknown argument: \"" << arg << "\"" << std::endl;
        this->Internals->LastArgument--;
        return 0;
      }
    }
  }
  return 1;
}

void CommandLineArguments::GetRemainingArguments(int* argc, char*** argv)
{
  CommandLineArguments::Internal::VectorOfStrings::size_type size =
    this->Internals->Argv.size() - this->Internals->LastArgument + 1;
  CommandLineArguments::Internal::VectorOfStrings::size_type m_pCustomCommand;

  // Copy Argv0 as the first argument
  char** args = new char*[size];
  args[0] = new char[this->Internals->Argv0.size() + 1];
  strcpy(args[0], this->Internals->Argv0.c_str());
  int cnt = 1;

  // Copy everything after the LastArgument, since that was not parsed.
  for (m_pCustomCommand = this->Internals->LastArgument + 1;
       m_pCustomCommand < this->Internals->Argv.size(); m_pCustomCommand++) {
    args[cnt] = new char[this->Internals->Argv[m_pCustomCommand].size() + 1];
    strcpy(args[cnt], this->Internals->Argv[m_pCustomCommand].c_str());
    cnt++;
  }
  *argc = cnt;
  *argv = args;
}

void CommandLineArguments::GetUnusedArguments(int* argc, char*** argv)
{
  CommandLineArguments::Internal::VectorOfStrings::size_type size =
    this->Internals->UnusedArguments.size() + 1;
  CommandLineArguments::Internal::VectorOfStrings::size_type m_pCustomCommand;

  // Copy Argv0 as the first argument
  char** args = new char*[size];
  args[0] = new char[this->Internals->Argv0.size() + 1];
  strcpy(args[0], this->Internals->Argv0.c_str());
  int cnt = 1;

  // Copy everything after the LastArgument, since that was not parsed.
  for (m_pCustomCommand = 0; m_pCustomCommand < this->Internals->UnusedArguments.size(); m_pCustomCommand++) {
    std::string& str = this->Internals->UnusedArguments[m_pCustomCommand];
    args[cnt] = new char[str.size() + 1];
    strcpy(args[cnt], str.c_str());
    cnt++;
  }
  *argc = cnt;
  *argv = args;
}

void CommandLineArguments::DeleteRemainingArguments(int argc, char*** argv)
{
  int m_pCustomCommand;
  for (m_pCustomCommand = 0; m_pCustomCommand < argc; ++m_pCustomCommand) {
    delete[] (*argv)[m_pCustomCommand];
  }
  delete[] *argv;
}

void CommandLineArguments::AddCallback(char const* argument,
                                       ArgumentTypeEnum type,
                                       CallbackType callback, void* call_data,
                                       char const* help)
{
  CommandLineArgumentsCallbackStructure s;
  s.m_argument = argument;
  s.ArgumentType = type;
  s.Callback = callback;
  s.CallData = call_data;
  s.VariableType = CommandLineArguments::NO_VARIABLE_TYPE;
  s.Variable = nullptr;
  s.Help = help;

  this->Internals->Callbacks[argument] = s;
  this->GenerateHelp();
}

void CommandLineArguments::AddArgument(char const* argument,
                                       ArgumentTypeEnum type,
                                       VariableTypeEnum vtype, void* variable,
                                       char const* help)
{
  CommandLineArgumentsCallbackStructure s;
  s.m_argument = argument;
  s.ArgumentType = type;
  s.Callback = nullptr;
  s.CallData = nullptr;
  s.VariableType = vtype;
  s.Variable = variable;
  s.Help = help;

  this->Internals->Callbacks[argument] = s;
  this->GenerateHelp();
}

#define CommandLineArgumentsAddArgumentMacro(type, ctype)                     \
  void CommandLineArguments::AddArgument(const char* argument,                \
                                         ArgumentTypeEnum type,               \
                                         ctype* variable, const char* help)   \
  {                                                                           \
    this->AddArgument(argument, type, CommandLineArguments::type##_TYPE,      \
                      variable, help);                                        \
  }

/* clang-format off */
CommandLineArgumentsAddArgumentMacro(BOOL, bool)
CommandLineArgumentsAddArgumentMacro(INT, int)
CommandLineArgumentsAddArgumentMacro(DOUBLE, double)
CommandLineArgumentsAddArgumentMacro(STRING, char*)
CommandLineArgumentsAddArgumentMacro(STL_STRING, std::string)

CommandLineArgumentsAddArgumentMacro(VECTOR_BOOL, std::vector<bool>)
CommandLineArgumentsAddArgumentMacro(VECTOR_INT, std::vector<int>)
CommandLineArgumentsAddArgumentMacro(VECTOR_DOUBLE, std::vector<double>)
CommandLineArgumentsAddArgumentMacro(VECTOR_STRING, std::vector<char*>)
CommandLineArgumentsAddArgumentMacro(VECTOR_STL_STRING,
                                     std::vector<std::string>)
#ifdef HELP_CLANG_FORMAT
;
#endif
/* clang-format on */

#define CommandLineArgumentsAddBooleanArgumentMacro(type, ctype)              \
  void CommandLineArguments::AddBooleanArgument(                              \
    const char* argument, ctype* variable, const char* help)                  \
  {                                                                           \
    this->AddArgument(argument, CommandLineArguments::NO_ARGUMENT,            \
                      CommandLineArguments::type##_TYPE, variable, help);     \
  }

/* clang-format off */
CommandLineArgumentsAddBooleanArgumentMacro(BOOL, bool)
CommandLineArgumentsAddBooleanArgumentMacro(INT, int)
CommandLineArgumentsAddBooleanArgumentMacro(DOUBLE, double)
CommandLineArgumentsAddBooleanArgumentMacro(STRING, char*)
CommandLineArgumentsAddBooleanArgumentMacro(STL_STRING, std::string)
#ifdef HELP_CLANG_FORMAT
;
#endif
/* clang-format on */

void CommandLineArguments::SetClientData(void* client_data)
{
  this->Internals->ClientData = client_data;
}

void CommandLineArguments::SetUnknownArgumentCallback(
  CommandLineArguments::ErrorCallbackType callback)
{
  this->Internals->UnknownArgumentCallback = callback;
}

char const* CommandLineArguments::GetHelp(char const* arg)
{
  auto it = this->Internals->Callbacks.find(arg);
  if (it == this->Internals->Callbacks.end()) {
    return nullptr;
  }

  // Since several arguments may point to the same argument, find the one this
  // one point to if this one is pointing to another argument.
  CommandLineArgumentsCallbackStructure* cs = &(it->second);
  for (;;) {
    auto hit = this->Internals->Callbacks.find(cs->Help);
    if (hit == this->Internals->Callbacks.end()) {
      break;
    }
    cs = &(hit->second);
  }
  return cs->Help;
}

void CommandLineArguments::SetLineLength(unsigned int ll)
{
  if (ll < 9 || ll > 1000) {
    return;
  }
  this->LineLength = ll;
  this->GenerateHelp();
}

char const* CommandLineArguments::GetArgv0()
{
  return this->Internals->Argv0.c_str();
}

unsigned int CommandLineArguments::GetLastArgument()
{
  return static_cast<unsigned int>(this->Internals->LastArgument + 1);
}

void CommandLineArguments::GenerateHelp()
{
  std::ostringstream str;

  // Collapse all arguments into the map of vectors of all arguments that do
  // the same thing.
  CommandLineArguments::Internal::CallbacksMap::iterator it;
  using MapArgs = std::map<CommandLineArguments::Internal::String,
                           CommandLineArguments::Internal::SetOfStrings>;
  MapArgs mp;
  MapArgs::iterator mpit, smpit;
  for (it = this->Internals->Callbacks.begin();
       it != this->Internals->Callbacks.end(); ++it) {
    CommandLineArgumentsCallbackStructure* cs = &(it->second);
    mpit = mp.find(cs->Help);
    if (mpit != mp.end()) {
      mpit->second.insert(it->first);
      mp[it->first].insert(it->first);
    } else {
      mp[it->first].insert(it->first);
    }
  }
  for (it = this->Internals->Callbacks.begin();
       it != this->Internals->Callbacks.end(); ++it) {
    CommandLineArgumentsCallbackStructure* cs = &(it->second);
    mpit = mp.find(cs->Help);
    if (mpit != mp.end()) {
      mpit->second.insert(it->first);
      smpit = mp.find(it->first);
      CommandLineArguments::Internal::SetOfStrings::iterator sit;
      for (sit = smpit->second.begin(); sit != smpit->second.end(); ++sit) {
        mpit->second.insert(*sit);
      }
      mp.erase(smpit);
    } else {
      mp[it->first].insert(it->first);
    }
  }

  // Find the length of the longest string
  CommandLineArguments::Internal::String::size_type maxlen = 0;
  for (mpit = mp.begin(); mpit != mp.end(); ++mpit) {
    CommandLineArguments::Internal::SetOfStrings::iterator sit;
    for (sit = mpit->second.begin(); sit != mpit->second.end(); ++sit) {
      CommandLineArguments::Internal::String::size_type clen = sit->size();
      switch (this->Internals->Callbacks[*sit].ArgumentType) {
        case CommandLineArguments::NO_ARGUMENT:
          clen += 0;
          break;
        case CommandLineArguments::CONCAT_ARGUMENT:
          clen += 3;
          break;
        case CommandLineArguments::SPACE_ARGUMENT:
          clen += 4;
          break;
        case CommandLineArguments::EQUAL_ARGUMENT:
          clen += 4;
          break;
        default:
          break;
      }
      if (clen > maxlen) {
        maxlen = clen;
      }
    }
  }

  CommandLineArguments::Internal::String::size_type maxstrlen = maxlen;
  maxlen += 4; // For the space before and after the option

  // Print help for each option
  for (mpit = mp.begin(); mpit != mp.end(); ++mpit) {
    CommandLineArguments::Internal::SetOfStrings::iterator sit;
    for (sit = mpit->second.begin(); sit != mpit->second.end(); ++sit) {
      str << std::endl;
      std::string argument = *sit;
      switch (this->Internals->Callbacks[*sit].ArgumentType) {
        case CommandLineArguments::NO_ARGUMENT:
          break;
        case CommandLineArguments::CONCAT_ARGUMENT:
          argument += "opt";
          break;
        case CommandLineArguments::SPACE_ARGUMENT:
          argument += " opt";
          break;
        case CommandLineArguments::EQUAL_ARGUMENT:
          argument += "=opt";
          break;
        case CommandLineArguments::MULTI_ARGUMENT:
          argument += " opt opt ...";
          break;
        default:
          break;
      }
      str << "  " << argument.substr(0, maxstrlen) << "  ";
    }
    char const* ptr = this->Internals->Callbacks[mpit->first].Help;
    size_t len = strlen(ptr);
    int cnt = 0;
    while (len > 0) {
      // If argument with help is longer than line length, split it on previous
      // space (or tab) and continue on the next line
      CommandLineArguments::Internal::String::size_type m_pCustomCommand;
      for (m_pCustomCommand = 0; ptr[m_pCustomCommand]; m_pCustomCommand++) {
        if (*ptr == ' ' || *ptr == '\t') {
          ptr++;
          len--;
        }
      }
      if (cnt > 0) {
        for (m_pCustomCommand = 0; m_pCustomCommand < maxlen; m_pCustomCommand++) {
          str << " ";
        }
      }
      CommandLineArguments::Internal::String::size_type skip = len;
      if (skip > this->LineLength - maxlen) {
        skip = this->LineLength - maxlen;
        for (m_pCustomCommand = skip - 1; m_pCustomCommand > 0; m_pCustomCommand--) {
          if (ptr[m_pCustomCommand] == ' ' || ptr[m_pCustomCommand] == '\t') {
            break;
          }
        }
        if (m_pCustomCommand != 0) {
          skip = m_pCustomCommand;
        }
      }
      str.write(ptr, static_cast<std::streamsize>(skip));
      str << std::endl;
      ptr += skip;
      len -= skip;
      cnt++;
    }
  }
  /*
  // This can help debugging help string
  str << endl;
  unsigned int cc;
  for ( cc = 0; cc < this->LineLength; cc ++ )
    {
    str << cc % 10;
    }
  str << endl;
  */
  this->Help = str.str();
}

void CommandLineArguments::PopulateVariable(bool* variable,
                                            std::string const& value)
{
  if (value == "1" || value == "ON" || value == "on" || value == "On" ||
      value == "TRUE" || value == "true" || value == "True" ||
      value == "yes" || value == "Yes" || value == "YES") {
    *variable = true;
  } else {
    *variable = false;
  }
}

void CommandLineArguments::PopulateVariable(int* variable,
                                            std::string const& value)
{
  char* res = nullptr;
  *variable = static_cast<int>(strtol(value.c_str(), &res, 10));
  // if ( res && *res )
  //  {
  //  Can handle non-int
  //  }
}

void CommandLineArguments::PopulateVariable(double* variable,
                                            std::string const& value)
{
  char* res = nullptr;
  *variable = strtod(value.c_str(), &res);
  // if ( res && *res )
  //  {
  //  Can handle non-double
  //  }
}

void CommandLineArguments::PopulateVariable(char** variable,
                                            std::string const& value)
{
  delete[] *variable;
  *variable = new char[value.size() + 1];
  strcpy(*variable, value.c_str());
}

void CommandLineArguments::PopulateVariable(std::string* variable,
                                            std::string const& value)
{
  *variable = value;
}

void CommandLineArguments::PopulateVariable(std::vector<bool>* variable,
                                            std::string const& value)
{
  bool val = false;
  if (value == "1" || value == "ON" || value == "on" || value == "On" ||
      value == "TRUE" || value == "true" || value == "True" ||
      value == "yes" || value == "Yes" || value == "YES") {
    val = true;
  }
  variable->push_back(val);
}

void CommandLineArguments::PopulateVariable(std::vector<int>* variable,
                                            std::string const& value)
{
  char* res = nullptr;
  variable->push_back(static_cast<int>(strtol(value.c_str(), &res, 10)));
  // if ( res && *res )
  //  {
  //  Can handle non-int
  //  }
}

void CommandLineArguments::PopulateVariable(std::vector<double>* variable,
                                            std::string const& value)
{
  char* res = nullptr;
  variable->push_back(strtod(value.c_str(), &res));
  // if ( res && *res )
  //  {
  //  Can handle non-int
  //  }
}

void CommandLineArguments::PopulateVariable(std::vector<char*>* variable,
                                            std::string const& value)
{
  char* var = new char[value.size() + 1];
  strcpy(var, value.c_str());
  variable->push_back(var);
}

void CommandLineArguments::PopulateVariable(std::vector<std::string>* variable,
                                            std::string const& value)
{
  variable->push_back(value);
}

bool CommandLineArguments::PopulateVariable(
  CommandLineArgumentsCallbackStructure* cs, char const* value)
{
  // Call the callback
  if (cs->Callback) {
    if (!cs->Callback(cs->m_argument, value, cs->CallData)) {
      this->Internals->LastArgument--;
      return false;
    }
  }
  CommandLineArguments_DEBUG("Set argument: " << cs->m_argument << " to "
                                              << value);
  if (cs->Variable) {
    std::string var = "1";
    if (value) {
      var = value;
    }
    switch (cs->VariableType) {
      case CommandLineArguments::INT_TYPE:
        this->PopulateVariable(static_cast<int*>(cs->Variable), var);
        break;
      case CommandLineArguments::DOUBLE_TYPE:
        this->PopulateVariable(static_cast<double*>(cs->Variable), var);
        break;
      case CommandLineArguments::STRING_TYPE:
        this->PopulateVariable(static_cast<char**>(cs->Variable), var);
        break;
      case CommandLineArguments::STL_STRING_TYPE:
        this->PopulateVariable(static_cast<std::string*>(cs->Variable), var);
        break;
      case CommandLineArguments::BOOL_TYPE:
        this->PopulateVariable(static_cast<bool*>(cs->Variable), var);
        break;
      case CommandLineArguments::VECTOR_BOOL_TYPE:
        this->PopulateVariable(static_cast<std::vector<bool>*>(cs->Variable),
                               var);
        break;
      case CommandLineArguments::VECTOR_INT_TYPE:
        this->PopulateVariable(static_cast<std::vector<int>*>(cs->Variable),
                               var);
        break;
      case CommandLineArguments::VECTOR_DOUBLE_TYPE:
        this->PopulateVariable(static_cast<std::vector<double>*>(cs->Variable),
                               var);
        break;
      case CommandLineArguments::VECTOR_STRING_TYPE:
        this->PopulateVariable(static_cast<std::vector<char*>*>(cs->Variable),
                               var);
        break;
      case CommandLineArguments::VECTOR_STL_STRING_TYPE:
        this->PopulateVariable(
          static_cast<std::vector<std::string>*>(cs->Variable), var);
        break;
      default:
        std::cerr << "Got unknown variable type: \"" << cs->VariableType
                  << "\"" << std::endl;
        this->Internals->LastArgument--;
        return false;
    }
  }
  return true;
}

} // namespace KWSYS_NAMESPACE
