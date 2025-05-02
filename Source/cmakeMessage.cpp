
#include "cmakeMessage.h"

#include <iostream>

void CMakeMessage::error(std::string& msg)
{
  std::cout << "Error: " << msg << "\n";
}

void CMakeMessage::error(const char* msg)
{
  std::cout << "Error: " << msg << "\n";
}
