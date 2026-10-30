# LlamaBoss: instructions for Claude Code (Mac mini)

## Bugs you find
- Mac-only code: fix it yourself, commit it, and add one line to the "## Mac" section of NEXT_RELEASE.md on the share.
- Windows-only or shared code (anything both platforms compile): do NOT change it. Add it under "## Known issues (fix in next release)" in NEXT_RELEASE.md as one line: file:line — what's wrong — suggested fix. Then mention it in your final summary under "For the PC side".
- If a shared-code bug blocks the Mac release, you may work around it in Mac-only code (behind #ifdef __APPLE__ / __WXMAC__ or in a Mac-only file), and say so in the Known issues line so the PC fix can replace the workaround.

## GitHub
- Push only to the `github` remote and only branches named `macos/*`.
- Never push `main`, never force-push, never delete remote branches.
- After a Mac release, push the Mac branch and report the pushed commit hash.
