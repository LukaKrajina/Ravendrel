// 真实动捕设备驱动单元测试
#include "test_framework.hpp"
#include "qcdrc/device.hpp"

using namespace rvdlrsp::qcdrc;

QTEST(external_mocap_push) {
    auto mocap = DeviceFactory::create_mocap(true);
    QCHECK(mocap->name() == "ExternalMocap");

    // 注入骨架
    Skeleton s;
    s.joints = {{"pelvis", 0, 1, 0}, {"head", 0, 2, 0}};
    auto *ext = static_cast<ExternalMocap *>(mocap.get());
    ext->push_skeleton(s);

    Skeleton got = mocap->update();
    QCHECK(got.joints.size() == 2);
    QCHECK(got.joints[0].name == "pelvis");
    QCHECK(got.joints[1].name == "head");
}

QTEST(sim_mocap_update) {
    auto mocap = DeviceFactory::create_mocap(false);
    QCHECK(mocap->name() == "SimMocap");
    Skeleton s = mocap->update();
    QCHECK(!s.joints.empty());
}

QTEST(mocap_estimate_from_frame) {
    auto mocap = DeviceFactory::create_mocap(true);
    RgbFrame f;
    f.width = 64; f.height = 48;
    // 未注入骨架时 estimate 返回空
    Skeleton s = mocap->estimate(f);
    QCHECK(s.joints.empty());

    // 注入后 estimate 应返回注入的骨架
    Skeleton in;
    in.joints = {{"pelvis", 0, 0, 0}};
    static_cast<ExternalMocap *>(mocap.get())->push_skeleton(in);
    Skeleton s2 = mocap->estimate(f);
    QCHECK(s2.joints.size() == 1);
}