#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include <QByteArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonValue>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>

#include "aes256.hpp"
#include "ed25519.hpp"

namespace rvdlrsp::editor
{


    struct PluginInfo
    {
        std::string name;
        std::string version;
        std::string author;
        std::string description;
        std::string entry;
        std::vector<std::string> dependencies;
        std::string rvdl_source;
        std::string path;
        qint64 mtime = 0;
        bool signature_ok = false;
    };

    class PluginManager
    {
    public:
        static constexpr const char *kMagic = "QRS2P1";
        static constexpr uint32_t kVersion = 3;
        static constexpr const char *kPassphrase = "rvdlRSP.Plugin.2026";

        static constexpr uint8_t kPublicKey[32] = {
            0xd7, 0x18, 0xc3, 0xa3, 0x5e, 0xd3, 0xbb, 0xb4,
            0x56, 0xbe, 0xcd, 0xbd, 0xf4, 0xe7, 0x20, 0x3e,
            0x79, 0x6e, 0xdd, 0x04, 0x74, 0xb2, 0x21, 0x45,
            0xaf, 0x05, 0x57, 0x9a, 0xeb, 0x4d, 0x3c, 0xb7};

        static QByteArray pack(const QJsonObject &manifest, const std::string &rvdl_source,
                               void *privkey = nullptr)
        {
            QByteArray body;
            QByteArray mj = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
            body.append(kMagic, 6);
            append_u32(body, kVersion);
            append_u32(body, static_cast<uint32_t>(mj.size()));
            body.append(mj);

            QByteArray compressed = qCompress(QByteArray::fromStdString(rvdl_source), 9);
            QByteArray payload = encrypt(compressed);
            append_u32(body, static_cast<uint32_t>(payload.size()));
            body.append(payload);

            if (privkey)
            {
                uint8_t sig[Ed25519::kSigSize];
                if (Ed25519::sign(reinterpret_cast<const uint8_t *>(body.constData()),
                                  static_cast<size_t>(body.size()),
                                  static_cast<EVP_PKEY *>(privkey), sig))
                {
                    append_u32(body, Ed25519::kSigSize);
                    body.append(reinterpret_cast<const char *>(sig), Ed25519::kSigSize);
                }
                else
                {
                    append_u32(body, 0);
                }
            }
            else
            {
                append_u32(body, 0);
            }
            return body;
        }

        static bool unpack(const QByteArray &data, PluginInfo &info, std::string &error)
        {
            error.clear();
            info = PluginInfo{};
            if (data.size() < 14)
            {
                error = "文件过小，不是有效的 .rvdlp 插件包。";
                return false;
            }
            if (std::memcmp(data.constData(), kMagic, 6) != 0)
            {
                error = "magic 不匹配：不是 .rvdlp 插件包。";
                return false;
            }
            uint32_t version = read_u32(data, 6);
            if (version != kVersion)
            {
                error = "不支持的插件包版本：" + std::to_string(version) + "（期望 " + std::to_string(kVersion) + "）";
                return false;
            }
            uint32_t manifest_len = read_u32(data, 10);
            int pos = 14 + manifest_len;
            if (pos + 4 > data.size())
            {
                error = "manifest 长度越界，插件包损坏。";
                return false;
            }

            QByteArray mj = data.mid(14, manifest_len);
            QJsonParseError pe{};
            QJsonDocument doc = QJsonDocument::fromJson(mj, &pe);
            if (pe.error != QJsonParseError::NoError || !doc.isObject())
            {
                error = "manifest JSON 解析失败：" + pe.errorString().toStdString();
                return false;
            }
            QJsonObject m = doc.object();
            info.name = m.value("name").toString().toStdString();
            info.version = m.value("version").toString().toStdString();
            info.author = m.value("author").toString().toStdString();
            info.description = m.value("description").toString().toStdString();
            info.entry = m.value("entry").toString("rvdl_main").toStdString();
            for (const QJsonValue &d : m.value("dependencies").toArray())
                info.dependencies.push_back(d.toString().toStdString());
            if (info.name.empty())
            {
                error = "manifest 缺少 name 字段。";
                return false;
            }

            uint32_t payload_len = read_u32(data, pos);
            pos += 4;
            if (pos + static_cast<int>(payload_len) + 4 > data.size())
            {
                error = "payload 长度越界，插件包损坏。";
                return false;
            }
            QByteArray payload = data.mid(pos, static_cast<int>(payload_len));
            pos += static_cast<int>(payload_len);

            uint32_t sig_len = read_u32(data, pos);
            pos += 4;
            QByteArray signature;
            if (sig_len > 0)
            {
                if (pos + static_cast<int>(sig_len) > data.size())
                {
                    error = "签名长度越界，插件包损坏。";
                    return false;
                }
                signature = data.mid(pos, static_cast<int>(sig_len));
            }

            if (sig_len == Ed25519::kSigSize)
            {
                QByteArray signed_part = data.left(pos - 4);
                info.signature_ok = Ed25519::verify(
                    reinterpret_cast<const uint8_t *>(signed_part.constData()),
                    static_cast<size_t>(signed_part.size()),
                    reinterpret_cast<const uint8_t *>(signature.constData()),
                    signature.size(), kPublicKey);
            }

            QByteArray compressed = decrypt(payload);
            if (compressed.isEmpty() && !payload.isEmpty())
            {
                error = "解密失败（密钥不匹配或数据被篡改）。";
                return false;
            }
            info.rvdl_source = QString::fromUtf8(qUncompress(compressed)).toStdString();
            if (info.rvdl_source.empty() && !compressed.isEmpty())
            {
                error = "解压失败，插件包损坏。";
                return false;
            }
            return true;
        }

        void set_require_signature(bool on) { require_signature_ = on; }
        bool require_signature() const { return require_signature_; }

        static bool allow_unsigned_env()
        {
            const char *v = std::getenv("RVDL_PLUGIN_ALLOW_UNSIGNED");
            return v && *v && std::strcmp(v, "0") != 0;
        }

        bool load_file(const std::string &path, std::string &error, std::string *loaded_name = nullptr)
        {
            error.clear();
            QFile f(QString::fromStdString(path));
            if (!f.open(QIODevice::ReadOnly))
            {
                error = "无法打开文件：" + path;
                return false;
            }
            QByteArray data = f.readAll();
            f.close();

            PluginInfo info;
            if (!unpack(data, info, error))
                return false;

            if (!info.signature_ok && (require_signature_ && !allow_unsigned_env()))
            {
                error = "插件 " + info.name + " 未通过 Ed25519 签名校验，默认拒绝加载（仅接受官方私钥签名的 .rvdlp）。"
                        "如确需调试未签名插件，请设置环境变量 RVDL_PLUGIN_ALLOW_UNSIGNED=1 后重试。";
                return false;
            }

            info.path = path;
            info.mtime = QFileInfo(QString::fromStdString(path)).lastModified().toMSecsSinceEpoch();

            QDir dir = QFileInfo(QString::fromStdString(path)).absoluteDir();
            for (const auto &dep : info.dependencies)
            {
                if (find(dep))
                    continue;
                std::string dep_path = dir.absoluteFilePath(QString::fromStdString(dep + ".rvdlp")).toStdString();
                std::string dep_err;
                if (!QFile::exists(QString::fromStdString(dep_path)) || !load_file(dep_path, dep_err))
                {
                    error = "插件 " + info.name + " 依赖缺失：" + dep + "（" + dep_err + "）";
                    return false;
                }
            }

            if (loaded_name)
                *loaded_name = info.name;

            for (auto &p : plugins_)
            {
                if (p.name == info.name)
                {
                    p = std::move(info);
                    return true;
                }
            }
            plugins_.push_back(std::move(info));
            return true;
        }

        int reload_changed(std::vector<std::string> &errors)
        {
            int reloaded = 0;
            for (auto &p : plugins_)
            {
                if (p.path.empty())
                    continue;
                qint64 mt = QFileInfo(QString::fromStdString(p.path)).lastModified().toMSecsSinceEpoch();
                if (mt == p.mtime)
                    continue;
                std::string e;
                std::string old_name = p.name;
                std::string path = p.path;
                if (load_file(path, e))
                    ++reloaded;
                else if (!e.empty())
                    errors.push_back(path + ": " + e);
                (void)old_name;
            }
            return reloaded;
        }

        bool unload(const std::string &name)
        {
            for (auto it = plugins_.begin(); it != plugins_.end(); ++it)
            {
                if (it->name == name)
                {
                    plugins_.erase(it);
                    return true;
                }
            }
            return false;
        }

        const std::vector<PluginInfo> &plugins() const { return plugins_; }

        const PluginInfo *find(const std::string &name) const
        {
            for (const auto &p : plugins_)
                if (p.name == name)
                    return &p;
            return nullptr;
        }

        bool dependencies_satisfied(const PluginInfo &p, std::string &missing) const
        {
            missing.clear();
            for (const auto &d : p.dependencies)
            {
                if (!find(d))
                {
                    if (!missing.empty())
                        missing += ", ";
                    missing += d;
                }
            }
            return missing.empty();
        }

        int scan_directory(const std::string &dir, std::vector<std::string> &errors)
        {
            QDir d(QString::fromStdString(dir));
            if (!d.exists())
                return 0;
            const QStringList files = d.entryList({QStringLiteral("*.rvdlp")}, QDir::Files);
            int loaded = 0;
            for (const QString &f : files)
            {
                std::string path = d.absoluteFilePath(f).toStdString();
                std::string e;
                if (load_file(path, e))
                    ++loaded;
                else if (!e.empty())
                    errors.push_back(path + ": " + e);
            }
            return loaded;
        }

    private:
        static void append_u32(QByteArray &b, uint32_t v)
        {
            char c[4];
            c[0] = static_cast<char>(v & 0xff);
            c[1] = static_cast<char>((v >> 8) & 0xff);
            c[2] = static_cast<char>((v >> 16) & 0xff);
            c[3] = static_cast<char>((v >> 24) & 0xff);
            b.append(c, 4);
        }

        static uint32_t read_u32(const QByteArray &b, int off)
        {
            const unsigned char *p = reinterpret_cast<const unsigned char *>(b.constData() + off);
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }

        static const char *passphrase()
        {
            if (const char *env = std::getenv("RVDL_PLUGIN_PASSPHRASE"); env && *env)
                return env;
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                std::fprintf(stderr,
                             "[plugin] 提示：未设置 RVDL_PLUGIN_PASSPHRASE，使用内置默认口令"
                             "（仅用于兼容示例包；生产环境请设置该环境变量）。\n");
            }
            return kPassphrase;
        }

        static void derive_key(uint8_t key[Aes256Gcm::kKeySize])
        {
            QByteArray h = QCryptographicHash::hash(QByteArray(passphrase()), QCryptographicHash::Sha256);
            std::memcpy(key, h.constData(), Aes256Gcm::kKeySize);
        }

        static QByteArray encrypt(const QByteArray &data)
        {
            uint8_t key[Aes256Gcm::kKeySize];
            derive_key(key);
            std::vector<uint8_t> in(data.constData(), data.constData() + data.size());
            std::vector<uint8_t> out = Aes256Gcm::encrypt(in, key);
            return QByteArray(reinterpret_cast<const char *>(out.data()), static_cast<int>(out.size()));
        }

        static QByteArray decrypt(const QByteArray &data)
        {
            uint8_t key[Aes256Gcm::kKeySize];
            derive_key(key);
            std::vector<uint8_t> in(data.constData(), data.constData() + data.size());
            std::vector<uint8_t> out;
            if (!Aes256Gcm::decrypt(in, key, out))
                return QByteArray();
            return QByteArray(reinterpret_cast<const char *>(out.data()), static_cast<int>(out.size()));
        }

        std::vector<PluginInfo> plugins_;
        bool require_signature_ = true;
    };
}