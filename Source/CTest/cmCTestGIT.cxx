/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmCTestGIT.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <utility>
#include <vector>

#include <cmext/algorithm>

#include "cmsys/FStream.hxx"

#include "cmCTest.h"
#include "cmCTestVC.h"
#include "cmList.h"
#include "cmMakefile.h"
#include "cmProcessOutput.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmUVProcessChain.h"

static unsigned int cmCTestGITVersion(unsigned int epic, unsigned int major,
                                      unsigned int minor, unsigned int fix)
{
  // 1.6.5.0 maps to 10605000
  return epic * 10000000 + major * 100000 + minor * 1000 + fix;
}

cmCTestGIT::cmCTestGIT(cmCTest* ct, cmMakefile* mf, std::ostream& log)
  : cmCTestGlobalVC(ct, mf, log)
{
  this->PriorRev = this->Unknown;
  this->CurrentGitVersion = 0;
}

cmCTestGIT::~cmCTestGIT() = default;

class cmCTestGIT::OneLineParser : public cmCTestVC::LineParser
{
public:
  OneLineParser(cmCTestGIT* git, char const* prefix, std::string& l)
    : Line1(l)
  {
    this->SetLog(&git->Log, prefix);
  }

private:
  std::string& Line1;
  bool ProcessLine() override
  {
    // Only the first line is of interest.
    this->Line1 = this->Line;
    return false;
  }
};

std::string cmCTestGIT::GetWorkingRevision()
{
  // Run plumbing "git rev-list" to get work tree revision.
  std::string git = this->CommandLineTool;
  std::vector<std::string> git_rev_list = { git, "rev-list", "-n",
                                            "1", "HEAD",     "--" };
  std::string rev;
  OneLineParser out(this, "rl-out> ", rev);
  OutputLogger err(this->Log, "rl-err> ");
  this->RunChild(git_rev_list, &out, &err);
  return rev;
}

bool cmCTestGIT::NoteOldRevision()
{
  this->OldRevision = this->GetWorkingRevision();
  cmCTestLog(this->CTest, HANDLER_OUTPUT,
             "   Old revision of repository is: " << this->OldRevision
                                                  << "\n");
  this->PriorRev.Rev = this->OldRevision;
  return true;
}

bool cmCTestGIT::NoteNewRevision()
{
  this->NewRevision = this->GetWorkingRevision();
  cmCTestLog(this->CTest, HANDLER_OUTPUT,
             "   New revision of repository is: " << this->NewRevision
                                                  << "\n");
  return true;
}

std::string cmCTestGIT::FindGitDir()
{
  std::string git_dir;

  // Run "git rev-parse --git-dir" to locate the real .git directory.
  std::string git = this->CommandLineTool;
  std::vector<std::string> git_rev_parse = { git, "rev-parse", "--git-dir" };
  std::string git_dir_line;
  OneLineParser rev_parse_out(this, "rev-parse-out> ", git_dir_line);
  OutputLogger rev_parse_err(this->Log, "rev-parse-err> ");
  if (this->RunChild(git_rev_parse, &rev_parse_out, &rev_parse_err,
                     std::string{}, cmProcessOutput::UTF8)) {
    git_dir = git_dir_line;
  }
  if (git_dir.empty()) {
    git_dir = ".git";
  }

  // Git reports a relative path only when the .git directory is in
  // the current directory.
  if (git_dir[0] == '.') {
    git_dir = this->m_sourceDirectory + "/" + git_dir;
  }
#if defined(_WIN32) && !defined(__CYGWIN__)
  else if (git_dir[0] == '/') {
    // Cygwin Git reports a full path that Cygwin understands, but we
    // are a Windows application.  Run "cygpath" to get Windows path.
    std::string cygpath_exe =
      cmStrCat(cmSystemTools::GetFilenamePath(git), "/cygpath.exe");
    if (cmSystemTools::FileExists(cygpath_exe)) {
      std::vector<std::string> cygpath = { cygpath_exe, "-w", git_dir };
      OneLineParser cygpath_out(this, "cygpath-out> ", git_dir_line);
      OutputLogger cygpath_err(this->Log, "cygpath-err> ");
      if (this->RunChild(cygpath, &cygpath_out, &cygpath_err, std::string{},
                         cmProcessOutput::UTF8)) {
        git_dir = git_dir_line;
      }
    }
  }
#endif
  return git_dir;
}

std::string cmCTestGIT::FindTopDir()
{
  std::string top_dir = this->m_sourceDirectory;

  // Run "git rev-parse --show-cdup" to locate the top of the tree.
  std::string git = this->CommandLineTool;
  std::vector<std::string> git_rev_parse = { git, "rev-parse", "--show-cdup" };
  std::string cdup;
  OneLineParser rev_parse_out(this, "rev-parse-out> ", cdup);
  OutputLogger rev_parse_err(this->Log, "rev-parse-err> ");
  if (this->RunChild(git_rev_parse, &rev_parse_out, &rev_parse_err, "",
                     cmProcessOutput::UTF8) &&
      !cdup.empty()) {
    top_dir += "/";
    top_dir += cdup;
    top_dir = cmSystemTools::ToNormalizedPathOnDisk(top_dir);
  }
  return top_dir;
}

bool cmCTestGIT::UpdateByFetchAndReset()
{
  std::string git = this->CommandLineTool;

  // Use "git fetch" to get remote commits.
  std::vector<std::string> git_fetch;
  git_fetch.push_back(git);
  git_fetch.emplace_back("fetch");

  // Add user-specified update options.
  std::string opts = this->m_pMakefile->GetSafeDefinition("CTEST_UPDATE_OPTIONS");
  if (opts.empty()) {
    opts = this->m_pMakefile->GetSafeDefinition("CTEST_GIT_UPDATE_OPTIONS");
  }
  std::vector<std::string> args = cmSystemTools::ParseArguments(opts);
  cm::append(git_fetch, args);

  // Fetch upstream refs.
  OutputLogger fetch_out(this->Log, "fetch-out> ");
  OutputLogger fetch_err(this->Log, "fetch-err> ");
  if (!this->RunUpdateCommand(git_fetch, &fetch_out, &fetch_err)) {
    return false;
  }

  // Identify the merge head that would be used by "git pull".
  std::string sha1;
  {
    std::string fetch_head = this->FindGitDir() + "/FETCH_HEAD";
    cmsys::ifstream fin(fetch_head.c_str(), std::ios::in | std::ios::binary);
    if (!fin) {
      this->Log << "Unable to open " << fetch_head << "\n";
      return false;
    }
    std::string line;
    while (sha1.empty() && cmSystemTools::GetLineFromStream(fin, line)) {
      this->Log << "FETCH_HEAD> " << line << "\n";
      if (line.find("\tnot-for-merge\t") == std::string::npos) {
        std::string::size_type pos = line.find('\t');
        if (pos != std::string::npos) {
          sha1 = std::move(line);
          sha1.resize(pos);
        }
      }
    }
    if (sha1.empty()) {
      this->Log << "FETCH_HEAD has no upstream branch candidate!\n";
      return false;
    }
  }

  // Reset the local branch to point at that tracked from upstream.
  std::vector<std::string> git_reset = { git, "reset", "--hard", sha1 };
  OutputLogger reset_out(this->Log, "reset-out> ");
  OutputLogger reset_err(this->Log, "reset-err> ");
  return this->RunChild(git_reset, &reset_out, &reset_err);
}

bool cmCTestGIT::UpdateByCustom(std::string const& custom)
{
  cmList git_custom_command{ custom, cmList::EmptyElements::Yes };
  std::vector<std::string> git_custom;
  git_custom.reserve(git_custom_command.size());
  cm::append(git_custom, git_custom_command);

  OutputLogger custom_out(this->Log, "custom-out> ");
  OutputLogger custom_err(this->Log, "custom-err> ");
  return this->RunUpdateCommand(git_custom, &custom_out, &custom_err);
}

bool cmCTestGIT::UpdateInternal()
{
  std::string custom =
    this->m_pMakefile->GetSafeDefinition("CTEST_GIT_UPDATE_CUSTOM");
  if (!custom.empty()) {
    return this->UpdateByCustom(custom);
  }
  return this->UpdateByFetchAndReset();
}

bool cmCTestGIT::UpdateImpl()
{
  if (!this->UpdateInternal()) {
    return false;
  }

  std::string top_dir = this->FindTopDir();
  std::string git = this->CommandLineTool;
  std::string recursive = "--recursive";
  std::string sync_recursive = "--recursive";

  // Git < 1.6.5 did not support submodule --recursive
  bool support_recursive = true;
  if (this->GetGitVersion() < cmCTestGITVersion(1, 6, 5, 0)) {
    support_recursive = false;
    // No need to require >= 1.6.5 if there are no submodules.
    if (cmSystemTools::FileExists(top_dir + "/.gitmodules")) {
      this->Log << "Git < 1.6.5 cannot update submodules recursively\n";
    }
  }

  // Git < 1.8.1 did not support sync --recursive
  bool support_sync_recursive = true;
  if (this->GetGitVersion() < cmCTestGITVersion(1, 8, 1, 0)) {
    support_sync_recursive = false;
    // No need to require >= 1.8.1 if there are no submodules.
    if (cmSystemTools::FileExists(top_dir + "/.gitmodules")) {
      this->Log << "Git < 1.8.1 cannot synchronize submodules recursively\n";
    }
  }

  OutputLogger submodule_out(this->Log, "submodule-out> ");
  OutputLogger submodule_err(this->Log, "submodule-err> ");

  bool ret;

  if (this->m_pMakefile->IsOn("CTEST_GIT_INIT_SUBMODULES")) {
    std::vector<std::string> git_submodule_init = { git, "submodule", "init" };
    ret = this->RunChild(git_submodule_init, &submodule_out, &submodule_err,
                         top_dir);

    if (!ret) {
      return false;
    }
  }

  std::vector<std::string> git_submodule_sync = { git, "submodule", "sync" };
  if (support_sync_recursive) {
    git_submodule_sync.push_back(sync_recursive);
  }
  ret = this->RunChild(git_submodule_sync, &submodule_out, &submodule_err,
                       top_dir);

  if (!ret) {
    return false;
  }

  std::vector<std::string> git_submodule = { git, "submodule", "update" };
  if (support_recursive) {
    git_submodule.push_back(recursive);
  }
  return this->RunChild(git_submodule, &submodule_out, &submodule_err,
                        top_dir);
}

unsigned int cmCTestGIT::GetGitVersion()
{
  if (!this->CurrentGitVersion) {
    std::string git = this->CommandLineTool;
    std::vector<std::string> git_version = { git, "--version" };
    std::string version;
    OneLineParser version_out(this, "version-out> ", version);
    OutputLogger version_err(this->Log, "version-err> ");
    unsigned int v[4] = { 0, 0, 0, 0 };
    if (this->RunChild(git_version, &version_out, &version_err) &&
        sscanf(version.c_str(), "git version %u.%u.%u.%u", &v[0], &v[1], &v[2],
               &v[3]) >= 3) {
      this->CurrentGitVersion = cmCTestGITVersion(v[0], v[1], v[2], v[3]);
    }
  }
  return this->CurrentGitVersion;
}

/* Diff format:

   :src-mode dst-mode src-sha1 dst-sha1 status\0
   src-path\0
   [dst-path\0]

   The format is repeated for every file changed.  The [dst-path\0]
   line appears only for lines with status 'C' or 'R'.  See 'git help
   diff-tree' for details.
*/
class cmCTestGIT::DiffParser : public cmCTestVC::LineParser
{
public:
  DiffParser(cmCTestGIT* git, char const* prefix)
    : LineParser('\0', false)
    , GIT(git)
  {
    this->SetLog(&git->Log, prefix);
  }

  using Change = cmCTestGIT::Change;
  std::vector<Change> Changes;

protected:
  cmCTestGIT* GIT;
  enum DiffFieldType
  {
    DiffFieldNone,
    DiffFieldChange,
    DiffFieldSrc,
    DiffFieldDst
  };
  DiffFieldType DiffField = DiffFieldNone;
  Change CurChange;

  void DiffReset()
  {
    this->DiffField = DiffFieldNone;
    this->Changes.clear();
  }

  bool ProcessLine() override
  {
    if (this->Line[0] == ':') {
      this->DiffField = DiffFieldChange;
      this->CurChange = Change();
    }
    if (this->DiffField == DiffFieldChange) {
      // :src-mode dst-mode src-sha1 dst-sha1 status
      if (this->Line[0] != ':') {
        this->DiffField = DiffFieldNone;
        return true;
      }
      char const* src_mode_first = this->Line.c_str() + 1;
      char const* src_mode_last = this->ConsumeField(src_mode_first);
      char const* dst_mode_first = this->ConsumeSpace(src_mode_last);
      char const* dst_mode_last = this->ConsumeField(dst_mode_first);
      char const* src_sha1_first = this->ConsumeSpace(dst_mode_last);
      char const* src_sha1_last = this->ConsumeField(src_sha1_first);
      char const* dst_sha1_first = this->ConsumeSpace(src_sha1_last);
      char const* dst_sha1_last = this->ConsumeField(dst_sha1_first);
      char const* status_first = this->ConsumeSpace(dst_sha1_last);
      char const* status_last = this->ConsumeField(status_first);
      if (status_first != status_last) {
        this->CurChange.m_action = *status_first;
        this->DiffField = DiffFieldSrc;
      } else {
        this->DiffField = DiffFieldNone;
      }
    } else if (this->DiffField == DiffFieldSrc) {
      // src-path
      if (this->CurChange.m_action == 'C') {
        // Convert copy to addition of destination.
        this->CurChange.m_action = 'A';
        this->DiffField = DiffFieldDst;
      } else if (this->CurChange.m_action == 'R') {
        // Convert rename to deletion of source and addition of destination.
        this->CurChange.m_action = 'D';
        this->CurChange.Path = this->Line;
        this->Changes.push_back(this->CurChange);

        this->CurChange = Change('A');
        this->DiffField = DiffFieldDst;
      } else {
        this->CurChange.Path = this->Line;
        this->Changes.push_back(this->CurChange);
        this->DiffField = this->DiffFieldNone;
      }
    } else if (this->DiffField == DiffFieldDst) {
      // dst-path
      this->CurChange.Path = this->Line;
      this->Changes.push_back(this->CurChange);
      this->DiffField = this->DiffFieldNone;
    }
    return true;
  }

  char const* ConsumeSpace(char const* c)
  {
    while (*c && cmIsSpace(*c)) {
      ++c;
    }
    return c;
  }
  char const* ConsumeField(char const* c)
  {
    while (*c && !cmIsSpace(*c)) {
      ++c;
    }
    return c;
  }
};

/* Commit format:

   commit ...\n
   tree ...\n
   parent ...\n
   author ...\n
   committer ...\n
   \n
       Log message indented by (4) spaces\n
       (even blank lines have the spaces)\n
 [[
   \n
   [Diff format]
 OR
   \0
 ]]

   The header may have more fields.  See 'git help diff-tree'.
*/
class cmCTestGIT::CommitParser : public cmCTestGIT::DiffParser
{
public:
  CommitParser(cmCTestGIT* git, char const* prefix)
    : DiffParser(git, prefix)
  {
    this->Separator = SectionSep[this->Section];
  }

private:
  using Revision = cmCTestGIT::Revision;
  enum SectionType
  {
    SectionHeader,
    SectionBody,
    SectionDiff,
    SectionCount
  };
  static char const SectionSep[SectionCount];
  SectionType Section = SectionHeader;
  Revision Rev;

  struct Person
  {
    std::string m_name;
    std::string EMail;
    unsigned long Time = 0;
    long TimeZone = 0;
  };

  void ParsePerson(char const* str, Person& person)
  {
    // Person Name <person@domain.com> 1234567890 +0000
    char const* c = str;
    while (*c && cmIsSpace(*c)) {
      ++c;
    }

    char const* name_first = c;
    while (*c && *c != '<') {
      ++c;
    }
    char const* name_last = c;
    while (name_last != name_first && cmIsSpace(*(name_last - 1))) {
      --name_last;
    }
    person.m_name.assign(name_first, name_last - name_first);

    char const* email_first = *c ? ++c : c;
    while (*c && *c != '>') {
      ++c;
    }
    char const* email_last = *c ? c++ : c;
    person.EMail.assign(email_first, email_last - email_first);

    person.Time = strtoul(c, const_cast<char**>(&c), 10);
    person.TimeZone = strtol(c, const_cast<char**>(&c), 10);
  }

  bool ProcessLine() override
  {
    if (this->Line.empty()) {
      if (this->Section == SectionBody && this->LineEnd == '\0') {
        // Skip SectionDiff
        this->NextSection();
      }
      this->NextSection();
    } else {
      switch (this->Section) {
        case SectionHeader:
          this->DoHeaderLine();
          break;
        case SectionBody:
          this->DoBodyLine();
          break;
        case SectionDiff:
          this->DiffParser::ProcessLine();
          break;
        case SectionCount:
          break; // never happens
      }
    }
    return true;
  }

  void NextSection()
  {
    this->Section =
      static_cast<SectionType>((this->Section + 1) % SectionCount);
    this->Separator = SectionSep[this->Section];
    if (this->Section == SectionHeader) {
      this->GIT->DoRevision(this->Rev, this->Changes);
      this->Rev = Revision();
      this->DiffReset();
    }
  }

  void DoHeaderLine()
  {
    // Look for header fields that we need.
    if (cmHasLiteralPrefix(this->Line, "commit ")) {
      this->Rev.Rev = this->Line.substr(7);
    } else if (cmHasLiteralPrefix(this->Line, "author ")) {
      Person author;
      this->ParsePerson(this->Line.c_str() + 7, author);
      this->Rev.Author = author.m_name;
      this->Rev.EMail = author.EMail;
      this->Rev.Date = this->FormatDateTime(author);
    } else if (cmHasLiteralPrefix(this->Line, "committer ")) {
      Person committer;
      this->ParsePerson(this->Line.c_str() + 10, committer);
      this->Rev.Committer = committer.m_name;
      this->Rev.CommitterEMail = committer.EMail;
      this->Rev.CommitDate = this->FormatDateTime(committer);
    }
  }

  void DoBodyLine()
  {
    // Commit log lines are indented by 4 spaces.
    if (this->Line.size() >= 4) {
      this->Rev.Log += this->Line.substr(4);
    }
    this->Rev.Log += "\n";
  }

  std::string FormatDateTime(Person const& person)
  {
    // Convert the time to a human-readable format that is also easy
    // to machine-parse: "CCYY-MM-DD hh:mm:ss".
    time_t seconds = static_cast<time_t>(person.Time);
    struct tm* t = gmtime(&seconds);
    char dt[1024];
    snprintf(dt, sizeof(dt), "%04d-%02d-%02d %02d:%02d:%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour,
             t->tm_min, t->tm_sec);
    std::string out = dt;

    // Add the time-zone field "+zone" or "-zone".
    char tz[32];
    if (person.TimeZone >= 0) {
      snprintf(tz, sizeof(tz), " +%04ld", person.TimeZone);
    } else {
      snprintf(tz, sizeof(tz), " -%04ld", -person.TimeZone);
    }
    out += tz;
    return out;
  }
};

char const cmCTestGIT::CommitParser::SectionSep[SectionCount] = { '\n', '\n',
                                                                  '\0' };

bool cmCTestGIT::LoadRevisions()
{
  // Use 'git rev-list ... | git diff-tree ...' to get revisions.
  std::string range = this->OldRevision + ".." + this->NewRevision;
  std::string git = this->CommandLineTool;
  std::vector<std::string> git_rev_list = { git, "rev-list", "--reverse",
                                            range, "--" };
  std::vector<std::string> git_diff_tree = {
    git,  "diff-tree", "--stdin",      "--always",
    "-z", "-r",        "--pretty=raw", "--encoding=utf-8"
  };
  this->Log << cmCTestGIT::ComputeCommandLine(git_rev_list) << " | "
            << cmCTestGIT::ComputeCommandLine(git_diff_tree) << "\n";

  cmUVProcessChainBuilder builder;
  builder.AddCommand(git_rev_list)
    .AddCommand(git_diff_tree)
    .SetWorkingDirectory(this->m_sourceDirectory);

  CommitParser out(this, "dt-out> ");
  OutputLogger err(this->Log, "dt-err> ");
  cmCTestGIT::RunProcess(builder, &out, &err, cmProcessOutput::UTF8);

  // Send one extra zero-byte to terminate the last record.
  out.Process("", 1);

  return true;
}

bool cmCTestGIT::LoadModifications()
{
  std::string git = this->CommandLineTool;

  // Use 'git update-index' to refresh the index w.r.t. the work tree.
  std::vector<std::string> git_update_index = { git, "update-index",
                                                "--refresh" };
  OutputLogger ui_out(this->Log, "ui-out> ");
  OutputLogger ui_err(this->Log, "ui-err> ");
  this->RunChild(git_update_index, &ui_out, &ui_err, "",
                 cmProcessOutput::UTF8);

  // Use 'git diff-index' to get modified files.
  std::vector<std::string> git_diff_index = { git, "diff-index", "-z", "HEAD",
                                              "--" };
  DiffParser out(this, "di-out> ");
  OutputLogger err(this->Log, "di-err> ");
  this->RunChild(git_diff_index, &out, &err, "", cmProcessOutput::UTF8);

  for (Change const& c : out.Changes) {
    this->DoModification(PathModified, c.Path);
  }
  return true;
}
