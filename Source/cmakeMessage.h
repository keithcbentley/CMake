#pragma once


#include <string>

class CMakeMessage
{
public:

	static void error(char const* msg);
	static void error(std::string& msg);

};
