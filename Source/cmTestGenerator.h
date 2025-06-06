/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <iosfwd>
#include <string>
#include <vector>

#include "cmScriptGenerator.h"

class cmGeneratorExpression;
class cmLocalGenerator;
class cmTest;

/** \class cmTestGenerator
 * \brief Support class for generating install scripts.
 *
 */
class cmTestGenerator : public cmScriptGenerator
{
public:
  cmTestGenerator(cmTest* test,
                  std::vector<std::string> const& configurations =
                    std::vector<std::string>());
  ~cmTestGenerator() override;

  cmTestGenerator(cmTestGenerator const&) = delete;
  cmTestGenerator& operator=(cmTestGenerator const&) = delete;

  void Compute(cmLocalGenerator* lg);

  /** Test if this generator installs the test for a given configuration.  */
  bool TestsForConfig(std::string const& config);

  cmTest* GetTest() const;

private:
  void GenerateInternalProperties(std::ostream& os);
  std::vector<std::string> EvaluateCommandLineArguments(
    std::vector<std::string> const& argv, cmGeneratorExpression& ge,
    std::string const& config) const;

protected:
  void GenerateScriptConfigs(std::ostream& os, Indent indent) override;
  void GenerateScriptActions(std::ostream& os, Indent indent) override;
  void GenerateScriptForConfig(std::ostream& os, std::string const& config,
                               Indent indent) override;
  void GenerateScriptNoConfig(std::ostream& os, Indent indent) override;
  bool NeedsScriptNoConfig() const override;
  void GenerateOldStyle(std::ostream& os, Indent indent);

  cmLocalGenerator* LG;
  cmTest* Test;
  bool TestGenerated;
};
