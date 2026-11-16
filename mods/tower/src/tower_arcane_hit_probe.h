#pragma once
namespace towerarcanehit {
struct Hit { void* ability=nullptr; int index=-1; void* target=nullptr; int targetIndex=-1; unsigned long long time=0; };
bool Start(void* player);
bool Pop(Hit* out);
void Stop();
}
