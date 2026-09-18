#pragma once

namespace regression {
inline void streakRegressions() {
    using namespace ember;
    const glm::mat4 identity(1);
    const auto perspective=glm::perspective(glm::radians(60.f),1.f,.01f,100.f);
    const auto orthographic=glm::ortho(-2.f,2.f,-2.f,2.f,.01f,100.f);
    constexpr int width=128;
    unsigned failures=0;
    auto test=[&](const char* name,auto body) {
        try {body();glClean(name);std::printf("streak: %s PASSED\n",name);}
        catch(const std::exception& e) {++failures;std::printf("streak: %s FAILED: %s\n",name,e.what());}
    };
    auto image=[&](glm::vec3 position,glm::vec3 velocity,float k,float spin,
                   const glm::mat4& projection,bool refractive=false,bool gpu=false,
                   const glm::mat4& view=glm::mat4(1),bool asymmetric=false) {
        Target target(width);ParticleSystem sys({4,4,123});Texture scene;
        sys.setForceMask(0);sys.setGpuDriven(gpu);
        Emitter e;e.position=position;e.baseVelocity=glm::length(velocity)>0?glm::normalize(velocity):glm::vec3(0);
        e.speedMin=e.speedMax=glm::length(velocity);e.rate=0;e.accumulator=1;
        e.sizeMin=e.sizeMax=.3f;e.lifeMin=e.lifeMax=10;e.refractive=refractive;
        sys.addEmitter(e);sys.update(0);
        // Age without displacement: spin remains observable at the same origin.
        auto p=sys.readParticles()[0];p.vel.w=.25f;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER,boundBuffer(1));glBufferSubData(GL_SHADER_STORAGE_BUFFER,0,sizeof(p),&p);
        sys.setUseSprite(asymmetric);sys.setStreak(k);sys.setSpin(spin);
        if(refractive) {
            std::vector<unsigned char> gradient(width*width*4);
            for(int y=0;y<width;++y)for(int x=0;x<width;++x) {
                const auto i=(y*width+x)*4;gradient[i]=x*2;gradient[i+1]=y*2;gradient[i+3]=255;
            }
            scene.uploadRGBA8(width,width,gradient.data());
            RefractionSettings r;r.enabled=true;r.sceneColorTex=scene.id();r.shape=1;r.absorption=1;
            r.strength=.3f;r.fresnel=1;r.dome=.7f;setOpenGLRefraction(sys,r);
        }
        if(asymmetric) {
            sys.render(view,projection,width,width,60); // binds the system's sprite on unit 0
            const unsigned char texels[]={255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,texels);
        }
        target.clear();sys.render(view,projection,width,width,60);return target.pixels();
    };
    auto bounds=[](const auto& pixels) {
        glm::ivec4 b(width,width,-1,-1);
        for(int y=0;y<width;++y)for(int x=0;x<width;++x)if(pixels[(y*width+x)*4]>8) {
            b.x=std::min(b.x,x);b.y=std::min(b.y,y);b.z=std::max(b.z,x);b.w=std::max(b.w,y);
        }
        require(b.z>=b.x,"streak fixture not visible");return b;
    };
    auto close=[](const auto& a,const auto& b) {
        unsigned error=0;for(std::size_t i=0;i<a.size();++i)error+=unsigned(std::abs(int(a[i])-int(b[i])));
        return error<80;
    };
    test("lateral direction and centered extent",[&] {
        for(bool gpu:{false,true}) {
            auto plain=bounds(image({0,0,-3},{2,0,0},0,0,perspective,false,gpu));
            auto horizontal=bounds(image({0,0,-3},{2,0,0},1,0,perspective,false,gpu));
            auto vertical=bounds(image({0,0,-3},{0,2,0},1,0,perspective,false,gpu));
            require(horizontal.z-horizontal.x>2*(plain.z-plain.x),"lateral streak missing");
            require(horizontal.w-horizontal.y==plain.w-plain.y,"streak changed width");
            require(vertical.w-vertical.y==horizontal.z-horizontal.x,"vertical streak length");
            require(std::abs(horizontal.x+horizontal.z-(width-1))<=1,"centered stretch drifted");
        }
    });
    test("off-axis perspective depth motion",[&] {
        const auto plain=bounds(image({1,0,-3},{0,0,6},0,0,perspective));
        const auto moving=bounds(image({1,0,-3},{0,0,6},1,0,perspective));
        require(moving.z-moving.x>2*(plain.z-plain.x),"perspective screen motion from depth velocity ignored");
    });
    test("view-ray motion and orthographic projection",[&] {
        // Movement along a camera ray changes apparent size, not center UV.
        auto reference=image({1,0,-3},{-1,0,3},0,0,perspective);
        require(close(reference,image({1,0,-3},{-1,0,3},1,0,perspective)),"radial motion generated a false lateral streak");
        reference=image({1,0,-3},{0,0,6},0,0,orthographic);
        require(close(reference,image({1,0,-3},{0,0,6},1,0,orthographic)),"orthographic depth motion generated a false streak");
    });
    test("near-zero speed orientation",[&] {
        const auto stopped=image({0,0,-3},{0,0,0},1,0,perspective,false,false,identity,true);
        const auto slow=image({0,0,-3},{0,1e-5f,0},1,0,perspective,false,false,identity,true);
        require(close(stopped,slow),"tiny velocity rotated the entire sprite");
        const auto spinning=image({0,0,-3},{0,0,0},0,2,perspective,false,false,identity,true);
        require(close(spinning,image({0,0,-3},{0,0,0},1,2,perspective,false,false,identity,true)),"zero-length streak suppressed ordinary spin");
    });
    test("refraction normal follows streak orientation",[&] {
        const auto first=image({0,0,-3},{0,2,0},1,0,perspective,true);
        const auto second=image({0,0,-3},{0,2,0},1,3,perspective,true);
        require(close(first,second),"spin changed facet lighting while streak fixed the geometry");
    });
    test("camera rotation",[&] {
        const auto view=glm::rotate(identity,glm::radians(90.f),glm::vec3(0,0,1));
        const auto turned=bounds(image({0,0,-3},{2,0,0},1,0,perspective,false,false,view));
        require(turned.w-turned.y>2*(turned.z-turned.x),"streak direction is not in camera coordinates");
    });
    if(failures)throw std::runtime_error("streak regressions failed: "+std::to_string(failures));
}
} // namespace regression
