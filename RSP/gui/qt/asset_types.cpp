#include "asset_types.h"
#include "i18n/i18n.h"

#include <QFileInfo>

namespace rvdlrsp::gui
{
    AssetType asset_type_from_path(const QString &path)
    {
        QFileInfo fi(path);
        if (fi.isDir())
            return AssetType::Folder;
        QString ext = fi.suffix().toLower();
        if (ext == "qrobot")
            return AssetType::Robot;
        if (ext == "obj" || ext == "gltf" || ext == "glb" || ext == "fbx" ||
            ext == "dae" || ext == "3ds" || ext == "stl")
            return AssetType::Mesh;
        if (ext == "qmat")
            return AssetType::Material;
        if (ext == "qscene")
            return AssetType::Scene;
        if (ext == "rvdl")
            return AssetType::Script;
        if (ext == "qbp" || ext == "blueprint")
            return AssetType::Blueprint;
        if (ext == "wav" || ext == "mp3" || ext == "ogg" || ext == "flac" || ext == "aac")
            return AssetType::Audio;
        if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp")
            return AssetType::Texture;
        return AssetType::Unknown;
    }

    QString asset_type_name(AssetType t)
    {
        switch (t)
        {
        case AssetType::Folder:
            return RVDLTR("文件夹");
        case AssetType::Robot:
            return RVDLTR("机器人");
        case AssetType::Mesh:
            return RVDLTR("模型");
        case AssetType::Material:
            return RVDLTR("材质");
        case AssetType::Scene:
            return RVDLTR("场景");
        case AssetType::Script:
            return RVDLTR("Rvdl 脚本");
        case AssetType::Blueprint:
            return RVDLTR("蓝图");
        case AssetType::Audio:
            return RVDLTR("音频");
        case AssetType::Texture:
            return RVDLTR("纹理");
        default:
            return RVDLTR("资产");
        }
    }

    bool asset_is_text_editable(const QString &path)
    {
        QString ext = QFileInfo(path).suffix().toLower();
        return ext == "qrobot" || ext == "qmat" || ext == "qscene" ||
            ext == "rvdl" || ext == "qbp" || ext == "blueprint" ||
            ext == "json" || ext == "txt" || ext == "obj" || ext == "gltf";
    }

    bool asset_is_json(const QString &path)
    {
        QString ext = QFileInfo(path).suffix().toLower();
        return ext == "qrobot" || ext == "qmat" || ext == "qscene" ||
            ext == "qbp" || ext == "blueprint" ||
            ext == "json" || ext == "gltf";
    }
}