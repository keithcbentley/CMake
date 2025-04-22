#pragma once

#include <stdexcept>

class CMakeException : public std::exception
{

public:
  CMakeException(std::string& message)
    : std::exception(message.c_str())
  {
  }
};
