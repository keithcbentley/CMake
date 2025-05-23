/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmCTestHG.h"

#include <ostream>
#include <vector>

#include <cmext/algorithm>

#include "cmsys/RegularExpression.hxx"

#include "cmCTest.h"
#include "cmCTestVC.h"
#include "cmMakefile.h"
#include "cmSystemTools.h"
#include "cmXMLParser.h"

cmCTestHG::cmCTestHG(cmCTest* ct, cmMakefile* mf, std::ostream& log)
  : cmCTestGlobalVC(ct, mf, log)
{
  this->PriorRev = this->Unknown;
}

cmCTestHG::~cmCTestHG() = default;

class cmCTestHG::IdentifyParser : public cmCTestVC::LineParser
{
public:
  IdentifyParser(cmCTestHG* hg, char const* prefix, std::string& rev)
    : Rev(rev)
  {
    this->SetLog(&hg->Log, prefix);
    this->RegexIdentify.compile("^([0-9a-f]+)");
  }

private:
  std::string& Rev;
  cmsys::RegularExpression RegexIdentify;

  bool ProcessLine() override
  {
    if (this->RegexIdentify.find(this->Line)) {
      this->Rev = this->RegexIdentify.match(1);
      return false;
    }
    return true;
  }
};

class cmCTestHG::StatusParser : public cmCTestVC::LineParser
{
public:
  StatusParser(cmCTestHG* hg, char const* prefix)
    : HG(hg)
  {
    this->SetLog(&hg->Log, prefix);
    this->RegexStatus.compile("([MARC!?I]) (.*)");
  }

private:
  cmCTestHG* HG;
  cmsys::RegularExpression RegexStatus;

  bool ProcessLine() override
  {
    if (this->RegexStatus.find(this->Line)) {
      this->DoPath(this->RegexStatus.match(1)[0], this->RegexStatus.match(2));
    }
    return true;
  }

  void DoPath(char status, std::string const& path)
  {
    if (path.empty()) {
      return;
    }

    // See "hg help status".  Note that there is no 'conflict' status.
    switch (status) {
      case 'M':
      case 'A':
      case '!':
      case 'R':
        this->HG->DoModification(PathModified, path);
        break;
      case 'I':
      case '?':
      case 'C':
      case ' ':
      default:
        break;
    }
  }
};

std::string cmCTestHG::GetWorkingRevision()
{
  // Run plumbing "hg identify" to get work tree revision.
  std::string hg = this->CommandLineTool;
  std::vector<std::string> hg_identify = { hg, "identify", "-i" };
  std::string rev;
  IdentifyParser out(this, "rev-out> ", rev);
  OutputLogger err(this->Log, "rev-err> ");
  this->RunChild(hg_identify, &out, &err);
  return rev;
}

bool cmCTestHG::NoteOldRevision()
{
  this->OldRevision = this->GetWorkingRevision();
  cmCTestLog(this->CTest, HANDLER_OUTPUT,
             "   Old revision of repository is: " << this->OldRevision
                                                  << "\n");
  this->PriorRev.Rev = this->OldRevision;
  return true;
}

bool cmCTestHG::NoteNewRevision()
{
  this->NewRevision = this->GetWorkingRevision();
  cmCTestLog(this->CTest, HANDLER_OUTPUT,
             "   New revision of repository is: " << this->NewRevision
                                                  << "\n");
  return true;
}

bool cmCTestHG::UpdateImpl()
{
  // Use "hg pull" followed by "hg update" to update the working tree.
  {
    std::string hg = this->CommandLineTool;
    std::vector<std::string> hg_pull = { hg, "pull", "-v" };
    OutputLogger out(this->Log, "pull-out> ");
    OutputLogger err(this->Log, "pull-err> ");
    this->RunChild(hg_pull, &out, &err);
  }

  // TODO: if(this->CTest->GetTestModel() == cmCTest::NIGHTLY)

  std::vector<std::string> hg_update;
  hg_update.emplace_back(this->CommandLineTool);
  hg_update.emplace_back("update");
  hg_update.emplace_back("-v");

  // Add user-specified update options.
  std::string opts = this->m_pMakefile->GetSafeDefinition("CTEST_UPDATE_OPTIONS");
  if (opts.empty()) {
    opts = this->m_pMakefile->GetSafeDefinition("CTEST_HG_UPDATE_OPTIONS");
  }
  std::vector<std::string> args = cmSystemTools::ParseArguments(opts);
  cm::append(hg_update, args);

  OutputLogger out(this->Log, "update-out> ");
  OutputLogger err(this->Log, "update-err> ");
  return this->RunUpdateCommand(hg_update, &out, &err);
}

class cmCTestHG::LogParser
  : public cmCTestVC::OutputLogger
  , private cmXMLParser
{
public:
  LogParser(cmCTestHG* hg, char const* prefix)
    : OutputLogger(hg->Log, prefix)
    , HG(hg)
  {
    this->InitializeParser();
  }
  ~LogParser() override { this->CleanupParser(); }

private:
  cmCTestHG* HG;

  using Revision = cmCTestHG::Revision;
  using Change = cmCTestHG::Change;
  Revision Rev;
  std::vector<Change> Changes;
  Change CurChange;
  std::vector<char> CData;

  bool ProcessChunk(char const* data, int length) override
  {
    this->OutputLogger::ProcessChunk(data, length);
    this->ParseChunk(data, length);
    return true;
  }

  void StartElement(std::string const& name, char const** atts) override
  {
    this->CData.clear();
    if (name == "logentry") {
      this->Rev = Revision();
      if (char const* rev =
            cmCTestHG::LogParser::FindAttribute(atts, "revision")) {
        this->Rev.Rev = rev;
      }
      this->Changes.clear();
    }
  }

  void CharacterDataHandler(char const* data, int length) override
  {
    cm::append(this->CData, data, data + length);
  }

  void EndElement(std::string const& name) override
  {
    if (name == "logentry") {
      this->HG->DoRevision(this->Rev, this->Changes);
    } else if (!this->CData.empty() && name == "author") {
      this->Rev.Author.assign(this->CData.data(), this->CData.size());
    } else if (!this->CData.empty() && name == "email") {
      this->Rev.EMail.assign(this->CData.data(), this->CData.size());
    } else if (!this->CData.empty() && name == "date") {
      this->Rev.Date.assign(this->CData.data(), this->CData.size());
    } else if (!this->CData.empty() && name == "msg") {
      this->Rev.Log.assign(this->CData.data(), this->CData.size());
    } else if (!this->CData.empty() && name == "files") {
      std::vector<std::string> paths = this->SplitCData();
      for (std::string const& path : paths) {
        // Updated by default, will be modified using file_adds and
        // file_dels.
        this->CurChange = Change('U');
        this->CurChange.Path = path;
        this->Changes.push_back(this->CurChange);
      }
    } else if (!this->CData.empty() && name == "file_adds") {
      std::string added_paths(this->CData.begin(), this->CData.end());
      for (Change& change : this->Changes) {
        if (added_paths.find(change.Path) != std::string::npos) {
          change.m_action = 'A';
        }
      }
    } else if (!this->CData.empty() && name == "file_dels") {
      std::string added_paths(this->CData.begin(), this->CData.end());
      for (Change& change : this->Changes) {
        if (added_paths.find(change.Path) != std::string::npos) {
          change.m_action = 'D';
        }
      }
    }
    this->CData.clear();
  }

  std::vector<std::string> SplitCData()
  {
    std::vector<std::string> output;
    std::string currPath;
    for (char i : this->CData) {
      if (i != ' ') {
        currPath += i;
      } else {
        output.push_back(currPath);
        currPath.clear();
      }
    }
    output.push_back(currPath);
    return output;
  }

  void m_reportError(int /*line*/, int /*column*/, char const* msg) override
  {
    this->HG->Log << "Error parsing hg log xml: " << msg << "\n";
  }
};

bool cmCTestHG::LoadRevisions()
{
  // Use 'hg log' to get revisions in a xml format.
  //
  // TODO: This should use plumbing or python code to be more precise.
  // The "list of strings" templates like {files} will not work when
  // the project has spaces in the path.  Also, they may not have
  // proper XML escapes.
  std::string range = this->OldRevision + ":" + this->NewRevision;
  std::string hg = this->CommandLineTool;
  std::string hgXMLTemplate = "<logentry\n"
                              "   revision=\"{node|short}\">\n"
                              "  <author>{author|person}</author>\n"
                              "  <email>{author|email}</email>\n"
                              "  <date>{date|isodate}</date>\n"
                              "  <msg>{desc}</msg>\n"
                              "  <files>{files}</files>\n"
                              "  <file_adds>{file_adds}</file_adds>\n"
                              "  <file_dels>{file_dels}</file_dels>\n"
                              "</logentry>\n";
  std::vector<std::string> hg_log = { hg,    "log",        "--removed",  "-r",
                                      range, "--template", hgXMLTemplate };

  LogParser out(this, "log-out> ");
  out.Process("<?xml version=\"1.0\"?>\n"
              "<log>\n");
  OutputLogger err(this->Log, "log-err> ");
  this->RunChild(hg_log, &out, &err);
  out.Process("</log>\n");
  return true;
}

bool cmCTestHG::LoadModifications()
{
  // Use 'hg status' to get modified files.
  std::string hg = this->CommandLineTool;
  std::vector<std::string> hg_status = { hg, "status" };
  StatusParser out(this, "status-out> ");
  OutputLogger err(this->Log, "status-err> ");
  this->RunChild(hg_status, &out, &err);
  return true;
}
