#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../Common/ViewControls.h"
#include "../Common/MonitorInterface.h"
#include <cstdio>
#include <limits>
#include <unordered_map>

namespace {
int passed = 0, failed = 0;
#define CHECK(value) do { if (value) ++passed; else { ++failed; std::printf("FAIL line %d: %s\n", __LINE__, #value); } } while (0)
struct Fixture {
    const uint64_t base = 0x140000000, manager = 0x20000000, global = 0x21000000, owner = 0x22000000;
    const uint64_t link = 0x23000000, state = 0x24000000, map = 0x25000000, descriptor = 0x26000000;
    const uint64_t limitSlot = base + 0x6000000, nativeLimit = base + 0x3622E68;
    viewcontrols::Scene scene{0x27000000, 0x28000000, 0x29000000};
    std::unordered_map<uint64_t, uint8_t> bytes;
    int writes = 0;
    template<class T> void Put(uint64_t a, const T& v) {
        auto* data = reinterpret_cast<const uint8_t*>(&v);
        for (size_t i = 0; i < sizeof(v); ++i) bytes[a+i] = data[i];
    }
    bool Read(uint64_t a, void* out, size_t n) const {
        for (size_t i = 0; i < n; ++i) {
            const auto it = bytes.find(a+i);
            if (it == bytes.end()) return false;
            static_cast<uint8_t*>(out)[i] = it->second;
        }
        return true;
    }
    auto Reader() { return [this](uint64_t a, void* b, size_t n) { return Read(a,b,n); }; }
    auto Writer() { return [this](uint64_t a, const void* b, size_t n) {
        for (size_t i = 0; i < n; ++i) bytes[a+i] = static_cast<const uint8_t*>(b)[i];
        ++writes; return true;
    }; }
    float Zoom() { float value = 0; Read(scene.camera+viewlayout::kZoomTarget,&value,sizeof(value)); return value; }
    auto Patcher() { return [this](uint64_t address, int32_t expected, int32_t replacement) {
        int32_t before = 0;
        if (!Read(address,&before,sizeof(before)) || before != expected) return false;
        Put(address,replacement); ++writes; return true;
    }; }
    float Wheel(float delta) {
        int32_t displacement = 0; float maximum = 0;
        Read(base+viewlayout::kZoomClampRva+4,&displacement,sizeof(displacement));
        Read(uint64_t(int64_t(base+viewlayout::kZoomClampRva+8)+displacement),&maximum,sizeof(maximum));
        const float next = (std::min)((std::max)(Zoom()+delta,0.0f),maximum);
        Put(scene.camera+viewlayout::kZoomTarget,next); return next;
    }
    void Area(uint64_t world) {
        scene.world = world;
        Put(state+viewlayout::kStateWorld,world); Put(scene.camera+viewlayout::kCameraWorld,world);
        Put(map,world+viewlayout::kWorldMinimap);
    }
    Fixture() {
        using namespace viewlayout;
        Put(base+kManagerRva,manager); Put(manager+kManagerClient,scene.client);
        Put(scene.client+kClientCamera,scene.camera); Put(base+BaseOffset,global);
        Put(global+kGlobalOwner,owner); Put(owner+kOwnerState,link); Put(link,state);
        Put(global+kGlobalCamera,scene.camera); Area(scene.world);
        Put(scene.camera+kZoomTarget,1.0f); Put(scene.camera+kZoomCurrent,1.0f);
        Put(base+kZoomClampRva,kZoomClampSignature); Put(nativeLimit,1.0f);
        Put(scene.client+kClientMinimap,map); Put(map+kMinimapRender,uint64_t(0x30000000));
        Put(map+kDescriptor,descriptor); Put(descriptor+kDimensions,uint32_t(87));
        Put(descriptor+kDimensions+sizeof(uint32_t),uint32_t(99));
        Put(descriptor+kWorldDimensions,87.0f); Put(descriptor+kWorldDimensions+sizeof(float),99.0f);
        Put(descriptor+kPixels,uint64_t(0x31000000)); Put(descriptor+kPixelsEnd,uint64_t(0x31000000+87*99*4));
        Put(descriptor+kPixelsCapacity,uint64_t(0x31000000+87*99*4));
        Put(map+kRevealWalkable,uint8_t(0)); Put(map+kRevealFull,uint8_t(0)); Put(map+kRevealReset,uint8_t(0));
    }
};
void Reads() {
    Fixture f; viewcontrols::Scene scene;
    CHECK(viewcontrols::Resolve(f.base,f.Reader(),scene) && scene == f.scene);
    uint64_t map = 0; CHECK(viewcontrols::Minimap(scene,f.Reader(),map) && map == f.map);
    f.Put(f.map,uint64_t(0x40000000)); CHECK(!viewcontrols::Minimap(scene,f.Reader(),map) && map == 0);
    f.Area(f.scene.world); f.Put(f.descriptor+viewlayout::kPixelsEnd,uint64_t(0x31000004));
    CHECK(!viewcontrols::Minimap(scene,f.Reader(),map));
    f.Put(f.descriptor+viewlayout::kPixelsEnd,uint64_t(0x31000000+87*99*4));
    f.Put(f.descriptor+viewlayout::kDimensions,UINT32_MAX); CHECK(!viewcontrols::Minimap(scene,f.Reader(),map));
    f.Put(f.descriptor+viewlayout::kDimensions,uint32_t(87));
    f.Put(f.map+viewlayout::kRevealReset,uint8_t(2)); CHECK(!viewcontrols::Minimap(scene,f.Reader(),map));
    f.Put(f.global+viewlayout::kGlobalCamera,uint64_t(0x40000000));
    CHECK(!viewcontrols::Resolve(f.base,f.Reader(),scene) && !scene.camera);
}
void Limit() {
    Fixture f; viewcontrols::ZoomLimit limit;
    const auto apply = [&](float value) { return limit.Apply(f.base,f.limitSlot,f.Reader(),f.Writer(),f.Patcher(),value); };
    CHECK(limit.Matches(f.base,f.Reader()));
    CHECK(apply(3.0f) && f.Zoom() == 1.0f); // Enabling raises the cap, never forces a zoom.
    CHECK(limit.Matches(f.base,f.Reader()));
    CHECK(f.Wheel(0.5f) == 1.5f);
    CHECK(f.Wheel(5.0f) == 3.0f);
    CHECK(f.Wheel(-0.5f) == 2.5f);
    CHECK(f.Wheel(-10.0f) == 0.0f); // Preserve native lower bound.
    CHECK(viewcontrols::ClampTarget(f.base,f.scene,f.Reader(),f.Writer(),3.0f) && f.Zoom()==0.0f);
    CHECK(f.Wheel(2.5f) == 2.5f);
    CHECK(apply(1.35f) && f.Zoom()==2.5f);
    CHECK(viewcontrols::ClampTarget(f.base,f.scene,f.Reader(),f.Writer(),1.35f) && f.Zoom()==1.35f);
    const auto before=f.writes;
    f.Put(f.scene.camera+viewlayout::kZoomTarget,0.75f);
    CHECK(viewcontrols::ClampTarget(f.base,f.scene,f.Reader(),f.Writer(),1.35f) && f.Zoom()==0.75f && f.writes==before);
    f.Area(f.scene.world+0x10000);
    CHECK(limit.Matches(f.base,f.Reader()) && f.Wheel(5.0f)==1.35f);
    float shared=0; f.Read(f.nativeLimit,&shared,sizeof(shared)); CHECK(shared==1.0f);
    CHECK(limit.Restore(f.Reader(),f.Patcher()));
    CHECK(f.Wheel(0.1f)==1.0f && limit.Matches(f.base,f.Reader()));
    CHECK(limit.Restore(f.Reader(),f.Patcher()));
    CHECK(apply(2.0f));
    CHECK(!limit.Restore(f.Reader(),[](uint64_t,int32_t,int32_t) { return false; }));
    CHECK(limit.Matches(f.base,f.Reader()));
    CHECK(limit.Restore(f.Reader(),f.Patcher()));
    CHECK(!apply(99.0f));
    CHECK(!limit.Apply(f.base,f.limitSlot+1,f.Reader(),f.Writer(),f.Patcher(),2.0f));
    CHECK(!limit.Apply(f.base,f.base+0x90000000,f.Reader(),f.Writer(),f.Patcher(),2.0f));
    // Unknown modifications are never overwritten, including during restore.
    CHECK(apply(2.0f)); f.Put(f.base+viewlayout::kZoomClampRva+4,int32_t(42));
    const auto writes=f.writes;
    CHECK(!limit.Matches(f.base,f.Reader()) && !apply(3.0f));
    CHECK(!limit.Restore(f.Reader(),f.Patcher()) && f.writes==writes);
}
void FailedLimit() {
    Fixture f; viewcontrols::ZoomLimit limit;
    CHECK(!limit.Apply(f.base,f.limitSlot,f.Reader(),[](uint64_t,const void*,size_t) { return false; },f.Patcher(),2.0f));
    CHECK(limit.Matches(f.base,f.Reader()));
    CHECK(!limit.Apply(f.base,f.limitSlot,f.Reader(),f.Writer(),[](uint64_t,int32_t,int32_t) { return false; },2.0f));
    CHECK(limit.Restore(f.Reader(),f.Patcher()));
    // A failed cache/protection step after writing still leaves tracked ownership.
    CHECK(!limit.Apply(f.base,f.limitSlot,f.Reader(),f.Writer(),[&](uint64_t a,int32_t old,int32_t next) {
        f.Patcher()(a,old,next); return false;
    },2.0f));
    CHECK(limit.Matches(f.base,f.Reader()));
    CHECK(limit.Restore(f.Reader(),f.Patcher()));
    f.Put(f.global+viewlayout::kGlobalCamera,uint64_t(0x40000000));
    const auto before=f.writes;
    CHECK(!viewcontrols::ClampTarget(f.base,f.scene,f.Reader(),f.Writer(),2.0f) && f.writes==before);
    f.Put(f.base+viewlayout::kZoomClampRva,uint8_t(0x90));
    CHECK(!limit.Matches(f.base,f.Reader()));
    CHECK(!limit.Apply(f.base,f.limitSlot,f.Reader(),f.Writer(),f.Patcher(),2.0f));
}
void Policy() {
    using viewcontrols::Config;
    CHECK(viewcontrols::Valid(Config{}));
    CHECK(viewcontrols::Valid(Config{1,0.25f}) && viewcontrols::Valid(Config{1,3.0f}));
    CHECK(!viewcontrols::Valid(Config{2,1.0f}));
    CHECK(!viewcontrols::Valid(Config{1,0.0f}) && !viewcontrols::Valid(Config{1,3.01f}));
    CHECK(!viewcontrols::Valid(Config{1,std::numeric_limits<float>::quiet_NaN()}));
    CHECK(!viewcontrols::Valid(Config{1,std::numeric_limits<float>::infinity()}));
    monitorabi::Info info;
    CHECK(monitorabi::Compatible(info)); info.capabilities &= ~monitorabi::kViewControls;
    CHECK(!monitorabi::Compatible(info));
    info = {}; info.capabilities &= ~monitorabi::kWheelZoomLimit; CHECK(!monitorabi::Compatible(info));
    CHECK(uint32_t(packet::Operation::SummonPortal)==12 && uint32_t(packet::Operation::ConfigureView)==13);
    CHECK(!packet::Sends(packet::Operation::ConfigureView) && !packet::Sends(packet::Operation::RevealMinimap));
}
}
int main() {
    Policy(); Reads(); Limit(); FailedLimit();
    std::printf("ViewControls: %d passed, %d failed\n",passed,failed);
    return failed ? 1 : 0;
}
