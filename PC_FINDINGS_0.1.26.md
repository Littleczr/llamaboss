# PC findings for 0.1.26 (from the Mac 0.1.25 pass, 2026-10-15)

Found while merging main `91bb2db` (Windows 0.1.25) into the Mac branch and testing
it on the Mac. None of these was fixed on the PC side; line numbers are on `91bb2db`.
Ordered by impact.

## 1. Old-format slot cache: restore is sent anyway and wipes the warm prefix

- **File:** `server_manager.cpp:1290-1302` (`RestoreSlotStateForConversation`)
- **What happens:** after the llama.cpp update (state-seq format 3 -> 4), every chat
  saved under the old build still has a `.kvbin` file. Reopening it sends
  `POST /slots/0?action=restore`; llama-server answers HTTP 400 ("unknown (magic,
  version) for sequence state file: 67677371, 00000003"). The app logs "full
  reprocess fallback" and shows no error (good), but the failed restore also
  empties the slot, so the pre-warmed system prompt is lost too. On the Mac
  (Qwen 3.5 4B, M4) the next message reprocessed all 16,912 tokens (53 s) instead
  of only the chat. The stale file stays, so the 400 repeats on every reopen until
  a new save overwrites it.
- **Windows impact:** every chat with a cache from 0.1.24 or earlier (b11146, format
  3) hits this once after updating to 0.1.25.
- **Suggested fix:** before enqueuing the restore, read the file's first 8 bytes
  (magic `ggsq` = `0x67677371`, then a uint32 version). If the version is not 4
  (`LLAMA_STATE_SEQ_VERSION` in the bundled llama.cpp), delete the file, log one
  line ("kvslot: dropped <name> (format v3, needs v4)") and skip the restore. The
  warm prefix survives and the chat rebuilds from it.

## 2. Transient-retry gate counts hidden reasoning as visible output

- **File:** `LlamaBoss.cpp:3367` (`m_chatHistory->HasAssistantPlaceholder()` before
  `BeginTransientRetry`)
- **What happens:** a stream cut after the first `reasoning_content` delta (nothing
  shown in the chat yet) is not retried. The agent stops with "Agent Status ·
  stopped · model request failed" and the user has to type `continue`. A cut before
  any data, or a 503, is retried correctly.
- **Repro (Mac, fault-injecting proxy in front of llama-server):** cut the SSE stream
  after ~700 bytes (role chunk + one reasoning token) -> no retry; cut right after the
  headers -> "retry 1 of 2 in 3000 ms", then the turn completes.
- **Suggested fix:** treat a placeholder that holds only reasoning (no answer text, no
  tool-call deltas) as empty for the retry decision, and drop that reasoning when
  retrying.

## 3. Double period in the Agent Status failure message

- **File:** `agent_controller.cpp:4008`
- **What happens:** `"The model request failed: " + ShortForm(errorText, 300) + "."`
  gives "...before the server sent a terminal event.. No tool from the failed reply
  ran." because the error text already ends with a period.
- **Suggested fix:** append "." only when the short form does not already end in
  `.`, `!` or `?`.

## 4. Empty assistant header above the retry notice

- **File:** `LlamaBoss.cpp:3367-3380` (transient retry branch)
- **What happens:** the placeholder message is removed from history and
  `DisplayAssistantComplete()` is called, but the on-screen assistant name ("qwen")
  stays with nothing under it, then the "Model request failed (...). Retrying in 3 s"
  notice follows.
- **Suggested fix:** remove the empty assistant header from the display when the
  placeholder is dropped (or show the notice inside that block).

## 5. Native tests hard-code Windows separators

- **Files:** `Tests/LlamaBossTests/var_store_tests.cpp:97, 130, 134, 153`;
  `Tests/LlamaBossTests/Cases/40_extended_native_boundaries.json:45, 49, 50, 51`
- **What happens:** the expectations require `Vars\...`. On the Mac the same code
  returns `Vars/...` (intended), so 7 of the 112 cases fail there for the separator
  alone. Behaviour on Windows is unaffected.
- **Suggested fix:** compare after normalizing `\` to `/`, or build the expected path
  with the platform separator. (The 8th Mac failure is the known stale
  `R0_S3_Prompt_Requires_Persistent_Python_For_Arithmetic`.)

## 6. "Image persisted ... (0 bytes)" for pasted pictures

- **File:** `attachment_manager.cpp:1069` (log), `attachment_manager.cpp:148`
  (`originalSize = 0` for clipboard images)
- **What happens:** the log always says 0 bytes for a pasted picture, though the file
  is written correctly (35,538 bytes in the test).
- **Suggested fix:** log the decoded byte count (count bytes while copying, or
  `file_size` after `close()`).

## Shared files changed on the Mac branch

None needs to go to the PC. Every change this pass is either Mac-only
(`lb_mac_clipboard.h`, `lb_mac_native.mm`, `CMakeLists.txt`) or inside a Mac fence in a
shared file whose `unifdef -D_WIN32 -D__WXMSW__` view is byte-identical to main:
`agent_prompt_builder.cpp`, `chat_input_ctrl.h`, `conversation_sidebar.cpp`,
`LlamaBoss.cpp`, `tool_path.cpp`, `var_store.cpp`. No installer package was needed.
