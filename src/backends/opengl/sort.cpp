#include "common.hpp"
#include <cstdio>
#include <stdexcept>

namespace ember::detail::opengl {
void OpenGLBackend::setSortEnabled(bool on) {
    if (on && gpuDriven_) {
        ensureSortProgram();
        if (!sortProg_ || !sortProg_.hasUniform("uGpuDriven") || !sortProg_.hasUniform("uTileMode"))
            throw std::invalid_argument("ember: GPU-driven mode requires the GPU scheduling sort protocol");
        if (!sortEnabled_) scheduleGpu(1); // allow render before another update
    }
    sortEnabled_ = on;
}

void OpenGLBackend::ensureSortProgram() {
    if (sortProg_) return;
    // Same sourcing policy as the sim/render programs: editable file first,
    // embedded copy as fallback (so embedding stays zero-file).
    const std::string dir = shaderDir_.empty() ? "" : shaderDir_ + "/";
    try {
        sortProg_ = Shader::fromCompute((dir + "sort.comp").c_str());
        if (debug_) std::fprintf(stderr, "[ember] sort shader loaded from %s\n", dir.c_str());
    } catch (const std::exception& e) {
        if (debug_) std::fprintf(stderr, "[ember] sort.comp unavailable (%s); using embedded\n", e.what());
        try {
            sortProg_ = Shader::fromSources({{GL_COMPUTE_SHADER, kSortComp}});
        } catch (const std::exception& e2) {
            // Degrade gracefully like bloom: never throw from render().
            std::fprintf(stderr, "[ember] warning: sort shader unavailable (%s); depth sort disabled\n", e2.what());
            sortEnabled_ = false;
        }
    }
}

void OpenGLBackend::sortParticles(const glm::mat4& view) {
    ensureSortProgram();
    if (!sortProg_) return;
    EMBER_BENCH_BEGIN(Sort);
    if (gpuDriven_ && (!sortProg_.hasUniform("uGpuDriven") || !sortProg_.hasUniform("uTileMode")))
        throw std::invalid_argument("ember: incompatible sort shader in GPU-driven mode");
    const std::uint32_t N = nextPow2(gpuDriven_?capacity_:alive_);
    const bool tiled = sortProg_.hasUniform("uTileMode");
    const auto groupSize = tiled ? 256u : kSimGroupSize;
    const auto groups = (N + groupSize - 1u) / groupSize;
    if (tiled) {
        if (sortKeyCapacity_ < N) {
            sortKeyBuf_.data(nullptr, (GLsizeiptr)N * sizeof(float), GL_DYNAMIC_COPY);
            sortKeyCapacity_ = N;
        }
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingScratch, sortKeyBuf_.id());
    }
    glUseProgram(sortProg_.id());
    sortProg_.setMat4("uView", view);
    sortProg_.setUint("uAlive", alive_);
    sortProg_.setUint("uCapacity", capacity_);
    sortProg_.setUint("uPaddedN", N);
    sortProg_.setInt("uGpuDriven",gpuDriven_?1:0);
    // Built-in shader reads a std140 block; the loose uniforms above serve
    // custom shaders on the legacy sort contract.
    SortParams sp{};
    sp.view = view;
    sp.capacity = capacity_; sp.alive = alive_; sp.paddedN = N;
    sp.gpuDriven = gpuDriven_ ? 1 : 0;
    glBindBufferBase(GL_UNIFORM_BUFFER, kBindingUboSort, uboSort_.id());
    const auto uploadSort = [&] { uboSort_.subData(0, sizeof(sp), &sp); };
    if (gpuDriven_) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER,kBindingSchedule,dispatchBuf_.id());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER,kBindingCounters,counterBuf_.id());
        glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER,dispatchBuf_.id());
    }
    GLintptr dispatchOffset=5*sizeof(GLuint);
    auto dispatch=[&]() {
        if (gpuDriven_) {
            glDispatchComputeIndirect(dispatchOffset);
            dispatchOffset+=3*sizeof(GLuint);
        } else glDispatchCompute(groups,1,1);
    };
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingCur, cur_->id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingLive, liveBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingSorted, sortedBuf_.id());

    // Fill the live permutation and cache keys; optimized shaders also sort
    // alternating 256-entry tiles here. Legacy custom shaders keep mode 0/1.
    sortProg_.setUint("uMode", 0);
    sortProg_.setUint("uTileMode", 1);
    sp.mode = 0; sp.tileMode = 1; sp.k = 0; sp.j = 0;
    uploadSort();
    dispatch();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // Cross-tile merges use cached keys; all final intra-tile steps execute
    // in one dispatch. No approximation or reduced sorting frequency.
    for (std::uint32_t k = tiled ? 512u : 2u; k <= N; k <<= 1) {
        for (std::uint32_t j = k >> 1; j >= (tiled ? 256u : 1u); j >>= 1) {
            sortProg_.setUint("uK", k);
            sortProg_.setUint("uJ", j);
            sortProg_.setUint("uMode", 1);
            sortProg_.setUint("uTileMode", 0);
            sp.k = k; sp.j = j; sp.mode = 1; sp.tileMode = 0;
            uploadSort();
            dispatch();
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        }
        if (tiled) {
            sortProg_.setUint("uTileMode", 2);
            sp.tileMode = 2;
            uploadSort();
            dispatch();
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        }
    }
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    EMBER_BENCH_END(Sort);
}


} // namespace ember::detail::opengl
