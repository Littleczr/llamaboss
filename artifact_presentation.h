// artifact_presentation.h
// Centralizes user-facing tool-card labels/icons for created artifacts.
//
// This keeps file-type presentation policy out of LlamaBoss.cpp:
// tool results can still carry PresentedFile entries, and this helper
// upgrades the card label/icon to "Create Spreadsheet", "Create PDF",
// "Create Markdown Document", etc. when the invocation succeeded.

#pragma once

struct ToolInvocationResult;

// Mutates a successful tool result in-place when it carries presented files.
// Error results are left unchanged.
void ApplyArtifactPresentation(ToolInvocationResult& result);
