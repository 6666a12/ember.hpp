#pragma once

// Included after regression helpers. Run in both the modular and amalgamated
// consumers: numeric checks for physics, pixel/depth checks for visual effects.
namespace regression {
inline void effectsRegressions() {
    using namespace ember;
    unsigned failures=0;
    auto test=[&](const char* name,auto body) {
        try { body();glClean(name);std::printf("effect: %s PASSED\n",name); }
        catch(const std::exception& e) {
            ++failures;std::printf("effect: %s FAILED: %s\n",name,e.what());
            glBlendEquation(GL_FUNC_ADD);glDisable(GL_CULL_FACE);glDisable(GL_SCISSOR_TEST);
        }
    };
    auto near=[](glm::vec3 a,glm::vec3 b) {return glm::length(a-b)<2e-4f;};
    auto emitter=[](glm::vec3 position,glm::vec3 velocity) {
        Emitter e;e.position=position;e.baseVelocity=velocity;
        e.speedMin=e.speedMax=glm::length(velocity);e.spread=0;
        e.lifeMin=e.lifeMax=10;e.rate=0;e.accumulator=1;
        e.sizeMin=e.sizeMax=.8f;return e;
    };
    // Independent analytic results, with each force enabled in isolation.
    auto step=[&](bool gpu,glm::vec3 position,glm::vec3 velocity,auto configure) {
        ParticleSystem sys({8,8,123});sys.setForceMask(0);sys.setGpuDriven(gpu);
        sys.addEmitter(emitter(position,velocity));sys.update(0);configure(sys);
        sys.update(.05f);return particles(sys)[0];
    };
    for(bool gpu:{false,true}) {
        test(gpu?"all forces GPU scheduling":"all forces sync",[&] {
            auto force=[&](glm::vec3 acc,auto configure) {
                auto p=step(gpu,{1,2,-3},{0,2,0},configure);
                const auto v=glm::vec3(0,2,0)+acc*.05f;
                require(near(glm::vec3(p.vel),v) && near(glm::vec3(p.pos),glm::vec3(1,2,-3)+v*.05f),"force/integrator numeric mismatch");
            };
            force({0,-4,0},[](auto& s){s.setGravity({0,-4,0});s.enableForce(Force::Gravity);});
            force({0,-1,0},[](auto& s){s.setDrag(.5f);s.enableForce(Force::Drag);});
            force({0,-2,0},[](auto& s){s.setDrag(.5f);s.setDragMode(DragMode::Quadratic);s.enableForce(Force::Drag);});
            force({3,0,0},[](auto& s){s.setWind({3,0,0});s.enableForce(Force::Wind);});
            force({2,0,0},[](auto& s){s.setAttractors({{{3,2,-3},8}});s.enableForce(Force::Attractors);});
            force({-2,0,0},[](auto& s){s.setAttractors({{{3,2,-3},-8}});s.enableForce(Force::Attractors);});
            force({0,0,1},[](auto& s){s.setVortexes({{{0,2,-3,1},{0,1,0,2}}});});
            force({2,-1,0},[](auto& s){s.setSprings({{{2,2,-3},2,.5f}});});
            force({-2,0,0},[](auto& s){s.setNoiseWind({1,0,0},2,0,0);});
            force({2*std::sin(.5f),0,0},[](auto& s){s.setWave({1,0,0},{0,0,0},2,10);});
            auto p=step(gpu,{1,2,-3},{0,2,0},[](auto& s){s.setTurbulence(3);s.enableForce(Force::Turbulence);});
            require(glm::length(glm::vec3(p.vel)-glm::vec3(0,2,0))>.001f,"turbulence has no effect");
            require(glm::length(glm::vec3(p.vel)-glm::vec3(0,2,0))<.27f,"turbulence exceeds bounded acceleration");
        });
        test(gpu?"signed amplitudes GPU":"signed amplitudes sync",[&] {
            auto p=step(gpu,{0,0,-2},{0,0,0},[](auto& s){s.setNoiseWind({1,0,0},-2,0,0);});
            require(near(glm::vec3(p.vel),{.1f,0,0}),"negative noise wind silently disabled");
            p=step(gpu,{0,0,-2},{0,0,0},[](auto& s){s.setWave({1,0,0},{0,0,0},-2,10);});
            require(near(glm::vec3(p.vel),{-.1f*std::sin(.5f),0,0}),"negative wave silently disabled");
        });
        test(gpu?"boundary GPU":"boundary sync",[&] {
            auto p=step(gpu,{0,-1,-2},{0,2,0},[](auto& s){s.setBoundary(BoundaryMode::Bounce,0,.5f);});
            require(p.pos.y==0 && p.vel.y==2,"upward particle reflected downward");
            p=step(gpu,{0,.05f,-2},{0,-2,0},[](auto& s){s.setBoundary(BoundaryMode::Bounce,0,.5f);});
            require(p.pos.y==0 && p.vel.y==1,"downward bounce restitution");
            ParticleSystem sys({8,8,1});sys.setGpuDriven(gpu);sys.setForceMask(0);
            sys.setBoundary(BoundaryMode::Kill,0);sys.burst(burst(0,-1));sys.update(0);sys.update(.01f);
            require(sys.aliveCount()==0,"kill boundary did not retire particle");
        });
        test(gpu?"velocity guard GPU":"velocity guard sync",[&] {
            auto p=step(gpu,{0,0,-2},{0,0,0},[](auto& s){s.setWind({1e7f,0,0});s.enableForce(Force::Wind);});
            require(p.vel.x==500 && std::abs(p.pos.x-25)<1e-4f,"velocity clamped after position exploded");
        });
        test(gpu?"emission shapes and attributes GPU":"emission shapes and attributes sync",[&] {
            for(auto shape:{Emitter::Shape::Point,Emitter::Shape::Box,Emitter::Shape::Sphere,Emitter::Shape::Cone}) {
                ParticleSystem sys({256,256,123});sys.setGpuDriven(gpu);sys.setForceMask(0);sys.setSizeScale(2);
                auto e=emitter({0,0,-2},{0,2,0});e.accumulator=128;e.shape=shape;
                e.extents={.3f,.4f,.5f};e.radius=.5f;e.axis={1,2,3};
                e.coneAngle=shape==Emitter::Shape::Cone?30.f:0.f;
                e.sizeMin=.2f;e.sizeMax=.6f;e.speedSizeLink=.5f;
                e.palette={{1,0,0,1},{0,1,0,.5f}};e.hasFadeColor=true;e.fadeColorMin=e.fadeColorMax={.1f,.2f,.3f};
                sys.addEmitter(e);sys.update(0);auto ps=particles(sys);
                require(ps.size()==128,"emitter request population");
                for(const auto& p:ps) {
                    const auto d=glm::vec3(p.pos)-e.position;
                    if(shape==Emitter::Shape::Point)require(glm::length(d)<1e-5f,"point origin");
                    if(shape==Emitter::Shape::Box)require(std::abs(d.x)<=.3f && std::abs(d.y)<=.4f && std::abs(d.z)<=.5f,"box bounds");
                    if(shape==Emitter::Shape::Sphere)require(glm::length(d)<=.50001f,"sphere bounds");
                    if(shape==Emitter::Shape::Cone) {
                        const auto axis=glm::normalize(e.axis);
                        require(glm::length(d)<=.50001f && std::abs(glm::dot(d,axis))<1e-5f,"cone disc bounds/orientation");
                        require(glm::dot(glm::normalize(glm::vec3(p.vel)),axis)>=std::cos(glm::radians(30.f))-1e-5f,"cone velocity bounds");
                    }
                    require(p.pos.w>=.2f && p.pos.w<=.6f,"size range");
                    require(std::abs(glm::length(glm::vec3(p.vel))-4*(1+.5f*(p.pos.w/.4f-1)))<1e-4f,"size/speed link");
                    require(p.color==e.palette[0] || p.color==e.palette[1],"palette selection");
                    require(near(glm::vec3(p.life.y,p.life.z,p.life.w),e.fadeColorMin),"fade target encoding");
                }
            }
        });
    }
    const glm::mat4 view(1);
    const auto proj=glm::perspective(glm::radians(50.f),1.f,.1f,100.f);
    auto depthAt=[&](float z){auto p=proj*glm::vec4(0,0,z,1);return (p.z/p.w+1)*.5f;};
    test("projection clipping",[&] {
        Target target;ParticleSystem sys({8,8,123});sys.setForceMask(0);
        auto b=burst(0,0,-.005f);b.sizeMin=b.sizeMax=.001f;sys.burst(b);sys.update(0);
        target.clear();sys.render(view,glm::perspective(glm::radians(50.f),1.f,.001f,1.f),64,64,50);
        require(sum(target.pixels())>0,"hardcoded near plane hid valid perspective particle");
        sys.clear();b=burst(0,0,.5f);sys.burst(b);sys.update(0);
        target.clear();sys.render(view,glm::ortho(-1.f,1.f,-1.f,1.f,-1.f,1.f),64,64,50);
        require(sum(target.pixels())>0,"view-Z culling rejected valid orthographic particle");
    });
    test("negative spin",[&] {
        Target target;ParticleSystem sys({8,8,123});sys.setForceMask(0);
        sys.setSpriteTexture("sprites/test.png");sys.burst(burst());sys.update(0);
        sys.update(.1f); // slot zero's static angle is zero; compare at nonzero age
        auto draw=[&]{target.clear();sys.render(view,proj,64,64,50);return target.pixels();};
        sys.setSpin(0);auto still=draw();sys.setSpin(-2);auto rotated=draw();
        require(still!=rotated,"negative spin ignored");
        sys.update(.1f);require(draw()!=rotated,"negative spin not animated");
    });
    test("streak and color fade",[&] {
        Target target;ParticleSystem sys({8,8,123});sys.setForceMask(0);sys.setUseSprite(false);
        auto e=emitter({0,0,-2},{2,0,0});e.lifeMin=e.lifeMax=1;
        e.colorMin=e.colorMax={1,0,0,1};e.hasFadeColor=true;e.fadeColorMin=e.fadeColorMax={0,1,0};
        sys.addEmitter(e);sys.update(0);
        auto draw=[&]{target.clear();sys.render(view,proj,64,64,50);return target.pixels();};
        auto initial=draw();require(sum(initial)>0 && sum(initial,1)==0,"initial particle color");
        sys.setStreak(.5f);auto stretched=draw();require(sum(stretched)>sum(initial)*1.5,"streak did not stretch along velocity");
        sys.setStreak(0);
        for(int i=0;i<5;++i)sys.update(.1f);
        auto faded=draw();require(sum(faded,1)>0 && sum(faded)<sum(initial),"age color/alpha fade missing");
    });
    test("blend equation and culling",[&] {
        Target target;ParticleSystem sys({8,8,123});sys.setForceMask(0);
        auto b=burst();b.colorMin=b.colorMax={4,2,1,1};sys.burst(b);sys.update(0);
        for(bool bloom:{false,true}) {
            sys.setBloom(bloom);target.clear();glBlendEquation(GL_FUNC_ADD);glDisable(GL_CULL_FACE);
            sys.render(view,proj,64,64,50);auto reference=target.pixels();
            target.clear();glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
            sys.render(view,proj,64,64,50);
            require(target.pixels()==reference,"host blend equation changed particle/Bloom result");
            target.clear();glEnable(GL_CULL_FACE);glCullFace(GL_FRONT_AND_BACK);
            sys.render(view,proj,64,64,50);
            require(target.pixels()==reference,"host face culling hid billboard/fullscreen pass");
            require(glIsEnabled(GL_CULL_FACE),"cull state not restored");
            glDisable(GL_CULL_FACE);glCullFace(GL_BACK);
        }
    });
    test("soft particles and depth writes",[&] {
        Target target;Texture depth;depth.uploadDepth(1,1);
        float d=depthAt(-1);glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&d);
        ParticleSystem sys({8,8,123});sys.setForceMask(0);sys.burst(burst());sys.update(0);
        sys.setDepthTest(true);sys.setDepthWrite(true);setOpenGLSoftParticles(sys,true,depth.id(),.5f);
        target.clear();sys.render(view,proj,64,64,50);
        require(sum(target.pixels())==0,"occluded soft particle remains visible");
        require(target.centerDepth()==1,"invisible soft particle wrote depth");
        bool rejected=false;
        try {setOpenGLSoftParticles(sys,true,depth.id(),0);}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"zero soft radius accepted");
        rejected=false;
        try {sys.apply(Config::fromString("[system]\ncapacity=4\nsoft_radius=-1\n"));}catch(const std::invalid_argument&){rejected=true;}
        require(rejected && sys.capacity()==8,"invalid soft radius partially applied config");
    });
    test("Bloom with host scissor",[&] {
        Target target;ParticleSystem sys({8,8,123});sys.setForceMask(0);sys.setBloom(true);
        auto b=burst();b.sizeMin=b.sizeMax=.3f;b.colorMin=b.colorMax={32,32,32,1};
        sys.burst(b);sys.update(0);target.clear();sys.render(view,proj,64,64,50);
        auto reference=target.pixels();
        require(reference[(32*64+40)*4]>0,"Bloom scissor fixture has no halo");
        // Prime intermediates with a different frame so a clipped clear/draw
        // cannot accidentally reuse exactly the expected previous image.
        sys.clear();b.position.x=1;sys.burst(b);sys.update(0);
        target.clear();sys.render(view,proj,64,64,50);
        sys.clear();b.position.x=0;sys.burst(b);sys.update(0);
        target.clear();glScissor(32,0,32,64);glEnable(GL_SCISSOR_TEST);
        sys.render(view,proj,64,64,50);auto clipped=target.pixels();
        const bool restored=glIsEnabled(GL_SCISSOR_TEST);glDisable(GL_SCISSOR_TEST);
        require(restored,"render lost host scissor enable");
        for(int y=0;y<64;++y)for(int x=0;x<64;++x) {
            const auto i=(y*64+x)*4;
            require(clipped[i]==(x>=32?reference[i]:0),"scissor leaked into Bloom intermediates");
        }
    });
    test("texture reset through config",[&] {
        Target target;ParticleSystem sys({8,8,123});sys.setForceMask(0);sys.burst(burst());sys.update(0);
        auto draw=[&]{target.clear();sys.render(view,proj,64,64,50);return target.pixels();};
        auto builtin=draw();sys.setSpriteTexture("sprites/test.png");
        require(draw()!=builtin,"sprite fixture not distinct");
        sys.apply(Config::fromString("[system]\ntexture=\n"));
        require(draw()==builtin,"empty texture config did not restore built-in sprite");
    });
    test("sprite sheet frames and filtering",[&] {
        Target target;ParticleSystem sys({8,8,123});sys.setForceMask(0);
        auto b=burst();b.lifeMin=b.lifeMax=1;sys.burst(b);sys.update(0);
        sys.render(view,proj,64,64,50); // sprite now bound on texture unit zero
        const unsigned char atlas[]={
            255,0,0,255, 255,0,0,255, 0,255,0,255, 0,255,0,255,
            255,0,0,255, 255,0,0,255, 0,255,0,255, 0,255,0,255};
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,4,2,0,GL_RGBA,GL_UNSIGNED_BYTE,atlas);
        sys.setSpriteSheet(2,1);
        auto draw=[&]{target.clear();sys.render(view,proj,64,64,50);return target.pixels();};
        auto first=draw();
        require(sum(first)>0 && sum(first,1)==0,"sprite sheet frame bleeds from its neighbor");
        for(int i=0;i<6;++i)sys.update(.1f);
        auto second=draw();
        require(sum(second)==0 && sum(second,1)>0,"sprite sheet age/frame selection");
    });
    test("refraction index",[&] {
        Target target;Texture scene,depth;
        std::vector<unsigned char> gradient(64*64*4);
        for(int y=0;y<64;++y)for(int x=0;x<64;++x) {
            int i=(y*64+x)*4;gradient[i]=x*4;gradient[i+1]=y*4;gradient[i+3]=255;
        }
        scene.uploadRGBA8(64,64,gradient.data());depth.uploadDepth(1,1);
        float d=depthAt(-4);glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&d);
        ParticleSystem sys({8,8,123});sys.setForceMask(0);auto b=burst();b.refractive=true;
        sys.burst(b);sys.update(0);
        RefractionSettings r;r.enabled=true;r.sceneColorTex=scene.id();r.sceneDepthTex=depth.id();
        r.mode=1;r.absorption=1;r.fresnel=0;r.strength=.5f;
        auto draw=[&]{setOpenGLRefraction(sys,r);target.clear();sys.render(view,proj,64,64,50);return target.pixels();};
        r.ior=1;auto air=draw();r.ior=1.5f;auto glass=draw();
        require(glass!=air,"refraction ior has no effect");
        r.strength=0;require(draw()==air,"ior=1 still bends light");
    });
    test("refraction opacity and lifetime",[&] {
        Target target;Texture scene;solid(scene,255,255,255);
        ParticleSystem sys({8,8,123});sys.setForceMask(0);
        RefractionSettings r;r.enabled=true;r.sceneColorTex=scene.id();r.tint={0,1,0};r.absorption=1;r.fresnel=0;r.shape=1;
        setOpenGLRefraction(sys,r);
        auto draw=[&]{target.clear();sys.render(view,proj,64,64,50);return target.pixels();};
        auto b=burst();b.refractive=true;b.colorMin=b.colorMax={1,1,1,0};sys.burst(b);sys.update(0);
        require(sum(draw(),1)==0,"zero-alpha refraction overwrote the host");
        sys.clear();b.lifeMin=b.lifeMax=.2f;b.colorMin=b.colorMax={1,1,1,1};sys.burst(b);sys.update(0);
        auto full=draw();sys.update(.1f);auto faded=draw();
        require(sum(faded)>sum(full)+100,"refractive particle did not fade toward the scene before death");
    });
    test("refraction depth policy",[&] {
        Target target;Texture scene;solid(scene,0,255,0);
        ParticleSystem sys({8,8,123});sys.setForceMask(0);auto b=burst();b.refractive=true;
        sys.burst(b);sys.update(0);sys.setDepthTest(true);sys.setDepthWrite(true);
        RefractionSettings r;r.enabled=true;r.sceneColorTex=scene.id();r.absorption=1;r.fresnel=0;r.shape=1;
        setOpenGLRefraction(sys,r);target.clear(depthAt(-1));sys.render(view,proj,64,64,50);
        require(sum(target.pixels(),1)==0,"refraction ignored explicit depth test through foreground");
        target.clear();sys.render(view,proj,64,64,50);
        require(sum(target.pixels(),1)>0 && std::abs(target.centerDepth()-depthAt(-2))<1e-5f,"refraction ignored explicit depth writes");
    });
    test("refraction appearance controls",[&] {
        Target target;Texture scene;std::vector<unsigned char> pixels(64*64*4);
        for(int y=0;y<64;++y)for(int x=0;x<64;++x) {
            const auto i=(y*64+x)*4;pixels[i]=x*3;pixels[i+1]=y*3;pixels[i+2]=(x+y)*2;pixels[i+3]=255;
        }
        scene.uploadRGBA8(64,64,pixels.data());
        ParticleSystem sys({8,8,123});sys.setForceMask(0);auto b=burst();b.refractive=true;sys.burst(b);sys.update(0);
        RefractionSettings base;base.enabled=true;base.sceneColorTex=scene.id();base.shape=1;
        base.strength=.3f;base.absorption=.8f;base.fresnel=0;base.lightDir={-1,-1,.2f};
        auto draw=[&](const RefractionSettings& r){setOpenGLRefraction(sys,r);target.clear();sys.render(view,proj,64,64,50);return target.pixels();};
        const auto plain=draw(base);
        auto r=base;r.tint={.6f,.8f,.3f};require(draw(r)!=plain,"refraction tint ignored");
        r=base;r.absorption=.2f;require(draw(r)!=plain,"refraction absorption ignored");
        r=base;r.chroma=.5f;require(draw(r)!=plain,"refraction dispersion ignored");
        r=base;r.specular=1;require(draw(r)!=plain,"refraction specular ignored");
        r=base;r.fresnel=2;auto rim=draw(r);require(rim!=plain,"refraction fresnel ignored");
        r.dome=2;require(draw(r)!=rim,"refraction dome ignored");
        r=base;r.mode=2;require(draw(r)!=plain,"heat noise mode ignored");
        r=base;r.shape=0;require(draw(r)!=plain,"refraction mask mode ignored");
        r=base;r.ior=0;bool rejected=false;
        try {setOpenGLRefraction(sys,r);}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"invalid refraction index accepted");
    });
    test("lifecycle curves",[&] {
        Target target;ParticleSystem sys({8,8,123});sys.setForceMask(0);sys.setUseSprite(false);
        auto e=emitter({0,0,-2},{0,0,0});
        e.lifeMin=e.lifeMax=1;e.sizeMin=e.sizeMax=1;e.colorMin=e.colorMax={1,1,1,1};
        sys.addEmitter(e);sys.update(0);
        auto draw=[&]{target.clear();sys.render(view,proj,64,64,50);return target.pixels();};
        const auto reference=draw();
        require(sum(reference)>0,"curve fixture has no particle");
        // size-over-life shrinks coverage monotonically with age.
        sys.setSizeOverLife({1.f,0.f});
        auto size0=draw();
        for(int i=0;i<5;++i)sys.update(.1f);auto size1=draw();
        for(int i=0;i<4;++i)sys.update(.1f);auto size2=draw();
        require(sum(size0)>sum(size1) && sum(size1)>sum(size2),"size-over-life did not shrink coverage");
        sys.update(.1f); // age crosses the lifetime: coverage drops to zero
        require(sum(draw())==0,"size-over-life particle did not die out");
        // color-over-life fades the red channel toward the end of life.
        ParticleSystem c({8,8,123});c.setForceMask(0);c.setUseSprite(false);
        auto ce=emitter({0,0,-2},{0,0,0});
        ce.lifeMin=ce.lifeMax=1;ce.sizeMin=ce.sizeMax=1;ce.colorMin=ce.colorMax={1,1,1,1};
        c.addEmitter(ce);c.update(0);
        auto cdraw=[&]{target.clear();c.render(view,proj,64,64,50);return target.pixels();};
        const auto cbase=cdraw();
        c.setColorOverLife({{1,1,1,1},{0,1,1,1}});
        for(int i=0;i<9;++i)c.update(.1f);
        require(sum(cdraw(),0)<sum(cbase,0)/2,"color-over-life did not fade the red channel");
        // Clearing both channels renders bit-identically to never setting them.
        c.setColorOverLife({});c.setSizeOverLife({});
        c.clear();c.emitter(0)->accumulator=1.f;c.update(0);
        require(cdraw()==cbase,"cleared curves did not restore the exact baseline");
    });
    if(failures)throw std::runtime_error("effect logic regressions failed: "+std::to_string(failures));
}
} // namespace regression
