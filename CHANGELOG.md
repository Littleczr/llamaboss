# Changelog

## v1.1.0 — 2026-03-31

### Added
- **Dark/Light theme system** — full theme engine with 26 color fields in `ThemeData`
- **Live theme switching** — change theme in Settings, applies instantly without restart
- **Theme persistence** — selected theme saved in Windows registry, restored on launch
- **Light theme** — clean, readable color scheme as an alternative to the default dark theme
- **Themed components** — every UI element (toolbar, sidebar, chat area, input, markdown renderer, status dot) respects the active theme with zero hardcoded colors

### Changed
- Extracted all color definitions from `openchat.cpp` into `theme.h` / `theme.cpp`
- `ChatDisplay` and `MarkdownRenderer` now accept theme colors dynamically via `ApplyTheme()`
- `StatusDot` supports configurable connected/disconnected colors
- Settings dialog includes a Theme dropdown (Dark / Light)
- `AppState` manages theme via `ThemeManager` with registry persistence

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
