#pragma once
// QA only: reversible native registration, never offered as a finished Hex.
namespace towerarcane {
bool Start(void* player);
bool Tick(void* player);
bool SetCountCandidate(bool enabled);
void ResetWave();
void Stop();
}
