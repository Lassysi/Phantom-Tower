// QA-only readback of named arena actors and the player's current standing point.
#ifdef PBZ_HEX_TEST
#include "tower_venue.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include "../../../src/core/host/log.h"
#include "../../../src/core/ipc/pipe.h"
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/services/process_event.h"
#include "../../../src/core/services/ue.h"
#include "tower_entry.h"
#include "tower_isolation.h"
#include "tower_perf.h"
#include "tower_reach.h"
#include "tower_widgets.h"

namespace towervenue {
namespace ue = pbz::ue;
namespace pe = pbz::pe;
using ue::UObject;
using tower::arena::P3;
namespace {
struct Sample {
    int cursor = 0;
    std::vector<UObject*> matches;
    std::vector<std::string> names;
    std::string world, label;
    UObject* pawn = nullptr;
    std::array<double, 4> at{};
    uint64_t deadline = 0;
};
std::optional<Sample> g_sample;

UObject* Fn(UObject* object, const char* name) {
    return object && ue::Live(object) ? pe::FindFunction(ue::ClassOf(object), name) : nullptr;
}
bool ScanStep(Sample& sample) {
    const auto* spawner = towerperf::ClassNamed("BP_AISpawner_C");
    const auto* door = towerperf::ClassNamed("BP_FogDoor_C");
    constexpr int batchSize = 8192;
    static UObject* batch[batchSize];
    const int count = ue::ObjBatch(sample.cursor, batchSize, batch);
    for (int i = 0; i < count; ++i) {
        auto* actor = batch[i];
        const auto* cls = actor ? ue::ClassOf(actor) : nullptr;
        if (!cls || (cls != spawner && cls != door)) continue;
        if (std::find(sample.names.begin(), sample.names.end(), ue::ObjName(actor)) != sample.names.end())
            sample.matches.push_back(actor);
    }
    sample.cursor += count ? count : batchSize;
    return sample.cursor >= ue::Num();
}


std::string SampleEvent(std::string_view fields) {
    return std::format(R"({{"op":"tower","event":"qa_venue_sample",{}}})", fields);
}
void SampleError(std::string_view message) {
    pbz::ipc::Send(SampleEvent(std::format(R"("ok":false,"error":"{}")", message)));
}
bool Finite(const P3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
std::string Point(const P3& p) { return std::format("[{:.3f},{:.3f},{:.3f}]", p.x, p.y, p.z); }
bool Fits(UObject* fn, int at, int bytes) {
    const int size = ue::Get<int>(fn + ue::Layout::StructSize);
    return at >= 0 && at + bytes <= size && size <= 128;
}
bool Loc(UObject* actor, P3* out) {
    auto* fn = Fn(actor, "K2_GetActorLocation");
    if (!fn) return false;
    const int at = ue::PropOffset(fn, "ReturnValue");
    if (!Fits(fn, at, 24)) return false;
    uint8_t args[128]{};
    pe::Call(actor, fn, args);
    double v[3]; std::memcpy(v, args + at, 24);
    *out = {v[0], v[1], v[2]};
    return Finite(*out);
}
std::string DoorSpline(UObject* actor) {
    const int offset = ue::PropOffset(ue::ClassOf(actor), "Spline");
    auto* spline = offset >= 0 ? ue::Get<UObject*>(actor + offset) : nullptr;
    auto* splineClass = towerperf::ClassNamed("SplineComponent");
    if (!spline || !splineClass || !ue::Live(spline) || !ue::IsA(spline, splineClass) ||
        ue::Get<UObject*>(spline + ue::Layout::ObjOuter) != actor) return "null";
    auto* count = Fn(spline, "GetNumberOfSplinePoints");
    auto* location = Fn(spline, "GetLocationAtSplinePoint");
    if (!count || !location) return "null";
    const int nAt = ue::PropOffset(count, "ReturnValue");
    const int indexAt = ue::PropOffset(location, "PointIndex"), spaceAt = ue::PropOffset(location, "CoordinateSpace");
    const int pointAt = ue::PropOffset(location, "ReturnValue");
    if (!Fits(count, nAt, 4) || !Fits(location, indexAt, 4) || !Fits(location, spaceAt, 1) || !Fits(location, pointAt, 24)) return "null";
    uint8_t args[128]{};
    pe::Call(spline, count, args);
    int n = 0;
    std::memcpy(&n, args + nAt, 4);
    if (n < 2 || n > 96) return "null";
    std::string points = "[";
    for (int i = 0; i < n; ++i) {
        std::memset(args, 0, sizeof(args));
        std::memcpy(args + indexAt, &i, 4);
        args[spaceAt] = 1;  // ESplineCoordinateSpace::World
        pe::Call(spline, location, args);
        double v[3]; std::memcpy(v, args + pointAt, 24);
        const P3 p{v[0], v[1], v[2]};
        if (!Finite(p)) return "null";
        if (i) points += ',';
        points += Point(p);
    }
    return points + ']';
}
std::string SampleObject(const std::string& name, const Sample& sample) {
    const bool door = name.starts_with("BP_FogDoor_C_");
    UObject* match = nullptr;
    for (auto* actor : sample.matches) {
        if (!ue::Live(actor) || ue::ObjName(actor) != name || !towerisolate::InPlayerWorld(actor)) continue;
        if (match) return "null";  // Ignore duplicate names in streamed instances.
        match = actor;
    }
    P3 p;
    if (!match || !Loc(match, &p) || !Finite(p)) return "null";
    return std::format(R"({{"class":"{}","position":{},"spline_world":{}}})", pbz::ipc::Esc(ue::ClassName(match)), Point(p), door ? DoorSpline(match) : "null");
}
}  // namespace

void BeginSample(const std::string& line) {
    if (g_sample) { SampleError("capture_in_progress"); return; }
    Sample next;
    next.label = pbz::ipc::Field(line, "label").value_or("");
    const auto expected = pbz::ipc::Field(line, "world").value_or("");
    const auto names = pbz::ipc::Field(line, "actors").value_or("");
    if (next.label.size() > 96 || names.size() > 2048) { SampleError("invalid_request"); return; }
    std::string_view remaining = names;
    while (!remaining.empty()) {
        const size_t end = remaining.find(',');
        const auto name = remaining.substr(0, end);
        if (name.size() > 128 || next.names.size() >= 16 ||
            !(name.starts_with("BP_AISpawner_C_") || name.starts_with("BP_FogDoor_C_"))) {
            SampleError("invalid_actor_name"); return;
        }
        next.names.emplace_back(name);
        if (end == std::string_view::npos) break;
        remaining.remove_prefix(end + 1);
    }
    next.pawn = towerwidgets::Pawn();
    if (!next.pawn || !towerentry::LoadingClear() || !pbz::PlayerWhere(next.at.data(), &next.world)) {
        SampleError("player_or_level_not_ready"); return;
    }
    if (!expected.empty() && expected != next.world) { SampleError("different_world"); return; }
    next.deadline = GetTickCount64() + 10000;
    g_sample = std::move(next);
}

void SampleTick() {
    if (!g_sample) return;
    auto& s = *g_sample;
    if (GetTickCount64() > s.deadline) { SampleError("scan_timeout"); g_sample.reset(); return; }
    if (!s.names.empty() && !ScanStep(s)) return;
    std::array<double, 4> at{};
    std::string world;
    if (towerwidgets::Pawn() != s.pawn || !towerentry::LoadingClear() || !pbz::PlayerWhere(at.data(), &world) || world != s.world ||
        std::hypot(at[0] - s.at[0], at[1] - s.at[1], at[2] - s.at[2]) > 100.0 ||
        !Finite({at[0], at[1], at[2]}) || !std::isfinite(at[3])) {
        SampleError("player_moved_or_changed_world"); g_sample.reset(); return;
    }
    std::string objects;
    for (const auto& name : s.names) {
        if (!objects.empty()) objects += ',';
        objects += std::format(R"("{}":{})", pbz::ipc::Esc(name), SampleObject(name, s));
    }
    const auto event = SampleEvent(std::format(R"("ok":true,"label":"{}","world":"{}","player":{},"yaw":{:.3f},"standing":{},"actors":{{{}}})",
        pbz::ipc::Esc(s.label), pbz::ipc::Esc(world), Point({at[0], at[1], at[2]}), at[3], towerreach::StandingAt(s.pawn, at.data()), objects));
    pbz::ipc::Send(event);
    pbz::log::Info("影塔场地采集：{}", event);
    g_sample.reset();
}
}  // namespace towervenue
#endif
