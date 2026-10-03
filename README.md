# LlamaBoss

**LlamaBoss** is a native Windows desktop AI assistant for local models, real
files, projects, reusable skills, and tools with approval controls. It is built
with C++ and wxWidgets and can also use remote OpenAI-compatible connections.

| Version | Status |
| --- | --- |
| Source in this repository | **0.1.20** development snapshot |
| Published Windows installer | **0.1.19**, available from [llamaboss.com](https://llamaboss.com/) |

Source updates and installer releases are separate. The website is the current
download destination; older GitHub releases do not represent the latest installer.

- [Download and setup](https://llamaboss.com/)
- [Product documentation](https://llamaboss.com/docs/)
- [Native regression suite](Tests/LlamaBossTests/README_TESTS.md)

## What you can do

- Chat with local GGUF models through an app-owned llama.cpp service, with
  streaming responses, image attachments, conversation search, and multiple
  independent windows sharing the service.
- Configure remote connections, including direct OpenAI Responses support.
  Remote requests send the selected conversation data to the configured service.
- Download local models from direct HTTPS links in Settings and choose context,
  reasoning, and KV-cache settings for supported models.
- Give Agent Mode file, search, document, Python, and PowerShell tasks, with
  readable approval cards for actions that require review.
- Inspect and create CSV/XLSX files, extract PDF/DOCX text, inspect and fill PDF
  forms, and generate artifacts with embedded Python helpers.
- Keep long-lived Projects with instructions, sources, templates, outputs,
  workflows, and notes, and import/export reusable Skills.
- Schedule persistent reminders and use bounded waits to monitor ongoing work.
- Benchmark text-model generation with `/bench`, with results under
  `Shared/Benchmarks`.

Goals and `/goal` have been retired. Use ordinary chat or Agent Mode for
multi-step work; the [documentation](https://llamaboss.com/docs/) includes
migration guidance. The old collection of typed file/tool commands is also
retired: ask the assistant to use its tools instead.

## Projects and Skills

Projects live under `%USERPROFILE%\LlamaBoss\Projects`. Each project can contain
`PROJECT.md`, `project.json`, `Sources`, `Templates`, `Notes`, `Outputs`, and
`Workflows`. Attach a project to a chat to use its instructions and files.

Global notes live in `%USERPROFILE%\LlamaBoss\NOTES.md`; project notes live in
`Notes/NOTES.md`. Saving a note with a project active stores the full note in
the project and a compact pointer in global notes.

Skills live under `%USERPROFILE%\LlamaBoss\Skills`. A Skill has a `SKILL.md`
contract and can include scripts and reference files. Import a `SKILL.md` to
copy its containing folder, or import/export a Skill ZIP. Name collisions get
a suffix rather than replacing an existing Skill. The Skill builder decides
whether a helper script is useful.

## Chat files and local storage

Current chat folders use this layout:

```text
%USERPROFILE%\LlamaBoss\
  Chats\<date_title_id>\
    Workspace\          # attached files and generated artifacts
      Vars\             # stored tool variables
    Scripts\
    Documents\
    Spreadsheets\
    PDFs\
    Downloads\
  Projects\
  Skills\
  Shared\
  System\
  NOTES.md
```

Older per-chat `Workflows\chat_<id>` folders are handled by migration logic.
Project `Workflows` folders remain part of the project structure. `/cd` changes
a chat's tool working directory; it does not move the conversation's Workspace.

## Chat commands

| Command | Purpose |
| --- | --- |
| `/cd` or `/cd <path>` | Show or change this chat's tool working directory. |
| `/think` or `/think auto\|on\|off\|low\|medium\|high` | Show or set this chat's reasoning override; support depends on the model/provider. |
| `/agent_steps` or `/agent_steps <n>` | Show or set the agent step cap, clamped to 4–60; the app saves the setting. |
| `/bench [runs] [cold] [long]` | Benchmark a loaded text model; defaults to five runs. Use `/bench help` or `/bench stop` for help or cancellation. |
| `/reminder_create`, `/reminder_list`, `/reminder_cancel` | Use the supported reminder tools directly; see the documentation for arguments. |

## Tools, approvals, and privacy

Agent Mode can run safer read operations automatically. Write, delete, script,
command, and package actions follow the configured approval policy. Review the
paths and commands on each approval card. Package installation requires approval.

Native file writes are restricted to the permitted working directory, project,
and Skill locations. **Python and PowerShell run with your Windows account's
permissions**; those native file-tool boundaries are not an operating-system
sandbox for scripts. Approved scripts can access files and the network.

Python is optional for basic chat. Document helpers and script tools need a
usable Python installation and, for some tasks, packages such as `openpyxl`,
`pymupdf`, or `python-docx`. Built-in helper source is embedded from
`assets/python`. The persistent Python session can reuse state between calls;
state is lost when its worker restarts.

Local model inference does not require a remote AI endpoint. Remote connections,
downloads, and network-enabled scripts use the network as requested. Direct
connection secrets are stored in plaintext locally and protected by Windows
folder permissions. Environment-variable references are available for API keys;
do not commit keys or local connection files to this repository.

LlamaBoss is beta software. Test automation on copies of important files first.

## Build from source

The app targets Windows 10/11 x64. You need:

- Visual Studio with Desktop development with C++, the **v145** platform toolset,
  a Windows SDK, and support for the `.slnx` solution format.
- C++17 and vcpkg manifest integration. The manifests specify wxWidgets and Poco.
- A compatible local model and llama.cpp runtime for local inference. Models
  and runtime binaries are not included in this source repository.
- Python 3 if you want document helpers or Python tools.

```powershell
git clone https://github.com/Littleczr/llamaboss.git
cd llamaboss
```

Open `LlamaBoss.slnx`, restore dependencies through vcpkg, and build **Release |
x64**. `LlamaBoss.vcxproj` is the application; `Tests/LlamaBossTests` is the native
regression runner. Build the app project alone if you only need the application.
The native regression project ships its maintained JSON case files in
`Tests/LlamaBossTests/Cases`; see its README for the complete run instructions.

The embedded Python resources must remain UTF-8 without a BOM and use LF line
endings. `.gitattributes` preserves that on Windows checkouts.

## License and author

[MIT](LICENSE). Created by Cesar Avelar.
