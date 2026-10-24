// LighthouseSync dashboard: updates from GitHub releases (Windows). Checks <repo>/releases/latest, downloads the
// SteamVR zip and SHA256SUMS, checks the zip against it and swaps the driver's files in place: files SteamVR has open
// are renamed to *.lhs-old (the driver deletes those at its next start), so the update applies at the next SteamVR start.
#pragma once
#include <string>

#include "model.h"

namespace lhs::upd {

// driver_root: the folder holding driver.vrdrivermanifest (the one SteamVR loads); data_dir: %LOCALAPPDATA%\QuestLHSync
void Start(const std::wstring &driver_root, const std::wstring &data_dir);
void Check(const std::string &repo);  // in the background
void Install();                       // the release the last check found
void Fill(UpdateInfo &u);
std::string Installed();              // the version installed.json records, else LHS_VERSION

}  // namespace lhs::upd
