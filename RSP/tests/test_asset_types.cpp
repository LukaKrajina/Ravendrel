#include <QtTest/QtTest>
#include <QTemporaryDir>

#include "asset_types.h"

using namespace rvdlrsp::gui;

class TestAssetTypes : public QObject
{
    Q_OBJECT
private slots:
    void test_type_from_path();
    void test_case_insensitive();
    void test_folder_and_unknown();
    void test_text_editable_and_json();
};

void TestAssetTypes::test_type_from_path()
{
    QCOMPARE(static_cast<int>(asset_type_from_path("a.qrobot")), static_cast<int>(AssetType::Robot));
    QCOMPARE(static_cast<int>(asset_type_from_path("a.obj")), static_cast<int>(AssetType::Mesh));
    QCOMPARE(static_cast<int>(asset_type_from_path("a.gltf")), static_cast<int>(AssetType::Mesh));
    QCOMPARE(static_cast<int>(asset_type_from_path("a.glb")), static_cast<int>(AssetType::Mesh));
    QCOMPARE(static_cast<int>(asset_type_from_path("a.fbx")), static_cast<int>(AssetType::Mesh));
    QCOMPARE(static_cast<int>(asset_type_from_path("a.stl")), static_cast<int>(AssetType::Mesh));
    QCOMPARE(static_cast<int>(asset_type_from_path("a.qmat")), static_cast<int>(AssetType::Material));
    QCOMPARE(static_cast<int>(asset_type_from_path("a.qscene")), static_cast<int>(AssetType::Scene));
    QCOMPARE(static_cast<int>(asset_type_from_path("a.rvdl")), static_cast<int>(AssetType::Script));
    QCOMPARE(static_cast<int>(asset_type_from_path("a.qbp")), static_cast<int>(AssetType::Blueprint));
    QCOMPARE(static_cast<int>(asset_type_from_path("a.blueprint")), static_cast<int>(AssetType::Blueprint));
    QCOMPARE(static_cast<int>(asset_type_from_path("a.wav")), static_cast<int>(AssetType::Audio));
    QCOMPARE(static_cast<int>(asset_type_from_path("a.mp3")), static_cast<int>(AssetType::Audio));
    QCOMPARE(static_cast<int>(asset_type_from_path("a.png")), static_cast<int>(AssetType::Texture));
    QCOMPARE(static_cast<int>(asset_type_from_path("a.jpg")), static_cast<int>(AssetType::Texture));
}

void TestAssetTypes::test_case_insensitive()
{
    QCOMPARE(static_cast<int>(asset_type_from_path("A.QROBOT")), static_cast<int>(AssetType::Robot));
    QCOMPARE(static_cast<int>(asset_type_from_path("A.PNG")), static_cast<int>(AssetType::Texture));
    QCOMPARE(static_cast<int>(asset_type_from_path("A.OBJ")), static_cast<int>(AssetType::Mesh));
}

void TestAssetTypes::test_folder_and_unknown()
{
    QCOMPARE(static_cast<int>(asset_type_from_path("file.xyz")), static_cast<int>(AssetType::Unknown));
    QCOMPARE(static_cast<int>(asset_type_from_path("noextension")), static_cast<int>(AssetType::Unknown));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QCOMPARE(static_cast<int>(asset_type_from_path(dir.path())), static_cast<int>(AssetType::Folder));
}

void TestAssetTypes::test_text_editable_and_json()
{
    QVERIFY(asset_is_text_editable("a.qrobot"));
    QVERIFY(asset_is_text_editable("a.qmat"));
    QVERIFY(asset_is_text_editable("a.json"));
    QVERIFY(asset_is_text_editable("a.txt"));
    QVERIFY(asset_is_text_editable("a.obj"));
    QVERIFY(asset_is_text_editable("a.gltf"));
    QVERIFY(!asset_is_text_editable("a.png"));
    QVERIFY(!asset_is_text_editable("a.wav"));
    QVERIFY(!asset_is_text_editable("a.glb"));

    QVERIFY(asset_is_json("a.qrobot"));
    QVERIFY(asset_is_json("a.qmat"));
    QVERIFY(asset_is_json("a.qscene"));
    QVERIFY(asset_is_json("a.qbp"));
    QVERIFY(asset_is_json("a.json"));
    QVERIFY(asset_is_json("a.gltf"));
    QVERIFY(!asset_is_json("a.txt"));
    QVERIFY(!asset_is_json("a.png"));
}

QObject *createTestAssetTypes() { return new TestAssetTypes; }

#include "test_asset_types.moc"
