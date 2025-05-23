/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */

#include "cmDebuggerVariables.h"

#include <algorithm>
#include <vector>

#include <cm3p/cppdap/optional.h>
#include <cm3p/cppdap/protocol.h>
#include <cm3p/cppdap/types.h>

#include "cmDebuggerVariablesManager.h"

namespace cmDebugger {

namespace {
dap::VariablePresentationHint const PrivatePropertyHint = { {},
                                                            "property",
                                                            {},
                                                            "private" };
dap::VariablePresentationHint const PrivateDataHint = { {},
                                                        "data",
                                                        {},
                                                        "private" };
}

std::atomic<int64_t> cmDebuggerVariables::NextId(1);

cmDebuggerVariables::cmDebuggerVariables(
  std::shared_ptr<cmDebuggerVariablesManager> variablesManager,
  std::string name, bool supportsVariableType)
  : m_id(NextId.fetch_add(1))
  , m_name(std::move(name))
  , SupportsVariableType(supportsVariableType)
  , VariablesManager(std::move(variablesManager))
{
  VariablesManager->RegisterHandler(
    m_id, [this](dap::VariablesRequest const& request) {
      (void)request;
      return this->HandleVariablesRequest();
    });
}

cmDebuggerVariables::cmDebuggerVariables(
  std::shared_ptr<cmDebuggerVariablesManager> variablesManager,
  std::string name, bool supportsVariableType,
  std::function<std::vector<cmDebuggerVariableEntry>()> getKeyValuesFunction)
  : m_id(NextId.fetch_add(1))
  , m_name(std::move(name))
  , GetKeyValuesFunction(std::move(getKeyValuesFunction))
  , SupportsVariableType(supportsVariableType)
  , VariablesManager(std::move(variablesManager))
{
  VariablesManager->RegisterHandler(
    m_id, [this](dap::VariablesRequest const& request) {
      (void)request;
      return this->HandleVariablesRequest();
    });
}

void cmDebuggerVariables::AddSubVariables(
  std::shared_ptr<cmDebuggerVariables> const& variables)
{
  if (variables) {
    SubVariables.emplace_back(variables);
  }
}

dap::array<dap::Variable> cmDebuggerVariables::HandleVariablesRequest()
{
  dap::array<dap::Variable> variables;

  if (GetKeyValuesFunction) {
    auto values = GetKeyValuesFunction();
    for (auto const& entry : values) {
      if (IgnoreEmptyStringEntries && entry.Type == "string" &&
          entry.Value.empty()) {
        continue;
      }
      variables.push_back(dap::Variable{
        {},
        {},
        {},
        entry.m_name,
        {},
        PrivateDataHint,
        SupportsVariableType ? entry.Type : dap::optional<dap::string>(),
        entry.Value,
        0 });
    }
  }

  EnumerateSubVariablesIfAny(variables);

  if (EnableSorting) {
    std::sort(variables.begin(), variables.end(),
              [](dap::Variable const& a, dap::Variable const& b) {
                return a.name < b.name;
              });
  }
  return variables;
}

void cmDebuggerVariables::EnumerateSubVariablesIfAny(
  dap::array<dap::Variable>& toBeReturned) const
{
  dap::array<dap::Variable> ret;
  for (auto const& variables : SubVariables) {
    toBeReturned.emplace_back(dap::Variable{
      {},
      {},
      {},
      variables->GetName(),
      {},
      PrivatePropertyHint,
      SupportsVariableType ? "collection" : dap::optional<dap::string>(),
      variables->GetValue(),
      variables->GetId() });
  }
}

void cmDebuggerVariables::ClearSubVariables()
{
  SubVariables.clear();
}

cmDebuggerVariables::~cmDebuggerVariables()
{
  ClearSubVariables();
  VariablesManager->UnregisterHandler(m_id);
}

} // namespace cmDebugger
