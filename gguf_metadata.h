// gguf_metadata.h
#pragma once

#include <string>
#include <cstdint>

// ── Minimal GGUF metadata sniffer ───────────────────────────────
// Reads only the key/value metadata region of a GGUF file (header +
// KV pairs, no tensor data).  Purpose-built for launch-time feature
// detection, not general metadata access.
//
// Failure philosophy: this runs on the server-launch hot path, so
// every failure mode (missing file, truncated header, unknown GGUF
// version, corrupt lengths) degrades to "feature not detected" —
// never an exception, never a blocked launch.

// Returns the value of `{arch}.nextn_predict_layers` from the GGUF
// metadata, or 0 if the key is absent, the file is unreadable, or
// the header is malformed.  A value > 0 means the metadata advertises
// embedded multi-token-prediction (MTP) layers.  This makes the model
// a candidate for `--spec-type draft-mtp`; successful llama-server
// startup remains the final compatibility check.
//
// `ggufPathUtf8` is a UTF-8 path (pass wxString::ToUTF8().data());
// on Windows it is widened internally so non-ASCII model folders
// work, matching the CreateProcessW conventions elsewhere.
int GgufNextnPredictLayers(const std::string& ggufPathUtf8);
