# macOS port status

Living checkpoint for the LlamaBoss macOS port. Update it at every milestone.

## Baseline

- Branch: `macos/port-v0.1.21`
- Based on GitHub `main` (v0.1.20, `9c73316`) plus the Windows source export
  `LlamaBoss_Source_h_cpp_20261009_174144.zip` (v0.1.21), committed unchanged as `c9a0021`.
  Every Mac change is a later commit, so `git diff c9a0021` shows the whole port.
- The Windows build still uses `LlamaBoss.vcxproj`; nothing Mac-specific is in it.
  **Mac commits have not been compiled on Windows yet.** Build this branch in Visual
  Studio before merging.

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
| 3 | Conversations save and reopen (Unicode paths) | Not yet verified |
| 4 | Local llama-server starts, reports errors, stops | Server verified standalone (Gemma 3 1B, ~92 tok/s); in-app launch not yet verified |
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

## Next task

Load Gemma 3 1B in the app, confirm the in-app server start/stop and a chat reply, and
check that the conversation saves and reopens.
