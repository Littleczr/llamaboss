#pragma once

// ─── goal_verifier_support.h ───────────────────────────────────────
// Helpers extracted from LlamaBoss.cpp for the Goal subsystem's
// wire-protocol layer.  These functions parse free-form responses
// from the verifier and contract-builder models into typed structs,
// classify natural-language goal control phrases, and clip transcript
// text down to verifier-friendly sizes.
//
// Everything in here is pure: no wxWidgets, no MyFrame, no global
// state.  Durable goal state lives in goal_state.h; these are the
// transient parser/scratch types that MyFrame copies into GoalState
// after each verifier or contract-builder turn.

#include <string>
#include <vector>

// ─── Verifier verdict ──────────────────────────────────────────────
//
// One verifier turn yields one verdict.  MyFrame inspects it via the
// predicates and the `reason` text, then drives the GoalState
// transitions (MarkVerifiedComplete, MarkVerifierContinue,
// MarkAwaitingUser, ...).

enum class GoalVerifierVerdictKind {
    Unknown,
    Complete,
    Continue,
    AwaitUser
};

struct GoalVerifierVerdict {
    GoalVerifierVerdictKind kind = GoalVerifierVerdictKind::Unknown;
    std::string             reason;

    bool Parsed()      const { return kind != GoalVerifierVerdictKind::Unknown; }
    bool IsComplete()  const { return kind == GoalVerifierVerdictKind::Complete; }
    bool IsAwaitUser() const { return kind == GoalVerifierVerdictKind::AwaitUser; }
};

// Parse a verifier model response.  Accepts the labeled format
// (`Verdict: complete` / `Reason: ...`) and a small fallback where
// the response simply begins with the verdict token.  Returns a
// verdict with Parsed() == false if no recognized verdict appears.
GoalVerifierVerdict ParseGoalVerifierVerdict(const std::string& response);

// ─── Contract builder draft ────────────────────────────────────────
//
// One contract-builder turn yields one draft.  MyFrame inspects it
// via Parsed() and the field counts, then copies the fields into the
// durable GoalContract (goal_state.h) via GoalState::SetContractReady.

struct GoalContractDraft {
    std::vector<std::string> successCriteria;
    std::vector<std::string> constraints;
    std::vector<std::string> evidenceChecks;
    std::string              reason;

    bool Parsed() const { return !successCriteria.empty(); }
};

// Parse a contract-builder model response.  Recognizes lines starting
// with `success:`, `criterion:`, `constraint:`, `evidence:`, or
// `reason:` (case-insensitive, leading bullet glyphs tolerated).
// Caps per-category items (5 success / 4 constraint / 5 evidence)
// and clips overlong items.  Returns a draft with Parsed() == false
// if no success criteria were found.
GoalContractDraft ParseGoalContractDraft(const std::string& response);

// ─── Natural-language goal control recognizers ─────────────────────

// Recognize "this is a Goal" command-like phrases at the start of a
// user message.  On match, returns true and fills `objective` with
// everything after the recognized prefix.  Conservative by design:
// these are explicit command starters, not a fuzzy intent classifier.
bool LbTryParseNaturalLanguageGoalStart(const std::string& userInput,
                                        std::string&       objective);

// Recognize natural-language goal control commands (status / pause /
// resume / continue / verify / rebuild / clear).  On match, fills
// `command` with the canonical command token and returns true.
// Like the start recognizer, this is an explicit allow-list match
// after polite-prefix stripping.
bool LbTryParseNaturalLanguageGoalControl(const std::string& userInput,
                                          std::string&       command);

// ─── Text shaping helpers ──────────────────────────────────────────

// Collapse a goal objective (or other free text) to a single line of
// at most `maxBytes` bytes for compact display in status strips.
// Whitespace runs collapse to single spaces; overflow ends with "...".
std::string LbCompactGoalStripText(std::string s, size_t maxBytes);

// Clip a long body of text to at most `maxBytes` bytes for inclusion
// in verifier prompts.  Keeps the head and tail with a "[middle
// truncated]" marker between, since both ends typically carry the
// most informative context (framing at head, most recent activity
// at tail).
std::string LbClipForGoalVerifier(const std::string& text, size_t maxBytes);
