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
