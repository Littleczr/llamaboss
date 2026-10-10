# LlamaBoss Beta v0.1.21

- Added ChatGPT account sign-in, add-by-ID provider/model setup, and exportable usage metrics.
- Improved conversation history and navigation.
- Retained the existing llama.cpp b11146 CPU and CUDA 12.4 binaries.

# LlamaBoss Beta v0.1.20

- Refined the chat interface with a centered model toolbar, refreshed icon and hover styling, and themed overlay scroll rails for the conversation list and composer.
- Updated the Windows app and bundled runtime dependencies; retained the existing llama.cpp b11146 CPU and CUDA 12.4 binaries.
- Additional UI polish, reliability improvements, and bug fixes.
# LlamaBoss Beta v0.1.19

- Updated the bundled llama.cpp CPU and CUDA 12.4 inference binaries to upstream build b11146 (0.5.0-dev).
- Improved file inspection with multi-range reads, grep context options, and more reliable handling of large tool outputs.
- Additional reliability improvements and bug fixes.

# LlamaBoss Beta v0.1.18

- Improved recovery of valid tool calls when assistant responses end with incomplete markup, while continuing to reject malformed calls safely.
- Fixed PowerShell command validation for escaped quotes and `#` comments without weakening approval safeguards.
- Improved agent progress detection so legitimate edits and changes in long tool-call arguments are not mistaken for repeated calls.
- Additional bug fixes and reliability improvements.
# LlamaBoss Beta v0.1.17

- Faster new-chat startup through prompt-cache warming.
- New context panel with token/context breakdown and reply timing/speed details.
- New `/bench` command for model performance tests, with results saved as TSV.
- Repetition protection helps stop model replies that get stuck repeating text.
- Chat sessions use readable per-chat folders; existing legacy chat folders remain recognized.

# LlamaBoss Beta v0.1.16

- New view_image tool: LlamaBoss can now look at images inside ZIP files, downloaded images, and pictures created by scripts — not just images pasted into the chat.
- zip_extract now lists extracted image files and points to view_image for visual inspection.
- read_range accepts up to 1,000 lines per range and combined; oversized requests are rejected with an error instead of being silently truncated.

# LlamaBoss — Add Model UX patch

Based on LlamaBoss_Source_h_cpp_20260910_085556.zip.

## Install

1. Close LlamaBoss and back up the seven matching source files in your project.
2. Replace them with the files in this ZIP, keeping their existing locations.
3. Rebuild in Visual Studio and launch LlamaBoss.

No new compilation units or project-file changes are required. This is a patch archive, not the full source tree.

Replacement files:
- agent_prompt_builder.cpp
- endpoints_dialog.cpp
- endpoints_dialog.h
- LlamaBoss.cpp
- model_switcher.cpp
- tool_context.h
- tool_router.cpp

## New behavior

Click the model name at the top of the chat and choose **Add model...**. Choose a saved provider, paste the exact model ID, optionally name it, and click **Add model**. Catalog membership is not required. The current chat keeps its selected model unless you check **Use this model in the current chat**.

The dialog reuses the provider's saved credential. Missing keys lead to **Connect a provider...**. It offers image generation and agent-tool options. The dialog scrolls and its initial size is limited to the monitor work area.

The model picker now opens even when no local GGUF files exist. Remote models and the two setup actions remain accessible.

In the existing provider editor, a saved connection with a usable saved key starts on the model page. **Connection settings** returns to the connection page. Use **Refresh models** to fetch the provider catalog. A visible exact-ID field and **Add ID** button allow a missing model to be selected before saving. The bulk text editor remains available as **Edit model list as text**.

## Add through chat

With Agent mode and a tool-capable model, ask:

> Add OpenRouter model openai/gpt-image-2.5-sunburst for image generation. Keep my current chat model selected.

The assistant can use the existing setup_connection tool with action add_model. This action adds a model directly to an existing provider without opening Settings or reading/writing credential files. It does not query the network or run a paid model test.

Tool argument example:

```json
{
  "provider": "openrouter",
  "action": "add_model",
  "model_id": "openai/gpt-image-2.5-sunburst",
  "display_name": "GPT Image 2.5 Sunburst",
  "image_output": true,
  "use_model": false
}
```

Provider is the exact saved connection ID; named custom connections are supported. For a new provider or missing credential, the assistant uses action setup and the existing native key-entry dialog. The tool accepts no key, URL, header or file-path arguments. Calling it ends the setup turn; an explicitly requested model switch happens after the agent stops streaming.

## Preservation and limitations

- Adding an existing ID is a no-op; its saved name and options are preserved.
- New IDs are appended without changing other models, endpoint URLs, paths, headers, protocol, reasoning format or credential references.
- A failed endpoint save restores the previous in-memory endpoint.
- Cancel in the compact dialog does not save a model.
- Manual registration does not verify model availability, access or API compatibility.
- The supplied Sunburst screenshot describes a dedicated Images API. This patch does not implement that API. LlamaBoss's existing image output uses chat completions, so adding the ID alone does not establish that Sunburst generation will work.

## Validation completed here

Compiled and ran two focused C++17 checks with -Wall -Wextra -Werror:

1. Production registration and ID-validation functions, using test doubles for EndpointStore/SecretsStore: additive registration; existing options and connection fields preserved; duplicate no-op; failed-save rollback; missing provider/key; no-auth provider; invalid ID/name rejection; image compatibility result.
2. Production request-validation function, using typed JSON-object test doubles: existing setup requests; exact-ID addition; custom connection ID; boolean options and defaults; unknown/credential fields rejected; URL/control/type/length checks; parser exception handling.

These checks do not build the complete Windows application, exercise the real Poco JSON decoder, or render wxWidgets. The provided archive contains .h/.cpp files only, and this environment lacks Visual Studio, wxWidgets and Poco development dependencies. Full compilation and the live UI remain to be verified on Windows.

## Suggested Windows check

1. Open the picker and add a model by ID. Confirm it appears while the current model stays selected.
2. Add the same ID again. Confirm only one entry remains and previous options stay intact.
3. Cancel an addition. Confirm no entry was created.
4. Edit a saved provider. Confirm it opens at the model page and that Add ID works when the search list has no match.
5. Ask the assistant to add an ID with use_model false. Confirm a result card appears without opening Settings or changing the current chat model.
6. Try the picker on a machine/profile with no local GGUF files. Confirm remote models and Add model remain accessible.
