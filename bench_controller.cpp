// bench_controller.cpp -- see bench_controller.h.

#include "bench_controller.h"

#include "chat_client.h"
#include "path_safety.h"
#include "ui_event_post.h"

#include <wx/datetime.h>

BenchController::BenchController(Callbacks callbacks)
    : m_cb(std::move(callbacks))
    , m_alive(std::make_shared<std::atomic<bool>>(true))
{
    m_client = std::make_unique<ChatClient>(this, m_alive);
    Bind(wxEVT_ASSISTANT_COMPLETE, &BenchController::OnComplete, this);
    Bind(wxEVT_ASSISTANT_ERROR,    &BenchController::OnError,    this);
    // Deltas are the generated story text -- not needed, just drained.
    Bind(wxEVT_ASSISTANT_DELTA, [](wxCommandEvent&) {});
}

BenchController::~BenchController()
{
    // Worker threads check this token before queueing events to us.
    // Must go through LbMarkUiEventTargetDead: it takes the same mutex
    // LbQueueEventIfAlive holds across check-and-queue, so this blocks
    // until any in-flight post finishes and no later post can pass the
    // check.  A bare store(false) let a worker that had already passed
    // the check call wxQueueEvent on this handler mid-destruction.
    LbMarkUiEventTargetDead(m_alive);
    if (m_client) m_client->StopGeneration();
}

bool BenchController::Start(const Setup& setup, std::string& error)
{
    if (m_running) { error = "A benchmark is already running (/bench stop to cancel)."; return false; }

    m_setup   = setup;
    m_runs.clear();
    m_prompt  = bench::BuildPrompt(m_setup.options.longPrompt);
    m_startedAt = std::string(wxDateTime::Now().Format("%Y-%m-%d_%H%M%S").ToUTF8().data());

    // One TSV per benchmark: <date>_<time>_<model>.tsv.  File-safe model name.
    std::string safe;
    for (char c : m_setup.label) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        safe.push_back(ok ? c : '-');
    }
    if (safe.size() > 60) safe.resize(60);
    m_tsvPath.clear();
    if (!m_setup.outDir.empty()) {
        wxFileName::Mkdir(wxString::FromUTF8(m_setup.outDir), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        m_tsvPath = m_setup.outDir + std::string(1, static_cast<char>(wxFILE_SEP_PATH)) + m_startedAt + "_" + (safe.empty() ? "model" : safe) + ".tsv";
    }

    m_running = true;
    if (!SendNext(error)) {
        m_running = false;
        return false;
    }
    return true;
}

std::string BenchController::BuildBody(bool forceCold) const
{
    // Same request shaping as a normal chat turn for this target.
    ChatHistory scratch;
    scratch.SetActiveReasoningDialect(m_setup.target.reasoningDialect);
    scratch.SetActiveResponsesApi(m_setup.target.responsesApi);
    scratch.SetThinkOverride(m_setup.think);
    scratch.AddUserMessage(m_prompt);
    std::string body = scratch.BuildChatRequestJson(
        m_setup.bodyModel, /*stream*/ true, /*systemPrompt*/ std::string(),
        /*contextTokens*/ 0, /*tools*/ std::string(), /*native*/ false,
        /*agentSampling*/ false, /*imageOutput*/ false);

    // Remote providers validate fields strictly; send them exactly what a
    // normal chat request would.  Their output length is whatever the
    // model chooses for the ~150-word ask.
    if (!m_setup.target.managed) return body;

    // Local llama-server: make runs repeatable.
    try {
        Poco::JSON::Parser parser;
        auto root = parser.parse(body).extract<Poco::JSON::Object::Ptr>();
        root->set("max_tokens",   m_setup.options.genTokens);
        root->set("ignore_eos",   true);   // always exactly genTokens
        root->set("temperature",  0.0);
        root->set("seed",         42);
        root->set("cache_prompt", !forceCold);
        std::ostringstream out;
        Poco::JSON::Stringifier::stringify(root, out);
        return out.str();
    } catch (...) {
        return body;   // unpatched is still a valid request
    }
}

bool BenchController::SendNext(std::string& error)
{
    const int index = static_cast<int>(m_runs.size()) + 1;
    // Run 1 is always a guaranteed-cold measurement on local models.
    const bool forceCold = m_setup.options.allCold || index == 1;
    ++m_genId;
    if (!m_client->SendMessage(m_setup.target, BuildBody(forceCold), m_genId)) {
        error = "Could not start benchmark request.";
        return false;
    }
    return true;
}

void BenchController::OnComplete(wxCommandEvent& e)
{
    // Take ownership of the payload first (see AssistantCompletePayload).
    std::unique_ptr<wxClientData> owner(e.GetClientObject());
    e.SetClientObject(nullptr);
    if (!m_running || static_cast<unsigned long>(e.GetExtraLong()) != m_genId) return;
    m_client->ResetStreamingState();

    bench::Run run;
    run.index = static_cast<int>(m_runs.size()) + 1;
    if (auto* p = dynamic_cast<AssistantCompletePayload*>(owner.get()))
        run.stats = p->Stats();
    run.cache = bench::ClassifyCache(run.stats);
    m_runs.push_back(run);
    AppendTsv(run);
    if (m_cb.progress) m_cb.progress(bench::RunLine(run, m_setup.options.runs));

    if (static_cast<int>(m_runs.size()) >= m_setup.options.runs) {
        Finish(std::string());
        return;
    }
    std::string error;
    if (!SendNext(error)) Finish(error);
}

void BenchController::OnError(wxCommandEvent& e)
{
    if (!m_running || static_cast<unsigned long>(e.GetExtraLong()) != m_genId) return;
    m_client->ResetStreamingState();
    Finish("Stopped: " + std::string(e.GetString().ToUTF8().data()));
}

void BenchController::Stop()
{
    if (!m_running) return;
    ++m_genId;                       // ignore anything the cancelled run still posts
    m_client->StopGeneration();
    Finish("Stopped by user.");
}

void BenchController::Finish(const std::string& note)
{
    if (!m_running) return;
    m_running = false;

    std::string text;
    if (!m_runs.empty()) {
        std::string saved = m_tsvPath;
        if (!saved.empty() && !wxFileExists(wxString::FromUTF8(saved))) saved.clear();
        text = bench::Summary(m_runs, m_setup.label, !m_setup.target.managed, saved);
    }
    if (!note.empty()) text = text.empty() ? note : text + "\n" + note;
    if (text.empty()) text = "Benchmark finished with no completed runs.";
    if (m_cb.finished) m_cb.finished(text);
}

void BenchController::AppendTsv(const bench::Run& run)
{
    if (m_tsvPath.empty()) return;
    try {
        const std::filesystem::path p(path_safety::Utf8ToWide(m_tsvPath));
        const bool fresh = !std::filesystem::exists(p);
        std::ofstream f(p, std::ios::app);
        if (!f) return;
        if (fresh) f << bench::TsvHeader() << '\n';
        f << bench::TsvRow(run, m_setup.options.longPrompt,
                           wxDateTime::Now().FormatISOCombined(' ').ToStdString(),
                           m_setup.label)
          << '\n';
    } catch (...) {
        // best-effort
    }
}
