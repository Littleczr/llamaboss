# Changelog

All notable changes to OpenChat will be documented in this file.

## v1.1.1 — 2026-04-01

### Added
* Clickable model picker — click the model pill in the toolbar to get a dropdown of all Ollama models, switch instantly without opening Settings
* Dropdown arrow indicator — model pill now shows a ▾ to signal it's clickable

### Changed
* Larger toolbar icons — hamburger, gear, info, and paperclip all bumped from 14pt to 18pt; new chat + bumped to 22pt; button hit areas increased to 44px height
* Better contrast — app title 13pt → 15pt, model label 11pt → 13pt for improved readability on high-res monitors
* Light theme overhaul — refined color palette for strong readability on white backgrounds (deeper greens/blues for chat text, more distinct toolbar/sidebar/input backgrounds, better border visibility)
* Toolbar title and model label now use theme-aware text color instead of hardcoded white, fixing invisible text in light theme

### Fixed
* Crash on window close after multiple conversations — replaced Unbind() calls in OnClose() with a frame-level m_isClosing flag; assistant event handlers (OnAssistantDelta, OnAssistantComplete, OnAssistantError) now early-return when the frame is closing, preventing wxWidgets debug assertions during shutdown
* Theme switching now re-renders existing conversation — previously, switching themes mid-conversation left old text rendered with the previous theme's colors (e.g. mint green on white background); the conversation is now replayed with the new theme's colors

## v1.0.0 — 2026-03-30

Initial public release.

* Streaming chat with any Ollama model via /api/chat
* Markdown rendering — code fences, headings, bold/italic/code inline, horizontal rules, bullets, numbered lists
* `<think>` block detection for reasoning models (DeepSeek-R1, QwQ, etc.)
* Image input — drag-and-drop, file picker, and Ctrl+V clipboard paste (via WM_PASTE interception)
* Conversation persistence — auto-save/load to %APPDATA%\openchat\conversations\
* Sidebar with conversation history, relative timestamps, click to load, right-click to delete
* Settings dialog with async model list fetching
* Dark and Light themes with live switching + System theme (reads Windows registry)
* Window position/size persistence across sessions
* Model unload on exit via keep_alive: 0
* Keyboard shortcuts: Ctrl+N (new chat), Ctrl+S (save), Ctrl+O (open)
