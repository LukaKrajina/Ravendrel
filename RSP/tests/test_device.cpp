// 设备抽象单元测试
#include "test_framework.hpp"
#include "qcdrc/device.hpp"

using namespace rvdlrsp::qcdrc;

QTEST(device_factory_camera) {
    auto cam = DeviceFactory::create_camera(false);
    QCHECK(cam != nullptr);
    QCHECK(cam->is_open());
    RgbFrame f = cam->capture();
    QCHECK(f.width == 640);
    QCHECK(cam->name() == "SimCamera");
}

QTEST(device_factory_bionic_eye) {
    auto cam = DeviceFactory::create_camera(CameraKind::Bionic);
    QCHECK(cam != nullptr);
    QCHECK(cam->is_open());
    QCHECK(cam->name() == "BionicEyeCamera");
    RgbFrame f = cam->capture();
    QCHECK(f.width == 640);
    QCHECK(f.height == 480);
    QCHECK(f.pixels.size() == 640u * 480u * 3u);
}

QTEST(device_factory_mocap) {
    auto mocap = DeviceFactory::create_mocap(false);
    QCHECK(mocap != nullptr);
    RgbFrame f;
    f.width = 640; f.height = 480;
    Skeleton s = mocap->estimate(f);
    QCHECK(!s.joints.empty());
    QCHECK(mocap->name() == "SimMocap");
}
