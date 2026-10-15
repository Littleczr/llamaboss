# LlamaBoss 0.1.25 — Patch Notes

## Highlights

- **Updated local engine.** The bundled llama.cpp moves to v0.6.0. New models such as GLM-5.3-Flash and Qwen4Exp now load, MTP speed-ups are more reliable, and several NVIDIA crash fixes are included.
- **Agents recover on their own.** If a connection drops or a provider is overloaded, LlamaBoss now retries automatically instead of stopping silently.
- **Cleaner image sending.** Thumbnails now sit inside the message box, and LlamaBoss no longer adds "What is in this image?" to your message.

## Agent and tools

- Automatic retry after a dropped connection or a busy server (after 3 seconds, then 10). If the turn still can't finish, an **Agent Status** card says why, and typing `continue` picks up where it left off. Usage-limit errors are not retried.
- PowerShell scripts (`.ps1`) can now be edited in place, with your approval, instead of being rewritten from scratch.
- Larger Skill and Workflow instruction files are shown to the model in full.
- A mistyped tool argument, such as a reversed line range or an empty path, now gets a plain error message. It no longer counts toward the "three malformed calls" stop, so the agent keeps going.
- Very long lines in a file preview are now trimmed and clearly marked, instead of being shown as the whole file.
- "Large output saved to" messages now give the full file path.
- PowerShell errors that contain accented or non-English text now show up instead of failing silently.
- Python syntax errors now name your file instead of a temporary one.
- Empty files and files with a byte-order mark (BOM) are reported clearly.

## Cost and speed

- With Claude models, prompt caching now lasts 1 hour, so long tool runs cost less.
- On OpenRouter, each conversation stays on the same provider, so caching works more consistently.
- The agent instructions are about a third shorter. This lowers cost on every message and leaves more room for smaller local models.
- Faster reading of line ranges in large files, plus general internal cleanup.

## Fixes

- Fixed accented and non-English characters getting garbled in some places.
- Fixed a curly apostrophe breaking every later message when using the ChatGPT plan connection.
- Fixed picture-only messages disappearing when you reopen a chat.
- Filling PDF forms no longer needs PyMuPDF to be downloaded. It now uses pypdf, which ships with LlamaBoss. Python must still be installed on Windows.

## Before you update

- **Saved conversation caches will be rebuilt.** The new engine uses a different cache format, so the first message in an older chat may take a little longer to start. Your chats themselves are unchanged.
- **Windows still uses your own Python** for PDF and office tools. Bundled Python on Windows is planned for a future release. The Mac version already includes it.
