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
`LlamaBoss.app/Contents/Resources/llama.cpp/` (override with `-DLLAMABOSS_LLAMA_DIR=...`).

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
| Shell tool | `cmd_executor.cpp` `RunOne` | Phase 3, after choosing pwsh vs zsh |
| Python helpers + `python_run_script` | `python_runner.cpp` `StartWorker` | Phase 3, needs a Mac `lb_pyres::Load` from the bundle |
| Persistent `py` session | `python_session.cpp` | Phase 3 |
| Web fetch (incl. SSRF guard) | `tool_web_fetch.cpp` | Phase 3, Poco transport + getaddrinfo checks |
| In-app update install | `update_installer.cpp` | Phase 4, Mac package |
| Sign in with ChatGPT | `chatgpt_auth.cpp` | Later: Keychain storage + RS256 verification. Never base64 storage |

## Known gaps and risks

- Orphan protection: Windows puts llama-server in a KILL_ON_JOB_CLOSE job. On Mac, a
  LlamaBoss crash leaves llama-server running; the next launch then sees the port busy
  and refuses (the existing foreign-server guard). Planned: a PID file with cleanup at startup.
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

## Next task

Phase 3 tool ports (shell: zsh vs pwsh decision pending), then the macOS themed-button fix.
