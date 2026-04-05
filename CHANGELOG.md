# Changelog

## v1.4.0 — 2026-04-05

### Workspace Folder (New Feature)

LlamaBoss now supports a dedicated workspace folder for file operations, controlled via Settings with a checkbox and directory picker. Disabled by default.

**Default path:** `C:\Users\<user>\Documents\LlamaBossWorkspace`

#### Slash Commands
- `/search <query>` — Search workspace files by name. Recursive scan with case-insensitive multi-term matching. Results show relative paths, file sizes, and total files scanned.
- `/savechat [name]` — Save the current conversation as a markdown file. Auto-generates timestamped filename if no name given. Sanitizes custom names, appends numeric suffix on duplicates.

#### Natural Language Understanding
- LlamaBoss can now understand plain English workspace requests without slash commands.
  - "Save this chat as meeting notes" → saves to workspace
  - "Find the budget file" → scans workspace folder
  - "What is C++?" → correctly routed as regular chat
- Uses a short synchronous classification call to the active Ollama model (~450ms warm). Falls back to regular chat on any failure.
- Slash commands always bypass the LLM classifier for instant execution.

#### Architecture
- **Three-stage message pipeline:** (1a) deterministic slash-command router → (1b) LLM-assisted NL classifier → (2) model routing
- **New modules:** `action_classifier.h/.cpp`, `llm_classifier.h/.cpp`, `workspace_executor.h/.cpp`
- **C++17 required** for `std::filesystem`

---

## v1.3.0 — 2026-04-04

Directed multi-LLM routing, sidebar extraction, batch delete, and conversation reliability fixes.

### Added
- **Directed messaging in group chat** — prefix with `@model`, `model,`, or `model:` to address one model; the other stays silent. `@all`/`@everyone`/`@both` forces a full group turn.
- **Visual routing indicator** — directed messages display as "You → modelname:" in the UI, persisted in conversation JSON.
- **Sidebar extraction** — conversation sidebar moved to its own `conversation_sidebar.h/.cpp` component (~247 lines removed from `openchat.cpp`).
- **Multi-select batch delete** — Ctrl+Click, Shift+Click range selection; delete multiple conversations at once from a single confirmation dialog.

### Fixed
- Assistant prefix/first-paragraph spacing on long replies
- UTF-8 conversation save/load corruption on Windows
- Conversation history reordering on load (stale timestamp updates)
- Visual replay issues on older long-form conversations

---

## v1.2.0 — 2026-03-30

Multi-model group chat and repo rename to LlamaBoss.

### Added
- **Multi-model group chat** — load two Ollama models simultaneously; both respond in round-robin order
- **Participant-aware request builder** — prevents models from treating each other's replies as their own

---

## v1.1.1 — 2026-04-04

Light theme rendering fixes and conversation replay on theme switch.

---

## v1.1.0 — 2026-03-31

### Added
- **Dark & Light theme system** — switchable at runtime from Settings
- Theme preference persisted across sessions

---

## v1.0.0 — 2026-03-30

Initial public release.

### Features
- Streaming chat with Ollama's `/api/chat` endpoint
- Markdown rendering: bold, italic, inline code, fenced code blocks, headings, bullet/numbered lists, horizontal rules
- `<think>` block support for reasoning models (DeepSeek-R1, QwQ, etc.)
- Image attachment via file picker, drag-and-drop, and Ctrl+V clipboard paste
- Conversation auto-save/load with sidebar browser
- Settings dialog with live model list from Ollama API
- Dark theme (Telegram-inspired)
- Window position/size persistence
- Keyboard shortcuts: Ctrl+N, Ctrl+S, Ctrl+O
- Model auto-unload on switch (frees VRAM)
