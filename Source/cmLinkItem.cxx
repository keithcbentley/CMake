/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmLinkItem.h"

#include <utility> // IWYU pragma: keep

#include <cm/optional>
#include <cm/string_view>
#include <cmext/string_view>

#include "cmGeneratorTarget.h"
#include "cmStringAlgorithms.h"

std::string const cmLinkItem::DEFAULT = "DEFAULT";

cmLinkItem::cmLinkItem(std::string n, bool c, cmListFileBacktrace bt,
                       std::string feature)
  : String(std::move(n))
  , Feature(std::move(feature))
  , Cross(c)
  , m_backtrace(std::move(bt))
{
}

cmLinkItem::cmLinkItem(cmGeneratorTarget const* t, bool c,
                       cmListFileBacktrace bt, std::string feature)
  : Target(t)
  , Feature(std::move(feature))
  , Cross(c)
  , m_backtrace(std::move(bt))
{
}

std::string const& cmLinkItem::AsStr() const
{
  return this->Target ? this->Target->GetName() : this->String;
}

bool operator<(cmLinkItem const& l, cmLinkItem const& r)
{
  // Order among targets.
  if (l.Target && r.Target) {
    if (l.Target != r.Target) {
      return l.Target < r.Target;
    }
    // Order identical targets via cross-config.
    return l.Cross < r.Cross;
  }
  // Order targets before strings.
  if (l.Target) {
    return true;
  }
  if (r.Target) {
    return false;
  }
  // Order among strings.
  if (l.String != r.String) {
    return l.String < r.String;
  }
  // Order identical strings via cross-config.
  return l.Cross < r.Cross;
}

bool operator==(cmLinkItem const& l, cmLinkItem const& r)
{
  return l.Target == r.Target && l.String == r.String && l.Cross == r.Cross;
}

std::ostream& operator<<(std::ostream& os, cmLinkItem const& item)
{
  return os << item.AsStr();
}

cmLinkImplItem::cmLinkImplItem(cmLinkItem item)
  : cmLinkItem(std::move(item))
{
}

namespace {
cm::string_view const LL_BEGIN = "<LINK_LIBRARY:"_s;
cm::string_view const LL_END = "</LINK_LIBRARY:"_s;
}
cm::optional<std::string> ParseLinkFeature(std::string const& item)
{
  if (cmHasPrefix(item, LL_BEGIN) && cmHasSuffix(item, '>')) {
    return item.substr(LL_BEGIN.length(),
                       item.find('>', LL_BEGIN.length()) - LL_BEGIN.length());
  }
  if (cmHasPrefix(item, LL_END) && cmHasSuffix(item, '>')) {
    return cmLinkItem::DEFAULT;
  }
  return cm::nullopt;
}
