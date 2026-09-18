#include "common.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace ember::detail::opengl {
void OpenGLBackend::validateGpuCapacity(std::uint32_t capacity) const {
    GLint limit=0;
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT,0,&limit);
    if ((capacity+63ull)/64ull > (std::uint64_t)limit ||
        (nextPow2(capacity)+255ull)/256ull > (std::uint64_t)limit)
        throw std::invalid_argument("ember: capacity exceeds GPU indirect dispatch limits");
}

void OpenGLBackend::scheduleGpu(int phase, std::uint32_t requests) {
    glUseProgram(scheduleProg_.id());
    scheduleProg_.setInt("uSchedulePhase",phase);
    scheduleProg_.setUint("uRequestCount",requests);
    const ScheduleParams params{phase, requests, 0u, 0u};
    uboSchedule_.subData(0, sizeof(params), &params);
    glBindBufferBase(GL_UNIFORM_BUFFER, kBindingUboSchedule, uboSchedule_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,kBindingCounters,counterBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,kBindingIndirect,indirectBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,kBindingSchedule,dispatchBuf_.id());
    // schedule.comp resets the event queue header (binding 22) in phase 0.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,kBindingEventQueue,eventQueueBuf_.id());
    glDispatchCompute(1,1,1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
}

void OpenGLBackend::setGpuDriven(bool enabled) {
    if (enabled==gpuDriven_) return;
    if (enabled) {
        validateGpuCapacity(capacity_);
        if (!simProg_.hasUniform("uInputAlive") || !simProg_.hasUniform("uGpuDriven"))
            throw std::invalid_argument("ember: GPU-driven mode requires the GPU scheduling simulation protocol");
        if (sortEnabled_) {
            ensureSortProgram();
            if (!sortProg_ || !sortProg_.hasUniform("uGpuDriven") || !sortProg_.hasUniform("uTileMode"))
                throw std::invalid_argument("ember: GPU-driven mode requires the GPU scheduling sort protocol");
        }
        if (!scheduleProg_) scheduleProg_=Shader::fromSources({{GL_COMPUTE_SHADER,kScheduleComp}});
        dispatchBuf_.data(nullptr,kScheduleBytes,GL_DYNAMIC_COPY);
        if (!statisticsBuffers_[0]) {
            glGenBuffers((GLsizei)statisticsBuffers_.size(),statisticsBuffers_.data());
            for (auto buffer:statisticsBuffers_) {
                glBindBuffer(GL_COPY_WRITE_BUFFER,buffer);
                glBufferData(GL_COPY_WRITE_BUFFER,5*sizeof(GLuint),nullptr,GL_STREAM_READ);
            }
        }
        // Support render immediately after enabling (without another update).
        if (sortEnabled_) scheduleGpu(1);
        gpuDriven_=true;
    } else {
        refreshCounts(); // deliberate one-time synchronization at mode switch
        spawnBuf_.data(nullptr,(GLsizeiptr)kMaxSpawnRequests*sizeof(SpawnRequest),GL_DYNAMIC_DRAW);
        gpuDriven_=false;
        resetStatistics();
    }
}

void OpenGLBackend::refreshCounts() const {
    if (countsFresh_) return;
    GLuint counters[5]{};
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_COPY_READ_BUFFER,counterBuf_.id());
    glGetBufferSubData(GL_COPY_READ_BUFFER,0,sizeof(counters),counters);
    alive_=std::min(counters[0],capacity_);
    allocated_=std::min(counters[4],capacity_);
    countsFresh_=true;
    statistics_={frameCount_,alive_,allocated_};
}

OpenGLBackend::Statistics OpenGLBackend::synchronizeStatistics() const {
    refreshCounts();
    return statistics_;
}

void OpenGLBackend::resetStatistics() {
    for (std::size_t i=0;i<statisticsFences_.size();++i) {
        if (statisticsFences_[i]) glDeleteSync(statisticsFences_[i]);
        statisticsFences_[i]=nullptr;
        if (statisticsBuffers_[i]) {
            // Orphan pending storage rather than reuse an in-flight copy.
            glBindBuffer(GL_COPY_WRITE_BUFFER,statisticsBuffers_[i]);
            glBufferData(GL_COPY_WRITE_BUFFER,5*sizeof(GLuint),nullptr,GL_STREAM_READ);
        }
    }
    countsFresh_=true;
    statistics_={frameCount_,alive_,allocated_};
}

OpenGLBackend::Statistics OpenGLBackend::pollStatistics() {
    // One flush is sufficient to make this context's pending samples progress;
    // repeating it for every occupied slot adds driver submission overhead.
    GLbitfield flags=GL_SYNC_FLUSH_COMMANDS_BIT;
    for (std::size_t i=0;i<statisticsFences_.size();++i) {
        if (!statisticsFences_[i]) continue;
        const GLenum ready=glClientWaitSync(statisticsFences_[i],flags,0);
        flags=0;
        if (ready==GL_WAIT_FAILED) throw std::runtime_error("ember: statistics fence failed");
        if (ready!=GL_ALREADY_SIGNALED && ready!=GL_CONDITION_SATISFIED) continue;
        GLuint counters[5]{};
        glBindBuffer(GL_COPY_READ_BUFFER,statisticsBuffers_[i]);
        glGetBufferSubData(GL_COPY_READ_BUFFER,0,sizeof(counters),counters);
        glDeleteSync(statisticsFences_[i]);statisticsFences_[i]=nullptr;
        if (statisticsFrames_[i]>statistics_.frame) {
            statistics_={statisticsFrames_[i],std::min(counters[0],capacity_),std::min(counters[4],capacity_)};
            if (statistics_.frame==frameCount_) {
                alive_=statistics_.alive;allocated_=statistics_.allocated;countsFresh_=true;
            }
        }
    }
    return statistics_;
}

void OpenGLBackend::enqueueStatistics() {
    pollStatistics();
    for (std::size_t i=0;i<statisticsFences_.size();++i) {
        if (statisticsFences_[i]) continue;
        glBindBuffer(GL_COPY_READ_BUFFER,counterBuf_.id());
        glBindBuffer(GL_COPY_WRITE_BUFFER,statisticsBuffers_[i]);
        glCopyBufferSubData(GL_COPY_READ_BUFFER,GL_COPY_WRITE_BUFFER,0,0,5*sizeof(GLuint));
        statisticsFrames_[i]=frameCount_;
        statisticsFences_[i]=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
        if (!statisticsFences_[i]) throw std::runtime_error("ember: cannot create statistics fence");
        return;
    }
    ++droppedStatistics_; // bounded queue: skip telemetry, never block simulation
}

std::vector<Particle> OpenGLBackend::readParticles(std::uint32_t max) const {
    refreshCounts();
    const std::uint32_t n = std::min(max == 0 ? alive_ : std::min(max, alive_), capacity_);
    std::vector<Particle> out;
    if (n == 0) return out;
    std::vector<Particle> slots(allocated_);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    cur_->getSubData(0, (GLsizeiptr)slots.size() * sizeof(Particle), slots.data());
    out.reserve(n);
    for (const auto& p : slots) {
        if (p.life.x >= 0.f) out.push_back(p);
        if (out.size() == n) break;
    }
    return out;
}
} // namespace ember::detail::opengl
