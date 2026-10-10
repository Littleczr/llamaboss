#pragma once
// ═══════════════════════════════════════════════════════════════════
//  tool_call_elision.h — shorten the arguments of OLD tool calls
// ═══════════════════════════════════════════════════════════════════
//
// In long agent chats most of the context is the model's own tool-call
// ARGUMENTS: 2-4 KB PowerShell scripts, file bodies for write_file, and
// so on.  Eliding only tool RESULTS leaves the request over budget even
// with every eligible result elided.
//
// ShortenToolCallArguments() returns a shorter, still-valid JSON
// object for one call's `function.arguments` string:
//   * every string value longer than keepChars is cut (UTF-8 safe) to
//     keepChars characters plus a plain-ASCII marker, so the model
//     still sees WHICH keys it passed and how each one began;
//   * key order is preserved (ParseHandler(preserveObjectOrder));
//   * if the result is still large (many small values), or the input
//     is not a JSON object, it falls back to
//     {"_elided_arguments": "<first keepChars>... [marker]"}.
// The input is returned unchanged when it is already small, so the
// operation is idempotent across builds.
//
// Spool + guard: models DO copy an old call to rerun it, marker
// included.  A cut between statements would run half a script, and a
// copied write_file body would write a truncated file.  Two defences:
//   * every cut value is spooled (when a spool callback is supplied) and
//     the marker names the file, so the full text is one read away --
//     the same pattern as tool-result elision;
//   * ContainsArgElisionMarker() lets argument validation reject any new
//     tool call that carries the marker, for every tool, before approval
//     or execution (tool_invocation.cpp / agent_controller.cpp).
//
// ShortenToolCallsArray() deep-copies a tool_calls array with every
// call's arguments shortened.  The source array is the one stored in
// ChatHistory::m_messages and must never be mutated — elision is a
// per-request projection only.
//
// Header-only, Poco::JSON only (no wx), so it compiles standalone in
// tool_call_elision_tests.cpp.

#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/ParseHandler.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/Dynamic/Var.h>

#include <cstddef>
#include <functional>
#include <sstream>
#include <string>
#include <typeinfo>
#include <vector>

namespace lb_toolcall_elision {

// Arguments at or under this many bytes are left alone: shortening
// them saves little and costs the model its exact record.
constexpr std::size_t kMinElideArgBytes = 600;
// Characters of each long string value that survive.
constexpr std::size_t kKeepChars = 200;
// After per-value cutting, a result still over this size falls back
// to the single-key form.
constexpr std::size_t kMaxShortenedArgBytes = 1200;

// Saves the full text of one cut value and returns the path the model
// should read (e.g. "Vars\elided_<hash>.txt"), or "" when spooling is
// unavailable.  Must be deterministic for identical input so the marker
// stays byte-stable across builds.
using SpoolFn = std::function<std::string(const std::string& fullText)>;

// Fixed phrase present in every marker this header has ever produced
// (including the first shipped form, "... [N more bytes of this old,
// already-executed tool call elided to fit context]").  Plain ASCII
// with no quotes or backslashes, so it survives JSON escaping and is
// found in raw JSON arguments and in projected args alike.
inline const char* ArgElisionSentinel()
{
    return "of this old, already-executed tool call elided to fit context";
}

// True when `s` carries a real marker: the sentinel immediately preceded
// by "[<digits> more bytes " or "[TRUNCATED: <digits> more bytes ".
// The bare phrase alone does not match, so the model can still grep
// for it (agents working on LlamaBoss itself do).
inline bool ContainsArgElisionMarker(const std::string& s)
{
    const std::string sentinel = ArgElisionSentinel();
    const std::string tail = " more bytes ";
    std::size_t at = s.find(sentinel);
    while (at != std::string::npos) {
        if (at >= tail.size() && s.compare(at - tail.size(), tail.size(), tail) == 0) {
            std::size_t d = at - tail.size();
            std::size_t digits = 0;
            while (d > 0 && s[d - 1] >= '0' && s[d - 1] <= '9') { --d; ++digits; }
            if (digits > 0) {
                const std::string t1 = "[", t2 = "[TRUNCATED: ";
                if ((d >= t2.size() && s.compare(d - t2.size(), t2.size(), t2) == 0) ||
                    (d >= t1.size() && s.compare(d - t1.size(), t1.size(), t1) == 0))
                    return true;
            }
        }
        at = s.find(sentinel, at + 1);
    }
    return false;
}

// The spool path named by the first marker in `s`, or "".  Works on
// raw JSON too: the doubled backslash is collapsed.
inline std::string SpoolPathFromMarker(const std::string& s)
{
    const std::string key = "full text: ";
    const std::size_t at = s.find(ArgElisionSentinel());
    if (at == std::string::npos) return {};
    std::size_t p = s.find(key, at);
    if (p == std::string::npos) return {};
    p += key.size();
    const std::size_t end = s.find(']', p);
    if (end == std::string::npos || end - p > 260) return {};
    std::string path = s.substr(p, end - p);
    std::string out;
    for (std::size_t i = 0; i < path.size(); ++i) {
        out += path[i];
        if (path[i] == '\\' && i + 1 < path.size() && path[i + 1] == '\\') ++i;
    }
    return out;
}

// Model-facing rejection text for a call that carries the marker.
inline std::string CopiedElisionRejection(const std::string& args)
{
    const std::string spool = SpoolPathFromMarker(args);
    std::string msg =
        "No tool was executed. These arguments contain text copied from an "
        "OLD tool call whose arguments were shortened in the conversation "
        "history (the marker \"... elided to fit context\"). That copy is "
        "truncated: running it would execute or write only part of the "
        "original. ";
    if (!spool.empty())
        msg += "The full original text is saved at " + spool +
               ": read it, then reissue the call with the complete text "
               "(edited as needed). ";
    else
        msg += "Rewrite the complete arguments instead of copying the "
               "shortened form. ";
    msg += "Never include the elision marker in a tool call.";
    return msg;
}

// Marker suffix.  Plain ASCII apart from the spool path.  The wording
// says the call already ran and the text is not runnable as shown.
inline std::string ElisionMarker(std::size_t droppedBytes,
                                 const std::string& spoolPath = std::string())
{
    std::string m = "... [TRUNCATED: " + std::to_string(droppedBytes) +
                    " more bytes " + ArgElisionSentinel() +
                    ". Not runnable as shown";
    if (!spoolPath.empty()) m += "; full text: " + spoolPath;
    return m + "]";
}

// Largest prefix of `s` that is <= maxBytes and does not split a
// UTF-8 sequence.
inline std::size_t Utf8SafeCut(const std::string& s, std::size_t maxBytes)
{
    if (s.size() <= maxBytes) return s.size();
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return cut;
}

inline std::string ShortenString(const std::string& s, std::size_t keepChars,
                                 const SpoolFn& spool = SpoolFn())
{
    // Only cut when it actually saves bytes after adding the marker.
    const std::size_t markerEstimate = 160;
    if (s.size() <= keepChars + markerEstimate) return s;
    if (ContainsArgElisionMarker(s)) return s;   // already shortened
    const std::size_t cut = Utf8SafeCut(s, keepChars);
    std::string path;
    if (spool) {
        try { path = spool(s); } catch (...) { path.clear(); }
    }
    return s.substr(0, cut) + ElisionMarker(s.size() - cut, path);
}

namespace detail {

inline Poco::Dynamic::Var ShortenVar(const Poco::Dynamic::Var& v, std::size_t keepChars,
                                     const SpoolFn& spool);

inline Poco::JSON::Object::Ptr ShortenObject(const Poco::JSON::Object::Ptr& in,
                                             std::size_t keepChars, const SpoolFn& spool)
{
    Poco::JSON::Object::Ptr out = new Poco::JSON::Object(Poco::JSON_PRESERVE_KEY_ORDER);
    std::vector<std::string> names;
    in->getNames(names);   // insertion order when the source preserves it
    for (const auto& k : names) out->set(k, ShortenVar(in->get(k), keepChars, spool));
    return out;
}

inline Poco::JSON::Array::Ptr ShortenArray(const Poco::JSON::Array::Ptr& in,
                                           std::size_t keepChars, const SpoolFn& spool)
{
    Poco::JSON::Array::Ptr out = new Poco::JSON::Array;
    for (std::size_t i = 0; i < in->size(); ++i)
        out->add(ShortenVar(in->get(static_cast<unsigned>(i)), keepChars, spool));
    return out;
}

inline Poco::Dynamic::Var ShortenVar(const Poco::Dynamic::Var& v, std::size_t keepChars,
                                     const SpoolFn& spool)
{
    if (v.isEmpty()) return v;
    if (v.type() == typeid(Poco::JSON::Object::Ptr))
        return ShortenObject(v.extract<Poco::JSON::Object::Ptr>(), keepChars, spool);
    if (v.type() == typeid(Poco::JSON::Array::Ptr))
        return ShortenArray(v.extract<Poco::JSON::Array::Ptr>(), keepChars, spool);
    if (v.isString())
        return ShortenString(v.convert<std::string>(), keepChars, spool);
    return v;   // numbers, bools
}

inline std::string Stringify(const Poco::Dynamic::Var& v)
{
    std::ostringstream oss;
    Poco::JSON::Stringifier::stringify(v, oss);
    return oss.str();
}

inline std::string FallbackForm(const std::string& args, std::size_t keepChars,
                                const SpoolFn& spool)
{
    Poco::JSON::Object::Ptr o = new Poco::JSON::Object;
    const std::size_t cut = Utf8SafeCut(args, keepChars);
    std::string path;
    if (spool) {
        try { path = spool(args); } catch (...) { path.clear(); }
    }
    o->set("_elided_arguments", args.substr(0, cut) + ElisionMarker(args.size() - cut, path));
    return Stringify(o);
}

} // namespace detail

// See header comment.  Never throws; returns `args` unchanged when it
// is small or when shortening would not make it smaller.
inline std::string ShortenToolCallArguments(const std::string& args,
                                            std::size_t keepChars = kKeepChars,
                                            std::size_t minBytes  = kMinElideArgBytes,
                                            const SpoolFn& spool  = SpoolFn())
{
    if (args.size() <= minBytes) return args;
    std::string out;
    try {
        Poco::JSON::Parser parser(new Poco::JSON::ParseHandler(true /*preserve order*/));
        const Poco::Dynamic::Var parsed = parser.parse(args);
        if (parsed.type() == typeid(Poco::JSON::Object::Ptr)) {
            out = detail::Stringify(detail::ShortenObject(
                parsed.extract<Poco::JSON::Object::Ptr>(), keepChars, spool));
            if (out.size() > kMaxShortenedArgBytes) out.clear();
        }
    } catch (...) {
        out.clear();
    }
    try {
        if (out.empty()) out = detail::FallbackForm(args, keepChars, spool);
    } catch (...) {
        return args;
    }
    return out.size() < args.size() ? out : args;
}

// Deep copy of `toolCalls` with every call's function.arguments run
// through ShortenToolCallArguments.  Returns nullptr when nothing got
// shorter (the caller keeps the original and its Responses sidecar).
// The input array and its objects are never modified.
inline Poco::JSON::Array::Ptr ShortenToolCallsArray(const Poco::JSON::Array::Ptr& toolCalls,
                                                    std::size_t keepChars = kKeepChars,
                                                    std::size_t minBytes  = kMinElideArgBytes,
                                                    const SpoolFn& spool  = SpoolFn())
{
    if (!toolCalls) return nullptr;
    bool changed = false;
    Poco::JSON::Array::Ptr out = new Poco::JSON::Array;
    try {
        for (std::size_t i = 0; i < toolCalls->size(); ++i) {
            const Poco::Dynamic::Var item = toolCalls->get(static_cast<unsigned>(i));
            Poco::JSON::Object::Ptr call;
            if (item.type() == typeid(Poco::JSON::Object::Ptr))
                call = item.extract<Poco::JSON::Object::Ptr>();
            Poco::JSON::Object::Ptr fn = call ? call->getObject("function") : nullptr;
            if (!fn || !fn->has("arguments") || fn->isNull("arguments") ||
                !fn->get("arguments").isString()) {
                out->add(item);
                continue;
            }
            const std::string args = fn->getValue<std::string>("arguments");
            const std::string shorter =
                ShortenToolCallArguments(args, keepChars, minBytes, spool);
            if (shorter == args) {
                out->add(item);
                continue;
            }
            // Shallow copies: setting a key on the copy never reaches
            // the stored objects.
            Poco::JSON::Object::Ptr fnCopy   = new Poco::JSON::Object(*fn);
            Poco::JSON::Object::Ptr callCopy = new Poco::JSON::Object(*call);
            fnCopy->set("arguments", shorter);
            callCopy->set("function", fnCopy);
            out->add(callCopy);
            changed = true;
        }
    } catch (...) {
        return nullptr;
    }
    return changed ? out : nullptr;
}

} // namespace lb_toolcall_elision
