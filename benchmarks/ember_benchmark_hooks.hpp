#pragma once

// Private instrumentation contract, only enabled in the benchmark copy of ember.
// No GL resources, public API, or profiling branches in the production library.
namespace ember::benchmark {
enum class Stage {
    Frame, Update, Render, Integrate, Spawn, BuildLive, Readback,
    Sort, Draw, BloomPrepare, BloomDraw, BloomPost, Refraction, Count
};
inline constexpr const char* stageNames[] = {
    "frame", "update", "render", "integrate", "spawn", "build_live", "readback",
    "sort", "draw", "bloom_prepare", "bloom_draw", "bloom_post", "refraction"
};
void beginStage(Stage stage);
void endStage(Stage stage);
}
