/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cm_codecvt.hxx"

#if defined(_WIN32)
#  include <cassert>
#  include <cstring>

#  include <windows.h>
#  undef max
#  include "cmsys/Encoding.hxx"

#  include "cm_utf8.h"
#endif

#include "cm_codecvt_Encoding.hxx"

codecvt::codecvt(codecvt_Encoding e)
#if defined(_WIN32)
  : m_codepage(0)
#endif
{
  switch (e) {
    case codecvt_Encoding::ConsoleOutput:
#if defined(_WIN32)
      m_noconv = false;
      m_codepage = GetConsoleOutputCP();
      break;
#endif
    case codecvt_Encoding::ANSI:
#if defined(_WIN32)
      m_noconv = false;
      m_codepage = CP_ACP;
      break;
#endif
    // We don't know which ANSI encoding to use for other platforms than
    // Windows so we don't do any conversion there
    case codecvt_Encoding::UTF8:
    case codecvt_Encoding::UTF8_WITH_BOM:
    // Assume internal encoding is UTF-8
    case codecvt_Encoding::None:
    // No encoding
    default:
      this->m_noconv = true;
  }
}

codecvt::~codecvt() = default;

bool codecvt::do_always_noconv() const noexcept
{
  return this->m_noconv;
}

std::codecvt_base::result codecvt::do_out(mbstate_t& state, char const* from,
                                          char const* from_end,
                                          char const*& from_next, char* to,
                                          char* to_end, char*& to_next) const
{
  from_next = from;
  to_next = to;
  if (this->m_noconv) {
    return std::codecvt_base::noconv;
  }
#if defined(_WIN32)
  // Use a const view of the state because we should not modify it until we
  // have fully processed and consume a byte (with sufficient space in the
  // output buffer).  We call helpers to re-cast and modify the state
  m_pState const& lstate = reinterpret_cast<m_pState&>(state);

  while (from_next != from_end) {
    // Count leading ones in the bits of the next byte.
    unsigned char const ones =
      cm_utf8_ones[static_cast<unsigned char>(*from_next)];

    if (ones != 1 && lstate.buffered != 0) {
      // We have a buffered partial codepoint that we never completed.
      return std::codecvt_base::error;
    } else if (ones == 1 && lstate.buffered == 0) {
      // This is a continuation of a codepoint that never started.
      return std::codecvt_base::error;
    }

    // Compute the number of bytes in the current codepoint.
    int need = 0;
    switch (ones) {
      case 0: // 0xxx xxxx: new codepoint of size 1
        need = 1;
        break;
      case 1: // 10xx xxxx: continues a codepoint
        assert(lstate.size != 0);
        need = lstate.size;
        break;
      case 2: // 110x xxxx: new codepoint of size 2
        need = 2;
        break;
      case 3: // 1110 xxxx: new codepoint of size 3
        need = 3;
        break;
      case 4: // 1111 0xxx: new codepoint of size 4
        need = 4;
        break;
      default: // invalid byte
        return std::codecvt_base::error;
    }
    assert(need > 0);

    if (lstate.buffered + 1 == need) {
      // This byte completes a codepoint.
      std::codecvt_base::result decode_result =
        this->Decode(state, need, from_next, to_next, to_end);
      if (decode_result != std::codecvt_base::ok) {
        return decode_result;
      }
    } else {
      // This byte does not complete a codepoint.
      this->BufferPartial(state, need, from_next);
    }
  }

  return std::codecvt_base::ok;
#else
  static_cast<void>(state);
  static_cast<void>(from);
  static_cast<void>(from_end);
  static_cast<void>(from_next);
  static_cast<void>(to);
  static_cast<void>(to_end);
  static_cast<void>(to_next);
  return std::codecvt_base::noconv;
#endif
}

std::codecvt_base::result codecvt::do_unshift(mbstate_t& state, char* to,
                                              char* to_end,
                                              char*& to_next) const
{
  to_next = to;
  if (this->m_noconv) {
    return std::codecvt_base::noconv;
  }
#if defined(_WIN32)
  m_pState& lstate = reinterpret_cast<m_pState&>(state);
  if (lstate.buffered != 0) {
    return this->DecodePartial(state, to_next, to_end);
  }
  return std::codecvt_base::ok;
#else
  static_cast<void>(state);
  static_cast<void>(to_end);
  return std::codecvt_base::ok;
#endif
}

#if defined(_WIN32)
std::codecvt_base::result codecvt::Decode(mbstate_t& state, int size,
                                          char const*& from_next,
                                          char*& to_next, char* to_end) const
{
  m_pState& lstate = reinterpret_cast<m_pState&>(state);

  // Collect all the bytes for this codepoint.
  char buf[4];
  memcpy(buf, lstate.partial, lstate.buffered);
  buf[lstate.buffered] = *from_next;

  // Convert the encoding.
  wchar_t wbuf[2];
  int wlen =
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf, size, wbuf, 2);
  if (wlen <= 0) {
    return std::codecvt_base::error;
  }

  int tlen = WideCharToMultiByte(m_codepage, 0, wbuf, wlen, to_next,
                                 to_end - to_next, nullptr, nullptr);
  if (tlen <= 0) {
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
      return std::codecvt_base::partial;
    }
    return std::codecvt_base::error;
  }

  // Move past the now-consumed byte in the input buffer.
  ++from_next;

  // Move past the converted codepoint in the output buffer.
  to_next += tlen;

  // Re-initialize the state for the next codepoint to start.
  lstate = m_pState();

  return std::codecvt_base::ok;
}

std::codecvt_base::result codecvt::DecodePartial(mbstate_t& state,
                                                 char*& to_next,
                                                 char* to_end) const
{
  m_pState& lstate = reinterpret_cast<m_pState&>(state);

  // Try converting the partial codepoint.
  wchar_t wbuf[2];
  int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, lstate.partial,
                                 lstate.buffered, wbuf, 2);
  if (wlen <= 0) {
    return std::codecvt_base::error;
  }

  int tlen = WideCharToMultiByte(m_codepage, 0, wbuf, wlen, to_next,
                                 to_end - to_next, nullptr, nullptr);
  if (tlen <= 0) {
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
      return std::codecvt_base::partial;
    }
    return std::codecvt_base::error;
  }

  // Move past the converted codepoint in the output buffer.
  to_next += tlen;

  // Re-initialize the state for the next codepoint to start.
  lstate = m_pState();

  return std::codecvt_base::ok;
}

void codecvt::BufferPartial(mbstate_t& state, int size,
                            char const*& from_next) const
{
  m_pState& lstate = reinterpret_cast<m_pState&>(state);

  // Save the byte in our buffer for later.
  lstate.partial[lstate.buffered++] = *from_next;
  lstate.size = size;

  // Move past the now-consumed byte in the input buffer.
  ++from_next;
}
#endif

int codecvt::do_max_length() const noexcept
{
  return 4;
}

int codecvt::do_encoding() const noexcept
{
  return 0;
}
