# LlamaBoss

A lightweight native desktop chat client for [Ollama](https://ollama.com), built with C++ using wxWidgets and Poco.

![LlamaBoss Screenshot](https://github.com/Littleczr/llamaboss/releases/download/v1.0.0/screenshot-1.jpg)

## Features

- **Streaming chat** with any Ollama model
- **Group chat** — invite two models into one conversation (round-robin or @directed)
- **Model picker** — click the model pill to switch, right-click to open Model Manager
- **Model Manager** — pull, delete, and create custom models via Modelfile Creator
- **Streaming markdown renderer** — headings, bold/italic, code blocks with one-click copy
- **Dark & light themes** with live switching
- **Image attachment** — file dialog, drag-and-drop, or Ctrl+V clipboard paste
- **Text file attachment** — 21 supported extensions, 100KB cap
- **Conversation sidebar** — auto-save, multi-select, and delete
- **Conversation persistence** — JSON format with Ctrl+S / Ctrl+O / Ctrl+N shortcuts
- **Think-block support** — collapsible `<think>` block rendering
- **Fast drag-scroll** in chat display
- **Window and sidebar state persistence** across sessions

## Screenshots

| Main UI | Group Chat | Theme Switch |
|---|---|---|
| ![Main UI](https://github.com/Littleczr/llamaboss/releases/download/v1.0.0/screenshot-1.jpg) | ![Group Chat](https://github.com/Littleczr/llamaboss/releases/download/v1.0.0/screenshot-2.jpg) | ![Theme Switch](https://github.com/Littleczr/llamaboss/releases/download/v1.0.0/screenshot-3.jpg) |

## Requirements

- Windows 10/11 (x64)
- [Ollama](https://ollama.com) running locally (default: `http://127.0.0.1:11434`)

## Quick Start

1. Install and run [Ollama](https://ollama.com)
2. Pull a model: `ollama pull gemma3`
3. Download the latest release from [Releases](https://github.com/Littleczr/llamaboss/releases)
4. Extract and run `LlamaBoss.exe`

## Building from Source

### Prerequisites

- Visual Studio 2022 or 2025 (C++17)
- [vcpkg](https://github.com/microsoft/vcpkg) (manifest mode)

### Dependencies

Managed automatically by vcpkg via `vcpkg.json`:

- **wxWidgets** — UI framework
- **Poco** (JSON + Net) — HTTP client and JSON parsing

### Build Steps

1. Clone the repo:
   ```
   git clone https://github.com/Littleczr/llamaboss.git
   cd llamaboss
   ```
2. Open `LlamaBoss.slnx` in Visual Studio
3. vcpkg will automatically restore dependencies on first build
4. Build in **Release | x64** configuration
5. The exe and required DLLs will be in `x64/Release/`

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Enter` | Send message |
| `Ctrl+N` | New chat |
| `Ctrl+S` | Save conversation |
| `Ctrl+O` | Open conversation |
| `Ctrl+V` | Paste image from clipboard |

## Group Chat

Click the **+** button next to the model pill to invite a second model. Both models respond to each message in round-robin order. You can direct a message to a specific model by starting with its name:

```
@gemma3 what do you think about that?
qwen, can you elaborate?
```

## Architecture

| Module | Purpose |
|--------|---------|
| `LlamaBoss.cpp` | Main frame, UI construction, event handlers, chat state machine |
| `chat_client` | Threaded HTTP streaming to Ollama `/api/chat` |
| `chat_display` | Message rendering to wxRichTextCtrl |
| `chat_history` | Conversation state, JSON persistence, API request building |
| `markdown_renderer` | Streaming markdown parser (headings, bold, code blocks, lists) |
| `conversation_sidebar` | Collapsible sidebar with file-explorer-style multi-select |
| `model_manager` | Pull, delete, unload models via Ollama API |
| `modelfile_creator` | Create custom models via `/api/create` |
| `settings` | Settings dialog with live model fetch |
| `theme` | Dark/light theme definitions (26-field ThemeData) |
| `app_state` | Config persistence, logger, window state |

## License

MIT
