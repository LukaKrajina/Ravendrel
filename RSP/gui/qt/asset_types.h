#pragma once
#include <QString>

namespace rvdlrsp::gui
{

// ─── 资产类型 ────────────────────────────────────────────────
    enum class AssetType
    {
        Folder,
        Robot,     // .qrobot
        Mesh,      // .obj / .gltf / .glb / .fbx / .dae / .3ds / .stl
        Material,  // .qmat
        Scene,     // .qscene
        Script,    // .rvdl
        Blueprint, // .qbp / .blueprint
        Audio,     // .wav / .mp3 / .ogg / .flac / .aac
        Texture,   // .png / .jpg / .jpeg / .bmp
        Unknown
    };
    
    // 纯函数：从路径识别资产类型（不依赖 UI / render，便于单元测试）。
    AssetType asset_type_from_path(const QString &path);
    QString asset_type_name(AssetType t);
    bool asset_is_text_editable(const QString &path); // 是否可用文本编辑器打开
    bool asset_is_json(const QString &path);          // 是否 JSON 文本（用于语法高亮）
}