// reminder_store.h
//
// LlamaBoss in-app reminders (Option 1).
//
// Header-only on purpose: this patch can be dropped into the existing source
// tree without adding a new .cpp file to the Visual Studio project.  The
// singleton store is shared across translation units by the inline accessor.
//
// Persistence:
//   %USERPROFILE%\LlamaBoss\REMINDERS.json
//
// Delivery:
//   MyFrame owns a lightweight wxTimer and claims due reminders while at least
//   one LlamaBoss window is running.  A claimed reminder is NOT persisted as
//   completed until the user dismisses it, so a crash during presentation does
//   not silently lose the reminder.  Snooze moves the due time forward.
#pragma once

#include "tool_notes.h"   // GetNotesPath() gives us the canonical user root.

#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/string.h>

#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/Types.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <exception>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lb_reminders {

struct ReminderRecord {
    std::string  id;
    std::int64_t dueEpoch = 0;   // local wall-clock target represented as epoch seconds
    std::string  dueLocal;       // human-readable local time, cached for UI/tool output
    std::string  message;
};

struct ReminderToolResult {
    std::vector<std::string> chips;
    std::string body;
    std::string errorBody;
    std::string bodyLang;
};

namespace detail {

inline std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) {
            if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
            return static_cast<char>(c);
        });
    return s;
}

inline wxString StoragePathWx()
{
    // Keep reminders next to the existing NOTES.md user root instead of
    // inventing a second location policy.
    wxFileName notes(wxString::FromUTF8(GetNotesPath().c_str()));
    return wxFileName(notes.GetPath(), "REMINDERS.json").GetFullPath();
}

inline std::string StoragePathUtf8()
{
    const wxScopedCharBuffer b = StoragePathWx().ToUTF8();
    return b.data() ? std::string(b.data()) : std::string();
}

inline bool EnsureParentDirectory(const wxString& path)
{
    wxFileName fn(path);
    const wxString dir = fn.GetPath();
    if (dir.empty()) return false;
    if (wxDirExists(dir)) return true;
    return wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
}

inline std::string ReadWholeFile(const wxString& path)
{
    std::ifstream f(path.fn_str(), std::ios::in | std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline bool WriteWholeFileAtomic(const wxString& path, const std::string& body)
{
    if (!EnsureParentDirectory(path)) return false;

    const wxString tmp = path + ".tmp";
    {
        std::ofstream f(tmp.fn_str(),
                        std::ios::out | std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        f.flush();
        if (!f) return false;
    }
    return wxRenameFile(tmp, path, true);
}

inline std::string FormatLocal(std::int64_t epoch)
{
    const std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tmLocal{};
#ifdef _MSC_VER
    localtime_s(&tmLocal, &t);
#else
    if (const std::tm* p = std::localtime(&t)) tmLocal = *p;
#endif

    char buf[96]{};
    if (std::strftime(buf, sizeof(buf), "%A, %B %d, %Y at %I:%M %p", &tmLocal) == 0)
        return std::to_string(epoch);
    return std::string(buf);
}

inline bool ParseAbsoluteLocal(const std::string& text, std::int64_t& epochOut)
{
    // Accepted examples:
    //   2026-08-07 09:30
    //   2026-08-07T09:30
    //   2026-08-07 09:30:15
    const char* formats[] = {
        "%Y-%m-%d %H:%M",
        "%Y-%m-%dT%H:%M",
        "%Y-%m-%d %H:%M:%S",
        "%Y-%m-%dT%H:%M:%S"
    };

    for (const char* fmt : formats) {
        std::tm tmLocal{};
        tmLocal.tm_isdst = -1;
        std::istringstream ss(text);
        ss >> std::get_time(&tmLocal, fmt);
        if (ss.fail()) continue;

        // Reject trailing non-whitespace so "09:30 nonsense" cannot silently
        // become a different reminder than the model/user requested.
        ss >> std::ws;
        if (!ss.eof()) continue;

        const std::tm requested = tmLocal;
        const std::time_t t = std::mktime(&tmLocal);
        if (t == static_cast<std::time_t>(-1)) continue;

        // mktime normalizes impossible wall-clock values (for example
        // February 31) instead of rejecting them. Round-trip through local
        // time so a malformed date cannot quietly become a different one.
        std::tm roundTrip{};
#ifdef _MSC_VER
        localtime_s(&roundTrip, &t);
#else
        if (const std::tm* p = std::localtime(&t)) roundTrip = *p;
#endif
        if (roundTrip.tm_year != requested.tm_year ||
            roundTrip.tm_mon  != requested.tm_mon  ||
            roundTrip.tm_mday != requested.tm_mday ||
            roundTrip.tm_hour != requested.tm_hour ||
            roundTrip.tm_min  != requested.tm_min  ||
            roundTrip.tm_sec  != requested.tm_sec) {
            continue;
        }

        epochOut = static_cast<std::int64_t>(t);
        return true;
    }
    return false;
}

inline bool UnitSeconds(const std::string& unitRaw, double& multiplierOut)
{
    std::string u = Lower(unitRaw);
    while (!u.empty() && (u.back() == ',' || u.back() == '.')) u.pop_back();

    if (u == "s" || u == "sec" || u == "secs" ||
        u == "second" || u == "seconds") {
        multiplierOut = 1.0;
        return true;
    }
    if (u == "m" || u == "min" || u == "mins" ||
        u == "minute" || u == "minutes") {
        multiplierOut = 60.0;
        return true;
    }
    if (u == "h" || u == "hr" || u == "hrs" ||
        u == "hour" || u == "hours") {
        multiplierOut = 3600.0;
        return true;
    }
    if (u == "d" || u == "day" || u == "days") {
        multiplierOut = 86400.0;
        return true;
    }
    return false;
}

inline bool SplitCreateArgs(const std::string& args,
                            std::string& scheduleOut,
                            std::string& messageOut)
{
    // Native/XML agent calls normally use two lines.  Slash-command users can
    // use a compact one-line separator:
    //   /reminder_create in 2 hours | Check payroll discrepancies
    const size_t nl = args.find_first_of("\r\n");
    if (nl != std::string::npos) {
        scheduleOut = Trim(args.substr(0, nl));
        size_t body = nl;
        if (args[body] == '\r' && body + 1 < args.size() && args[body + 1] == '\n')
            body += 2;
        else
            body += 1;
        messageOut = Trim(args.substr(body));
        return !scheduleOut.empty() && !messageOut.empty();
    }

    const size_t pipe = args.find('|');
    if (pipe != std::string::npos) {
        scheduleOut = Trim(args.substr(0, pipe));
        messageOut  = Trim(args.substr(pipe + 1));
        return !scheduleOut.empty() && !messageOut.empty();
    }

    return false;
}

inline bool ParseSchedule(const std::string& schedule,
                          std::int64_t nowEpoch,
                          std::int64_t& dueEpochOut,
                          std::string& errorOut)
{
    const std::string trimmed = Trim(schedule);
    const std::string lower   = Lower(trimmed);

    if (lower.rfind("in ", 0) == 0) {
        std::istringstream ss(trimmed.substr(3));
        double amount = 0.0;
        std::string unit;
        if (!(ss >> amount >> unit) || !std::isfinite(amount) || amount <= 0.0) {
            errorOut = "Relative reminder syntax is 'in <number> <seconds|minutes|hours|days>'.";
            return false;
        }
        ss >> std::ws;
        if (!ss.eof()) {
            errorOut = "Relative reminder schedule has unexpected extra text. Put the reminder message on the next line (or after '|').";
            return false;
        }

        double multiplier = 0.0;
        if (!UnitSeconds(unit, multiplier)) {
            errorOut = "Unsupported reminder time unit. Use seconds, minutes, hours, or days.";
            return false;
        }

        const double delta = amount * multiplier;
        if (delta < 1.0 || delta > (365.0 * 86400.0)) {
            errorOut = "Reminder delay must be between 1 second and 365 days.";
            return false;
        }
        dueEpochOut = nowEpoch + static_cast<std::int64_t>(std::llround(delta));
        return true;
    }

    if (lower.rfind("at ", 0) == 0) {
        std::int64_t parsed = 0;
        if (!ParseAbsoluteLocal(Trim(trimmed.substr(3)), parsed)) {
            errorOut = "Absolute reminder syntax is 'at YYYY-MM-DD HH:MM' (24-hour local time).";
            return false;
        }
        if (parsed <= nowEpoch) {
            errorOut = "Reminder time is in the past. Choose a future local date/time.";
            return false;
        }
        dueEpochOut = parsed;
        return true;
    }

    errorOut = "Reminder schedule must start with 'in' or 'at'. Examples: 'in 2 hours' or 'at 2026-08-07 09:30'.";
    return false;
}

inline std::string OneLinePreview(const std::string& s, size_t maxChars = 120)
{
    std::string out = s;
    for (char& c : out) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }
    // Collapse repeated spaces for cleaner list output.
    std::string compact;
    compact.reserve(out.size());
    bool prevSpace = false;
    for (char c : out) {
        const bool isSpace = (c == ' ');
        if (isSpace && prevSpace) continue;
        compact.push_back(c);
        prevSpace = isSpace;
    }
    compact = Trim(compact);
    if (compact.size() > maxChars) {
        size_t cut = maxChars > 3 ? (maxChars - 3) : 0;
        // Do not split a UTF-8 continuation byte at the preview boundary.
        while (cut > 0 &&
               (static_cast<unsigned char>(compact[cut]) & 0xC0u) == 0x80u) {
            --cut;
        }
        compact = compact.substr(0, cut) + "...";
    }
    return compact;
}

} // namespace detail

class ReminderStore {
public:
    ReminderToolResult CreateFromArgs(const std::string& args)
    {
        std::string schedule;
        std::string message;
        if (!detail::SplitCreateArgs(args, schedule, message)) {
            return Error("reminder_create requires a schedule on the first line and the reminder message on following line(s). Slash form may use: in 2 hours | message");
        }
        if (message.size() > 4096) {
            return Error("Reminder message is too long (maximum 4096 UTF-8 bytes).");
        }

        const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        std::int64_t due = 0;
        std::string parseError;
        if (!detail::ParseSchedule(schedule, now, due, parseError)) {
            return Error(parseError);
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!EnsureLoadedLocked()) return LoadErrorResultLocked();

        ReminderRecord rec;
        rec.id       = MakeIdLocked();
        rec.dueEpoch = due;
        rec.dueLocal = detail::FormatLocal(due);
        rec.message  = message;
        m_items.push_back(rec);

        if (!SaveLocked()) {
            m_items.pop_back();
            return Error("Could not save the reminder store at " + detail::StoragePathUtf8());
        }

        ReminderToolResult out;
        out.chips = { "created", rec.dueLocal };
        out.bodyLang = "markdown";
        std::ostringstream body;
        body << "Reminder created.\n\n"
             << "- ID: `" << rec.id << "`\n"
             << "- Due: " << rec.dueLocal << "\n"
             << "- Message: " << rec.message << "\n"
             << "- Delivery: in-app while LlamaBoss is running; if it was closed at the due time, the overdue reminder appears after the next launch.";
        out.body = body.str();
        return out;
    }

    ReminderToolResult ListPending()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!EnsureLoadedLocked()) return LoadErrorResultLocked();

        std::vector<ReminderRecord> pending = m_items;
        std::sort(pending.begin(), pending.end(),
            [](const ReminderRecord& a, const ReminderRecord& b) {
                if (a.dueEpoch != b.dueEpoch) return a.dueEpoch < b.dueEpoch;
                return a.id < b.id;
            });

        ReminderToolResult out;
        out.bodyLang = "markdown";
        if (pending.empty()) {
            out.chips = { "no reminders" };
            out.body = "No pending reminders.";
            return out;
        }

        out.chips = { std::to_string(pending.size()) + " pending" };
        std::ostringstream body;
        body << "Pending reminders:\n";
        for (const auto& r : pending) {
            body << "- `" << r.id << "` — " << r.dueLocal
                 << " — " << detail::OneLinePreview(r.message) << "\n";
        }
        out.body = body.str();
        return out;
    }

    ReminderToolResult Cancel(const std::string& idRaw)
    {
        const std::string id = detail::Trim(idRaw);
        if (id.empty()) return Error("reminder_cancel requires a reminder ID.");

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!EnsureLoadedLocked()) return LoadErrorResultLocked();

        const auto it = std::find_if(m_items.begin(), m_items.end(),
            [&](const ReminderRecord& r) { return r.id == id; });
        if (it == m_items.end()) {
            return Error("Reminder not found: " + id);
        }

        const ReminderRecord removed = *it;
        m_items.erase(it);
        m_claimed.erase(id);
        if (!SaveLocked()) {
            m_items.push_back(removed);
            return Error("Could not save the reminder store after cancellation.");
        }

        ReminderToolResult out;
        out.chips = { "cancelled" };
        out.body = "Cancelled reminder `" + id + "`.";
        out.bodyLang = "markdown";
        return out;
    }

    // Called by each MyFrame timer.  Claimed IDs live only in memory so two
    // windows cannot show the same reminder, while a process crash still leaves
    // it pending on disk for the next launch.
    std::vector<ReminderRecord> ClaimDue(std::int64_t nowEpoch = 0)
    {
        if (nowEpoch <= 0) nowEpoch = static_cast<std::int64_t>(std::time(nullptr));

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!EnsureLoadedLocked() || m_presentationActive) return {};

        // Present one reminder per timer turn. This keeps the UI responsive,
        // avoids a stack of modal dialogs when several reminders are overdue,
        // and ensures we never claim reminders we have not yet shown.
        const ReminderRecord* earliest = nullptr;
        for (const auto& r : m_items) {
            if (r.dueEpoch > nowEpoch || m_claimed.count(r.id) != 0) continue;
            if (!earliest || r.dueEpoch < earliest->dueEpoch ||
                (r.dueEpoch == earliest->dueEpoch && r.id < earliest->id)) {
                earliest = &r;
            }
        }

        std::vector<ReminderRecord> due;
        if (earliest) {
            m_claimed.insert(earliest->id);
            due.push_back(*earliest);
            m_presentationActive = true;
        }
        return due;
    }

    // Releases the process-wide presentation gate after the UI has handled
    // the batch returned by ClaimDue(). This prevents a second LlamaBoss
    // window (or a nested modal event loop) from stacking reminder dialogs.
    void EndPresentation()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_presentationActive = false;
    }

    bool Complete(const std::string& id)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!EnsureLoadedLocked()) return false;

        const auto it = std::find_if(m_items.begin(), m_items.end(),
            [&](const ReminderRecord& r) { return r.id == id; });
        if (it == m_items.end()) {
            m_claimed.erase(id);
            return true; // already removed/cancelled
        }

        const ReminderRecord removed = *it;
        m_items.erase(it);
        m_claimed.erase(id);
        if (SaveLocked()) return true;

        m_items.push_back(removed);
        m_claimed.insert(id);
        return false;
    }

    bool Snooze(const std::string& id, std::int64_t seconds)
    {
        if (seconds < 1) return false;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!EnsureLoadedLocked()) return false;

        const auto it = std::find_if(m_items.begin(), m_items.end(),
            [&](const ReminderRecord& r) { return r.id == id; });
        if (it == m_items.end()) {
            m_claimed.erase(id);
            return false;
        }

        const std::int64_t oldDue = it->dueEpoch;
        const std::string oldLocal = it->dueLocal;
        it->dueEpoch = static_cast<std::int64_t>(std::time(nullptr)) + seconds;
        it->dueLocal = detail::FormatLocal(it->dueEpoch);
        m_claimed.erase(id);

        if (SaveLocked()) return true;

        it->dueEpoch = oldDue;
        it->dueLocal = oldLocal;
        m_claimed.insert(id);
        return false;
    }

    std::string StoragePath() const
    {
        return detail::StoragePathUtf8();
    }

private:
    static ReminderToolResult Error(const std::string& message)
    {
        ReminderToolResult out;
        out.chips = { "error" };
        out.errorBody = message;
        return out;
    }

    bool EnsureLoadedLocked()
    {
        if (m_loaded) return m_loadError.empty();
        m_loaded = true;
        m_items.clear();
        m_loadError.clear();

        const wxString path = detail::StoragePathWx();
        if (!wxFileExists(path)) return true;

        const std::string body = detail::ReadWholeFile(path);
        if (body.empty()) return true;

        try {
            Poco::JSON::Parser parser;
            const auto var = parser.parse(body);
            Poco::JSON::Object::Ptr root = var.extract<Poco::JSON::Object::Ptr>();
            if (!root || !root->has("reminders")) {
                m_loadError = "REMINDERS.json is missing its reminders array.";
                return false;
            }

            Poco::JSON::Array::Ptr arr = root->getArray("reminders");
            if (!arr) {
                m_loadError = "REMINDERS.json has an invalid reminders array.";
                return false;
            }

            for (size_t i = 0; i < arr->size(); ++i) {
                try {
                    Poco::JSON::Object::Ptr obj = arr->getObject(i);
                    if (!obj) continue;

                    ReminderRecord r;
                    r.id       = obj->getValue<std::string>("id");
                    r.dueEpoch = static_cast<std::int64_t>(
                        obj->getValue<Poco::Int64>("due_epoch"));
                    r.message  = obj->getValue<std::string>("message");
                    if (obj->has("due_local"))
                        r.dueLocal = obj->getValue<std::string>("due_local");
                    if (r.dueLocal.empty()) r.dueLocal = detail::FormatLocal(r.dueEpoch);

                    if (!r.id.empty() && r.dueEpoch > 0 && !r.message.empty())
                        m_items.push_back(std::move(r));
                }
                catch (...) {
                    // Skip one malformed record rather than losing every valid
                    // reminder in an otherwise readable file.
                }
            }
            return true;
        }
        catch (const std::exception& e) {
            // Do not overwrite a corrupted reminder file on the next create or
            // cancel. The user can repair/remove it and restart LlamaBoss.
            m_items.clear();
            m_loadError = std::string("Could not parse REMINDERS.json: ") + e.what();
            return false;
        }
        catch (...) {
            m_items.clear();
            m_loadError = "Could not parse REMINDERS.json.";
            return false;
        }
    }

    ReminderToolResult LoadErrorResultLocked() const
    {
        return Error(m_loadError + " The file was left unchanged: " +
                     detail::StoragePathUtf8());
    }

    bool SaveLocked()
    {
        if (!m_loadError.empty()) return false;

        Poco::JSON::Object::Ptr root = new Poco::JSON::Object(true);
        root->set("version", 1);

        Poco::JSON::Array::Ptr arr = new Poco::JSON::Array;
        for (const auto& r : m_items) {
            Poco::JSON::Object::Ptr obj = new Poco::JSON::Object(true);
            obj->set("id", r.id);
            obj->set("due_epoch", static_cast<Poco::Int64>(r.dueEpoch));
            obj->set("due_local", r.dueLocal);
            obj->set("message", r.message);
            arr->add(obj);
        }
        root->set("reminders", arr);

        std::ostringstream body;
        Poco::JSON::Stringifier::stringify(root, body, 2);
        return detail::WriteWholeFileAtomic(detail::StoragePathWx(), body.str());
    }

    std::string MakeIdLocked()
    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        for (;;) {
            std::ostringstream id;
            id << "rem_" << ms << "_" << (++m_sequence);
            const std::string candidate = id.str();
            const bool exists = std::any_of(m_items.begin(), m_items.end(),
                [&](const ReminderRecord& r) { return r.id == candidate; });
            if (!exists) return candidate;
        }
    }

    std::mutex m_mutex;
    bool m_loaded = false;
    bool m_presentationActive = false;
    std::string m_loadError;
    std::vector<ReminderRecord> m_items;
    std::unordered_set<std::string> m_claimed;
    std::uint64_t m_sequence = 0;
};

inline ReminderStore& GetReminderStore()
{
    static ReminderStore store;
    return store;
}

} // namespace lb_reminders
