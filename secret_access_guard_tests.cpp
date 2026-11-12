// secret_access_guard_tests.cpp
//
// Regression harness for secret_access_guard.h and its PowerShell wiring.
// Pure, no wx:
//   g++ -std=c++17 -I . secret_access_guard_tests.cpp command_policy.cpp && ./a.out
#include "secret_access_guard.h"
#include "command_policy.h"
#include <iostream>
#include <string>

static int fails = 0;

static void blocked(const std::string& text, bool want)
{
    const bool got = !DetectSecretStoreAccess(text).empty();
    const bool ok = got == want;
    if (!ok) ++fails;
    std::cout << (ok ? "PASS " : "FAIL ") << (got ? "BLOCK" : "allow")
              << " (want " << (want ? "BLOCK" : "allow") << ")  " << text << "\n";
}

int main()
{
    // The two commands from the 2026-10-30 ClipJoiner chat.
    blocked("$env:GIT_TERMINAL_PROMPT='0'; $response = @('protocol=https','host=github.com','') | git credential fill; if ($LASTEXITCODE -ne 0) { throw 'Git credential fill failed' }", true);
    blocked("$request = 'protocol=https' + [Environment]::NewLine + 'host=github.com'; $response = $request | git credential fill", true);
    // Variants
    blocked("git -C 'C:\\repo' credential   fill", true);
    blocked("GIT CREDENTIAL APPROVE", true);
    blocked("git credential reject", true);
    blocked("git credential-manager get", true);
    blocked("subprocess.run([\"git\", \"credential\", \"fill\"], input=b\"protocol=https\\nhost=github.com\\n\\n\")", true);
    blocked("gh auth token", true);
    blocked("gh auth status --show-token", true);
    blocked("cmdkey /list", true);
    blocked("vaultcmd /listcreds:\"Windows Credentials\"", true);
    blocked("Get-StoredCredential -Target git:https://github.com", true);
    blocked("[Windows.Security.Credentials.PasswordVault,Windows.Security.Credentials,ContentType=WindowsRuntime]", true);
    blocked("[System.Security.Cryptography.ProtectedData]::Unprotect($b, $null, 'CurrentUser')", true);
    blocked("import keyring; keyring.get_password('github', 'me')", true);
    blocked("Get-Content $env:USERPROFILE\\.git-credentials", true);
    blocked("Get-Content \"$env:APPDATA\\GitHub CLI\\hosts.yml\"", true);
    blocked("Get-Content $env:LOCALAPPDATA\\LlamaBoss\\secrets.json", true);
    blocked("Copy-Item \"$env:LOCALAPPDATA\\Google\\Chrome\\User Data\\Default\\Login Data\" x.db", true);
    // Must stay allowed
    blocked("git -C 'C:\\Users\\Cesar\\source\\repos\\ClipJoiner' config --show-origin --get-all credential.helper", false);
    blocked("git config --global credential.helper manager", false);
    blocked("git push -u origin main", false);
    blocked("gh auth status", false);
    blocked("gh release create v0.3 x64\\Release\\ClipJoiner.exe --notes-file notes.md", false);
    blocked("$c = Get-Credential", false);
    blocked("Get-ChildItem C:\\repo -Filter secrets.json", false);
    blocked("Write-Output 'never store a credential in a file'", false);
    blocked("ToolArgsMayReadSecrets", false);

    // PowerShell policy: a secret read is REJECTED, not approval-gated.
    {
        const auto d = EvaluatePowerShellCommand("git credential fill");
        const bool ok = !d.allowed && !d.requiresApproval &&
                        d.reason.find("stored credential") != std::string::npos;
        if (!ok) ++fails;
        std::cout << (ok ? "PASS " : "FAIL ") << "policy rejects git credential fill  [" << d.reason << "]\n";
    }
    {
        const auto d = EvaluatePowerShellCommand("git push -u origin main");
        const bool ok = !d.allowed && d.requiresApproval;
        if (!ok) ++fails;
        std::cout << (ok ? "PASS " : "FAIL ") << "policy still approval-gates git push\n";
    }
    {
        const bool ok = ToolArgsMayReadSecrets("py") && ToolArgsMayReadSecrets("powershell") &&
                        !ToolArgsMayReadSecrets("read") && !ToolArgsMayReadSecrets("grep");
        if (!ok) ++fails;
        std::cout << (ok ? "PASS " : "FAIL ") << "scanned tool set\n";
    }

    std::cout << (fails ? "FAILURES: " + std::to_string(fails) : std::string("ALL PASSED")) << "\n";
    return fails ? 1 : 0;
}
