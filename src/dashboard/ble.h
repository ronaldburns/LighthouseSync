// LighthouseSync dashboard: Lighthouse 2.0 base stations' power over Bluetooth LE (Windows). A background thread
// watches for their advertisements ("LHB-XXXXXXXX") and reads or writes their power characteristic.
#pragma once
#include <string>
#include <vector>

#include "model.h"

namespace lhs::ble {

void Start();
void Stop();
void Scan();  // forget what's been seen and listen again
// cmd: PowerCmd. name "": every station found, and the known ones as they turn up
void Power(const std::string &name, int cmd);
// the adapter's state (Model::ble), whether it's listening, and the stations found
void Fill(int &state, bool &scanning, std::vector<StationPower> &out);
// for SteamVR quitting: sends cmd to every station found or known and waits up to timeout_s for the writes
void PowerAllAndWait(int cmd, const std::vector<std::string> &known, double timeout_s);

}  // namespace lhs::ble
