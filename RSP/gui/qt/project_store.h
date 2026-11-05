#pragma once
#include <string>
#include <vector>
#include "simulation_config.h"

namespace rvdlrsp::gui
{

    std::string projects_directory();
    std::string archive_directory();
    std::string project_path_for(const std::string &project_name);
    std::string content_directory_for(const std::string &project_name); // 项目资产目录（Content/）

    bool save_project(const SimulationConfig &cfg);
    bool load_project(const std::string &path, SimulationConfig &out);
    bool delete_project(const std::string &path);
    bool archive_project(const std::string &path);
    std::vector<std::string> list_projects(); // 按最近修改时间排序
}