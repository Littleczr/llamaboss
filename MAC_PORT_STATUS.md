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
| API-key encryption key | login Keychain, service `LlamaBoss`, account `secrets-file-key` |

## Milestones

| # | Milestone | State |
| --- | --- | --- |
| 1 | Reproducible arm64 build | **Done**: all 91 translation units compile and link |
| 2 | App launches, main window | **Done**: main window and Welcome dialog open |
| 3 | Conversations save and reopen (Unicode paths) | **Verified 2026-10-09** for ASCII titles: the chat saved to `~/LlamaBoss/Chats/…`, reopened after quit/relaunch, and the model auto-reloaded. Unicode file names not yet tested |
| 4 | Local llama-server starts, reports errors, stops | **Verified 2026-10-09** with Gemma 3 1B Q4_K_M: bundled server on Metal, ready in 0.85 s, single slot confirmed, two chat turns, stopped cleanly on quit (0.1 s). The error path (bad model, crash at startup) is not yet exercised |
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

## Next task

Test the startup error path (a broken model file), then an OpenRouter endpoint
(milestone 5), then the Phase 3 tool ports.
