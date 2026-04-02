# OpenChat

A native desktop chat client for [Ollama](https://ollama.com), built with C++, wxWidgets, and Poco.

Fast, lightweight, and entirely local — no cloud, no telemetry, no Electron.

![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)

## Features

- **Streaming responses** with real-time token display
- **Markdown rendering** — bold, italic, code, headings, lists, fenced code blocks with language labels
- **`<think>` block support** — reasoning models (DeepSeek-R1, QwQ, etc.) display thought process in muted text before the answer
- **Image vision** — attach images via file picker, drag-and-drop, or Ctrl+V clipboard paste
- **Conversation history** — auto-save/load with a sidebar browser, right-click to delete
- **Model management** — live model list from Ollama's API, auto-unload on model switch
- **Keyboard shortcuts** — Ctrl+N (new chat), Ctrl+S (save), Ctrl+O (open)
- **Dark theme** — Telegram-inspired color scheme
- **Window persistence** — remembers position, size, and maximized state across sessions
- **Settings dialog** — change model and API URL at runtime with live model fetching

## Screenshot

<!-- Replace with an actual screenshot -->
<!-- ![OpenChat screenshot](docs/screenshot.png) -->

## Requirements

- **OS:** Windows 10+ (64-bit)
- **Compiler:** Visual Studio 2022 or later (MSVC v143+)
- **Package manager:** [vcpkg](https://vcpkg.io) (for dependencies)
- **Runtime:** An Ollama instance running locally or on your network

## Building

### 1. Install vcpkg (if you haven't already)

```
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
bootstrap-vcpkg.bat
```

Set the environment variable `VCPKG_ROOT` to your vcpkg directory, or pass it to CMake directly.

### 2. Clone and build

```
git clone https://github.com/yourusername/openchat.git
cd openchat
```

**Option A — Visual Studio (recommended)**

Open `openchat.sln` in Visual Studio. The project is configured with vcpkg manifest mode (`vcpkg.json`), so dependencies install automatically on first build. Select **Release | x64** and build.

**Option B — CMake**

```
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

### 3. Run

```
x64\Release\openchat.exe
```

Make sure Ollama is running (`ollama serve`) before launching.

## Dependencies

Managed via vcpkg manifest (`vcpkg.json`):

| Library | Purpose |
|---|---|
| **wxWidgets** | GUI framework (wxRichTextCtrl, dialogs, drag-and-drop) |
| **Poco** | HTTP client, JSON parsing, Base64 encoding, logging, UUID |

## Configuration

Settings are stored in the Windows registry under `HKCU\Software\OllamaChatApp` (via `wxFileConfig`).

| Key | Default | Description |
|---|---|---|
| `Model` | `llama3` | Ollama model name |
| `ApiBaseUrl` | `http://127.0.0.1:11434` | Ollama API endpoint |
| `WindowX/Y/W/H` | — | Window geometry |
| `WindowMaximized` | `false` | Maximized state |

Conversations are saved as JSON files in `%APPDATA%\openchat\conversations\`.

## Project Structure

```
openchat/
├── openchat.cpp            # Main frame, UI layout, event wiring
├── chat_client.h/.cpp      # HTTP streaming thread (Ollama /api/chat)
├── chat_display.h/.cpp     # Message rendering (user/assistant/system/think)
├── chat_history.h/.cpp     # Conversation state, JSON save/load
├── markdown_renderer.h/.cpp # Streaming markdown → wxRichTextCtrl
├── app_state.h/.cpp        # Config, logging, window state persistence
├── settings.h/.cpp         # Settings dialog with async model fetching
├── CMakeLists.txt          # CMake build (alternative to .sln)
├── vcpkg.json              # Dependency manifest
├── openchat.sln            # Visual Studio solution
└── docs/                   # Documentation
```

## Usage Notes

- **Remote Ollama:** Change the API URL in Settings to point to any reachable Ollama instance (e.g. `http://192.168.1.74:11434`).
- **Vision models:** Attach an image, then type your question. If you send an image with no text, the default prompt is "What is in this image?"
- **Pasting images:** Ctrl+V detects clipboard images at the Windows `WM_PASTE` level, so it works even though the input is a multiline text control.
- **Model unloading:** When you switch models in Settings, the previous model is automatically unloaded from VRAM via `keep_alive: 0`.

## License

[MIT](LICENSE)
