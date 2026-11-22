#pragma once
#include <QString>

namespace rvdlrsp::gui
{

    enum class AssetType
    {
        Folder,
        Robot,
        Mesh,
        Material,
        Scene,
        Script,
        Blueprint,
        Audio,
        Texture,
        Unknown
    };
    
    AssetType asset_type_from_path(const QString &path);
    QString asset_type_name(AssetType t);
    bool asset_is_text_editable(const QString &path);
    bool asset_is_json(const QString &path);
}