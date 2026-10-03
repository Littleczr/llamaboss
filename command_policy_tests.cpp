// command_policy_tests.cpp
//
// Regression harness for command_policy.cpp (2026-09-30).  Pure, no wx:
//   g++ -std=c++17 -I . command_policy_tests.cpp command_policy.cpp && ./a.out
//
// Covers the quote-scanner fix: backtick escapes and '#' comments used to
// make valid PowerShell fail the quote-balance check and be REJECTED
// outright (not even offered for approval).  Also pins the safety
// properties around comments: skipping comment text must never widen
// what auto-runs.
#include "command_policy.h"
#include <iostream>
#include <string>
static int fails=0;
void t(const std::string& c, const char* want){
  auto d=EvaluatePowerShellCommand(c);
  const char* got = d.allowed?"ALLOW":d.requiresApproval?"APPROVE":"REJECT";
  bool ok = std::string(got)==want; if(!ok) ++fails;
  std::cout << (ok?"PASS ":"FAIL ") << got << " (want " << want << ")  " << c << (d.reason.empty()?"":"   ["+d.reason+"]") << "\n";
}
int main(){
  // ChatGPT's two repros
  t("Write-Output \"hello`\"\"", "APPROVE");
  t("Get-Item . # user's folder", "ALLOW");
  // comments
  t("Get-Item . # users folder", "ALLOW");
  t("Get-Item . # x | Remove-Item C:\\x", "APPROVE");    // separators in comment: stay gated
  t("Get-Item . #x ; Remove-Item C:\\x", "APPROVE");
  t("Get-ChildItem D:\\ -Recurse -File # find docs, don't recurse twice", "ALLOW");
  t("Get-Item .#; Remove-Item C:\\x", "APPROVE");       // glued # is NOT a comment
  t("Get-Item 'a#b'", "ALLOW");
  t("Get-Item . # c\nRemove-Item x", "APPROVE");        // newline still gates
  t("Get-Item <# note #> .", "APPROVE");
  t("Get-Item <# user's #> .", "APPROVE");
  t("Get-Item <# never closed", "REJECT");
  // backticks
  t("Write-Output 'it`s'", "APPROVE");                  // literal in single quotes
  t("Write-Output 'a`' ; Get-Item .", "APPROVE");       // ` does not escape the closing '
  t("Write-Output \"a``\"", "APPROVE");                 // escaped backtick then close
  t("Write-Output \"unterminated", "REJECT");
  t("Write-Output 'unterminated", "REJECT");
  // unchanged behavior
  t("Get-ChildItem -Path 'D:\\' -Recurse -File -Filter '*child*'", "ALLOW");
  t("Write-Output 'it''s'", "APPROVE");
  t("Remove-Item C:\\x", "APPROVE");
  // here-strings (2026-10-01): quotes inside a here-string body are
  // ordinary characters.  The M9 rejection was L'"' inside @'...'@.
  t("$t=@'\nconst wchar_t q = L'\"';\n'@\nWrite-Output $t", "APPROVE");
  t("$t=@'\r\nconst wchar_t q = L'\"';\r\n'@\r\nWrite-Output $t", "APPROVE");  // CRLF
  t("$t=@'\ndon't \"stop\" ; | here\n'@", "APPROVE");                         // separators inert
  t("$t=@\"\nvalue: $($x) and 'quoted' \"inner\"\n\"@", "APPROVE");           // expandable
  t("$t=@'\n'@", "APPROVE");                                                    // empty body
  t("$t=@'   \nbody\n'@", "APPROVE");                                          // trailing spaces after opener
  t("$t=@'\nbody\n  '@\nWrite-Output x", "REJECT");                            // closer must start a line
  t("$t=@'\nnever closed", "REJECT");
  t("$t=@\"\nnever closed", "REJECT");
  t("$a=@'\none\n'@\n$b=@'\ntwo's\n'@", "APPROVE");                           // two in a row
  t("$x = @('a','b')", "APPROVE");                                              // @( still the array digraph
  t("Get-Item 'a@b'", "ALLOW");                                                 // @ inside a plain string
  // lint: underscore variable inside a double here-string is still flagged,
  // and a quote inside a literal here-string no longer misleads the linter
  {
    auto w = LintPowerShellHazards("$s=@\"\nv=$Version_DRAFT\n\"@");
    bool ok = w.size()==1; if(!ok) ++fails;
    std::cout << (ok?"PASS ":"FAIL ") << "lint flags $Version_DRAFT in @\"..\"@ (" << w.size() << " warning(s))\n";
    w = LintPowerShellHazards("$s=@'\nL'\"' $Not_A_Var\n'@");
    ok = w.empty(); if(!ok) ++fails;
    std::cout << (ok?"PASS ":"FAIL ") << "lint ignores literal @'..'@ body (" << w.size() << " warning(s))\n";
  }
  std::cout << (fails?"FAILURES: "+std::to_string(fails) : std::string("ALL PASSED")) << "\n";
}
