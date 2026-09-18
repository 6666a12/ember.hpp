#pragma once

// Shared behavioral regressions for modular and single-header consumption.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>

namespace regression {
inline void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
inline void glClean(const char* where) {
    if (ember::gl::popErrors(where)) throw std::runtime_error(where);
}
inline std::vector<ember::Particle> particles(const ember::ParticleSystem& sys, bool empty = false) {
    auto ps = sys.readParticles();
    require(ps.size() == sys.aliveCount(), "readback count differs from population");
    require(empty || !ps.empty(), "expected particles, got empty readback");
    for (const auto& p : ps) {
        for (const auto& v : {p.pos, p.vel, p.life, p.color})
            for (int i = 0; i < 4; ++i) require(std::isfinite(v[i]), "nonfinite particle");
        require(p.life.x >= 0 && p.vel.w < p.life.x, "readback contains retired particle");
    }
    return ps;
}
inline ember::BurstParams burst(float x = 0, float y = 0, float z = -2, float life = 10) {
    ember::BurstParams b;
    b.count = 1; b.position = {x,y,z};
    b.speedMin = b.speedMax = 0;
    b.sizeMin = b.sizeMax = 1;
    b.lifeMin = b.lifeMax = life;
    return b;
}
struct Target {
    ember::Texture color, depth;
    GLuint fbo = 0;
    int width;
    explicit Target(int w = 64) : width(w) {
        color.uploadRGBA8(w,w,nullptr);
        depth.uploadDepth(w,w);
        glGenFramebuffers(1,&fbo);
        glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color.id(),0);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,depth.id(),0);
        require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"test FBO incomplete");
        clear();
    }
    ~Target() { glBindFramebuffer(GL_FRAMEBUFFER,0); glDeleteFramebuffers(1,&fbo); }
    void clear(float d=1) {
        glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        glViewport(0,0,width,width);
        glDepthMask(GL_TRUE);
        glClearDepth(d);
        glClearColor(0,0,0,0);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glClearDepth(1);
    }
    std::vector<unsigned char> pixels() {
        glBindFramebuffer(GL_READ_FRAMEBUFFER,fbo);
        std::vector<unsigned char> data(width*width*4);
        glReadPixels(0,0,width,width,GL_RGBA,GL_UNSIGNED_BYTE,data.data());
        return data;
    }
    float centerDepth() {
        glBindFramebuffer(GL_READ_FRAMEBUFFER,fbo);
        float d=0;
        glReadPixels(width/2,width/2,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&d);
        return d;
    }
};
inline int sum(const std::vector<unsigned char>& p, int channel=0) {
    int n=0; for (std::size_t i=channel;i<p.size();i+=4) n+=p[i]; return n;
}
inline void solid(ember::Texture& t, unsigned char r, unsigned char g, unsigned char b) {
    const unsigned char px[]={r,g,b,255}; t.uploadRGBA8(1,1,px);
}
inline GLuint boundBuffer(GLuint binding) {
    GLint id=0; glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING,binding,&id);
    require(id!=0,"missing regression buffer binding"); return GLuint(id);
}
inline std::vector<GLuint> indices(GLuint binding, unsigned count) {
    std::vector<GLuint> result(count);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER,boundBuffer(binding));
    if (count) glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,0,count*sizeof(GLuint),result.data());
    return result;
}
inline void allocatorInvariant(const ember::ParticleSystem& sys) {
    const auto ctr=indices(4,5);
    require(ctr[0]==sys.aliveCount() && ctr[0]+ctr[1]==ctr[4] && ctr[4]<=sys.capacity(),
            "allocator population/extent invariant");
    const auto live=indices(11,ctr[0]), dead=indices(3,ctr[1]);
    std::set<GLuint> all(live.begin(),live.end());
    require(all.size()==live.size(),"duplicate live index after batched birth");
    for (auto id:dead) require(all.insert(id).second,"dead/live overlap or duplicate free slot");
    require(all.size()==ctr[4],"allocator lost slots");
    for (auto id:all) require(id<ctr[4],"allocator slot outside extent");
    const auto draw=indices(10,4);
    require(draw[0]==4 && draw[1]==ctr[0],"published draw count differs");
}
inline void performanceRegressions() {
    using namespace ember;
    {
        // Multi-workgroup allocation: recycled and newly appended slots in
        // the same batch, overflow, long sparse tails, then complete reuse.
        ParticleSystem sys({1031,2000,987}); sys.setForceMask(0);
        auto b=burst(1,0,-3,.02f);b.count=700;sys.burst(b);sys.update(0);
        b=burst(2,0,-3,100);b.count=137;sys.burst(b);sys.update(0);
        sys.update(.03f);allocatorInvariant(sys);
        for (int f=0;f<8;++f) {sys.update(.01f);require(particles(sys).size()==137,"sparse corpse resurrected");}
        b=burst(3,0,-3,.02f);b.count=1000;sys.burst(b);sys.update(0);
        require(sys.aliveCount()==1031,"mixed free/append reservation failed");
        allocatorInvariant(sys);
        sys.update(.03f);require(sys.aliveCount()==137,"batch retirement failed");
        for (int f=0;f<24;++f) {
            b=burst(float(f+4),0,-3,.01f);b.count=1500;sys.burst(b);
            sys.update(.02f);allocatorInvariant(sys);particles(sys);
        }
        sys.update(.02f);require(sys.aliveCount()==137,"overflow damaged survivors");
        ParticleSystem moved(std::move(sys));
        for (int f=0;f<4;++f) {moved.update(.01f);allocatorInvariant(moved);particles(moved);}
        moved.apply(Config::fromString("[system]\ncapacity=257\n"));
        require(moved.aliveCount()==0,"resize did not reset live state");
        b=burst();b.count=1000;moved.burst(b);moved.update(0);
        require(moved.aliveCount()==257,"resize batch bounds");allocatorInvariant(moved);
        moved.clear();moved.update(0);allocatorInvariant(moved);
    }
    Target target(16);
    const auto proj=glm::perspective(glm::radians(50.f),1.f,.1f,100.f);
    for (bool gpu:{false,true})
    for (unsigned n:{1u,63u,64u,65u,255u,256u,257u,511u,512u,513u,4093u,65537u}) {
        ParticleSystem sys({n,n,123});sys.setForceMask(0);sys.setSortEnabled(true);
        sys.setGpuDriven(gpu);
        auto b=burst();b.count=n;sys.burst(b);sys.update(0);
        std::vector<Particle> slots(n);
        for (unsigned i=0;i<n;++i) {
            // Both signs, equal keys, non-power-of-two padding, and holes.
            slots[i]={{0,0,float(int((i*37u)%101u)-60),.001f},{0,0,0,0},
                      {i%7==3?.001f:10.f,0,0,0},{1,1,1,1}};
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER,boundBuffer(1));
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,0,n*sizeof(Particle),slots.data());
        sys.update(.01f);allocatorInvariant(sys);
        for (int reverse=0;reverse<2;++reverse) {
            glm::mat4 view(1);view[2][2]=reverse?-1.f:1.f;
            target.clear();sys.render(view,proj,16,16,50);
            auto actual=indices(8,sys.aliveCount());
            std::vector<GLuint> expected;
            for (unsigned i=0;i<n;++i) if(i%7!=3) expected.push_back(i);
            std::sort(expected.begin(),expected.end(),[&](GLuint a,GLuint b) {
                float az=slots[a].pos.z*view[2][2],bz=slots[b].pos.z*view[2][2];
                return az<bz || (az==bz && a<b);
            });
            require(actual==expected,"tiled GPU sort differs from CPU reference");
        }
        // Sorting scratch must not clobber the next simulation's live list.
        sys.update(.01f);allocatorInvariant(sys);
        particles(sys);
    }
    glClean("optimized allocation and tiled sorting");
}
// Force telemetry congestion deterministically; a fast GPU may otherwise never
// fill the ring. Observe API calls as well as output, so a hidden sync fails.
struct StatisticsSpy {
    inline static PFNGLCLIENTWAITSYNCPROC originalWait=nullptr;
    inline static PFNGLGETBUFFERSUBDATAPROC originalRead=nullptr;
    inline static bool timeout=true;
    inline static unsigned waits=0,reads=0;
    static GLenum APIENTRY wait(GLsync fence,GLbitfield flags,GLuint64 ns) {
        require(ns==0,"statistics used a blocking fence wait");
        ++waits;
        return timeout?GL_TIMEOUT_EXPIRED:originalWait(fence,flags,ns);
    }
    static void APIENTRY read(GLenum target,GLintptr offset,GLsizeiptr size,void* data) {
        ++reads;originalRead(target,offset,size,data);
    }
    StatisticsSpy() {
        originalWait=glad_glClientWaitSync;originalRead=glad_glGetBufferSubData;
        waits=reads=0;timeout=true;
        glad_glClientWaitSync=wait;glad_glGetBufferSubData=read;
    }
    ~StatisticsSpy() {glad_glClientWaitSync=originalWait;glad_glGetBufferSubData=originalRead;}
};
inline void gpuDrivenRegressions() {
    using namespace ember;
    Target target(16);
    const auto proj=glm::perspective(glm::radians(50.f),1.f,.1f,100.f);
    ParticleSystem sys({1031,2000,987});sys.setForceMask(0);sys.setUseSprite(false);
    sys.setGpuDriven(true);
    {
        StatisticsSpy spy;
        for(unsigned f=1;f<=12;++f) {
            auto b=burst();b.count=17;sys.burst(b);sys.update(0);
            target.clear();sys.render(glm::mat4(1),proj,16,16,50);
            require(sys.pollStatistics().frame==0,"unfinished statistics published");
        }
        require(StatisticsSpy::reads==0 && StatisticsSpy::waits>0,"GPU update/render read unfinished counters");
        require(sys.droppedStatistics()==8,"full statistics ring did not skip samples");
        require(sum(target.pixels())>0,"render depended on stale zero CPU count");
        glFinish();StatisticsSpy::timeout=false;
        const auto sample=sys.pollStatistics();
        require(sample.frame==4 && sample.alive==68 && sample.allocated==68,"snapshot does not match its frame");
        require(StatisticsSpy::reads==4,"statistics ring read count");
        require(sys.aliveCount()==204,"exact aliveCount returned stale data");
        require(sys.synchronizeStatistics().frame==12,"explicit synchronization frame");
    }
    sys.setSortEnabled(true);
    target.clear();sys.render(glm::mat4(1),proj,16,16,50);
    const auto sorted=indices(8,204);
    for(unsigned i=0;i<sorted.size();++i) require(sorted[i]==i,"enabling sort required another update");
    require(sum(target.pixels())>0,"enabling sort hid existing particles");
    // Consecutive updates with recycling, without querying the asynchronous
    // system between checkpoints. Compare particle multisets, not slot order.
    ParticleSystem sync({1031,2000,987}),async({1031,2000,987});
    sync.setForceMask(0);async.setForceMask(0);async.setGpuDriven(true);
    for(int f=0;f<64;++f) {
        for(auto* s:{&sync,&async}) {
            if(f%5) {
                auto b=burst(float(f),0,-2,float(f%3+1)*.025f);
                b.count=unsigned(f%4+1)*150;s->burst(b);
            }
            s->update(.01f);
        }
        if(f%16==15) {
            allocatorInvariant(async);
            auto a=particles(sync,true),b=particles(async,true);
            auto less=[](const Particle& x,const Particle& y){return std::memcmp(&x,&y,sizeof(Particle))<0;};
            std::sort(a.begin(),a.end(),less);std::sort(b.begin(),b.end(),less);
            require(a.size()==b.size() && std::memcmp(a.data(),b.data(),a.size()*sizeof(Particle))==0,
                    "GPU scheduling changed simulation results");
        }
    }
    // Move pending fences, invalidate old snapshots on clear/resize, switch
    // modes, and render repeatedly without an intervening update.
    auto b=burst();b.count=900;async.burst(b);async.update(0);
    ParticleSystem moved(std::move(async)),assigned({16,16,123});
    assigned.setGpuDriven(true);assigned.burst(burst());assigned.update(0);
    assigned=std::move(moved);
    const auto sequence=assigned.updateSequence();
    assigned.clear();glFinish();
    require(assigned.pollStatistics().alive==0 && assigned.updateSequence()==sequence,"clear retained stale statistics");
    assigned.setSortEnabled(true);target.clear();assigned.render(glm::mat4(1),proj,16,16,50);
    require(sum(target.pixels())==0,"clear left indirect draw instances");
    b.count=1031;assigned.burst(b);assigned.update(0);
    assigned.apply(Config::fromString("[system]\ncapacity=257\n"));
    glFinish();require(assigned.pollStatistics().alive==0,"resize retained stale statistics");
    assigned.burst(b);assigned.update(0);
    glFinish();auto ready=assigned.pollStatistics();
    require(ready.frame==assigned.updateSequence() && ready.alive==257,"completed async statistics not current");
    assigned.setGpuDriven(false);require(assigned.aliveCount()==257,"disable lost counts");
    assigned.update(.01f);assigned.setGpuDriven(true);
    for(int i=0;i<2;++i) {target.clear();assigned.render(glm::mat4(1),proj,16,16,50);require(sum(target.pixels())>0,"mode switch lost draw");}
    // Device launch bounds must be rejected before reallocating large buffers.
    GLint limit=0;glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT,0,&limit);
    if(std::uint64_t(limit)*64+1 <= (1u<<30)) {
        bool rejected=false;
        try {assigned.apply(Config::fromString("[system]\ncapacity="+std::to_string(std::uint64_t(limit)*64+1)+"\n"));}
        catch(const std::invalid_argument&){rejected=true;}
        require(rejected && assigned.capacity()==257,"invalid indirect capacity mutated system");
    }
    glClean("GPU scheduling and asynchronous statistics");
}
inline void legacyShaderRegressions() {
    using namespace ember;
    // Minimal external shader using the previous five-counter, three-phase
    // contract. It has no births in this test: all particles are initialized
    // by the built-in shader before switching programs.
    const char* legacySim=R"GLSL(#version 430 core
layout(local_size_x=64) in;
struct P {vec4 pos;vec4 vel;vec4 life;vec4 color;};
layout(binding=0,std430) readonly buffer A {P cur[];};
layout(binding=1,std430) buffer B {P nxt[];};
layout(binding=3,std430) buffer D {uint dead[];};
layout(binding=4,std430) buffer C {uint alive;uint head;uint requests;uint capacity;uint allocated;};
layout(binding=10,std430) buffer I {uint vertexCount;uint instanceCount;uint first;uint base;};
layout(binding=11,std430) buffer L {uint live[];};
uniform int uPhase;uniform float uDt;
void main() {
 uint i=gl_GlobalInvocationID.x;if(i>=allocated)return;
 if(uPhase==0) {
  P p=cur[i];
  if(p.life.x>=0) {
   p.vel.w+=uDt;
   if(p.vel.w>=p.life.x) {p.life.x=-1;dead[atomicAdd(head,1u)]=i;atomicAdd(alive,-1u);}
  }
  nxt[i]=p;
 } else if(uPhase==2 && nxt[i].life.x>=0) live[atomicAdd(instanceCount,1u)]=i;
}
)GLSL";
    const char* legacySort=R"GLSL(#version 430 core
layout(local_size_x=64) in;
struct P {vec4 pos;vec4 vel;vec4 life;vec4 color;};
layout(binding=0,std430) readonly buffer A {P cur[];};
layout(binding=8,std430) buffer S {uint sorted[];};
layout(binding=11,std430) readonly buffer L {uint live[];};
uniform mat4 uView;uniform uint uAlive,uCapacity,uPaddedN,uK,uJ,uMode;
float key(uint id) {return id>=uCapacity?1e30:(uView*vec4(cur[id].pos.xyz,1)).z;}
void main() {
 uint i=gl_GlobalInvocationID.x;if(i>=uPaddedN)return;
 if(uMode==0) {sorted[i]=i<uAlive?live[i]:uCapacity;return;}
 uint j=i^uJ;if(j<=i)return;
 float a=key(sorted[i]),b=key(sorted[j]);
 if(((i&uK)==0u)?a>b:a<b) {uint t=sorted[i];sorted[i]=sorted[j];sorted[j]=t;}
}
)GLSL";
    struct Temp {
        std::filesystem::path path=std::filesystem::temp_directory_path()/
            ("ember-legacy-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Temp(){std::filesystem::create_directory(path);}
        ~Temp(){std::error_code ec;std::filesystem::remove(path/"sort.comp",ec);std::filesystem::remove(path,ec);}
    } temp;
    {std::ofstream out(temp.path/"sort.comp");out<<legacySort;require(bool(out),"write legacy sort fixture");}
    auto render=[](){return Shader::fromVertFrag("shaders/particle.vert","shaders/particle.frag");};
    ParticleSystem sys({259,259,123});sys.setForceMask(0);
    auto b=burst(0,0,-3,.01f);b.count=65;sys.burst(b);sys.update(0);
    b=burst(0,0,-2,10);b.count=129;sys.burst(b);sys.update(0);
    setOpenGLPrograms(sys.backend(),render(),Shader::fromSources({{GL_COMPUTE_SHADER,legacySim}}));
    bool rejected=false;
    try {sys.setGpuDriven(true);} catch(const std::invalid_argument&){rejected=true;}
    require(rejected && !sys.gpuDriven(),"GPU mode accepted legacy simulation");
    sys.update(.02f);require(sys.aliveCount()==129,"legacy simulation dispatch contract");
    allocatorInvariant(sys);
    Target target(16);
    sys.setShaderDirectory(temp.path.string().c_str());sys.setSortEnabled(true);
    const auto proj=glm::perspective(glm::radians(50.f),1.f,.1f,100.f);
    sys.render(glm::mat4(1),proj,16,16,50);
    auto sorted=indices(8,129);
    require(std::set<GLuint>(sorted.begin(),sorted.end()).size()==129,"legacy sort duplicates");
    for(auto id:sorted)require(id>=65 && id<194,"legacy sort drew retired slot");
    // Immediately switching after a legacy death must repair nxt tombstones.
    setOpenGLPrograms(sys.backend(),render(),Shader::fromCompute("shaders/simulate.comp"));
    for(int i=0;i<4;++i) {sys.update(.01f);allocatorInvariant(sys);require(particles(sys).size()==129,"legacy transition resurrected corpse");}
    b=burst();b.count=259;sys.burst(b);sys.update(0);allocatorInvariant(sys);
    require(sys.aliveCount()==259,"legacy transition broke batched reuse");
    rejected=false;
    try {sys.setGpuDriven(true);} catch(const std::invalid_argument&){rejected=true;}
    require(rejected && !sys.gpuDriven(),"GPU mode accepted legacy sort");
    sys.setSortEnabled(false);sys.setGpuDriven(true);
    rejected=false;
    try {sys.setSortEnabled(true);} catch(const std::invalid_argument&){rejected=true;}
    require(rejected && !sys.sortEnabled(),"GPU mode enabled legacy sort");
    rejected=false;
    try {sys.apply(Config::fromString("[system]\ncapacity=128\nsort=true\n"));}
    catch(const std::invalid_argument&){rejected=true;}
    require(rejected && sys.capacity()==259 && sys.aliveCount()==259,"incompatible config partially applied");
    rejected=false;
    try {setOpenGLPrograms(sys.backend(),render(),Shader::fromSources({{GL_COMPUTE_SHADER,legacySim}}));}
    catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"GPU mode accepted shader replacement with legacy simulation");
    sys.update(.01f);require(sys.aliveCount()==259,"rejected shader replacement damaged system");
    sys.setGpuDriven(false);
    setOpenGLPrograms(sys.backend(),render(),Shader::fromSources({{GL_COMPUTE_SHADER,legacySim}}));
    sys.update(.01f);allocatorInvariant(sys);particles(sys);
    // WO-08: a legacy custom simulation shader has no event queue protocol;
    // registering a non-empty event template must be rejected on upload.
    {
        Emitter spark;spark.eventCount=2;
        sys.addEventEmitter(spark);
        rejected=false;
        try {sys.update(.01f);} catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"custom simulation shader accepted event templates");
        sys.clearEventEmitters();
    }
    glClean("legacy shader compatibility");
}
} // namespace regression
#include "effects.hpp"
#include "streak.hpp"
namespace regression {
inline void run(ember::Window& window) {
    using namespace ember;
    glClean("regression baseline");
    {
        // The injected factory and legacy constructor must share the same GL
        // implementation. Keep a borrowed adapter across facade moves.
        auto backend = makeOpenGLBackend();
        auto* native = backend.get();
        ParticleSystem injected({16,16,123},std::move(backend));
        ParticleSystem legacy({16,16,123});
        require(std::string(injected.backendName())=="opengl", "OpenGL factory selection");
        const auto caps = injected.backendCapabilities();
        require(caps.gpuScheduling && caps.sorting && caps.bloom && caps.refraction &&
                caps.softParticles && caps.spriteTextures, "OpenGL capabilities missing");
        injected.setForceMask(0);legacy.setForceMask(0);
        injected.burst(burst());legacy.burst(burst());
        injected.update(.01f);legacy.update(.01f);
        const auto a=particles(injected), b=particles(legacy);
        require(a.size()==1 && b.size()==1 && a[0].pos==b[0].pos &&
                a[0].vel==b[0].vel && a[0].color==b[0].color, "injected GL behavior differs");
        ParticleSystem moved(std::move(injected));
        setOpenGLSoftDepth(*native,0);
        setOpenGLRefractionInputs(*native,0,0);
        moved.setGpuDriven(true);moved.update(0);
        require(moved.synchronizeStatistics().alive==1, "move lost injected backend");
        glClean("injected OpenGL backend");
    }
    {
        // Call the registered C trampoline directly, then verify that the
        // wrapper rethrows only after the callback has returned.
        auto key=glfwSetKeyCallback(window.handle(),nullptr);
        glfwSetKeyCallback(window.handle(),key);
        window.onKey=[](int,int,int,int){throw std::runtime_error("callback sentinel");};
        key(window.handle(),GLFW_KEY_A,0,GLFW_PRESS,0);
        bool caught=false;
        try {window.pollEvents();} catch(const std::runtime_error& e) {caught=std::string(e.what())=="callback sentinel";}
        require(caught,"callback exception not deferred");
        window.onKey={};
    }
    {
        ParticleSystem sys({16,16,123}); sys.setForceMask(0);
        sys.burst(burst(0,0,-2,0.05f)); sys.update(0);
        sys.burst(burst(1)); sys.update(0);
        sys.update(0.06f);
        for (int frame=0;frame<12;++frame) {
            auto ps=particles(sys);
            require(ps.size()==1 && ps[0].pos.x==1,"survivor lost or corpse resurrected");
            require(std::abs(ps[0].vel.w-(0.06f+frame*0.01f))<1e-5f,"survivor age did not advance");
            sys.update(0.01f);
        }
        sys.clear(); require(sys.aliveCount()==0,"clear");
        // Sustained recycling, over-capacity bursts, and dead-only frames.
        for (int f=0;f<160;++f) {
            if (f%5!=0) { auto b=burst(float(f),0,-2,0.025f);b.count=24;sys.burst(b); }
            sys.update(0.01f);
            auto ps=particles(sys,true);
            require(ps.size()<=16,"capacity exceeded");
            GLint id=0; glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING,10,&id);
            GLuint args[4]{};glBindBuffer(GL_SHADER_STORAGE_BUFFER,id);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,0,sizeof(args),args);
            require(args[0]==4 && args[1]==ps.size(),"indirect draw population mismatch");
            glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING,11,&id);
            std::vector<GLuint> indices(ps.size()); glBindBuffer(GL_SHADER_STORAGE_BUFFER,id);
            if (!indices.empty()) glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,0,indices.size()*4,indices.data());
            std::set<GLuint> unique(indices.begin(),indices.end());
            require(unique.size()==indices.size(),"duplicate live slot");
            for (auto i:indices) require(i<sys.capacity(),"live index out of bounds");
        }
        for (int i=0;i<10;++i) sys.update(0.01f);
        require(sys.aliveCount()==0 && sys.readParticles().empty(),"particles failed to expire");
    }
    glClean("lifecycle");
    for (auto mode:{BoundaryMode::Kill,BoundaryMode::Bounce}) {
        ParticleSystem sys({16,16});sys.setForceMask(0);sys.setBoundary(mode,0,0.5f);
        auto b=burst(0,-1);sys.burst(b);sys.update(0);sys.update(0.01f);
        if (mode==BoundaryMode::Kill) require(sys.aliveCount()==0,"Kill did not retire particle");
        else { auto ps=particles(sys);require(ps.size()==1 && ps[0].pos.y==0,"Bounce did not preserve particle"); }
    }
    {
        ParticleSystem sys({16,16});sys.setForceMask(0);
        Spring a,b;a.anchor={1,0,0};a.stiffness=1;b.anchor={0,2,0};b.stiffness=1;
        sys.setSprings({a,b});sys.burst(burst(0,0,0));sys.update(0);sys.update(0.1f);
        auto ps=particles(sys);
        require(std::abs(ps[0].vel.x-0.1f)<1e-5f && std::abs(ps[0].vel.y-0.2f)<1e-5f,"multiple spring layout");
    }
    {
        ParticleSystem sys({8192,8192});sys.setForceMask(0);
        Emitter e;e.rate=10;e.speedMin=e.speedMax=0;
        for (int i=0;i<4100;++i) sys.addEmitter(e);
        auto b=burst();b.count=1;sys.burst(b);sys.update(0.1f);
        require(sys.aliveCount()==4096,"combined request budget");
        particles(sys);glClean("request overflow capped");
    }
    {
        ParticleSystem a({32,32,123}),b({32,32,456}),c({32,32,123});
        Emitter e;e.shape=Emitter::Shape::Box;e.rate=100;
        for (auto* sys:{&a,&b,&c}) { sys->setForceMask(0);sys->addEmitter(e);sys->update(0.1f); }
        auto pa=particles(a),pb=particles(b),pc=particles(c);
        auto less=[](const Particle& x,const Particle& y){return x.pos.x<y.pos.x;};
        std::sort(pa.begin(),pa.end(),less);std::sort(pb.begin(),pb.end(),less);std::sort(pc.begin(),pc.end(),less);
        require(pa.size()==pc.size() && std::memcmp(pa.data(),pc.data(),pa.size()*sizeof(Particle))==0,"same seed differs");
        require(std::memcmp(pa.data(),pb.data(),pa.size()*sizeof(Particle))!=0,"different seeds ignored");
    }
    {
        ParticleSystem sys({32,32});sys.setForceMask(0);
        Emitter e;e.rate=10;e.palette={{1,0,0,1}};sys.addEmitter(e);
        sys.emitter(0)->palette={{0,1,0,1}};sys.update(0.1f);
        require(particles(sys)[0].color==glm::vec4(0,1,0,1),"palette edits ignored");
        sys.clear();sys.emitter(0)->palette.clear();sys.emitter(0)->colorMin=sys.emitter(0)->colorMax={0,0,1,1};
        sys.update(0.1f);require(particles(sys)[0].color==glm::vec4(0,0,1,1),"palette clear ignored");
        sys.setNoiseWind({1,0,0},2,0.4f,1);
        sys.apply(Config::fromString("[system]\nnoise_wind_speed = 2\n"));
        require(sys.forceEnabled(Force::NoiseWind),"partial wind update resets amplitude");
        sys.setWave({1,0,0},{1,0,0},2,1);
        sys.apply(Config::fromString("[system]\nwave_omega = 3\n"));
        require(sys.forceEnabled(Force::Wave),"partial wave update resets amplitude");
        sys.setForceMask(0);sys.setBoundary(BoundaryMode::Bounce,2,0.5f);
        sys.apply(Config::fromString("[system]\nrestitution = 0.1\n"));
        sys.clearEmitters();sys.clear();sys.burst(burst(0,1));sys.update(0);sys.update(0.01f);
        require(particles(sys)[0].pos.y==2,"partial boundary update resets plane");
        require(!sys.forceEnabled(static_cast<Force>(32)),"invalid force getter");
        bool threw=false;try {sys.enableForce(static_cast<Force>(32));}catch(const std::invalid_argument&){threw=true;}
        require(threw,"invalid force setter");
        threw=false;try {sys.update(-0.1f);}catch(const std::invalid_argument&){threw=true;}
        require(threw,"negative dt accepted");
    }
    glClean("API regressions");
    effectsRegressions();
    streakRegressions();
    performanceRegressions();
    gpuDrivenRegressions();
    legacyShaderRegressions();
    const glm::mat4 view(1);
    const auto proj=glm::perspective(glm::radians(50.f),1.f,0.1f,100.f);
    auto depthAt=[&](float z){ auto p=proj*glm::vec4(0,0,z,1);return (p.z/p.w+1)*0.5f; };
    Target target;
    {
        ParticleSystem sys({16,16});sys.setForceMask(0);sys.setBlendMode(BlendMode::Normal);
        // Reverse insertion order forces sorting to make a visible difference.
        auto near=burst(0,0,-2);near.colorMin=near.colorMax={1,0,0,0.6f};
        auto far=burst(0,0,-3);far.colorMin=far.colorMax={0,0,1,0.6f};
        far.sizeMin=far.sizeMax=1.5f;
        sys.burst(near);sys.update(0);sys.burst(far);sys.update(0);
        // Live-list atomic insertion order is intentionally unspecified.
        // Arrange this fixture in that actual order so unsorted rendering
        // is reliably wrong on every driver.
        GLint list=0,slots=0;GLuint order[2]{};
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING,11,&list);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER,list);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,0,sizeof(order),order);
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING,1,&slots);
        const Particle nearParticle{{0,0,-2,1},{0,0,0,0},{10,1,0,0},{1,0,0,0.6f}};
        const Particle farParticle{{0,0,-3,1.5f},{0,0,0,0},{10,0,0,1},{0,0,1,0.6f}};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER,slots);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,order[0]*sizeof(Particle),sizeof(Particle),&nearParticle);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,order[1]*sizeof(Particle),sizeof(Particle),&farParticle);
        target.clear();sys.render(view,proj,64,64,50);auto unsorted=target.pixels();
        sys.setSortEnabled(true);target.clear();sys.render(view,proj,64,64,50);auto sorted=target.pixels();
        const int center=(32*64+32)*4;
        require(sorted[center]>sorted[center+2],"sort is not back to front");
        require(sorted[center]>unsorted[center]+10,"sort had no observable effect");
    }
    {
        ParticleSystem sys({16,16});sys.setForceMask(0);
        auto b=burst();b.sizeMin=b.sizeMax=0.3f;b.colorMin=b.colorMax={8,8,8,1};sys.burst(b);sys.update(0);
        target.clear();sys.render(view,proj,64,64,50);auto plain=target.pixels();
        sys.setBloom(true);target.clear(0.37f);glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);
        sys.render(view,proj,64,64,50);auto bloomed=target.pixels();
        GLint bound=0;glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&bound);
        require((GLuint)bound==target.fbo,"Bloom changed host framebuffer");
        require(!glIsEnabled(GL_DEPTH_TEST),"Bloom changed host depth test");
        GLboolean mask=GL_TRUE;glGetBooleanv(GL_DEPTH_WRITEMASK,&mask);require(!mask,"Bloom changed depth mask");
        require(std::abs(target.centerDepth()-0.37f)<1e-5f,"Bloom overwrote host depth");
        int halo=0;for (std::size_t i=0;i<plain.size();i+=4) if (plain[i]==0 && bloomed[i]>4) ++halo;
        require(halo>0,"Bloom has no halo outside particle footprint");
        sys.setDepthTest(true);target.clear(depthAt(-1));
        sys.render(view,proj,64,64,50);
        require(sum(target.pixels())==0,"Bloom particles leaked through foreground depth");
        require(std::abs(target.centerDepth()-depthAt(-1))<1e-5f,"fullscreen pass wrote depth");
        // Resize while bound to a different host target.
        Target small(32);small.clear();sys.render(view,proj,32,32,50);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&bound);require((GLuint)bound==small.fbo,"Bloom resize lost host FBO");
    }
    glClean("sort and bloom pixels");
    {
        ParticleSystem sys({16,16});sys.setForceMask(0);sys.burst(burst());sys.update(0);
        Texture depth;std::vector<float> ds(64*64,depthAt(-4));
        depth.uploadDepth(64,64);glTexSubImage2D(GL_TEXTURE_2D,0,0,0,64,64,GL_DEPTH_COMPONENT,GL_FLOAT,ds.data());
        target.clear();sys.render(view,proj,64,64,50);auto normal=target.pixels();
        setOpenGLSoftParticles(sys,true,depth.id(),1);
        target.clear();sys.render(view,proj,64,64,50);auto front=target.pixels();
        require(sum(front)>0 && std::abs(sum(front)-sum(normal))<20,"soft particle in front incorrectly hidden");
        std::fill(ds.begin(),ds.end(),depthAt(-2.1f));depth.bind();
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,64,64,GL_DEPTH_COMPONENT,GL_FLOAT,ds.data());
        target.clear();sys.render(view,proj,64,64,50);
        int wide=sum(target.pixels());
        sys.apply(Config::fromString("[system]\nsoft_radius = 0.05\n"));
        target.clear();sys.render(view,proj,64,64,50);
        require(sum(target.pixels())>wide*2,"soft_radius-only update ignored");
        // Move camera and particle together: view depth is identical.
        sys.clear();sys.burst(burst(0,0,3));sys.update(0);
        const auto translated=glm::translate(glm::mat4(1),glm::vec3(0,0,-5));
        target.clear();sys.render(translated,proj,64,64,50);
        require(std::abs(sum(target.pixels())-sum(front))<20,"soft particle uses world-space depth");
    }
    {
        ParticleSystem sys({16,16});sys.setForceMask(0);
        auto b=burst();b.refractive=true;sys.burst(b);sys.update(0);
        Texture scene;solid(scene,255,0,0);
        RefractionSettings r;r.enabled=true;r.sceneColorTex=scene.id();r.absorption=1;r.fresnel=0;r.shape=1;
        setOpenGLRefraction(sys,r);sys.setSpin(1.5f);
        sys.update(0.1f); // nonzero spin angle, observable polygon silhouette
        target.clear();sys.render(view,proj,64,64,50);auto red=target.pixels();
        require(sum(red)>0 && sum(red,1)==0,"refractive scene not sampled");
        solid(scene,0,255,0);
        target.clear();sys.render(view,proj,64,64,50);auto green=target.pixels();
        require(sum(green)==0 && sum(green,1)>0,"changing scene texture has no effect");
        ParticleSystem moved(std::move(sys));
        target.clear();moved.render(view,proj,64,64,50);
        require(target.pixels()==green,"move lost refraction/spin state");
        ParticleSystem assigned({1,1});assigned=std::move(moved);
        target.clear();assigned.render(view,proj,64,64,50);require(target.pixels()==green,"move assignment lost render settings");
        // Valid scene + no depth exercises the actual depth-to-simple fallback.
        r.mode=1;setOpenGLRefraction(assigned,r);
        target.clear();assigned.render(view,proj,64,64,50);require(target.pixels()==green,"depth fallback differs from simple");
        // A gradient makes displacement observable for heat/depth modes.
        std::vector<unsigned char> gradient(64*64*4);
        for(int y=0;y<64;++y)for(int x=0;x<64;++x) {
            int i=(y*64+x)*4;gradient[i]=x*4;gradient[i+1]=y*4;gradient[i+3]=255;
        }
        scene.uploadRGBA8(64,64,gradient.data());
        r.mode=0;r.strength=0;setOpenGLRefraction(assigned,r);
        target.clear();assigned.render(view,proj,64,64,50);auto zero=target.pixels();
        r.mode=2;r.strength=0.2f;setOpenGLRefraction(assigned,r);
        target.clear();assigned.render(view,proj,64,64,50);require(target.pixels()!=zero,"heat displacement missing");
        Texture depth;depth.uploadDepth(64,64);std::vector<float> ds(64*64,depthAt(-2));
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,64,64,GL_DEPTH_COMPONENT,GL_FLOAT,ds.data());
        r.mode=1;r.sceneDepthTex=depth.id();setOpenGLRefraction(assigned,r);
        target.clear();assigned.render(view,proj,64,64,50);auto near=target.pixels();
        std::fill(ds.begin(),ds.end(),depthAt(-4));depth.bind();
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,64,64,GL_DEPTH_COMPONENT,GL_FLOAT,ds.data());
        target.clear();assigned.render(view,proj,64,64,50);
        require(target.pixels()!=near,"depth refraction ignores scene depth");
    }
    glClean("soft and refractive pixels");
    {
        const auto original=std::filesystem::current_path();
        const auto empty=std::filesystem::temp_directory_path()/
            ("ember-embedded-regression-"+std::to_string(std::random_device{}()));
        struct WorkingDirectory {
            std::filesystem::path original,temporary;
            ~WorkingDirectory(){std::filesystem::current_path(original);std::error_code ec;std::filesystem::remove_all(temporary,ec);}
        } cwd{original,empty};
        std::filesystem::create_directory(empty);
        std::filesystem::current_path(empty);
        ParticleSystem sys({16,16});sys.setForceMask(0);
        sys.setGpuDriven(true);
        sys.burst(burst(1,0,-2,0.005f));sys.update(0);
        sys.burst(burst());sys.update(0);sys.update(0.01f);
        require(particles(sys).size()==1 && particles(sys)[0].pos.x==0,"embedded simulation differs");
        sys.setSortEnabled(true);
        target.clear();sys.render(view,proj,64,64,50);
        require(sum(target.pixels())>0,"embedded rendering/sort missing");
        glClean("embedded fallback");
    }
    {
        // Test a multisampled default framebuffer, and verify instance
        // methods preserve the previously current context.
        struct RestoreContext {
            GLFWwindow* previous;
            ~RestoreContext(){glfwMakeContextCurrent(previous);}
        } restore{glfwGetCurrentContext()};
        Window msaa(64,64,"multisample regression",4,false,false);
        GLFWwindow* current=glfwGetCurrentContext();
        window.setVsync(false);
        require(glfwGetCurrentContext()==current,"setVsync did not restore context");
        ParticleSystem sys({16,16});sys.setForceMask(0);
        sys.burst(burst());sys.update(0);sys.setBloom(true);sys.setDepthTest(true);
        glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,64,64);
        glDepthMask(GL_TRUE);glClearDepth(depthAt(-1));glClearColor(0,0,0,0);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);glClearDepth(1);
        sys.render(view,proj,64,64,50);
        std::vector<unsigned char> pixels(64*64*4);
        glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
        require(sum(pixels)==0,"MSAA Bloom leaked through foreground depth");
        glClean("multisampled default framebuffer");
    }
    {
        // Partial shader load must fall back in the same call and recover.
        const auto path=std::filesystem::temp_directory_path()/
            ("ember-shader-regression-"+std::to_string(std::random_device{}()));
        struct Cleanup { std::filesystem::path p;~Cleanup(){std::error_code ec;std::filesystem::remove_all(p,ec);} } cleanup{path};
        std::filesystem::create_directory(path);
        for (const char* f:{"bloom.vert","bloom_threshold.frag"})
            std::filesystem::copy_file(std::filesystem::path("shaders")/f,path/f);
        ParticleSystem sys({16,16});sys.setForceMask(0);sys.burst(burst());sys.update(0);
        sys.setShaderDirectory(path.string().c_str());sys.setBloom(true);
        target.clear();sys.render(view,proj,64,64,50);
        require(!sys.bloom() && sum(target.pixels())>0,"partial Bloom load did not fall back");
        glClean("partial shader failure");
        sys.setShaderDirectory("shaders");sys.setBloom(true);
        target.clear();sys.render(view,proj,64,64,50);require(sys.bloom(),"Bloom did not recover");
        sys.render(view,proj,0,0,50);glClean("zero viewport");
    }
    glClean("regressions complete");
    std::printf("behavioral regressions: ALL PASSED\n");
}
} // namespace regression
