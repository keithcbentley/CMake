#pragma once

#include <stdexcept>

class CMakeException : public std::exception
{

public:
  CMakeException(std::string const& message)
    : std::exception(message.c_str())
  {
  }

  CMakeException(char const* message)
    : std::exception(message)
  {
  }
};
