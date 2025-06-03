#pragma once

#include <optional>

#include <windows.h>

class SafeArgs
{
  static inline std::string s_nullStr;

  std::vector<std::string> const& m_args;

public:
  SafeArgs(std::vector<std::string> const& args)
    : m_args(args)
  {
  }

  std::string const& at(int index)
  {
    if (index < 0) {
      index += m_args.size();
    }

    if (index < 0) {
      return s_nullStr;
    }
    if (index >= m_args.size()) {
      return s_nullStr;
    }
    return m_args[index];
  }

  std::string const& operator[](int index) { return at(index); }

  bool isEmpty() const { return m_args.empty(); }

  int argCount() const { return m_args.size(); }
};

class StringUtil
{
public:
  static std::wstring narrowToWide(std::string const& nString)
  {
    //    TODO: check for errors.
    int wBufferSize =
      MultiByteToWideChar(CP_UTF8, 0, nString.c_str(), -1, nullptr, 0);

    wchar_t* pWideBuffer = new wchar_t[wBufferSize];

    //  TODO: check for errors.
    MultiByteToWideChar(CP_UTF8, 0, nString.c_str(), -1, pWideBuffer,
                        wBufferSize);
    std::wstring wString(pWideBuffer);
    delete[] pWideBuffer;

    return wString;
  }

  static std::string wideToNarrow(std::wstring const& wString)
  {
    //    TODO: check for errors.
    int nBufferSize = WideCharToMultiByte(CP_UTF8, 0, wString.c_str(), -1,
                                          nullptr, 0, nullptr, nullptr);

    char* pNarrowBuffer = new char[nBufferSize];

    //  TODO: check for errors.
    WideCharToMultiByte(CP_UTF8, 0, wString.c_str(), -1, pNarrowBuffer,
                        nBufferSize, nullptr, nullptr);
    std::string str(pNarrowBuffer);
    delete[] pNarrowBuffer;

    return str;
  }

  static std::string substrStartEnd(std::string const& str, int startIndex,
                                    int endIndex)
  {
    if (startIndex < 0) {
      throw std::out_of_range("substrStartEnd: start index < 0.");
    }

    if (startIndex >= str.size()) {
      throw std::out_of_range("substrStartEnd: start index >= size().");
    }

    if (endIndex < 0) {
      endIndex += str.size();
    }

    if (endIndex < 0) {
      throw std::out_of_range("substrStartEnd: effective end index < 0.");
    }

    int length = endIndex - startIndex;
    if (length < 0) {
      throw std::out_of_range("substrStartEnd: effective length < 0.");
    }

    return str.substr(startIndex, length);
  }
};

class SysEnv
{

public:
  static std::optional<std::string> getEnv(char const* key)
  {
    std::wstring wKey = StringUtil::narrowToWide(key);
    auto const resultSize = GetEnvironmentVariableW(wKey.c_str(), nullptr, 0);
    if (resultSize == 0) {
      return {};
    }
    int wBufferSize = resultSize + 1;
    wchar_t* pWideBuffer = new wchar_t[wBufferSize];

    //  TODO: is this worth another return value check to be safe?
    GetEnvironmentVariableW(wKey.c_str(), pWideBuffer, wBufferSize);
    std::wstring wValue(pWideBuffer);
    delete[] pWideBuffer;

    std::string value = StringUtil::wideToNarrow(wValue);
    return value;
  }

  static std::optional<std::string> getEnv(std::string const& key)
  {
    return getEnv(key.c_str());
  }

  static void setEnv(std::string key, std::string value)
  {
    std::wstring wKey = StringUtil::narrowToWide(key);
    std::wstring wValue = StringUtil::narrowToWide(value);
    SetEnvironmentVariableW(wKey.c_str(), wValue.c_str());
  }

  static void unsetEnv(std::string key)
  {
    std::wstring wKey = StringUtil::narrowToWide(key);
    SetEnvironmentVariableW(wKey.c_str(), nullptr);
  }
};
