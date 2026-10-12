// command_policy_zsh_tests.cpp
//
// Standalone checks for EvaluateZshCommand (macOS shell tool policy).
// Built by the command_policy_zsh_tests CMake target on Apple only:
//   cmake --build build_macos --target command_policy_zsh_tests &&
//   build_macos/command_policy_zsh_tests
#include "command_policy_zsh.h"

#include <cstdio>

namespace {
int g_fail = 0;
enum Want { kAllow, kApprove, kReject };
void Check(const char* cmd, Want want)
{
    const PolicyDecision d = EvaluateZshCommand(cmd);
    const Want got = d.allowed ? kAllow : d.requiresApproval ? kApprove : kReject;
    if (got != want) {
        static const char* names[] = {"allow", "approve", "reject"};
        std::printf("FAIL  %-48s want %-7s got %-7s (%s)\n", cmd, names[want],
                    names[got], d.reason.c_str());
        ++g_fail;
    }
}
} // namespace

int main()
{
    // Read-only inspection runs immediately.
    Check("ls -la", kAllow);
    Check("ls ~/Desktop", kAllow);
    Check("ls $HOME/Documents", kAllow);
    Check("cat notes.txt | grep -i todo | wc -l", kAllow);
    Check("find . -name '*.cpp' -maxdepth 2", kAllow);
    Check("find / -name foo 2>/dev/null", kAllow);
    Check("find . -name x 2> /dev/null | head -5", kAllow);
    Check("grep -rn \"timeout\" src", kAllow);
    Check("du -sh *", kAllow);
    Check("df -h", kAllow);
    Check("sw_vers", kAllow);
    Check("date +%Y-%m-%d", kAllow);
    Check("date", kAllow);
    Check("sysctl -n hw.memsize", kAllow);
    Check("defaults read com.apple.dock", kAllow);
    Check("mdfind -name report.pdf", kAllow);
    Check("ps aux | sort -k3 -r | head", kAllow);
    Check("which python3 2>&1", kAllow);
    Check("shasum -a 256 file.bin", kAllow);
    Check("plutil -p Info.plist", kAllow);
    Check("echo \"user is $USER\"", kAllow);

    // Anything that writes, deletes, runs code or chains needs approval.
    Check("rm -rf build", kApprove);
    Check("mv a b", kApprove);
    Check("git status", kApprove);
    Check("brew install wget", kApprove);
    Check("python3 script.py", kApprove);
    Check("./run.sh", kApprove);
    Check("/bin/ls", kApprove);
    Check("ls; rm x", kApprove);
    Check("ls && rm x", kApprove);
    Check("ls || rm x", kApprove);
    Check("sleep 5 &", kApprove);
    Check("echo hi > out.txt", kApprove);
    Check("echo hi >> out.txt", kApprove);
    Check("cat < in.txt", kApprove);
    Check("echo $(rm x)", kApprove);
    Check("echo `rm x`", kApprove);
    Check("echo \"$(rm x)\"", kApprove);
    Check("echo ${HOME}", kApprove);
    Check("(cd /tmp && ls)", kApprove);
    Check("ls *(e:'rm x':)", kApprove);
    Check("find . -delete", kApprove);
    Check("find . -name '*.o' -exec rm {} +", kApprove);
    Check("sort -o out.txt in.txt", kApprove);
    Check("sort --output=out.txt in.txt", kApprove);
    Check("uniq in.txt out.txt", kApprove);
    Check("xxd -r dump.hex", kApprove);
    Check("tree -o out.txt", kApprove);
    Check("sysctl -w kern.x=1", kApprove);
    Check("defaults write com.apple.dock autohide -bool true", kApprove);
    Check("hostname newname", kApprove);
    Check("date 0101120026", kApprove);
    Check("rg --pre ./evil pattern", kApprove);
    Check("DYLD_INSERT_LIBRARIES=x ls", kApprove);
    Check("ls\nrm x", kApprove);
    Check("ls caf\xC3\xA9", kApprove);
    Check("ls | xargs rm", kApprove);
    Check("cat x | sh", kApprove);
    Check("ls |& cat", kApprove);

    // Malformed or empty commands are rejected outright.
    Check("", kReject);
    Check("   ", kReject);
    Check("echo 'unterminated", kReject);
    Check("echo \"unterminated", kReject);

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "all zsh policy checks passed",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
