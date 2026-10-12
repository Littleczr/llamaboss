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
| Python packages | `pip install --user` | LlamaBoss environment in the data folder (`--system-site-packages`) | Homebrew Python refuses `--user` (PEP 668) |
| Python helpers | RCDATA in the .exe | files in `Contents/Resources/python` | App-bundle resources |
| llama.cpp | CPU/CUDA builds | Metal build in `Contents/Helpers/llama` | Apple silicon GPU |
| Fonts | Consolas / Cascadia Code, Windows point sizes | Menlo, sizes x4/3 (`lb_ui_font.h`) | Same physical size; built-in Mac face |
| App icon | `app_icon.ico` | `macos/app_icon.icns` (generated from the .ico) | Bundle icon format |
| Missing `.ico` at startup | file fallback | icon comes from the bundle; no file lookup | Avoids a modal error on launch |
| Buttons | flat buttons filled with theme colours | native push buttons (`lb_mac_native.mm`): one accent-tinted button per dialog (default/OK), Send and New Chat accent in the main window, stop-red stays red, the rest plain; widened to fit | NSButton ignores theme fills; Mac convention is one default button per window |
| Data-losing confirmations | Yes is the default button | No is the default (deletes, replace, cancel download) | macOS convention; a stray Return cannot destroy data |
| GPU memory wording | "Needs 10+ GB VRAM", "fits in VRAM" | "Needs a Mac with 16 GB+ of memory" (GPU GB x 4/3, rounded up to a Mac memory size), "fits in memory" (`lb_mac_wording.h`) | Apple silicon has unified memory; the GPU can use about 3/4 of it |
| `py` session memory cap | Job Object: 2 GiB per process; allocations fail with `MemoryError` and the session survives | watchdog stops the kernel when its physical footprint passes 2 GiB; the session is lost and the card says why | macOS refuses `RLIMIT_AS`/`RLIMIT_DATA`, so a failing-allocation cap is not available |
| Web fetch transport | WinHTTP, system proxy settings | Poco::Net with lb_ssl's verified TLS context; no system proxy; Mac browser user-agent | No WinHTTP on macOS; the UA names the platform it runs on |
| Menu bar | none (toolbar only) | LlamaBoss menu (About, Settings… Cmd+,, Hide, Quit), File (New Chat, New Window, Open/Save Conversation, Close Window), Edit, View (Show or Hide Sidebar Ctrl+Cmd+S), Window, Help (llamaboss.com/docs) | Mac convention; verified by the user 2026-10-12 |
| New window | second launch hands off to the running app | Dock icon menu "New Window" (Finder/Dock never start a second copy) | Mac convention; verified by the user 2026-10-12 |
| Control appearance | n/a | app appearance (dark/light Aqua) set from the theme's background at startup | Native controls match the theme on a system in the other mode |

## Toolchain (verified 2026-10-09)

| Item | Version |
| --- | --- |
| Machine | Apple M4 Mac mini, 16 GB, macOS 26.6.2 |
| Compiler | Apple clang 21.0.0 (arm64), C++17 (matches the vcxproj) |
| Build | CMake 4.3.1 + Ninja (Homebrew) |
| wxWidgets | Homebrew `wxwidgets@3.2` (`wx-config-3.2`) |
| Poco | Homebrew `poco` 1.15.3 (OpenSSL 3.6) |
| llama.cpp | release `b11539`, `llama-b11539-bin-macos-arm64.tar.gz` (Metal) |

## Build

```sh
# one-time: unpack a llama.cpp macOS release (git-ignored)
mkdir -p third_party/llama.cpp
curl -L -o third_party/llama-b11539-bin-macos-arm64.tar.gz \
  https://github.com/ggml-org/llama.cpp/releases/download/b11539/llama-b11539-bin-macos-arm64.tar.gz
tar -xzf third_party/llama-b11539-bin-macos-arm64.tar.gz -C third_party/llama.cpp

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
| 1 | Reproducible arm64 build | **Done**: all 91 translation units compile and link |
| 2 | App launches, main window | **Done**: main window and Welcome dialog open |
| 3 | Conversations save and reopen (Unicode paths) | **Verified 2026-10-09** for ASCII titles: the chat saved to `~/LlamaBoss/Chats/…`, reopened after quit/relaunch, and the model auto-reloaded. Unicode file names not yet tested |
| 4 | Local llama-server starts, reports errors, stops | **Verified 2026-10-09** with Gemma 3 1B Q4_K_M: bundled server on Metal, ready in 0.85 s, single slot confirmed, two chat turns, stopped cleanly on quit (0.1 s). Error path **verified 2026-10-11** (see below) |
| 5 | Remote endpoint streams; stop works | Not started |
| 6 | Shell and Python tools | Stubbed (see below) |
| 7 | File-tool boundaries, protected credentials, packaged app | Partly implemented, not verified |

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
- Tests: the native test project (`Tests/`) has no CMake target yet.

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

## Next task

1. Remaining UI checks: project attach, message boxes, light themes.
2. Phase 4: test the notarized DMG on another Mac or a fresh user account; host the DMG;
   Mac entry in the update feed (`version.json`); "Download for Mac" + Mac wording on
   llamaboss.com (preview first, publish only with the user's approval).
3. Skipped: the `Tests/` suite is for the Windows side (user decision 2026-10-12).
