#include <QtTest/QtTest>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <cmath>

#include "project_store.h"
#include "simulation_config.h"

using namespace rvdlrsp::gui;

class TestProjectStore : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void test_roundtrip();
    void test_defaults();
    void test_path_sanitize();
    void cleanupTestCase();
};

void TestProjectStore::initTestCase()
{
    // 确保项目目录存在（test mode 下指向临时目录）
    QDir().mkpath(QString::fromStdString(projects_directory()));
}

void TestProjectStore::test_roundtrip()
{
    SimulationConfig cfg;
    cfg.project_name = "TestProject";
    cfg.robot_type = RobotType::Drone;
    cfg.scene_template = SceneTemplate::Outdoor;
    cfg.workspace = Workspace::SceneEdit;
    cfg.backend = SimBackend::QM_Superconducting;
    cfg.physics_engine = PhysicsEngine::AlphaPHY_v2;
    cfg.gpu_accel = true;
    cfg.engine_version = "2.0.0";
    cfg.env.temperature_c = 25.5;
    cfg.env.wind_speed = 3.2;
    cfg.env.wind_dir_deg = 90.0;
    cfg.env.gravity = -9.81;
    cfg.env.sun_dir[0] = 0.1f;
    cfg.env.sun_dir[1] = 0.2f;
    cfg.env.sun_dir[2] = 0.3f;
    cfg.env.sun_intensity = 1.5f;
    cfg.fractal.enabled = false;
    cfg.fractal.resolution = 256;
    cfg.fractal.extent = 5.0;
    cfg.fractal.height_scale = 0.05;
    cfg.fractal.max_iter = 256;
    cfg.fractal.slice_s = 1.5;
    cfg.custom_robot_json = "/tmp/robot.json";

    cfg.project_path = project_path_for(cfg.project_name);
    QVERIFY(save_project(cfg));

    SimulationConfig loaded;
    QVERIFY(load_project(cfg.project_path, loaded));

    QCOMPARE(QString::fromStdString(loaded.project_name), QString::fromStdString(cfg.project_name));
    QVERIFY(loaded.robot_type == RobotType::Drone);
    QVERIFY(loaded.scene_template == SceneTemplate::Outdoor);
    QVERIFY(loaded.workspace == Workspace::SceneEdit);
    QVERIFY(loaded.backend == SimBackend::QM_Superconducting);
    QVERIFY(loaded.physics_engine == PhysicsEngine::AlphaPHY_v2);
    QCOMPARE(loaded.gpu_accel, true);
    QCOMPARE(QString::fromStdString(loaded.engine_version), QString::fromStdString("2.0.0"));

    QVERIFY(std::fabs(loaded.env.temperature_c - 25.5) < 1e-9);
    QVERIFY(std::fabs(loaded.env.wind_speed - 3.2) < 1e-9);
    QVERIFY(std::fabs(loaded.env.wind_dir_deg - 90.0) < 1e-9);
    QVERIFY(std::fabs(loaded.env.gravity + 9.81) < 1e-9);
    QVERIFY(std::fabs(loaded.env.sun_dir[0] - 0.1f) < 1e-6);
    QVERIFY(std::fabs(loaded.env.sun_dir[1] - 0.2f) < 1e-6);
    QVERIFY(std::fabs(loaded.env.sun_dir[2] - 0.3f) < 1e-6);
    QVERIFY(std::fabs(loaded.env.sun_intensity - 1.5f) < 1e-6);

    QCOMPARE(loaded.fractal.enabled, false);
    QCOMPARE(loaded.fractal.resolution, 256);
    QVERIFY(std::fabs(loaded.fractal.extent - 5.0) < 1e-9);
    QVERIFY(std::fabs(loaded.fractal.height_scale - 0.05) < 1e-9);
    QCOMPARE(loaded.fractal.max_iter, 256);
    QVERIFY(std::fabs(loaded.fractal.slice_s - 1.5) < 1e-9);

    QCOMPARE(QString::fromStdString(loaded.custom_robot_json), QString::fromStdString("/tmp/robot.json"));
    QCOMPARE(QString::fromStdString(loaded.project_path), QString::fromStdString(cfg.project_path));
}

void TestProjectStore::test_defaults()
{
    // 只含 project_name 的最小 JSON，缺失字段应回退默认值
    const QString path = QString::fromStdString(projects_directory()) + "/minimal.qrsp.json";
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write(R"({"project_name":"Minimal"})");
        f.close();
    }

    SimulationConfig cfg;
    QVERIFY(load_project(path.toStdString(), cfg));

    QCOMPARE(QString::fromStdString(cfg.project_name), QString("Minimal"));
    QVERIFY(cfg.robot_type == RobotType::Humanoid);
    QVERIFY(cfg.scene_template == SceneTemplate::Industrial);
    QVERIFY(cfg.workspace == Workspace::SimDebug);
    QVERIFY(cfg.backend == SimBackend::QVM);
    QVERIFY(cfg.physics_engine == PhysicsEngine::AlphaPHY_v1);
    QCOMPARE(cfg.gpu_accel, false);
    QVERIFY(std::fabs(cfg.env.gravity + 9.81) < 1e-9);
    QCOMPARE(cfg.fractal.enabled, true);
    QCOMPARE(cfg.fractal.resolution, 128);

    QFile::remove(path);
}

void TestProjectStore::test_path_sanitize()
{
    // 非法字符（\/:*? 等）应被替换为下划线（检查文件名部分，
    // 完整路径包含目录分隔符 /，故仅校验 basename）
    const std::string p = project_path_for("a/b:c*d");
    const QString base = QFileInfo(QString::fromStdString(p)).fileName();
    QVERIFY(!base.contains('/'));
    QVERIFY(!base.contains(':'));
    QVERIFY(!base.contains('*'));
    QVERIFY(base.endsWith(".qrsp.json"));
}

void TestProjectStore::cleanupTestCase()
{
    delete_project(project_path_for("TestProject"));
    QFile::remove(QString::fromStdString(projects_directory()) + "/minimal.qrsp.json");
}

QObject *createTestProjectStore() { return new TestProjectStore; }

#include "test_project_store.moc"