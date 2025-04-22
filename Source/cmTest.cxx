/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmTest.h"

#include "cmMakefile.h"
#include "cmProperty.h"
#include "cmState.h"
#include "cmValue.h"

cmTest::cmTest(cmMakefile* mf)
  : m_backtrace(mf->GetBacktrace())
  , PolicyStatusCMP0158(mf->GetPolicyStatus(cmPolicies::CMP0158))
  , PolicyStatusCMP0178(mf->GetPolicyStatus(cmPolicies::CMP0178))
{
  this->m_pMakefile = mf;
  this->OldStyle = true;
}

cmTest::~cmTest() = default;

cmListFileBacktrace const& cmTest::GetBacktrace() const
{
  return this->m_backtrace;
}

void cmTest::SetName(std::string const& name)
{
  this->Name = name;
}

void cmTest::SetCommand(std::vector<std::string> const& command)
{
  this->m_command = command;
}

cmValue cmTest::GetProperty(std::string const& prop) const
{
  cmValue retVal = this->Properties.GetPropertyValue(prop);
  if (!retVal) {
    bool const chain =
      this->m_pMakefile->GetState()->IsPropertyChained(prop, cmProperty::TEST);
    if (chain) {
      if (cmValue p = this->m_pMakefile->GetProperty(prop, chain)) {
        return p;
      }
    }
    return nullptr;
  }
  return retVal;
}

bool cmTest::GetPropertyAsBool(std::string const& prop) const
{
  return this->GetProperty(prop).IsOn();
}

void cmTest::SetProperty(std::string const& prop, cmValue value)
{
  this->Properties.SetProperty(prop, value);
}

void cmTest::AppendProperty(std::string const& prop, std::string const& value,
                            bool asString)
{
  this->Properties.AppendProperty(prop, value, asString);
}

bool cmTest::GetCommandExpandLists() const
{
  return this->CommandExpandLists;
}

void cmTest::SetCommandExpandLists(bool b)
{
  this->CommandExpandLists = b;
}
