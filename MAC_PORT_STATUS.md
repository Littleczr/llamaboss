# macOS port status

Living checkpoint for the LlamaBoss macOS port. Update it at every milestone.

## Baseline

- Branch: `macos/port-v0.1.21`
- **Base: release tag `v0.1.21` = `d914c1c` on `origin/main`** (rebased 2026-10-09).
  `git diff v0.1.21` shows the whole port. Backup of the pre-rebase branch:
  `macos/port-v0.1.21-backup` (based on the zip snapshot commit `c9a0021`).
- The rebase dropped `c9a0021`. The only conflict was `.gitignore`: the release block was
  kept verbatim and the Mac entries appended. Every other release file matches `v0.1.21`
  exactly, and the Mac patch is byte-identical to the pre-rebase one.
- The Windows build still uses `LlamaBoss.vcxproj`; nothing Mac-specific is in it.
  **Mac commits have not been compiled on Windows yet.** Build this branch in Visual
  Studio before merging.

### Files added by the Mac commits

| File(s) | Windows needs it? |
| --- | --- |
| `lb_utf.h` | **Yes, as an include**: `path_safety.cpp`, `cmd_executor.cpp`, `python_session.cpp`, `tool_ls.cpp`, `tool_grep.cpp`, `tool_web_fetch.cpp`, `tool_open.cpp` and `workspace_delta.h` include it unconditionally (it sits in the repo root, so it resolves without project changes). Only its functions are unused on Windows. Should be listed as a `ClInclude` in `LlamaBoss.vcxproj`/`.filters` for the IDE; not yet done. |
| `src/platform/lb_process.h`, `lb_process_posix.cpp`, `secrets_backend.h`, `secrets_backend_macos.cpp` | Mac-only; included only under `#ifndef _WIN32` / `#ifndef __WXMSW__` |
| Other `src/platform/*` (June Phase 0 layer, incl. `*_win.cpp`) | Not used by any app code on either platform; not in the vcxproj |
| `CMakeLists.txt`, `macos/Info.plist`, `MAC_PORT_STATUS.md` | Mac build/docs only |

### Windows-compiled lines the Mac commits changed (outside new `#ifdef` branches)

All are intended to be behavior-identical on Windows, but none has been compiled with MSVC:
- 30 `std::ifstream/ofstream(Utf8ToWide(p))` → `std::ifstream/ofstream(std::filesystem::path(Utf8ToWide(p)))`
  (on MSVC both open through the wide path).
- `secrets_store.cpp` (2): `SecureZeroMemory` → `wxSecureZeroMemory` (wx calls `SecureZeroMemory` on MSW).
- `agent_controller.cpp` `AgentJoinPath`, `chat_history.cpp` elided relPath: `'\\'` → `wxFILE_SEP_PATH` (`'\\'` on MSW).
- `tool_grep.cpp`: `"\\"` → `kPathSep` (`"\\"` on Windows).
- `tool_mkdir.cpp`, `tool_delete.cpp`: the error check moved into a `const bool` set in each platform branch.
- `server_manager.cpp` `StartServer`: the whole-function `#ifdef __WXMSW__` was narrowed to the spawn section;
  the Windows statement sequence is unchanged.

## Design principle: Mac identity, shared behavior

The macOS build should feel like a Mac app, not a copy of the Windows app.
Same features and the same safety guarantees; platform-native implementation and feel.

**Shared (must match Windows):**
- Features, agent behavior, tool semantics, approval and safety rules
- File formats: chats, notes, projects, settings JSON
- Error handling: honest "not available on macOS yet", never fake success

**Mac-native (do it the Mac way, don't imitate Windows):**
- Shell: zsh, not a PowerShell port
- Secrets: Keychain; data in ~/Library/Application Support
- UI: Cocoa conventions. Native controls, the menu bar (About/Settings
  under the app menu, Cmd+, for Settings), Cmd shortcuts, standard
  window and sheet behavior. Don't fight native widgets to copy Windows
  styling; when a Windows look can't be reproduced cleanly, design the
  Mac equivalent.
- Wording: "unified memory", not "VRAM"; Finder, not Explorer
- Packaging: .app bundle, .icns, signing/notarization, a Mac-style updater

**Rules:**
- Never change Windows behavior or text to accommodate the Mac. Mac
  differences go in #ifdef/platform files; `unifdef -D_WIN32` of
  touched files must still match the Windows view.
- When unsure whether something should match Windows or be Mac-native,
  ask: is it behavior (match) or look/feel/convention (Mac-native)?
- Record each deliberate Mac divergence in a "Mac differences" list in
  this file, so it's clear what's intended versus not yet ported.

## Workflow

The user develops the Windows version on the PC. Work on this branch is the Mac version
only: Mac code lives in `#ifdef __WXMAC__` / `#ifndef _WIN32` blocks or Mac-only files, and
the Windows view of every touched file stays identical to main (checked with `unifdef`).
The Mac may behave differently from Windows where that suits Mac users; ideas for the
Windows version go into this file as suggestions, not code.

**Running changelog (standing rule, user 2026-10-15):** `D:\ForMacMini\NEXT_RELEASE.md` on the
share is the single changelog between releases.
- Each Mac change: one plain-English line under `## Mac`: date · what changed for users ·
  commit hash.
- PC-side findings go under `## Known issues` as `- [ ] <issue> (file:line)`. No separate
  PC_FINDINGS files.
- Changed but not verified: a line under `## Not yet tested`.
- Never remove lines Claude did not write. Keep the file's BOM and line endings.

**Website (standing rule, user 2026-10-15; the PC enforces it too):**
- Build any site change from the package named in `D:\ForMacMini\Websites\LATEST.json`,
  never an older zip.
- Before deploying, confirm the live site still matches that package. If it does not, stop
  and tell the user. Checker: scratchpad `tools/site_live_check.py <folder>` compares every
  file. It ignores the Cloudflare Web Analytics beacon tag that Cloudflare injects into
  served HTML.
- After a successful production deploy:
  1. Zip the exact deployed folder with the files at the zip root (no wrapping folder) to
     `Websites\LlamaBoss_Website_v<site>_mac_v<macver>_<yyyyMMdd_HHmmss>.zip`.
  2. Overwrite `LATEST.json` with `{"package": "<full D:\ path>", "sha256": "<UPPERCASE>",
     "site_version", "app_windows", "app_mac" (both from live version.json), "by": "mac",
     "recorded": "<ISO time>"}`.

**Mac release role (after the PC publishes Windows):**
1. Merge main.
2. Build, notarize and staple the Mac DMG.
3. Upload it to R2.
4. Update the docs from the repo's `PATCH_NOTES.md` (both platforms).
5. Deploy the site per the website rule.
6. Add the Mac lines to `NEXT_RELEASE.md`.

## Mac differences (deliberate)

Intended divergences from Windows. Features that are simply not ported yet are listed under
"Stubbed or unavailable on macOS" instead.

| Area | Windows | macOS | Why |
| --- | --- | --- | --- |
| Shell tool | `powershell` tool, PowerShell 5.1/7 | `shell` tool, `/bin/zsh -f`; read-only allowlist of BSD tools (`command_policy_zsh.cpp`) | Native shell; no PowerShell install needed |
| .ps1 scripts | `write_powershell_script` tool | not registered | No PowerShell on macOS |
| Agent prompt | PowerShell rules, backslash paths | zsh/BSD rules, forward-slash paths, `~` (under `#else`) | Same rules, Mac wording |
| Secrets at rest | DPAPI | AES-256-GCM, key in the login Keychain | Platform key store |
| App data | `%LOCALAPPDATA%\LlamaBoss` | `~/Library/Application Support/LlamaBoss` | Mac convention |
| Python runtime | the user's installed Python (`py -3`, `python`, `python3`) | bundled CPython 3.13 in `Contents/Resources/python-runtime`, always used when present (2026-10-13) | Works on a Mac with no Python installed; same version everywhere |
| Python packages | `pip install --user` | LlamaBoss environment in the data folder (`--system-site-packages`), built on the bundled Python | Homebrew Python refuses `--user` (PEP 668); the bundle is signed and read-only |
| Bundled Python packages | none | pypdf and openpyxl ship with the runtime; PyMuPDF installs on first use through the approval card | PyMuPDF is AGPL; LlamaBoss does not redistribute it (user decision 2026-10-13) |
| Python helpers | RCDATA in the .exe | files in `Contents/Resources/python` | App-bundle resources |
| llama.cpp | CPU/CUDA builds | Metal build in `Contents/Helpers/llama` | Apple silicon GPU |
| Fonts | Consolas / Cascadia Code, Windows point sizes | Menlo, sizes x4/3 (`lb_ui_font.h`) | Same physical size; built-in Mac face |
| App icon | `app_icon.ico` | `macos/app_icon.icns` (generated from the .ico) | Bundle icon format |
| Missing `.ico` at startup | file fallback | icon comes from the bundle; no file lookup | Avoids a modal error on launch |
| Buttons | flat buttons filled with theme colours | native push buttons (`lb_mac_native.mm`): one accent-tinted button per dialog (default/OK), Send and New Chat accent in the main window, stop-red stays red, the rest plain; widened to fit | NSButton ignores theme fills; Mac convention is one default button per window |
| Data-losing confirmations | Yes / No, Yes is the default | Native NSAlert (`LbMacConfirm`): the buttons name the action (Delete / Replace / Sign Out / Stop Download) and Cancel (or Keep Downloading); Cancel is the default (blue, Return), Esc cancels, the action is red (destructive) | macOS convention (HIG); a stray Return cannot destroy data |
| GPU memory wording | "Needs 10+ GB VRAM", "fits in VRAM" | "Needs a Mac with 16 GB+ of memory" (GPU GB x 4/3, rounded up to a Mac memory size), "fits in memory" (`lb_mac_wording.h`) | Apple silicon has unified memory; the GPU can use about 3/4 of it |
| `py` session memory cap | Job Object: 2 GiB per process; allocations fail with `MemoryError` and the session survives | watchdog stops the kernel when its physical footprint passes 2 GiB; the session is lost and the card says why | macOS refuses `RLIMIT_AS`/`RLIMIT_DATA`, so a failing-allocation cap is not available |
| Web fetch transport | WinHTTP, system proxy settings | Poco::Net with lb_ssl's verified TLS context; no system proxy; Mac browser user-agent | No WinHTTP on macOS; the UA names the platform it runs on |
| Menu bar | none (toolbar only) | LlamaBoss menu (About, Settings… Cmd+,, Hide, Quit), File (New Chat, New Window, Open/Save Conversation, Close Window), Edit, View (Show or Hide Sidebar Ctrl+Cmd+S), Window, Help (llamaboss.com/docs) | Mac convention; verified by the user 2026-10-12 |
| New window | second launch hands off to the running app | Dock icon menu "New Window" (Finder/Dock never start a second copy) | Mac convention; verified by the user 2026-10-12 |
| Sidebar selection | stays on the last clicked row after New Chat | a single selection follows the open chat | Mac list convention (Mail, Notes) |
| Control appearance | n/a | app appearance (dark/light Aqua) set from the theme's background at startup | Native controls match the theme on a system in the other mode |
| MTP + vision projector | on the GPU up to `kMtpVisionGpuCtxLimit` (262144), CPU (`--no-mmproj-offload`) above it | always on the GPU (Metal) | Unified memory: moving the projector to the CPU frees nothing and only slows image encoding (2026-10-30) |
| Provider dialog scan wording | "SERVERS FOUND ON THIS PC", "Looking for servers on this PC..." | "... THIS MAC", "... on this Mac..." | Platform name (2026-10-30) |

## Toolchain (verified 2026-10-09)

| Item | Version |
| --- | --- |
| Machine | Apple M4 Mac mini, 16 GB, macOS 26.6.2 |
| Compiler | Apple clang 21.0.0 (arm64), C++17 (matches the vcxproj) |
| Build | CMake 4.3.1 + Ninja (Homebrew) |
| wxWidgets | Homebrew `wxwidgets@3.2` (`wx-config-3.2`) |
| Poco | Homebrew `poco` 1.15.3 (OpenSSL 3.6) |
| llama.cpp | release `b11429` = v0.6.0 (matches Windows 0.1.25), `llama-b11429-bin-macos-arm64.tar.gz` (Metal), minimum macOS 13.3 |

## Build

```sh
# one-time: unpack a llama.cpp macOS release (git-ignored)
mkdir -p third_party/llama.cpp
curl -L -o third_party/llama-b11429-bin-macos-arm64.tar.gz \
  https://github.com/ggml-org/llama.cpp/releases/download/b11429/llama-b11429-bin-macos-arm64.tar.gz
tar -xzf third_party/llama-b11429-bin-macos-arm64.tar.gz -C third_party/llama.cpp

cmake -S . -B build_macos -G Ninja
cmake --build build_macos
open build_macos/LlamaBoss.app
```

CMake copies `llama-server` and its dylibs into
`LlamaBoss.app/Contents/Helpers/llama/` (override with `-DLLAMABOSS_LLAMA_DIR=...`).
When the login keychain has an "Apple Development" (or, failing that, a self-signed
"LlamaBoss Development") code-signing identity, the build signs the bundle with it (see
"Keychain prompt after every rebuild" below).

## Data locations on macOS

| What | Where |
| --- | --- |
| App data, settings, logs, secrets.json | `~/Library/Application Support/LlamaBoss/` |
| Managed models (casual mode) | `~/Library/Application Support/LlamaBoss/models/<bundle>/` |
| Chats, projects, workspace | `~/LlamaBoss/` (Windows: `%USERPROFILE%\LlamaBoss`) |
| Notes (v0.1.23+) | `~/LlamaBoss/Notes/` (`paths.md` + subject files; index built on load) |
| API-key encryption key | login Keychain, service `LlamaBoss`, account `secrets-file-key` |

## Milestones

| # | Milestone | State |
| --- | --- | --- |
| 1 | Reproducible arm64 build | **Done**: all translation units compile and link (Homebrew dev build and static release build) |
| 2 | App launches, main window | **Done**: main window and Welcome dialog open |
| 3 | Conversations save and reopen (Unicode paths) | **Verified**: ASCII titles (2026-10-09); Unicode title with accents, CJK and emoji (2026-10-13). Chat folders use an ASCII slug |
| 4 | Local llama-server starts, reports errors, stops | **Verified**: Gemma 3 1B on Metal (2026-10-09); error path (2026-10-11); orphan cleanup (2026-10-12) |
| 5 | Remote endpoint streams; stop works | **Verified** streaming with OpenRouter GPT 6 Luna (2026-10-11 onward). Stop during a remote stream verified (2026-10-13) |
| 6 | Shell and Python tools | **Done**: zsh shell tool (2026-10-11), Python helpers and scripts (2026-10-11), `py` session (2026-10-12), web fetch (2026-10-12), bundled Python runtime (2026-10-13) |
| 7 | File-tool boundaries, protected credentials, packaged app | Credentials in the Keychain **verified**; packaged, notarized app **done** (2026-10-12). File-tool boundary cases (symlinks, case-sensitive volumes) implemented, not systematically tested |
| 8 | Public Mac release | **Done 2026-10-12**: Beta 0.1.23 on R2 and llamaboss.com. 0.1.24 with the bundled Python notarized and published 2026-10-13 (R2 + site 0.2.9). 0.1.25 notarized and published 2026-10-15 (R2 + site 0.2.11) |

## Implemented for macOS (compiles; runtime verification pending unless noted)

- `lb_utf.h`: UTF-8 ↔ `std::wstring` (wchar_t is UTF-32 on Darwin).
- `lb_windows.h`: pulls in POSIX headers on non-Windows builds.
- `tool_mutation_guard.h`: POSIX guard. It pins ancestor directories with `openat(O_NOFOLLOW)`,
  rejects symlinks (except root-owned `/tmp`, `/var`, `/etc` links directly under `/`),
  and checks dev/inode/size/mtime/birthtime.
- `tool_staged_write.h`: `O_EXCL` sibling temp file, `F_FULLFSYNC`, `rename` /
  `renamex_np(RENAME_EXCL)`.
- File tools: `tool_path`, `tool_ls`, `tool_grep`, `tool_delete`, `tool_mkdir`, `tool_write`,
  `tool_edit`, `tool_notes`, `tool_open` (`/usr/bin/open`), `var_store`, `workspace_delta`.
- `src/platform/lb_process`: `Run()` (synchronous, argv-based, cwd, stdin, timeout, cancel,
  process-group kill) and `Spawn()`/`Child` for long-running servers.
- `server_manager`: llama-server discovery (bundle → `bin/` → PATH), spawn, health
  thread exit detection, SIGTERM→SIGKILL group stop.
- `secrets_store`: AES-256-GCM, with the key in the login Keychain.
- `update_checker`: Poco HTTPS. The installer URL is ignored on Mac.
- `tool_python_syntax`: `python3 -I -B -c compile(...)` through `lb_process`.

## Stubbed or unavailable on macOS (each reports "not available on macOS yet", never fake success)

| Feature | File | Planned |
| --- | --- | --- |
| In-app update install | `update_installer.cpp` | Phase 4, Mac package |
| Sign in with ChatGPT | `chatgpt_auth.cpp` | Later: Keychain storage + RS256 verification. Never base64 storage |

## Known gaps and risks

- Orphan protection (fixed 2026-10-12): Windows puts llama-server in a KILL_ON_JOB_CLOSE
  job. On the Mac, each launch writes `<data dir>/llama-server.pid` (PID + binary path)
  and a normal stop removes it. Before the foreign-server guard, a leftover record is acted
  on only if that PID is still our llama-server binary AND its parent is launchd (its
  LlamaBoss is gone); then its process group is stopped (SIGTERM, SIGKILL after 5 s).
  Verified with a ServerManager harness: an orphaned server (parent exited) is stopped and
  the start proceeds; a server whose parent is alive is left alone and the guard refuses
  as before. `server_manager.cpp`, Mac-only; Windows view identical.
- `tool_path_safety.h` compares paths ASCII-case-insensitively. That is correct for the default
  case-insensitive APFS volume, but slightly permissive on a case-sensitive volume.
- About 150 places still join paths with `\`. The ones on the chat, agent and approval paths
  are fixed; the rest are in stubbed Windows-only code or accept either separator.
- The single-instance "open a new window" signal is Windows-only. On Mac a second
  direct launch shows a message and exits (no duplicate server). Normal Finder/Dock
  launches just activate the running app.
- Tests: the native test project (`Tests/`) has no CMake target; it is built against the app objects by a scratch script (see "Merge of main 91bb2db").

## Small follow-ups found while testing

- "Could not load application icon": the app ships `app_icon.ico`; macOS needs an `.icns` (Phase 4).

## Re-verification after rebase onto v0.1.21 (2026-10-09)

- Clean CMake build (`rm -rf build_macos`): 0 errors, 2 warnings
  (`posix_spawn_file_actions_addchdir_np` deprecated in macOS 26; still works).
- Launched with `open`: auto-loaded Gemma 3 1B, server ready in 0.84 s, single slot verified.
  A chat request to the app's server (port 8384) answered at about 91 tok/s. The in-app chat box was not
  driven (no one was at the Mac); it was verified before the rebase with identical source.
- Quit: a normal Apple Event quit of the pre-rebase instance stopped llama-server cleanly.
  The fresh instance **could not be quit normally**, because a modal "LlamaBoss Error" popup was open
  (see the bug below). It was stopped with SIGTERM, which left llama-server orphaned as
  expected (the crash-cleanup gap). It was stopped by hand; nothing is left running and port 8384 is free.

### Bug found: error popup on every Finder/`open` launch — **fixed**

`AppState` loads the window icon with the relative path `"app_icon.ico"`. A Finder or
`open` launch has working directory `/`, so the load fails and wxWidgets shows a modal
"LlamaBoss Error" popup (its text goes to the popup, not the app log). It only
went unnoticed earlier because a terminal launch from the repo folder found the file. Fix: on Mac, use
the bundle icon (`.icns` in `Contents/Resources`) and do not try the `.ico`, or silence
wx logging around the attempt.

Fixed: macOS skips the `.ico` load (the Dock icon comes from `app_icon.icns` in the bundle once
added), and the Windows file fallback runs under `wxLogNull`. Re-verified with `open`: no popup,
normal quit, llama-server stopped, port 8384 free. Commit authors were rewritten to the
GitHub noreply identity before the first push; `macos/port-v0.1.21-backup` keeps the
old author email and must stay local.

## Merge of `main` 9d08ccd (post-0.1.21) — 2026-10-10

- Merged `origin/main` (9d08ccd) into this branch. **No textual conflicts.**
- Semantic follow-ups (main changed Windows-only code that has a Mac twin):
  - `tool_mutation_guard.h`: Windows guard gained `MissingFolder()` and a plain
    "Folder does not exist: <path>." message. The POSIX guard now does the same (ENOENT
    while pinning a parent); mkdir's allow-missing-parents path clears it, as on Windows.
  - `tool_write.cpp`: the new copy tool skips the Python syntax gate
    (`opt.pythonSyntaxCheck`). The Mac staging branch now honors the same flag.
- Connections scoping for `python_run_script` (`skill_connections.h`): the filter runs in
  the Windows worker thread. The Mac runner is still a stand-in that reads **no** secrets, so
  nothing leaks. The Phase 3 runner must call `BuildEnvInjectionSnapshot()` +
  `SelectDeclaredConnections()`. `skill_connections.h` was tested on macOS paths (spaces,
  nested `scripts/`, no-owner and out-of-root cases): only the declared providers pass.
- New tools `tool_copy` / `tool_hash` and helper headers (`lb_sha256.h`,
  `answer_receipts.h`, `markdown_image_line.h`) compile on the Mac unchanged.
- `tool_web_fetch.cpp` changes are inside the Windows-only fetch (stubbed on Mac); fold
  them into the Phase 3 port.

## Merge of `main` 7929ef7 (v0.1.23) — 2026-10-11

- One conflict, in `tool_notes.cpp`: main moved `ReadFileUtf8` above `AtomicWriteFile` and
  added `maxBytes`. Took main's version and re-applied the two Mac edits: the
  `std::filesystem::path` wrap on the `ifstream` (also covers `DescribeNoteFile`) and the
  `#ifndef _WIN32` `AtomicReplaceFile` branch in `AtomicWriteFile`.
- `NotesAppend` now uses `tool_mutation_guard::Guard` (same pattern as `tool_edit`); the
  POSIX guard has the same API, so no further changes were needed.
- Verified by linking the app objects into a small harness and calling the real
  `NotesAppend` / `ListNoteFiles` / `BuildNotesIndexBlock`: `~/LlamaBoss/Notes/paths.md` is
  created with the frontmatter description, a duplicate reports "already saved", a missing
  explicit `file:` is refused, the index shows the description, and no temp files are left.
- In-app chat test of notes (save, then recall in a new chat) still needs a person at the
  keyboard: this terminal has no Accessibility permission to type into the app.

## Merge of `main` dc9d27b — 2026-10-12

- dc9d27b "Fix agent harness, prompt caching, metrics, paste and shutdown safety" (29 files).
  One conflict, `agent_prompt_builder.cpp` (two places): main added a Windows-only `py -3`
  launcher tip to the "Host OS: Windows" line, which the Mac branch keeps inside an
  `#ifdef _WIN32 / #else` split. Resolved with the user's OK: the Windows branch takes
  main's line verbatim, the Mac branch keeps the macOS line.
- Every file the merge touched has the same Windows-view differences from main as before
  the merge (no new ones). The Mac build compiles unchanged; new code scanned for
  Windows-only behavior (the batch-write path check normalizes '/' to '\' only for
  comparison, so Mac absolute and `~` paths are handled).
- Verified: the app starts, llama-server ready with Gemma 3 1B, prompt prewarm primed
  12,046 tokens in 6.8 s. In-app with GPT 6 Luna: shell `sw_vers` (exit 0), then `write`
  version.txt with a file card, correct answer. (Gemma 3 1B printed the command as text
  instead of calling the tool: a 1B model's limit, not the merge.)

## Startup error path — verified 2026-10-11

Both cases use shared code (`model_switcher.cpp`, `server_manager.cpp`), so the behavior matches Windows.

- **Saved model file missing:** startup falls back to the first installed model
  (`model_switcher.cpp:582`), saves it as the new selection, and loads it normally.
- **Corrupt GGUF** (`GGUF` magic + random bytes): llama-server exits with code 1 in about 0.6 s
  ("failed to read header"). `lb_process::Child::Exited` reports it, and the app logs
  the error with the server's last output. The chat shows "Server error: llama-server exited
  (code 1) before becoming ready ..." and the status dot goes disconnected. There is no
  --jinja retry, because the error isn't template-related. There is no respawn loop, no
  zombie process, and no modal popup. Quit with no server running is clean.

## OpenRouter and in-app notes — verified 2026-10-11

- The user added OpenRouter through Settings → Connections → Add AI provider.
  `secrets.json` is `keychain-aes256gcm` with no plaintext key, and `endpoints.json` holds no key.
- Chat with `openai/gpt-6-luna` worked: "Activated remote model ... via openrouter",
  the local llama-server stopped on the switch, and the context meter showed the 262.1k remote
  budget. With Agent Mode off, the model correctly said it can't save. With Agent Mode on, it
  sent a native structured `notes_append`, which created `~/LlamaBoss/Notes/paths.md` with the
  description header. No errors in the log.
- Found while testing:
  - Fixed: the Agent Mode ON banner listed `powershell`, which is unavailable on macOS.
    The Mac build now omits it; Windows text is unchanged.
  - Open (UI polish): themed `wxButton`s (accent, flat and destructive helpers in 11 files)
    show light text on light accent backgrounds on macOS, because Cocoa ignores
    `SetForegroundColour` on native buttons. Needs one shared Mac-drawn themed button.
  - Minor: the Settings help text says "fits in VRAM"; on Apple silicon it is unified memory.

## App icon — 2026-10-11

`macos/app_icon.icns` is generated from `app_icon.ico` by `macos/make_icns.sh`. The .ico's
largest frame is 256 px, so the 512 and 1024 slots are upscaled. CMake copies the .icns into
`Contents/Resources`, and Info.plist already names it. A 1024 px source would give sharper
large icons. macOS 26 may show a full-bleed square icon inside its standard rounded frame.

## Phase 3a: shell tool (zsh), 2026-10-11

- **Runner** (`cmd_executor.cpp` `RunOnePosix`): `/bin/zsh -f -c` via `lb_process::Run` in its own
  process group. cwd comes from /cd (home when unset), the Homebrew prefixes are added to
  launchd's PATH, `PAGER/GIT_PAGER=cat`, and stdin is `/dev/null`. Timeout and Stop kill the group;
  survivors are killed and reported (`background stopped` chip). It has live last-line progress,
  workspace delta, file cards, ARTIFACT_FILE markers and large-output files.
- **Policy** (`command_policy_zsh.cpp`, reached from `EvaluatePowerShellCommand` under
  `#ifndef _WIN32`): read-only allowlist with write-flag checks; everything else needs approval;
  malformed input is rejected. Tests: `cmake --build build_macos --target
  command_policy_zsh_tests && build_macos/command_policy_zsh_tests` (65 checks).
- **Model-facing:** the tool is named `shell` on macOS (`tool_names::kPowerShell`), with a zsh
  description and schema. `write_powershell_script` is not registered. The agent prompts (XML and
  native) have Mac lines for every PowerShell-specific rule under `#ifdef _WIN32 / #else`.
  `unifdef -D_WIN32` of every touched file matches the pre-change Windows view.
- **Display:** "Shell" cards with `bash` highlighting, and the Agent Mode banner lists `shell`.
- **Verified in-app 2026-10-11** with GPT 6 Luna via OpenRouter, Agent Mode on:
  - `sw_vers` and `df -h /` auto-ran without a card and gave a correct answer
    (macOS 26.6.2, 147 GiB free).
  - `mkdir -p "$HOME/Desktop/shell-test"` showed an Approval Required card, ran after Allow,
    and the folder exists. The model then checked it with `ls -ld` (auto-ran).
  - The Deny path has not been exercised in-app.
- Seen while testing (not Mac-specific): with Agent Mode on, the prompt is about 12.4k tokens,
  so an 8k local context fails with a raw llama-server 400 "exceeds the available context
  size". Use 16k+ for agent work; a friendlier message belongs in main.
- Open decision: `RiskyExtensions()` (blocks write/edit/delete of executable types) has no Mac
  script types (.command, .sh, .app, .scpt, .webloc ...). `open` can't launch them anyway, because
  launching is allowlisted. Adding them would also stop the agent writing ordinary `.sh` files.

## Phase 3b: Python helpers and scripts, 2026-10-11

- **Decision:** use the user's installed Python, not a bundled one (user, 2026-10-11).
  **Changed 2026-10-13:** a bundled runtime is now preferred; see "Bundled Python runtime" below.
- **Interpreter** (`src/platform/lb_python_posix.h`): LlamaBoss's environment
  `~/Library/Application Support/LlamaBoss/python` if it exists, else Homebrew /
  python.org `python3`. `/usr/bin/python3` is used only when the Command Line Tools exist.
- **`python_install_package`** creates that environment on first use
  (`--system-site-packages`) and pip-installs into it, because Homebrew Python refuses
  `pip install --user` (PEP 668).
- **`python_runner.cpp`** is portable; only the Win32 pieces are fenced, each with a POSIX
  twin. Helper scripts ship in `Contents/Resources/python`.
- **Verified** (harness and in-app): health check, csv_inspect, csv_to_xlsx with file card,
  python_run_script (bare name, Skills/..., absolute path, argv, ARTIFACT_FILE), and package
  install.
- The persistent `py` session followed on 2026-10-12 (next section).

## Persistent `py` session, 2026-10-12

- `python_session.cpp`: the session manager, idle reaping, restart notes and `StartExec`
  are now shared; only the process layer differs. The Mac layer sits in the `#else` of
  the existing `#ifdef _WIN32` helper block: `lb_process::SpawnPiped` (new: a child in
  its own process group with stdin/stdout/stderr pipes, `F_SETNOSIGPIPE` on the write
  end), `poll()`-gated frame reads and a deadline on frame writes (a frame can exceed
  the 64 KiB pipe buffer), SIGKILL to the group as the Job Object kill, temp dirs in
  `$TMPDIR/LlamaBoss/PySessions`, interpreter from `lb_python::RunInterpreter()`.
  The Windows view of the file is identical (`unifdef`).
- Memory cap: see the Mac differences table. The crash-channel drain thread checks the
  kernel's physical footprint every 0.5 s.
- Verified with a harness driving `PythonSessionManager` (11 cases): state kept across
  calls, exception keeps the session, timeout with salvaged partial output and a restart
  note, memory stop (exit 137, explained), cancel, subprocess (`sw_vers`), reset via
  `CloseSessionFor`, shutdown; no stray kernels or temp dirs. In-app: two agent `py`
  calls with approval cards, `x` kept, Python 3.14.3; quitting the app stops the kernel
  and removes its temp dir.

## Web fetch, 2026-10-12

- `tool_web_fetch.cpp`: the one big `#ifdef _WIN32` around the tool is gone; the shared
  logic (URL rules, SSRF guard, redirect resolution, charset sniffing, HTML to Markdown,
  artifacts, previews) now builds on both. Win32-only pieces have POSIX alternatives next
  to them: WinHTTP loop -> `FetchUrlWithPoco` (same timeouts, 4x wall-clock budget,
  manual redirects re-validated every hop, 10 MB cap on the decoded body, gzip/deflate,
  cancel via `HTTPSession::abort()`); `WinHttpCrackUrl` -> a parser with the same split;
  `GetAddrInfoW` -> `getaddrinfo`; `MultiByteToWideChar` code pages -> iconv (all 28
  mapped charsets checked; unmappable bytes become U+FFFD); UTF-16 surrogates paired
  explicitly (wchar_t is 32-bit); artifact paths joined with '/'. Small type aliases
  (`DWORD`, `UINT`, `INTERNET_PORT`, `HINTERNET` = Poco session) keep the shared code
  unchanged. Windows view identical (`unifdef`). Links the system `iconv`.
- Verified with a harness (20 cases): example.com (title + text artifact), JSON,
  gzip, deflate, PNG, 404, UTF-8 page, redirects, explicit port + fragment; blocked:
  localhost, 127.0.0.1, 192.168.x, [::1], 169.254.169.254, `file://`, a public redirect to
  127.0.0.1; 100 MB file refused by Content-Length; unresolvable host fails closed;
  timeout; cancel mid-request (about 0.5 s).
- In-app, 2026-10-12: the agent fetched https://llamaboss.com (http 200, 17.8 KB, 0.75 s);
  Markdown and raw HTML file cards with Open / Save As / Open Folder; correct title and
  first heading in the answer.

## Screen checks, 2026-10-12

- About: says "for Mac" (`lb_about_dialog.cpp`, Mac branch). The update check reads the
  Windows feed; on a Mac a newer version only opens the download page (never the .exe).
  A Mac entry in `version.json` belongs to Phase 4.
- Manage Models: the Mac "delete" key (Backspace) deletes the selected model too, and the
  hint reads "Press Delete or Control-click to delete". The irreversible-delete
  confirmation defaults to **No** (see below).
- Incident while testing: before that change, a delete-key test confirmed the dialog's
  default **Yes** (a Return reached it while a macOS permission prompt was on screen) and
  removed the user's Gemma 3 1B model. It was re-downloaded from Download Models at once.
- On the Mac, the confirmations that lose data default to **No**: model delete and
  cancel-download (`model_manager.cpp`), conversation delete
  (`conversation_controller.cpp`), connection delete and replace
  (`connections_dialog.cpp`), endpoint delete (`endpoints_dialog.cpp`). Mac-only; the
  Windows code is unchanged (a brief shared version in 0129720 was reverted to Mac-only
  per the workflow below).
- **Suggestion for the Windows version (the user's call, on the PC):** the same
  `wxNO_DEFAULT` on those prompts. The model-switch "interrupt it?" prompts, Skill import,
  reminders and save recovery lose no data and stay as they are on both.

## Release packaging groundwork, 2026-10-12

- Target: Apple silicon, **macOS 13.3+** (llama.cpp's minimum; user decision). Homebrew's
  wxWidgets/OpenSSL are built for macOS 26 only, so a release links static libraries.
- `macos/build_deps.sh [dir]`: builds OpenSSL 3.6.2, Poco 1.15.4, wxWidgets 3.2.11 as static
  libraries for macOS 13.3 into `~/LlamaBossDeps/prefix` (sources pinned by SHA-256:
  OpenSSL's published checksum, Homebrew's recorded ones for Poco and wx; patch releases
  newer than the dev build's 1.15.3 / 3.2.10). OpenSSL's openssldir is `/private/etc/ssl`,
  so the default CA bundle is macOS's own `/etc/ssl/cert.pem`.
- CMake `-DLLAMABOSS_DEPS_PREFIX=<prefix>` switches to those libraries and sets the
  deployment target 13.3 / arm64; empty keeps the Homebrew dev build.
  `LSMinimumSystemVersion` is now 13.3 (was 11.0, which was never true).
- llama-server and its dylibs moved to `Contents/Helpers/llama` (nested code belongs there
  for signing; a folder named `llama.cpp` is mistaken for a bundle by codesign).
  `FindServerBinary` tries Helpers first, then the old `Resources/llama.cpp`. Its license
  is `Resources/LICENSE-llama.cpp.txt`. Verified: a server harness in a mock bundle found,
  started (ready) and stopped llama-server from Helpers.
- `macos/sign_bundle.sh <identity> <app> [--release]`: signs inside-out (Helpers, then the
  app); `--release` adds hardened runtime + secure timestamp. Used by the CMake post-build.
- `macos/package.sh [--identity ...] [--notarize <profile>]`: release build, a check that
  only system libraries are referenced, release signing, DMG with an Applications link,
  optional notarization + stapling. Output `dist/LlamaBoss-<version>-macOS.dmg`.
- First package run, 2026-10-12, with the Apple Development identity (local test only):
  `dist/LlamaBoss-0.1.23-macOS.dmg`, 25 MB (app 93 MB before trimming). The app links
  only system libraries, minimum macOS 13.3, hardened runtime + timestamp, Team ID
  5QV7H398ZS. package.sh stages a copy and keeps only the 11 llama.cpp files
  llama-server loads (of 35). Run from the mounted DMG: llama-server `--version` loads,
  the app starts with TLS verifying, the remote model activates. A static-OpenSSL HTTPS
  test reached llamaboss.com and openrouter.ai (200) and rejected an expired certificate.
- The bundle version now comes from `LLAMABOSS_VERSION` in LlamaBoss.cpp (the one place
  the Windows side updates); Info.plist had a stale 0.1.2. The CMake signing step is
  `VERBATIM` (identity names contain parentheses).
- **First notarized release, 2026-10-12:** `dist/LlamaBoss-0.1.23-macOS.dmg`, signed with
  "Developer ID Application: Cesar Avelar (5QV7H398ZS)" (membership renewed through
  2027-10-12), notarytool submission c2a5b818-ee05-48fb-9c0e-f2a3ea3ca854 **Accepted**,
  ticket stapled. `spctl`: the DMG and the app inside it are "accepted, Notarized
  Developer ID". SHA-256 bc2c59d6f65e82c28b4c10c346f12b5e9d9fd3a45d67b45a41702108163b37cf.
  Built from commit 02d126a (the untested menu bar / Dock work was set aside).
  Copy on the share: `Releases/Mac/`.
- **Release build matching Windows Beta 0.1.23, 2026-10-12:** built from 34f2193 (merge of
  main 9e8b3d3, the Windows 0.1.23 release metadata). Notarization 27f05ea9-f884-4b73-9308-
  a0295bb87ecd Accepted, stapled, `spctl` accepted. SHA-256
  e9ecf29daeeaf8fbd20e5b6ba09ce64672009f0f43eb6c5175fc8ef9b0c8d6b8. Replaces the earlier
  test DMG on the share (`Releases/Mac/`, with a `_SHA256.txt`). The Windows installer is
  hosted on Cloudflare R2 (version.json `url`); the Mac DMG is not hosted yet.
- **Published 2026-10-12:** DMG on R2 bucket `llamaboss-releases` as
  `LlamaBoss_Beta_v0.1.23_macOS.dmg` (public URL verified: checksum matches, Gatekeeper
  accepts it with a quarantine flag). llamaboss.com site 0.2.8 (deployment 43d24ecf,
  approved by the user): Download for Mac buttons, Windows and Mac wording and
  requirements, docs for 0.1.23 with a rewritten Mac section and FAQ answer; it also
  restores the 0.2.7 docs that the PC's 0.1.23 deploy (built from 0.2.6) had replaced.
  Package on the share: `Websites/LlamaBoss_Website_v0_2_8_mac_v0_1_23.zip`.
- Notary credentials: keychain profile `LlamaBoss-notary` (Apple ID + app-specific
  password, entered by the user). Release command: `macos/package.sh --notarize LlamaBoss-notary`.
- Still to design: the Mac entry in the update feed (`version.json`) and the website button.

## UI fonts, 2026-10-11

- `lb_ui_font.h`: `LbUiPt(n)` (identity on Windows, n*4/3 on macOS, because Windows draws
  1pt = 96/72 px), `LbMonoFaceName()` and `LbFace()` (Consolas / Cascadia -> Menlo on macOS).
  They apply to all fixed sizes in 14 UI files, `SidebarMonoFont` and
  `AppState::CreateMonospaceFont`.
- The user compared against PC screenshots: the chat, composer and top bar now match.
  Sidebar badge and "TODAY" header checked 2026-10-12: not clipped.

## Native buttons, 2026-10-12

- `lb_mac_native.mm` (Objective-C++, Mac only, hooked from `MyApp::OnInit` under
  `__WXMAC__`): an app-wide `wxEventFilter` restyles every text `wxButton` when a top-level
  window is shown and, throttled to 250 ms, on idle (covers theme switches and relabels).
  It reads the role from the colour the shared code set, remembers it, clears the wx
  colours (wx otherwise paints the flat fill over the bezel) and applies a native bezel.
  No shared UI file changed; `app.cpp`'s Windows view is identical.
- The NSApp appearance is only set when it differs from the effective one: setting it
  makes wx send `wxEVT_SYS_COLOUR_CHANGED`, which reset the chat `wxRichTextCtrl` to the
  system background (seen once, fixed).
- Checked by screenshots: Settings (one blue OK, grey Cancel and actions, no truncation),
  Connections (grey Add/Edit, red Delete, blue Close), Download Models, main window Send,
  sidebar. Not yet looked at: Model manager, About, project attach, themed message boxes,
  light themes.

## Keychain prompt after every rebuild — fixed 2026-10-12

- Cause: Keychain records an "Always Allow" against the app's partition ID. For an app
  with a Team ID that is `teamid:<ID>`, the same for every build; for an app without one
  it is tied to the exact build (cdhash), so every rebuild asked again. Ad-hoc and
  self-signed signatures have no Team ID.
- Fix: the build signs with the user's free **Apple Development** certificate (Xcode >
  Settings > Accounts, personal team, Team ID `5QV7H398ZS`, expires 2027-10-12; renew
  from the same screen). CMake auto-detects it at configure time (`security
  find-identity`), falls back to the self-signed "LlamaBoss Development" certificate,
  then to ad-hoc; override with `-DLLAMABOSS_CODESIGN_IDENTITY=...`. The codesign step
  runs after the llama-server bundling step.
- The Apple WWDR G3 intermediate certificate was added to the login keychain (the new
  certificate showed as invalid without it).
- Verified: one last "Always Allow", then a forced relink (CDHash changed), relaunch and an
  OpenRouter chat (key read from Keychain) with no prompt.
- Released builds will use Developer ID signing and notarization (Phase 4); that also
  carries a Team ID, so users' updates will not prompt either.

## Polish, 2026-10-13

- **File card right border** (`chat_display.cpp`, Mac only): the `[Open] [Save As...]
  [Open Folder]` row stuck out 2-4 px past the card's right border. Core Text advances
  are fractional (Menlo 18 pt: 10.837 px) and wx rounds each styled run's width up, so
  the action row (8 runs) came out wider than the one-run rows (measured: 535 vs 532 px).
  On the Mac every card row is now written as the same three runs (2 chars, middle,
  2 chars); the action row's middle is one italic run with the chips registered by
  offset. wx merges equal neighbouring runs, so the middle run carries a character
  style name (no style sheet, so it draws the same). Menlo Italic and Regular have equal
  advances. Verified by screenshot: the border is straight.
- **Empty chat saved on every launch** (`LlamaBoss.cpp`, Mac only): the New Chat prompt
  pre-warm (from main dc9d27b) builds the agent prompt, whose cwd line called
  `ResolveCurrentCwd()`. That gave the untouched chat a file path and created
  `~/LlamaBoss/Chats/<date>_<id>/Workspace`, and `HasPersistableContent()` counts a file
  path, so switching away saved an empty `chat_<id>.json` (sidebar showed it by file
  name). While the pre-warm builds its prompt, a chat with no file now gets the Chats
  root as a stand-in cwd; the cwd is after the cached cut, and the pre-warm key is
  unchanged (`5ccbfa498e2a1e9f` before and after). Verified: launch + pre-warm + switch
  creates no file or folder; a real first message still creates and titles both.
- **Sidebar selection follows the open chat** (`conversation_sidebar.cpp`, Mac only):
  after New Chat, the previously clicked row stayed selected, so two rows looked current.
  A single selection that marked the old open chat now moves to the new one (or clears
  for an unsaved New Chat), as in Mail and Notes; multi-selections are left alone.
  Verified: open a chat, New Chat, no row stays highlighted.
- **Suggestion for the Windows version:** the same pre-warm path exists on Windows, so it
  likely leaves an empty chat (and empty Chats folder) after each launch or New Chat that
  is then left. The empty `chat_<id>.json` entries in the sidebar are the symptom.

## Bundled Python runtime, 2026-10-13

- **Decisions (user, 2026-10-13):** bundle Python with pypdf and openpyxl only; PyMuPDF
  (AGPL-3.0 or a paid Artifex license; LlamaBoss is MIT) is not bundled and installs on
  first use through the `python_install_package` approval card. When the bundled Python
  exists, LlamaBoss always uses it.
- **Runtime:** `macos/fetch_python.sh` downloads python-build-standalone 20261009, CPython
  3.13.16 `aarch64-apple-darwin-install_only_stripped` (SHA-256 pinned), removes Tcl/Tk,
  IDLE, turtle, pydoc data, man pages and the embedding `libpython3.13.dylib`
  (`bin/python3.13` is statically linked), installs `macos/python-requirements.txt`
  (pypdf 6.20.1, openpyxl 3.1.5, et-xmlfile 2.0.0; `--require-hashes`, wheels only), then
  removes the runtime's own pip (the environment gets pip 26.2.1 from `ensurepip`) and
  precompiles everything as unchecked-hash `.pyc`, so Python never writes `__pycache__`
  into the signed app. Output `third_party/python`: 49 MB, 2 Mach-O files.
- **Location:** `Contents/Resources/python-runtime`. Not `Contents/Helpers`: codesign
  takes any folder with a dot in its name there (`lib/python3.13`, `*.dist-info`) for a
  nested bundle and refuses to sign the app. CMake copies it (rsync) before signing.
- **Signing:** `sign_bundle.sh` signs the runtime's Mach-O files; `bin/python3.13` gets
  `macos/python.entitlements` (`com.apple.security.cs.disable-library-validation`).
  Verified necessary: under the hardened runtime, the same binary without the entitlement
  refuses PyMuPDF's `_extra.so` ("different Team IDs"); with it, PyMuPDF loads.
  `package.sh` requires the runtime and checks its Mach-O files are self-contained.
- **Interpreter** (`src/platform/lb_python_posix.h`): bundled Python first, then the old
  Homebrew / python.org / Command Line Tools order (only builds without the runtime). The
  environment `~/Library/Application Support/LlamaBoss/python` must be built on the
  bundled Python (`pyvenv.cfg` `home`): if the app moved and the minor version matches,
  `venv --upgrade` re-points it and keeps its packages; otherwise (or if that fails) it is
  renamed to `python.previous-<timestamp>` and rebuilt on the next package install. This
  runs in `RunInterpreter()`, which is only called on worker threads. Both `venv` runs use
  the data folder as cwd: wx sets the app's cwd to the folder holding the bundle, and
  Python reads its cwd at startup, so an app run from the Desktop or Downloads raised a
  folder-access prompt (seen in testing: the run waited on it until the 60 s timeout).
- **Size:** release DMG 58 MB (was 25 MB without Python).
- **Verified (GPT 6 Luna, Agent Mode):**
  - Dev build: `python_health` reports the bundled 3.13.16 with pypdf and openpyxl;
    `pdf_extract_text` works with the bundled pypdf; `pdf_inspect_form` reports PyMuPDF
    missing, the approval card installs it into a new environment on the bundled Python
    (2.3 s), and the retry uses it.
  - Release build (Developer ID, hardened runtime; local test DMG, not notarized or
    published) run from the mounted DMG: the environment from the dev build was
    re-pointed in place and kept PyMuPDF; `pdf_inspect_form` and `pdf_fill_form` worked
    (the filled PDF reads back `full_name` = Cesar Test, `city` = Los Angeles). Repeated
    after the trim and cwd fix: re-pointed in place, inspect in 0.84 s, no prompt.
  - The set-aside path also ran once (the timeout above): the old environment was kept
    as `python.previous-…` and the next install built a new one.
- **First-run prompts a user can see:** macOS asks once for Desktop/Documents/Downloads
  access the first time a helper reads a file there (normal for Mac apps). On this Mac the
  dev and Developer ID copies count as different apps, so each asked separately.
- **Not done:** notarization of a build with the runtime (do it with the next release).
- **Suggestion for the Windows version:** the PDF form helpers' error text says
  `py -3 -m pip install --user pymupdf`, which is wrong on the Mac. A neutral message
  ("Install the PyMuPDF package (pymupdf)") would suit both; the script is shared.

## Packages for the PC and Mac fixes, 2026-10-13 (afternoon)

Standing instructions from the user (2026-10-13): Claude works on the Mac alone; shared-code
fixes go to the PC as packages on the share (`D:\ForMacMini\Patches`), built on a local branch
from `main`, compiled on the Mac, each with an `install_<item>.py` (dry run, hash check, backup
outside the repo, sha256 verify, restore). Built by `macos/make_pc_package.py`.

| Package | Branch (local only) | Files | State |
| --- | --- | --- | --- |
| `LlamaBoss_Patch_ascii_ctype.zip` | `suggest/ascii-ctype-8814026` (a2bf8d6, on main 8814026) | 43 | **Applied** by the user (main 69a12e6) |
| `LlamaBoss_Patch_pdf_forms_pypdf.zip` | `suggest/pdf-forms-pypdf-8814026` (4f8a114, on ascii-ctype) | 7 | **Applied** by the user (main 69a12e6) |

Rebuilt 2026-10-13 on main **8814026** ("Transient error handling and agent/tool fixes"),
fetched read-only from GitHub. The ASCII change was regenerated with the same script rather
than rebased (only an `#include` line conflicted), so it also covers the 4 new calls in
`transient_error.h`, `tool_approval.h`, `tool_edit.cpp`, `tool_read.cpp` (161 calls, 40 files);
the PDF commit applied unchanged. Checked: Mac branch + 8814026 + both packages builds, zsh
policy tests and `transient_error_tests.cpp` pass; installer suite and chain test on a CRLF
checkout of 8814026; the 9e8b3d3 packages are refused there. Those are kept in
`Patches\superseded_9e8b3d3` (do not install).

- **ascii_ctype (bug):** wxWidgets sets `LC_CTYPE` to "UTF-8" at startup on macOS (checked with
  a bare wx app: "C" before, "UTF-8" after). There `isspace(0xA0)` and `isalpha(0xC3)` are true
  and `tolower(0xC3)` is `0xE3`, so byte-wise `<cctype>` use corrupts UTF-8 (à = C3 A0,
  ⚠ = E2 9A A0). Seen: a fetched llamaboss.com page saved `E2 9A 20` into a chat file (invalid
  UTF-8). Setting `LC_CTYPE` back to "C" is not an option: `wxConvLibc` (implicit wxString
  conversions) depends on it. Fix: `lb_ascii.h` (`lb_ascii::IsSpace`, `ToLower`, ...), 157
  calls in 37 files; identical results on Windows. Verified on the Mac: cherry-picked onto
  the Mac branch, full build, zsh policy tests, in-app web fetch keeps ⚠ and the chat file is
  valid UTF-8. `Tests/` left alone (Windows only).
- **pdf_forms_pypdf (user request):** the form helpers use pypdf (bundled on the Mac) instead
  of PyMuPDF: offline, no install prompt. Same JSON contract. Verified with a synthetic form
  of every field type and the real IRS W-9: inspect parity with the old helper, fill read
  back, values visible in Quick Look and in Preview (text with accents, check mark, radio dot,
  choice fields), validation errors, missing-pypdf path. In-app (GPT 6 Luna): inspect 0.09 s,
  fill 0.08 s, no prompts. Test forms: `~/LlamaBoss-pytest/all_fields_form.pdf`, `fw9.pdf`,
  `form.pdf`.
- **Choices made (standing instruction: take the recommendation and record it):**
  - Shared fixes are not applied to the Mac branch: its Windows view must match `main`. The
    Mac gets them when the user applies the packages and Claude merges `main`. Until then the
    Mac build keeps the `<cctype>` bug and the PyMuPDF form helpers.
  - The two packages are stacked (both touch `agent_controller.cpp` and `tool_router.cpp`);
    chain-tested from a CRLF checkout of main: install both, restore both, byte-identical.
  - pypdf only, no PyMuPDF fallback in the form helpers: one code path, permissive license.
  - A form with both AcroForm and XFA (the W-9) gets `/XFA` removed from the filled copy, with
    a warning: Acrobat would otherwise show the empty XFA copy.
  - The test form is written with raw pypdf objects (PyMuPDF's radio creation failed), which
    also keeps it independent of the code under test.
- **Mac-only fixes committed:** `669210f` expands a leading `~` in Python helper path arguments
  (the Mac prompt allows `~/`; helpers resolved it under the chat folder); `90f0710` lets CMake
  configure without `third_party/llama.cpp` (empty glob) and adds the package builder.
- **Suggestion for the Windows version:** none beyond the two packages.

## Merge of `main` 69a12e6 — 2026-10-13

- The user applied both PC packages on 8814026 and pushed **69a12e6** ("ASCII-only ctype
  helpers; PDF form tools on pypdf"). Its tree is byte-identical to the package branch
  `suggest/pdf-forms-pypdf-8814026`, so main now has exactly what was tested on the Mac.
- Merged into `macos/port-v0.1.21` as `10da291`. Conflicts, all mechanical: `#include
  "lb_ascii.h"` next to Mac-only includes (kept both) in 7 files; `.gitignore` (kept the Mac
  and the Windows-signing blocks); the agent prompt's Python helper line (Mac `#else` branch
  kept, shared line taken from main and checked equal); `python_runner.cpp`
  IsDriveRootedPath (Mac early return kept, main's `lb_ascii::IsAlpha`).
- Windows view: the same 46 files differ from main as before the merge, with identical
  differences (the long-standing ones listed under Baseline). No new ones.
- Verified: build, zsh policy tests, `transient_error_tests.cpp`; in-app (GPT 6 Luna) a form
  fill with the bundled pypdf (0.08 s, no install prompt, `~/` path) and a web fetch that
  keeps ⚠ intact in the saved chat (valid UTF-8).
- The `<cctype>` bug and the PyMuPDF dependency are now fixed on both platforms. The packages
  table above is done; local `suggest/*` and `scratch/*` branches can be deleted.
- Main sets `LLAMABOSS_VERSION` 0.1.24.

## UI checks and Mac fixes, 2026-10-13 (late)

- **Update feed** (`c91680d`, Mac only): the Mac build read the Windows `url` (the Setup.exe)
  and treated a Windows-only release as a Mac update. It now reads `mac_version`, `mac_url`,
  `mac_sha256`, `mac_notes`; with no `mac_version` it reports up to date. Harness: Windows-only
  feed → no update; Mac fields → update with the DMG URL; live llamaboss.com → up to date.
  About shows "You're up to date (v0.1.23)". **For the PC side:** add the `mac_*` fields to
  version.json at the next Mac release (they are ignored by Windows builds).
- **Theme switching** (`fbf0f97`, Mac only): switching dark ↔ light at runtime flips the app
  appearance, and wx then reset colours: the chat view went to the system background and the
  ☰ + ⓘ glyphs kept the old colour (invisible) without hover. Fixed by re-applying the theme
  on `wxEVT_SYS_COLOUR_CHANGED` and syncing borderless button titles in the native layer.
  Verified Nord → Light (glyphs visible, hover tint shows) → Nord.
- **Confirmation alerts** (`02c76b8`, Mac only): verb buttons (Delete / Replace / Sign Out /
  Stop Download) with Cancel as the default, Esc cancels, action shown as destructive; native
  NSAlert because wx's custom labels dropped `wxNO_DEFAULT`. Verified with the delete-
  conversation alert: Esc and Return cancel, clicking Delete deletes (a test chat).
- **Light theme, menus, project attach:** checked by screenshot. Row menu and Project menu are
  native; New Project and Add Source Files (native Open panel) work; Clear from This Chat
  detaches. Test project deleted afterwards.
- **Milestone gaps closed:** Unicode chat title (Café crème, 東京, 🦙) saved, listed and reopened
  after a relaunch; Stop during a remote stream ("Generation stopped by user", no further
  output).
- **Suggestion for the Windows version:** the new-project message says "Use [ Change... ] in
  the project strip", but the strip shows "[ Project ▾ ]" (`project_controller.cpp:99`).

## Notarized 0.1.24 with the bundled Python — 2026-10-13 (not published)

- Built from `a44d05f` (merge of main 69a12e6) with `macos/package.sh --notarize
  LlamaBoss-notary`: static deps, Developer ID "Cesar Avelar (5QV7H398ZS)", hardened runtime.
  Notarization `f8f48f6f-af83-4706-a34a-e6f334e8ad5f` **Accepted**, ticket stapled to the DMG.
- `dist/LlamaBoss-0.1.24-macOS.dmg`, 57.3 MB (0.1.23 without Python: 25.0 MB), SHA-256
  `6591d4af917db1b0a31390b2857a5d47150b733a43fdceb004c97387ee847117`. Copy on the share:
  `Releases/Mac/` with `_SHA256.txt`. Log: `dist/package-0.1.24.log`.
- Checked: a quarantined copy of the DMG and the app inside are "accepted, Notarized Developer
  ID"; bundle version 0.1.24; runtime Python 3.13.16, pypdf 6.20.1, openpyxl 3.1.5; python3
  carries the library-validation entitlement. Run from the mounted DMG: no prompts, OpenRouter
  connected, form inspect + fill with the bundled pypdf (0.93 s / 0.09 s).
- As with 0.1.23, the ticket is stapled to the DMG, not to the app inside, so a user who copies
  the app out offline gets the online check on first launch. Stapling the app as well needs a
  second notarization of the app before the DMG is built (left for later).
- **Publishing (user asked 2026-10-13):** DMG uploaded to R2 as
  `LlamaBoss_Beta_v0.1.24_macOS.dmg`; public URL verified (200, 57,295,617 bytes, checksum
  matches, Gatekeeper accepts it with a quarantine flag). Site 0.2.9 (Windows stays 0.1.23; Mac
  buttons and `data-ver-mac` show 0.1.24; docs Mac section and FAQ updated; version.json adds
  the `mac_*` fields, Windows fields unchanged) deployed to the preview
  https://mac-0-1-24.llamaboss.pages.dev , then, with the user's OK, to **production**
  (deployment 4036fd72, 2026-10-13). Live llamaboss.com/version.json carries `mac_version`
  0.1.24 with the Windows fields unchanged; both Mac buttons link the 0.1.24 DMG. Package:
  `Websites/LlamaBoss_Website_v0_2_9_mac_v0_1_24.zip`.
- The released 0.1.23 Mac app predates the Mac feed fields (it reads the Windows `version`), so
  it will not announce 0.1.24; Mac builds from 0.1.24 on will announce later Mac releases.

## Merge of `main` 91bb2db (Windows 0.1.25) — 2026-10-15

- **Merge** `f5bbc19`. main brought llama.cpp v0.6.0, agent retries (`transient_error.h`)
  with the Agent Status card, `utf8_scrub.h`, harness fixes, lean passes 1-2 (prompt
  section functions, `lb_string_utils.h`, `path_safety` helpers, `lb_win_process.h`),
  the `tool_read.cpp` range rewrite and the attachment tray inside the input box.
  Conflicts in 10 files, Mac code kept in each:
  - `agent_prompt_builder.cpp`: main's new layout taken verbatim; each PowerShell line has
    a zsh/macOS twin under `#ifdef _WIN32 / #else` (18 fences; `AppendPowerShell` body ->
    "SHELL (ZSH)"). The Mac prompt also drops the `.ps1` edit exception (no
    `write_powershell_script` on the Mac) and says Finder, not File Explorer.
  - `cmd_executor`, `python_runner`, `python_session`, `tool_grep`, `tool_ls`, `tool_open`:
    main removed the file-local UTF-8 helpers; Mac branches now call the portable
    `path_safety::` ones. `project_manager`: main's `file_size()`.
  - Mac-only callers of removed helpers renamed (`ElapsedChip` -> `LbElapsedChip`).
  - `lb_win_process.h` is wholly inside `#ifdef _WIN32`; every `using lb_win::...` sits in
    an existing Windows fence. Nothing from it compiles on the Mac.
  - Windows view (`unifdef -D_WIN32 -D__WXMSW__`): the same long-standing differences as
    before the merge, nothing new (checked file by file against main).
- **llama.cpp** now `b11429` (v0.6.0), the Windows 0.1.25 build. Upstream's macOS arm64
  asset has minimum macOS **13.3** (`vtool`: minos 13.3, SDK 26.5), so no source build
  was needed. The Mac had `b11539` (also libllama 0.6.0, 4 days newer); it is kept in
  `third_party/dl/superseded/`. Note: b11539 already had session v11 / state-seq v4, so
  Mac 0.1.23/0.1.24 users never had old-format caches; Windows users (b11146, v10/v3) do.
  CMake now replaces a cached `LLAMABOSS_LLAMA_DIR` that no longer exists with the
  current folder (a stale cache would have bundled no llama-server).
- **Native tests** (`macos/run_native_tests.zsh`, new): 14 standalone suites, 13 pass in
  full (announced_step 30/30, copy_line_fold 17/17, elision_budget 43/43,
  export_metrics 39/39, python_resources 122/122, tool_call_elision 35/35, zsh policy,
  tool_call_parser, transient_error, turn_stats, workspace_delta, chat_folders,
  prompt_prewarm). `command_policy_tests` (PowerShell policy) 30/43: not applicable on the
  Mac, whose shell policy is zsh. `Tests/` runner **104/112**: 7 expect `Vars\`
  separators (Mac returns `Vars/`, intended), 1 is the known stale R0_S3 prompt test.
- **Mac fixes** (Windows view unchanged in each):
  - `da63f8c` lane paths with backslashes (`Vars\cmd_0001.txt`, as the shared prompt
    writes them) resolve on the Mac, a whole-file read of an existing Vars spool no longer
    writes a duplicate, and reuse cards quote `Vars/...` (found by the Tests/ suite).
  - `450fdae` Cmd+V pastes pictures (and large text) as attachments. It never worked on
    the Mac: the paste hook was Windows' `WM_PASTE`, wxOSX sends no paste event for Cmd+V,
    and wx cannot read pasteboard PNG/TIFF. New `lb_mac_clipboard.h` reads NSPasteboard.
    Edit > Paste from the menu still pastes text only.
  - `6fad0ec` one sidebar click opened a chat twice (wxOSX sends LEFT_UP twice to a
    wxStaticText), restoring its KV slot twice.
- **Verified in the app** (dev build, Qwen 3.5 4B MTP from unsloth, now in the models
  folder; GPT 6 Luna not used):
  - Local chat with MTP: "creating MTP draft context", draft acceptance 0.80-0.89,
    ~36-42 tok/s; vision projector loaded.
  - Agent turn: shell `sw_vers` + `ls`, correct answer.
  - Old slot cache: a real format-3 file (made with upstream b11146) under a saved chat's
    cache name. Reopen: HTTP 400 logged as "full reprocess fallback", no error shown; the
    next message rebuilt the context (16.9k tokens, 53 s) and answered; not stuck busy.
    New saves are format 4 and restore in ~45 ms.
  - Paste: thumbnail inside the input box (square crop, not stretched), attach/agent/Send
    on the text line; picture sent with an empty box (no default text), described by the
    model; chat reopened with the picture.
  - PDF form: inspect 0.88 s, fill 0.11 s with bundled pypdf 6.20.1; read back
    `full_name` = Mac Tester, `address.city` = Cupertino.
  - Retry: a fault proxy in front of llama-server (test endpoint, since removed). 503 ->
    "Retrying in 3 s (attempt 1 of 2)" -> completed. Stream closed before any data ->
    retried -> completed. Stream cut after one reasoning token -> not retried, Agent Status
    card "stopped · model request failed" (see PC findings #2).
- **PC findings:** 6 items, now under `## Known issues` in the share's `NEXT_RELEASE.md`
  with file:line and suggested fix (no shared-file package needed).

- **Release 0.1.25 (published 2026-10-15):** `macos/package.sh --notarize LlamaBoss-notary` from
  the merge plus the fixes above. Notarization `05998afb-1d8a-4dd2-aa49-7271ea45cd4d`
  **Accepted**, ticket stapled; `spctl`: "accepted, Notarized Developer ID". Bundle 0.1.25,
  LSMinimumSystemVersion 13.3, llama-server b11429 (minos 13.3), Python 3.13.16, pypdf
  6.20.1, openpyxl 3.1.5. Launched from the mounted DMG: no prompts, TLS verifying,
  OpenRouter model activated. `dist/LlamaBoss-0.1.25-macOS.dmg`, 57,341,367 bytes,
  SHA-256 `1f284f22ae266aec6808a69334b80ba96e75e5ceeb0c8355e66fe21bec0bea67`
  (`_SHA256.txt` beside it). Log: `dist/package-0.1.25.log`. Copied to the share
  `Releases/Mac/` (hash checked there).
- **Published 2026-10-15 (user approved):** R2 `llamaboss-releases/LlamaBoss_Beta_v0.1.25_macOS.dmg`,
  public URL re-downloaded and hash-matched. Site 0.2.11 = live 0.2.10 (the PC's Windows 0.1.25
  deploy, re-checked byte-identical just before deploying) + Mac 0.1.25 links, `data-ver-mac`,
  docs/FAQ Mac versions and `mac_*` fields in version.json; Windows fields unchanged. Preview
  `mac-0-1-25.llamaboss.pages.dev` checked, then production: llamaboss.com/version.json
  serves Windows 0.1.25 (`eb4b6e3f…`) + mac 0.1.25 (`1f284f22…`). Package on the share:
  `Websites/LlamaBoss_Website_v0_2_11_mac_v0_1_25.zip`.

## Merge of `main` 6c163f2 (Windows 0.1.27) and Mac 0.1.27 release — 2026-10-30

- **Merge** `3cde471`. main brought the one-window Add/Edit AI provider dialog
  (`endpoints_dialog.cpp` rewrite: provider row, local-server port scan, per-model context
  window, masked keys), send-time image downscale + EXIF rotation (`chat_history.cpp`,
  portable wxImage; the static wx has the JPEG handler), Retro Hi-Fi theme and accent
  `[ Copy ]`, "Copied" only on the latest block, no empty saved chat on New Chat, context
  HUD layout, repetition stop in metrics export. Windows view of the touched files:
  the same long-standing helper differences as before, nothing new.
- **MTP + vision** (`server_manager.cpp`): main's `kMtpVisionGpuCtxLimit` is CUDA-oriented,
  so it sits under `#else`; the Mac keeps llama.cpp's default (projector on the GPU) at
  every context. Verified in the log with Qwen3.5-4B-MTP + mmproj at 131072:
  `mmproj on GPU (Metal, unified memory)`, no `--no-mmproj-offload` (0.1.25 always had it).
  Note: `app_state.cpp` clamps ctx to 262144, so main's CPU branch is unreachable today.
- **Provider scan fix** `c08465a` (Mac-fenced): llama-server's `/v1/models` also returns an
  Ollama-style `"models"` array (name/model only). The ChatGPT-catalog branch took it, found
  no slug/id and returned an empty list. On the Mac it now falls through to `"data"` when
  that branch found nothing. Verified: a llama-server on 8081 is listed under "SERVERS FOUND
  ON THIS MAC" with its model and "Read from the server: 4K". Same bug on Windows: logged in
  NEXT_RELEASE Known issues (suggestion only). Cosmetic: the footer hint is cut off with
  "…" at the default dialog width (not changed).
- **Retro Hi-Fi** checked on the Mac: theme applies to the main window; code block shows the
  amber code and accent `[ Copy ]`.
- **Native tests**: unchanged baseline (command_policy PowerShell 13 N/A; Tests/ 104/112 =
  7 `Vars\` separator cases + stale R0_S3); export_metrics now 51/51.
- **Published** (user asked in the 0.1.27 release request): `dist/LlamaBoss-0.1.27-macOS.dmg`,
  57,049,768 bytes, SHA-256 `ffab898931ae6f6de1a1934131d3691bf61428118b2b097f16c8a9e2d9f09f7a`,
  notarized (Accepted), stapled, Gatekeeper "Notarized Developer ID". R2
  `LlamaBoss_Beta_v0.1.27_macOS.dmg`, public URL re-downloaded and hash-matched. Copied to the
  share `Releases/Mac/`. Log: `dist/package-0.1.27.log`.
- **Site 0.2.12** = LATEST package `LlamaBoss_Website_v0_2_11_win_v0_1_27_20261030_081922.zip`
  (live checked identical, 26 files + 404) + docs: "New in 0.1.27" on Start here, the
  provider window callout on AI providers, Retro Hi-Fi on Settings, FAQ/Start here now
  "0.1.27 for Windows and Mac. Updated October 30, 2026", Mac 0.1.27 links and `mac_*` in
  version.json (Windows fields unchanged). Preview `mac-0-1-27.llamaboss.pages.dev`, then
  production; live re-checked identical. Package
  `Websites/LlamaBoss_Website_v0_2_12_mac_v0_1_27_20261030_085554.zip`, LATEST.json updated
  (no `treehash`: the PC's recipe isn't documented; asked in NEXT_RELEASE).

## Next task

1. **Needs the user:** fresh-account test with `/Users/Shared/LlamaBoss-FreshTest` (checklist
   inside): create a standard user account and log in to it.
2. **Needs the user:** rotate the Cloudflare Pages API token (it was printed into a session
   transcript on 2026-10-13; not sent anywhere else) and update the credentials file on the share.
3. Site deploys from either side start from `Websites/LATEST.json`. As of 2026-10-30 it names
   `LlamaBoss_Website_v0_2_12_mac_v0_1_27_20261030_085554.zip` (site 0.2.12, Windows 0.1.27,
   Mac 0.1.27; live checked identical).
4. PC findings for 0.1.26: `## Known issues` in the share's `NEXT_RELEASE.md`.
5. File-tool boundary cases on the Mac (symlinks, case-sensitive volume), milestone 7.
6. The `Tests/` suite now also runs on the Mac (`macos/run_native_tests.zsh`); see the
   0.1.25 section for the expected non-Mac failures. The Qwen 3.5 4B MTP test model stays.
