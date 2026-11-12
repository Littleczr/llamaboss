# LlamaBoss 0.1.29 - Patch Notes

## Safety

- Tools can no longer read stored credentials. `git credential fill`, `gh auth token`, Windows Credential Manager, keyring, DPAPI, `.git-credentials`, gh `hosts.yml`, LlamaBoss `secrets.json` and browser password files are refused in PowerShell, Python and script-writing tools, even when tools are approved for the chat. On the Mac this also covers Keychain password reads.
- The [Open] button on file chips now respects the same blocked list as the `open` tool.
- `.html` files opened by a model now open in your default browser instead of being read inline.

## Models and providers

- Add AI provider now lists models from a running llama-server (the list used to come up empty).
- The local server scan checks ports 8080–8090 and labels what it finds "local server".
- Provider window polish: clearer model counts, no stale unticked model rows, and Edit/Delete now work after you add service keys.
- New Eject button in Manage Models (right-click the model pill) and an eject icon on the Project/Skills strip, to free VRAM when you need the GPU for something else. A model can't be ejected while it's working.

## Agent and tools

- Remote chats always send the native tool catalog. Before, loading a model in another window could make the next request go out without tools.
- Long chats keep their prompt cache. Once a chat needs trimming, it trims once to 85% and reuses that cut instead of trimming one item per request.
- When a reply starts with two or more read-only calls, they now run together instead of only the first.
- Models that announce a step ("I'm writing the file now…") and then stop are nudged to actually do it.
- Better recovery from malformed tool calls: invented "[elided]" markers are rejected, and arguments wrapped in an extra `"arguments"` layer are unwrapped.
- `write` creates missing parent folders.
- `read`, `read_head` and `read_range` decode UTF-16 files that start with a BOM instead of showing a hex dump.
- A tool result that arrives after its agent turn has ended is no longer added to the chat as a stray message.
- When a Python script fails on a missing package like `yaml.constructor`, LlamaBoss now suggests the right package to install (`pyyaml`).
- Python scripts blocked with HTTP 403 get a hint to use `web_fetch_url`.
- python_run_script cards show the script's arguments.

## Documents

- Many Excel and PDF fixes from a pre-release review: workbook creation, xlsx inspect/report, CSV to Excel, PDF text extraction and PDF form filling.

## Interface

- The context meter no longer counts reasoning tokens.
- Chat titles skip the "[Attached ZIP archive …]" block.

## Reliability

- Saved provider settings and API keys are written safely: a failed save no longer leaves a half-written file.
- A reminders file that can't be read no longer causes problems.

# LlamaBoss 0.1.27 - Patch Notes

## Highlights

- **New Add / Edit AI provider window.** One screen instead of a series of dialogs: choose Local server, OpenRouter, OpenAI, ChatGPT plan or Custom across the top, connect on the left, pick a model on the right.
- **Finds local servers for you.** Choosing Local server scans the usual ports on this PC and lists what answers (Strata, LM Studio, Ollama, another llama-server), with no key needed.
- **Photos send much faster.** Large images are scaled down before they are sent, and phone photos now arrive the right way up.
- **New theme: Retro Hi-Fi.** A cassette-deck look, with cyan for actions and amber for code and readouts.

## Models and providers

- You can set a context window for each model (for example 128K). LlamaBoss reads it from the server when the server reports one, so the context meter matches what the server actually accepts.
- Saved API keys are shown masked, with a Replace key button.
- Models with built-in MTP that also read images now keep image processing on the GPU at every context size in Settings, so reading an image takes seconds instead of minutes.

## Chat

- The code-block Copy button is now a bold, accent-coloured **[ Copy ]** in every theme, so it reads as something you can click.
- Only the code block you copied most recently says "Copied". Earlier ones go back to "Copy".
- Clicking + for a new chat no longer leaves an empty saved chat and folder behind.
- Tool-card [show details], file actions and approval buttons are easier to read on light themes.
- The context meter fits its theme and no longer clips when you switch themes.

## Diagnostics

- Exported chat metrics now note when LlamaBoss stopped a reply that was repeating itself, and explain why such replies have no token counts.
