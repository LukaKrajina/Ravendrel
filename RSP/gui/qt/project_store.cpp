#include "project_store.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>

namespace rvdlrsp::gui
{

    std::string projects_directory()
    {
        QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (base.isEmpty())
            base = QDir::homePath() + "/.rvdlrsp";
        QString dir = base + "/projects";
        QDir().mkpath(dir);
        return dir.toStdString();
    }

    std::string archive_directory()
    {
        QString dir = QString::fromStdString(projects_directory()) + "/archive";
        QDir().mkpath(dir);
        return dir.toStdString();
    }

    std::string project_path_for(const std::string &project_name)
    {
        QString safe = QString::fromStdString(project_name);
        safe.replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_");
        return (QString::fromStdString(projects_directory()) + "/" + safe + ".qrsp.json").toStdString();
    }

    std::string content_directory_for(const std::string &project_name)
    {
        QString safe = QString::fromStdString(project_name);
        safe.replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_");
        QString dir = QString::fromStdString(projects_directory()) + "/" + safe + "/Content";
        QDir().mkpath(dir);
        return dir.toStdString();
    }

    static QString js(const QJsonObject &o, const char *k)
    {
        return o.value(QLatin1String(k)).toString();
    }

    bool save_project(const SimulationConfig &cfg)
    {
        QJsonObject o;
        o["project_name"] = QString::fromStdString(cfg.project_name);
        o["robot_type"] = QString::fromStdString(to_string(cfg.robot_type));
        o["scene_template"] = QString::fromStdString(to_string(cfg.scene_template));
        o["workspace"] = QString::fromStdString(to_string(cfg.workspace));
        o["backend"] = QString::fromStdString(to_string(cfg.backend));
        o["physics_engine"] = QString::fromStdString(to_string(cfg.physics_engine));
        o["gpu_accel"] = cfg.gpu_accel;
        o["engine_version"] = QString::fromStdString(cfg.engine_version);

        QJsonObject env;
        env["temperature_c"] = cfg.env.temperature_c;
        env["wind_speed"] = cfg.env.wind_speed;
        env["wind_dir_deg"] = cfg.env.wind_dir_deg;
        env["gravity"] = cfg.env.gravity;
        env["sun_dir"] = QJsonArray{cfg.env.sun_dir[0], cfg.env.sun_dir[1], cfg.env.sun_dir[2]};
        env["sun_intensity"] = cfg.env.sun_intensity;
        o["env"] = env;

        QJsonObject fr;
        fr["enabled"] = cfg.fractal.enabled;
        fr["resolution"] = cfg.fractal.resolution;
        fr["extent"] = cfg.fractal.extent;
        fr["height_scale"] = cfg.fractal.height_scale;
        fr["max_iter"] = cfg.fractal.max_iter;
        fr["slice_s"] = cfg.fractal.slice_s;
        o["fractal"] = fr;

        o["custom_robot_json"] = QString::fromStdString(cfg.custom_robot_json);

        QFile f(QString::fromStdString(cfg.project_path));
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
        return true;
    }

    bool load_project(const std::string &path, SimulationConfig &out)
    {
        QFile f(QString::fromStdString(path));
        if (!f.open(QIODevice::ReadOnly))
            return false;
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (!doc.isObject())
            return false;
        QJsonObject o = doc.object();

        out.project_name = js(o, "project_name").toStdString();
        out.robot_type = robot_type_from_string(js(o, "robot_type").toStdString());
        out.scene_template = scene_template_from_string(js(o, "scene_template").toStdString());
        out.workspace = workspace_from_string(js(o, "workspace").toStdString());
        out.backend = backend_from_string(js(o, "backend").toStdString());
        out.physics_engine = physics_engine_from_string(js(o, "physics_engine").toStdString());
        out.gpu_accel = o.value("gpu_accel").toBool();
        out.engine_version = js(o, "engine_version").toStdString();

        QJsonObject env = o.value("env").toObject();
        out.env.temperature_c = env.value("temperature_c").toDouble(22.0);
        out.env.wind_speed = env.value("wind_speed").toDouble(0.0);
        out.env.wind_dir_deg = env.value("wind_dir_deg").toDouble(0.0);
        out.env.gravity = env.value("gravity").toDouble(-9.81);
        QJsonArray sun = env.value("sun_dir").toArray();
        if (sun.size() == 3)
        {
            out.env.sun_dir[0] = static_cast<float>(sun[0].toDouble());
            out.env.sun_dir[1] = static_cast<float>(sun[1].toDouble());
            out.env.sun_dir[2] = static_cast<float>(sun[2].toDouble());
        }
        out.env.sun_intensity = static_cast<float>(env.value("sun_intensity").toDouble(1.2));

        QJsonObject fr = o.value("fractal").toObject();
        out.fractal.enabled = fr.value("enabled").toBool(true);
        out.fractal.resolution = fr.value("resolution").toInt(128);
        out.fractal.extent = fr.value("extent").toDouble(3.0);
        out.fractal.height_scale = fr.value("height_scale").toDouble(0.02);
        out.fractal.max_iter = fr.value("max_iter").toInt(128);
        out.fractal.slice_s = fr.value("slice_s").toDouble(0.0);

        out.custom_robot_json = js(o, "custom_robot_json").toStdString();
        out.project_path = path;
        return true;
    }

    bool delete_project(const std::string &path)
    {
        return QFile::remove(QString::fromStdString(path));
    }

    bool archive_project(const std::string &path)
    {
        QString src = QString::fromStdString(path);
        QString dst = QString::fromStdString(archive_directory()) + "/" + QFileInfo(src).fileName();
        if (QFile::exists(dst))
            QFile::remove(dst);
        return QFile::rename(src, dst);
    }

    std::vector<std::string> list_projects()
    {
        std::vector<std::string> out;
        QDir dir(QString::fromStdString(projects_directory()));
        const auto list = dir.entryInfoList(QStringList() << "*.qrsp.json", QDir::Files, QDir::Time);
        for (const auto &fi : list)
            out.push_back(fi.absoluteFilePath().toStdString());
        return out;
    }
}