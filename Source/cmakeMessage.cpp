
#include "cmakeMessage.h"

#include <iostream>

void CMakeMessage::error(std::string const& msg)
{
  std::cout << "Error: " << msg << "\n";
}

void CMakeMessage::error(char const* const msg)
{
  std::cout << "Error: " << msg << "\n";
}

FunctionTrace::FunctionTrace(char const* const functionName)
  : m_functionName(functionName)
{
  std::cout << "FunctionTrace: " << m_functionName << "\n";
}

FunctionTrace::FunctionTrace(
  char const* const functionName,
  std::string const& more)
  : m_functionName(functionName)
{
  std::cout << "FunctionTrace: " << m_functionName << " " << more << "\n";
}

FunctionTrace::~FunctionTrace()
{
  std::cout << "~FunctionTrace: ~" << m_functionName << "\n";
}

void FunctionTrace::more(
  char const* const msg1,
  std::string& msg2)
{
  std::cout << m_functionName << " " << msg1 << msg2 << "\n";
}
