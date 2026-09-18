#include "common.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace ember::detail::opengl {
void OpenGLBackend::update(const SimulationParameters& p,const UpdateBatch& batch) {
    const auto& reqs=batch.requests;
    const auto total=batch.total;
    const auto dt=batch.dt;
    const std::uint32_t nr = (std::uint32_t)reqs.size();
    if (nr > 0) {
        const auto bytes=(GLsizeiptr)nr * (GLsizeiptr)sizeof(SpawnRequest);
        // Rename request storage to avoid overwriting a previous frame's input.
        if (gpuDriven_) spawnBuf_.data(reqs.data(),bytes,GL_STREAM_DRAW);
        else spawnBuf_.subData(0,bytes,reqs.data());
    }
    if (gpuDriven_) scheduleGpu(0,nr);
    else {
        counterBuf_.subData(2 * sizeof(GLuint), sizeof(GLuint), &nr);
        const GLuint drawArgs[4] = {4, 0, 0, 0};
        indirectBuf_.subData(0, sizeof(drawArgs), drawArgs);
        const GLuint queueZero[4] = {0, 0, 0, 0}; // event queue header reset (GPU mode: schedule.comp)
        eventQueueBuf_.subData(0, sizeof(queueZero), queueZero);
    }

    // 2) Run the simulation.
    if (!simProg_) ensurePrograms();
    const bool livePipeline = simProg_.hasUniform("uInputAlive");
    glUseProgram(simProg_.id());
    // (Shader::set* caches uniform locations; -1 locations are no-ops)
    simProg_.setFloat("uDt", dt);
    simProg_.setFloat("uTime", batch.time);
    simProg_.setUint("uForceMask", p.forceMask);
    simProg_.setVec3("uGravity", p.gravity);
    simProg_.setFloat("uDrag", p.drag);
    simProg_.setInt("uDragMode", (int)p.dragMode);
    simProg_.setVec3("uWind", p.wind);
    simProg_.setFloat("uTurbulence", p.turbulence);
    simProg_.setInt("uAttractorCount", (int)p.attractors.size());
    simProg_.setInt("uVortexCount", (int)p.vortexes.size());
    simProg_.setInt("uSpringCount", (int)p.springs.size());
    simProg_.setVec3("uNoiseWindDir", p.noiseWindDir);
    simProg_.setFloat("uNoiseWindAmp", p.noiseWindAmp);
    simProg_.setFloat("uNoiseWindScale", p.noiseWindScale);
    simProg_.setFloat("uNoiseWindSpeed", p.noiseWindSpeed);
    simProg_.setVec3("uWaveDir", p.waveDir);
    simProg_.setVec3("uWaveK", p.waveK);
    simProg_.setFloat("uWaveAmp", p.waveAmp);
    simProg_.setFloat("uWaveOmega", p.waveOmega);
    simProg_.setInt("uBoundaryMode", p.boundaryMode == BoundaryMode::Kill ? 1 : 2);
    simProg_.setFloat("uBoundaryY", p.boundaryY);
    simProg_.setFloat("uRestitution", p.restitution);
    simProg_.setUint("uSpawnTotal", total);
    simProg_.setUint("uFrameSeed", batch.seed);
    simProg_.setUint("uInputAlive", alive_);
    simProg_.setInt("uGpuDriven",gpuDriven_?1:0);

    // Built-in shaders read one std140 block instead of the loose uniforms
    // above; both are written so custom shaders keep their legacy contract.
    SimParams sp{};
    sp.gravity = p.gravity;         sp.drag = p.drag;
    sp.wind = p.wind;               sp.turbulence = p.turbulence;
    sp.noiseWindDir = p.noiseWindDir; sp.noiseWindAmp = p.noiseWindAmp;
    sp.noiseWindScale = p.noiseWindScale; sp.noiseWindSpeed = p.noiseWindSpeed;
    sp.waveAmp = p.waveAmp;         sp.waveOmega = p.waveOmega;
    sp.waveDir = p.waveDir;         sp.dt = dt;
    sp.waveK = p.waveK;             sp.time = batch.time;
    sp.forceMask = p.forceMask;     sp.dragMode = (int)p.dragMode;
    sp.attractorCount = (std::int32_t)p.attractors.size();
    sp.vortexCount = (std::int32_t)p.vortexes.size();
    sp.springCount = (std::int32_t)p.springs.size();
    sp.boundaryMode = p.boundaryMode == BoundaryMode::Kill ? 1 : 2;
    sp.boundaryY = p.boundaryY;     sp.restitution = p.restitution;
    sp.spawnTotal = total;          sp.frameSeed = batch.seed;
    sp.inputAlive = alive_;         sp.gpuDriven = gpuDriven_ ? 1 : 0;
    sp.eventTemplates = eventTemplateCount_;
    glBindBufferBase(GL_UNIFORM_BUFFER, kBindingUboSim, uboSim_.id());
    const auto uploadPhase = [&](int phase) {
        sp.phase = phase;
        uboSim_.subData(0, sizeof(sp), &sp);
    };

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingCur, cur_->id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingNext, nxt_->id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingSpawn, spawnBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingDead, deadBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingCounters, counterBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingAttractors, attractorBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingVortexes, vortexBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingSprings, springBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingPalette, paletteBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingLive, liveBuf_.id());
    if (livePipeline) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingScratch, nextLiveBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingIndirect, indirectBuf_.id()); // phase 2 writes draw args
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingEventTags, eventTagBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingEventTemplates, eventTemplateBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingEventQueue, eventQueueBuf_.id());

    EMBER_BENCH_BEGIN(Integrate);
    simProg_.setInt("uPhase", 0); // simulate
    uploadPhase(0);
    const auto integrationCount = livePipeline ? alive_ : allocated_;
    if (gpuDriven_) {
        glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER,dispatchBuf_.id());
        glDispatchComputeIndirect(2*sizeof(GLuint));
    } else if (integrationCount > 0) {
        glDispatchCompute((integrationCount + kSimGroupSize - 1u) / kSimGroupSize, 1, 1);
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    EMBER_BENCH_END(Integrate);
    EMBER_BENCH_BEGIN(Spawn);
    if (eventTemplateCount_ > 0) {
        // phase 4: serial event compression + child slot prefix sum.
        simProg_.setInt("uPhase", 4);
        uploadPhase(4);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }
    if (livePipeline && (total > 0 || eventTemplateCount_ > 0)) {
        simProg_.setInt("uPhase", 3); // reserve this entire birth batch once
        uploadPhase(3);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }
    simProg_.setInt("uPhase", 1); // spawn (GPU samples particles from requests)
    uploadPhase(1);
    if (total > 0 || eventTemplateCount_ > 0) {
        // Only the accepted prefix can write a particle. Bound the launch by
        // capacity in GPU mode so indirect-limit validation also covers births.
        // Active event templates may add up to the per-frame child cap; the
        // shader drops the excess (j >= uSpawnAccepted) without a readback.
        const std::uint32_t eventExtra = eventTemplateCount_ ? kMaxEventChildren : 0u;
        const auto spawnCount = gpuDriven_ ? std::min(total + eventExtra, capacity_)
                                           : (total + eventExtra);
        glDispatchCompute((spawnCount + kSimGroupSize - 1u) / kSimGroupSize, 1, 1);
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    EMBER_BENCH_END(Spawn);
    EMBER_BENCH_BEGIN(BuildLive);
    // Publish draw args; the optimized pipeline already built the next live
    // list during integration/spawning. Legacy shaders scan the extent here.
    simProg_.setInt("uPhase", 2);
    uploadPhase(2);
    const auto extent = (std::uint32_t)std::min<std::uint64_t>(capacity_, (std::uint64_t)allocated_ + total);
    if (livePipeline) glDispatchCompute(1, 1, 1);
    else if (extent) glDispatchCompute((extent + kSimGroupSize - 1u) / kSimGroupSize, 1, 1);
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    if (gpuDriven_ && sortEnabled_) scheduleGpu(1);
    EMBER_BENCH_END(BuildLive);

    // 3) Publish CPU statistics. GPU scheduling never depends on this snapshot.
    EMBER_BENCH_BEGIN(Readback);
    ++frameCount_;
    countsFresh_=false;
    if (gpuDriven_) enqueueStatistics();
    else refreshCounts();
    EMBER_BENCH_END(Readback);

    // 4) Debug diagnostics (EMBER_DEBUG=1 / config debug=true): ~10 lines/sec + GL error sweep.
    if (debug_ && !gpuDriven_ && (frameCount_ % 6 == 0)) {
        GLuint ctr[4] = {0, 0, 0, 0};
        counterBuf_.getSubData(0, sizeof(ctr), ctr);
        Particle p0{};
        cur_->getSubData(0, sizeof(Particle), &p0);
        std::fprintf(stderr,
                     "[ember] frame=%llu alive=%u head=%u reqs=%u total=%u cap=%u mask=0x%X dt=%.4f\n"
                     "        p0 pos=(%.3f,%.3f,%.3f) size=%.3f vel=(%.2f,%.2f,%.2f) age=%.3f life=%.3f col=(%.2f,%.2f,%.2f,%.2f)\n",
                     (unsigned long long)frameCount_, ctr[0], ctr[1], nr, total, ctr[3], p.forceMask, dt,
                     p0.pos.x, p0.pos.y, p0.pos.z, p0.pos.w,
                     p0.vel.x, p0.vel.y, p0.vel.z,
                     p0.vel.w, p0.life.x,
                     p0.color.r, p0.color.g, p0.color.b, p0.color.a);
        const int errs = gl::popErrors("update");
        if (errs > 0) std::fprintf(stderr, "[ember] %d GL error(s) after update\n", errs);
    }

    // 5) Swap: the freshly written buffer becomes the render/sim source.
    std::swap(cur_, nxt_);
    if (livePipeline) {
        std::swap(liveBuf_, nextLiveBuf_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingLive, liveBuf_.id());
    }
}
} // namespace ember::detail::opengl
