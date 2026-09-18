// Deliberately linked only to ember_core + glm. This must need no GL loader,
// window system or device, even for config application and frame preparation.
#include "ember/system.hpp"
#include "ember/config.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#ifdef GL_VERSION_4_3
#error "Backend-independent public API leaked OpenGL headers"
#endif

namespace {
void check(bool value,const char* message) {
    if(!value) throw std::runtime_error(message);
}
class RecordingBackend final : public ember::ParticleBackend {
public:
    unsigned capacity=0, paletteUploads=0, renders=0, eventUploads=0, curveUploads=0;
    bool recordEvents=true; // false routes through the default rejection path
    std::uint32_t lastSeed=0, lastTotal=0;
    float lastDt=0,lastTime=0;
    ember::ParticleStatistics stats;
    ember::SimulationParameters simulation;
    ember::RenderParameters rendering;
    ember::RenderView camera;
    std::vector<ember::SpawnRequest> requests;
    std::vector<ember::SpawnRequest> eventTemplates;
    ember::LifeCurvesLut curves{};
    std::vector<glm::vec4> palette;
    const char* name() const noexcept override {return "recording";}
    ember::BackendCapabilities capabilities() const noexcept override {return caps;}
    ember::BackendCapabilities caps;
    void initialize(std::uint32_t n,bool) override {capacity=n;}
    void validateConfiguration(std::uint32_t n,bool sort) override {
        if(n==0 || n>100000 || sort) throw std::invalid_argument("unsupported recording config");
    }
    void resize(std::uint32_t n) override {capacity=n;clear();}
    void clear() override {stats.alive=stats.allocated=0;}
    void setDebug(bool) override {}
    void uploadAttractors(const std::vector<ember::Attractor>&) override {}
    void uploadVortexes(const std::vector<ember::Vortex>&) override {}
    void uploadSprings(const std::vector<ember::Spring>&) override {}
    void uploadPalette(const std::vector<glm::vec4>& p) override {palette=p;++paletteUploads;}
    void uploadEventTemplates(const std::vector<ember::SpawnRequest>& t) override {
        if(!recordEvents) return ember::ParticleBackend::uploadEventTemplates(t);
        eventTemplates=t;++eventUploads;
    }
    void uploadLifeCurves(const ember::LifeCurvesLut& lut) override {curves=lut;++curveUploads;}
    void update(const ember::SimulationParameters& p,const ember::UpdateBatch& batch) override {
        simulation=p;requests=batch.requests;lastTotal=batch.total;
        lastDt=batch.dt;lastTime=batch.time;lastSeed=batch.seed;
        ++stats.frame;
        stats.alive=std::min(capacity,stats.alive+batch.total);
        stats.allocated=stats.alive;
    }
    void render(const ember::RenderParameters& p,const ember::RenderView& v) override {
        rendering=p;camera=v;++renders;
    }
    ember::ParticleStatistics pollStatistics() override {return stats;}
    ember::ParticleStatistics synchronizeStatistics() const override {return stats;}
    std::vector<ember::Particle> readParticles(std::uint32_t) const override {return {};}
    std::uint64_t updateSequence() const override {return stats.frame;}
    std::uint64_t droppedStatistics() const override {return 0;}
};
template<class F> void rejects(F action) {
    bool caught=false;try {action();} catch(const std::invalid_argument&) {caught=true;}
    check(caught,"expected rejection");
}
}
int main() {
    try {
        auto owned=std::make_unique<RecordingBackend>();
        auto* recording=owned.get();
        ember::ParticleSystem sys({128,7,123},std::move(owned));
        check(std::string(sys.backendName())=="recording","backend selection");
        ember::Emitter emitter;emitter.rate=100;emitter.palette={{1,0,0,1}};
        sys.addEmitter(emitter);
        ember::BurstParams burst;burst.count=5;burst.position={9,0,0};
        sys.burst(burst);sys.update(.1f);
        check(recording->requests.size()==2 && recording->lastTotal==7,"combined spawn budget");
        check(recording->requests[0].count==5 && recording->requests[0].base==0 &&
              recording->requests[1].count==2 && recording->requests[1].base==5,"burst precedence/prefix");
        check(recording->lastSeed==123 && sys.updateSequence()==1,"seed and sequence");
        check(sys.aliveCount()==7 && sys.pollStatistics().alive==7,"statistics forwarding");
        sys.emitter(0)->palette={{0,1,0,1}};
        sys.update(1.f);
        check(recording->lastDt==.1f && recording->lastSeed==124,"dt clamp/seed progression");
        check(recording->palette==sys.emitter(0)->palette && recording->paletteUploads==2,"live palette edit");
        rejects([&]{sys.update(std::numeric_limits<float>::quiet_NaN());});
        check(sys.updateSequence()==2,"invalid dt submitted work");
        sys.apply(ember::Config::fromString("[system]\ngravity=1,2,3\nforces=gravity\nsize_scale=2\n"));
        sys.update(0);
        check(recording->simulation.gravity==glm::vec3(1,2,3) &&
              recording->simulation.forceMask==1,"configuration not reflected in backend input");
        check(sys.emitter(0)->speedScale==2,"size/speed link lost");
        sys.setSpriteSheet(0,3);sys.setStreak(.2f);sys.setSpin(.4f);
        sys.render(glm::mat4(2),glm::mat4(3),320,180,55);
        check(recording->rendering.sizeScale==2 && recording->rendering.sheetCols==1 &&
              recording->rendering.sheetRows==3 && recording->rendering.spin==.4f &&
              recording->camera.width==320 && recording->camera.projection==glm::mat4(3),"render input");
        rejects([&]{sys.setGpuDriven(true);});
        rejects([&]{sys.setSortEnabled(true);});
        rejects([&]{sys.apply(ember::Config::fromString("[system]\ncapacity=64\ntexture=test.png\n"));});
        rejects([&]{sys.apply(ember::Config::fromString("[system]\ncapacity=64\nbloom=true\n"));});
        check(sys.capacity()==128 && recording->capacity==128,"unsupported effect partially applied config");
        ember::RefractionParameters refraction;refraction.enabled=true;
        rejects([&]{sys.setRefractionParameters(refraction);});
        rejects([&]{sys.setSoftParticleParameters(true);});
        sys.clearEmitters();sys.burst(burst);
        ember::ParticleSystem moved(std::move(sys));
        moved.update(0);
        check(recording->lastTotal==5 && recording->lastSeed==126,"move lost pending burst/seed");
        moved.clear();
        check(moved.aliveCount()==0 && moved.updateSequence()==4,"clear reset sequence");
        moved.burst(burst);moved.clear();moved.update(0);
        check(recording->lastTotal==0,"clear retained pending burst");
        moved.apply(ember::Config::fromString("[system]\ncapacity=64\n"));
        check(moved.capacity()==64 && recording->capacity==64,"resize dispatch");
        // Many emitter requests must obey the shared 4096-request bound even
        // when the particle budget allows more.
        moved.setMaxSpawnPerFrame(100000);
        emitter.palette.clear();emitter.rate=0;emitter.accumulator=1;
        for(unsigned i=0;i<4100;++i)moved.addEmitter(emitter);
        moved.update(0);
        check(recording->requests.size()==ember::maxSpawnRequests &&
              recording->lastTotal==ember::maxSpawnRequests,"request count bound");
        // ---- event templates: encoding, lazy upload, rejection ----------------
        {
            auto owned2=std::make_unique<RecordingBackend>();auto* rec2=owned2.get();
            ember::ParticleSystem sys2({256,128,55},std::move(owned2));
            ember::Emitter child;child.eventCount=12;child.inheritVelocity=.25f;
            const std::size_t ci=sys2.addEventEmitter(child);
            check(ci==0 && sys2.eventEmitterCount()==1,"event template registration");
            check(sys2.eventEmitter(0) && !sys2.eventEmitter(1),"event template accessor");
            ember::Emitter parent;parent.rate=0;parent.accumulator=3;
            parent.onDeath=0;sys2.addEmitter(parent);
            sys2.update(0);
            check(rec2->eventUploads==1 && rec2->eventTemplates.size()==1,"event template upload");
            check(rec2->eventTemplates[0].count==12,"event template child count");
            check(rec2->eventTemplates[0].pad2==0u,"event template self tag");
            float inherit=0;std::memcpy(&inherit,&rec2->eventTemplates[0].pad3,sizeof(inherit));
            check(inherit==.25f,"event inheritVelocity bits");
            check(!rec2->requests.empty() && (rec2->requests.back().pad2&0xFFFFu)==1u,
                  "onDeath tag packing");
            ember::Emitter bouncer;bouncer.rate=0;bouncer.accumulator=1;
            bouncer.onBounce=0;sys2.addEmitter(bouncer);
            sys2.update(0);
            check((rec2->requests.back().pad2>>16)==1u,"onBounce tag packing");
            sys2.update(0);
            check(rec2->eventUploads==1,"clean event templates re-uploaded");
            sys2.clearEventEmitters();
            check(rec2->eventUploads==2 && rec2->eventTemplates.empty(),"clear event templates");
            check(sys2.eventEmitterCount()==0,"event template count reset");
            ember::Emitter orphan;orphan.onDeath=0;
            rejects([&]{sys2.addEmitter(orphan);});
            // INI [event] parsing + two-phase (chained) reference resolution.
            const auto cfg=ember::Config::fromString(
                "[system]\ncapacity=128\n"
                "[event \"spark\"]\ncount=6\ninherit=0.5\nlife_min=0.1\nlife_max=0.2\n"
                "[event \"shell\"]\non_death=spark\n"
                "[emitter \"bomb\"]\non_death=shell\nrate=10\n");
            check(cfg.events.size()==2 && cfg.events[0].name=="spark","event section parse");
            check(cfg.events[0].eventCount==6 && cfg.events[0].inheritVelocity==.5f,"event count/inherit");
            check(cfg.events[1].onDeathName=="spark","event chaining name");
            check(cfg.emitters[0].onDeathName=="shell","emitter on_death name");
            check(cfg.findEvent("shell")==1 && cfg.findEvent("nope")==-1,"findEvent");
            check(cfg.has("event"),"event section presence");
            sys2.apply(cfg);
            check(sys2.eventEmitterCount()==2,"event templates from config");
            check(sys2.emitter(0) && sys2.emitter(0)->onDeath==1,"on_death index resolved");
            check(sys2.eventEmitter(1) && sys2.eventEmitter(1)->onDeath==0,"chained on_death resolved");
            bool unknown=false;
            try {sys2.apply(ember::Config::fromString("[emitter \"e\"]\non_death=missing\n"));}
            catch(const std::runtime_error&){unknown=true;}
            check(unknown,"unknown event reference accepted");
            // [event] palette references bind and validate like [emitter] ones.
            sys2.apply(ember::Config::fromString(
                "[palette \"hot\"]\ncolors=1,0.5,0,1|1,0,0,1\n"
                "[event \"spark\"]\ncount=4\npalette=hot\n"));
            check(sys2.eventEmitter(0) && sys2.eventEmitter(0)->palette.size()==2 &&
                  sys2.eventEmitter(0)->palette[0]==glm::vec4(1.f,.5f,0.f,1.f),
                  "event palette binding");
            bool badPal=false;
            try {sys2.apply(ember::Config::fromString("[event \"x\"]\npalette=missing\n"));}
            catch(const std::runtime_error&){badPal=true;}
            check(badPal,"unknown palette in [event] accepted");
            // Template-count overflow must fail in phase 1, before any mutation.
            std::string many;
            for(int i=0;i<65;++i) many+="[event \"e"+std::to_string(i)+"\"]\ncount=1\n";
            const auto before=sys2.eventEmitterCount();
            bool overflow=false;
            try {sys2.apply(ember::Config::fromString(many));}
            catch(const std::invalid_argument&){overflow=true;}
            check(overflow,"event template overflow accepted");
            check(sys2.eventEmitterCount()==before,"overflow apply mutated templates");
            // Default backend (no override) rejects non-empty, accepts empty.
            auto owned3=std::make_unique<RecordingBackend>();auto* rec3=owned3.get();
            rec3->recordEvents=false;
            ember::ParticleSystem sys3({64,64,1},std::move(owned3));
            ember::Emitter t;t.eventCount=4;sys3.addEventEmitter(t);
            rejects([&]{sys3.update(0);});
            sys3.clearEventEmitters(); // empty upload through the base never throws
        }
        // ---- lifecycle curves: capability gate, baking, atomic apply --------
        {
            check(!recording->caps.lifeCurves,"default curves capability");
            rejects([&]{moved.setColorOverLife({{1,1,1,1},{1,0,0,0}});});
            rejects([&]{moved.setSizeOverLife({1.f,0.f});});
            rejects([&]{moved.apply(ember::Config::fromString("[curves]\nsize=0:1, 1:0\n"));});
            check(moved.capacity()==64,"failed curves config partially applied");
            moved.setColorOverLife({});moved.setSizeOverLife({});
            check(moved.colorOverLifeKeys().empty() && moved.sizeOverLifeKeys().empty(),
                  "empty curves rejected");
            recording->caps.lifeCurves=true;
            moved.setSizeOverLife({1.f,0.f});
            check(recording->curves.mask==2u,"size curve mask");
            check(std::abs(recording->curves.size[0]-1.f)<1e-6f &&
                  std::abs(recording->curves.size[63])<1e-6f,"size curve endpoints");
            check(std::abs(recording->curves.size[31]-.5f)<.03f,"size curve midpoint");
            moved.setColorOverLife({{1,1,1,1},{1,0,0,0}});
            check((recording->curves.mask&1u)!=0u,"color curve mask");
            check(recording->curves.color[0]==glm::vec4(1,1,1,1) &&
                  recording->curves.color[63]==glm::vec4(1,0,0,0),"color curve endpoints");
            check(glm::length(recording->curves.color[31]-glm::vec4(1.f,.5f,.5f,.5f))<.05f,
                  "color curve midpoint");
            moved.setColorOverLife({});moved.setSizeOverLife({});
            check(recording->curves.mask==0u && recording->curves.size[0]==1.f &&
                  recording->curves.color[0]==glm::vec4(1.f),"curves reset to identity");
            // INI explicit empty value closes the channel through apply() too —
            // allowed with the capability on and (as a no-op close) without it.
            moved.setSizeOverLife({1.f,0.f});
            moved.apply(ember::Config::fromString("[curves]\nsize=\n"));
            check(moved.sizeOverLifeKeys().empty() && (recording->curves.mask&2u)==0u,
                  "apply empty curve value did not close the channel");
            recording->caps.lifeCurves=false;
            moved.apply(ember::Config::fromString("[curves]\nsize=\ncolor=\n"));
            recording->caps.lifeCurves=true;
        }
        rejects([&]{ember::ParticleSystem invalid({1,1,1},nullptr);});
        std::puts("backend contract: ALL PASSED (no graphics dependencies)");
    } catch(const std::exception& e) {
        std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;
    }
}
