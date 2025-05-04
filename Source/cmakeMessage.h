#pragma once

#include <string>

class CMakeMessage
{
public:
  static void error(char const* const msg);
  static void error(std::string const& msg);

  static void function(char const* const msg);
  static void function(std::string const& msg);
};

class FunctionTrace
{
  std::string m_functionName;

public:
  FunctionTrace(char const* const functionName);
  FunctionTrace(
    char const* const functionName,
    std::string const& more);
  ~FunctionTrace();

  void more(
    char const* const msg1,
    std::string& msg2);
};
