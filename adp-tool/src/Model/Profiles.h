#pragma once

#include <string>
#include <nlohmann/json.hpp>

// Profile storage: each profile is one JSON file (<id>.json) in a directory,
// typically a shared network folder so every venue machine sees the same set.
// Pure filesystem I/O, no device/HID access, so it is unit-testable on its own.
//
// `id` is the filename stem and the trust boundary: it comes from untrusted
// WebSocket clients and is turned into a path, so every function validates it
// with ValidId() and refuses anything outside [a-z0-9-] (no traversal).
namespace adp::Profiles
{
using json = nlohmann::json;

// True for a safe filename stem: non-empty, only lowercase letters/digits/'-'.
bool ValidId(const std::string& id);

// Turns an arbitrary display name into a valid id (never empty).
std::string SlugifyId(const std::string& name);

// True if <dir>/<id>.json exists (false for an invalid id).
bool Exists(const std::string& dir, const std::string& id);

// Every stored profile as a JSON array (unparseable files are skipped).
json List(const std::string& dir);

// Reads one profile into `out`; false if missing, invalid id, or bad JSON.
bool Read(const std::string& dir, const std::string& id, json& out);

// Atomically writes (temp file + rename); false on invalid id or I/O error.
bool Write(const std::string& dir, const std::string& id, const json& profile);

// Deletes one profile; false on invalid id. True even if it was already gone.
bool Delete(const std::string& dir, const std::string& id);
} // namespace adp::Profiles
