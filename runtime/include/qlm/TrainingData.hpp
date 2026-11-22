#pragma once

#include "LF2C2.hpp"
#include "../qml/Inference.hpp"

#include <algorithm>
#include <array>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace qlm
{
    namespace training
    {
        struct TextValidation
        {
            uint64_t bytes = 0;
            bool has_bom = false;
            size_t control_bytes = 0;
            size_t checked_bytes = 0;
            std::string error;
        };

        inline TextValidation validate_training_text(const std::string &path)
        {
            TextValidation r;
            std::error_code ec;
            r.bytes = static_cast<uint64_t>(std::filesystem::file_size(path, ec));
            if (ec)
            {
                r.error = "无法读取训练文件大小 '" + path + "': " + ec.message();
                return r;
            }
            if (r.bytes < 3)
            {
                r.error = "训练文件不足 3 字节: " + path;
                return r;
            }

            std::ifstream in(std::filesystem::path(path), std::ios::binary);
            if (!in)
            {
                r.error = "训练文件打不开（权限或被占用）: " + path;
                return r;
            }
            std::array<unsigned char, 4096> head{};
            in.read(reinterpret_cast<char *>(head.data()),
                    static_cast<std::streamsize>(head.size()));
            const size_t got = static_cast<size_t>(in.gcount());
            r.checked_bytes = got;
            if (got >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF)
            {
                r.has_bom = true;
                r.error = "训练文件带 UTF-8 BOM（会把三个字节当成 token 学进去，请先去掉）: " + path;
                return r;
            }
            for (size_t i = 0; i < got; ++i)
            {
                const unsigned char c = head[i];
                if (c < 0x09 || (c > 0x0D && c < 0x20)) ++r.control_bytes;
            }
            if (got > 0 && r.control_bytes * 100 > got)
            {
                r.error = "训练文件头部超过 1% 是控制字节，疑似二进制文件被当成语料: " + path;
            }
            return r;
        }

        struct SampleView
        {
            std::vector<uint8_t> window;
            std::array<double, 8> image_angles{};
            bool has_image = false;
            uint8_t target = 0;
        };

        inline std::vector<std::complex<double>> product_state_amplitudes(
            const std::vector<double> &angles, size_t num_qubits)
        {
            if (num_qubits == 0 || num_qubits > 20)
                throw std::invalid_argument(
                    "product_state_amplitudes: num_qubits must be in [1, 20]");

            std::vector<std::complex<double>> amps(1, {1.0, 0.0});
            amps.reserve(size_t(1) << num_qubits);
            for (size_t i = 0; i < num_qubits; ++i)
            {
                const double theta = (i < angles.size()) ? angles[i] : 0.0;
                const double c = std::cos(theta * 0.5);
                const double s = std::sin(theta * 0.5);
                const size_t old = amps.size();
                amps.resize(old * 2);
                for (size_t j = 0; j < old; ++j)
                {
                    const std::complex<double> a = amps[j];
                    amps[j] = a * c;
                    amps[j + old] = a * s;
                }
            }
            return amps;
        }


        class TextCorpusStream
        {
        public:
            explicit TextCorpusStream(const std::string &path, uint64_t limit_bytes = 0)
                : path_(path), in_(std::filesystem::path(path), std::ios::binary)
            {
                const TextValidation check = validate_training_text(path);
                if (!check.error.empty())
                    throw std::runtime_error("[Train] 训练数据预检失败: " + check.error);
                size_ = check.bytes;
                if (limit_bytes > 0 && size_ > limit_bytes)
                    size_ = limit_bytes;

                if (!in_.is_open())
                    throw std::runtime_error(
                        "无法打开训练文件 '" + path +
                        "'（路径不存在、权限不足或文件被占用）");
            }

            const std::string &path() const { return path_; }

            size_t total_samples() const
            {
                return (size_ > 2) ? static_cast<size_t>(size_ - 2) : 0;
            }

            void rewind()
            {
                primed_ = false;
                next_index_ = 0;
            }

            bool get(size_t index, SampleView &out)
            {
                if (index >= total_samples())
                    return false;

                const uint64_t target_pos = static_cast<uint64_t>(index) + 2;

                if (!primed_ || index != next_index_)
                {
                    const uint64_t begin = (target_pos > kWindow) ? (target_pos - kWindow) : 0;

                    in_.clear();
                    in_.seekg(static_cast<std::streamoff>(begin), std::ios::beg);
                    if (!in_)
                        throw std::runtime_error(
                            "训练文件 '" + path_ + "' seek 失败（第 " +
                            std::to_string(index) + " 个样本）");

                    history_.resize(static_cast<size_t>(target_pos - begin));
                    if (!history_.empty())
                    {
                        in_.read(reinterpret_cast<char *>(history_.data()),
                                 static_cast<std::streamsize>(history_.size()));
                        if (in_.gcount() != static_cast<std::streamsize>(history_.size()))
                            throw std::runtime_error(
                                "训练文件 '" + path_ + "' 读取上下文失败（第 " +
                                std::to_string(index) + " 个样本，偏移 " +
                                std::to_string(begin) + "）");
                    }

                    char t = 0;
                    in_.read(&t, 1);
                    if (in_.gcount() != 1)
                        throw std::runtime_error(
                            "训练文件 '" + path_ + "' 读取目标 token 失败（第 " +
                            std::to_string(index) + " 个样本）");
                    out.target = static_cast<uint8_t>(t);
                    primed_ = true;
                }
                else
                {
                    char t = 0;
                    in_.read(&t, 1);
                    if (in_.gcount() != 1)
                        throw std::runtime_error(
                            "训练文件 '" + path_ + "' 顺序读取目标 token 失败（第 " +
                            std::to_string(index) + " 个样本）");
                    out.target = static_cast<uint8_t>(t);
                }

                next_index_ = index + 1;
                out.window.assign(history_.begin(), history_.end());
                out.has_image = false;
                out.image_angles.fill(0.0);

                history_.push_back(out.target);
                if (history_.size() > kWindow)
                    history_.erase(history_.begin());
                return true;
            }

        private:
            static constexpr size_t kWindow = 32;

            std::string path_;
            std::ifstream in_;
            uint64_t size_ = 0;
            std::vector<uint8_t> history_;
            bool primed_ = false;
            size_t next_index_ = 0;
        };


        class MultiFileTextCorpus
        {
        public:
            void add_file(const std::string &path, uint64_t limit_bytes = 0)
            {
                auto stream = std::make_unique<TextCorpusStream>(path, limit_bytes);
                const size_t count = stream->total_samples();
                if (count == 0)
                {
                    skipped_.push_back(path);
                    return;
                }

                Part part;
                part.path = path;
                part.stream = std::move(stream);
                part.first = total_;
                part.count = count;
                total_ += count;
                parts_.push_back(std::move(part));
            }

            size_t total_samples() const { return total_; }
            bool empty() const { return parts_.empty(); }
            size_t file_count() const { return parts_.size(); }
            const std::vector<std::string> &skipped_files() const { return skipped_; }

            void rewind()
            {
                for (auto &p : parts_)
                    p.stream->rewind();
                cursor_part_ = 0;
            }

            bool get(size_t index, SampleView &out)
            {
                if (index >= total_)
                    return false;

                if (!(index >= parts_[cursor_part_].first &&
                      index < parts_[cursor_part_].first + parts_[cursor_part_].count))
                {
                    size_t lo = 0;
                    size_t hi = parts_.size();
                    while (lo + 1 < hi)
                    {
                        const size_t mid = (lo + hi) / 2;
                        if (parts_[mid].first <= index)
                            lo = mid;
                        else
                            hi = mid;
                    }
                    cursor_part_ = lo;
                }

                return parts_[cursor_part_].stream->get(
                    index - parts_[cursor_part_].first, out);
            }

        private:
            struct Part
            {
                std::string path;
                std::unique_ptr<TextCorpusStream> stream;
                size_t first = 0;
                size_t count = 0;
            };

            std::vector<Part> parts_;
            std::vector<std::string> skipped_;
            size_t total_ = 0;
            size_t cursor_part_ = 0;
        };

        inline std::string json_unescape(const std::string &raw)
        {
            std::string out;
            out.reserve(raw.size());
            for (size_t i = 0; i < raw.size(); ++i)
            {
                if (raw[i] != '\\' || i + 1 >= raw.size())
                {
                    out.push_back(raw[i]);
                    continue;
                }
                const char c = raw[++i];
                switch (c)
                {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u':
                {
                    if (i + 4 < raw.size())
                    {
                        int code = 0;
                        bool ok = true;
                        for (int k = 1; k <= 4; ++k)
                        {
                            const char h = raw[i + k];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code += h - '0';
                            else if (h >= 'a' && h <= 'f') code += h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') code += h - 'A' + 10;
                            else { ok = false; break; }
                        }
                        if (ok)
                        {
                            i += 4;
                            if (code < 0x80)
                                out.push_back(static_cast<char>(code));
                            else if (code < 0x800)
                            {
                                out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                            }
                            else
                            {
                                out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                            }
                        }
                        else
                        {
                            out.push_back('u');
                        }
                    }
                    break;
                }
                default: out.push_back(c); break;
                }
            }
            return out;
        }

        inline bool json_string_field(const std::string &line,
                                      const std::string &key,
                                      std::string &out)
        {
            const std::string needle = "\"" + key + "\"";
            size_t p = line.find(needle);
            if (p == std::string::npos)
                return false;
            p = line.find(':', p + needle.size());
            if (p == std::string::npos)
                return false;
            p = line.find('"', p);
            if (p == std::string::npos)
                return false;

            std::string raw;
            size_t q = p + 1;
            while (q < line.size())
            {
                if (line[q] == '\\' && q + 1 < line.size())
                {
                    raw.push_back(line[q]);
                    raw.push_back(line[q + 1]);
                    q += 2;
                    continue;
                }
                if (line[q] == '"')
                    break;
                raw.push_back(line[q]);
                ++q;
            }
            out = json_unescape(raw);
            return true;
        }


        class CaptionCorpus
        {
        public:
            using ImageLoader =
                std::function<bool(const std::string &path, std::array<double, 8> &angles)>;

            void load(const std::string &manifest_path,
                      const std::string &image_root,
                      size_t limit,
                      const ImageLoader &load_image,
                      bool skip_bad_images = false)
            {
                std::ifstream in(manifest_path);
                if (!in.is_open())
                    throw std::runtime_error(
                        "无法打开 caption 清单 '" + manifest_path + "'");

                entries_.clear();
                skipped_.clear();
                total_ = 0;
                caption_count_ = 0;

                std::unordered_map<std::string, std::array<double, 8>> image_cache;
                std::string line;
                size_t line_no = 0;

                while (std::getline(in, line))
                {
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    if (line.empty())
                        continue;
                    if (limit && line_no >= limit)
                        break;
                    ++line_no;

                    std::string image_rel, caption;
                    if (!json_string_field(line, "image", image_rel) ||
                        !json_string_field(line, "caption", caption))
                        throw std::runtime_error(
                            "caption 清单第 " + std::to_string(line_no) +
                            " 行缺少 image/caption 字段");

                    const std::string image_path =
                        (std::filesystem::path(image_root) / image_rel).string();

                    auto it = image_cache.find(image_path);
                    if (it == image_cache.end())
                    {
                        std::array<double, 8> angles{};
                        if (!load_image || !load_image(image_path, angles))
                        {
                            if (skip_bad_images)
                            {
                                skipped_.push_back(image_path);
                                continue;
                            }
                            throw std::runtime_error(
                                "无法解码图像 '" + image_path +
                                "'（caption 清单第 " + std::to_string(line_no) +
                                " 行）。修复图像路径，或使用 --skip-bad-images。");
                        }
                        it = image_cache.emplace(image_path, angles).first;
                    }

                    std::vector<uint8_t> tokens = qqnt::global_tokenizer.encode(caption);
                    if (tokens.size() < 3)
                        continue;

                    Entry e;
                    e.tokens = std::move(tokens);
                    e.image_angles = it->second;
                    e.first_sample = total_;
                    e.sample_count = e.tokens.size() - 2;
                    total_ += e.sample_count;
                    entries_.push_back(std::move(e));
                    ++caption_count_;
                }

                image_count_ = image_cache.size();
            }

            size_t total_samples() const { return total_; }
            size_t caption_count() const { return caption_count_; }
            size_t image_count() const { return image_count_; }
            const std::vector<std::string> &skipped_images() const { return skipped_; }
            bool empty() const { return total_ == 0; }

            void rewind() { cursor_entry_ = 0; }

            bool get(size_t index, SampleView &out)
            {
                if (index >= total_)
                    return false;

                if (!(index >= entries_[cursor_entry_].first_sample &&
                      index < entries_[cursor_entry_].first_sample +
                                  entries_[cursor_entry_].sample_count))
                {
                    size_t lo = 0;
                    size_t hi = entries_.size();
                    while (lo + 1 < hi)
                    {
                        const size_t mid = (lo + hi) / 2;
                        if (entries_[mid].first_sample <= index)
                            lo = mid;
                        else
                            hi = mid;
                    }
                    cursor_entry_ = lo;
                }

                const Entry &e = entries_[cursor_entry_];
                const size_t local = index - e.first_sample;
                const size_t target_pos = local + 2;
                const size_t begin = (target_pos > kWindow) ? (target_pos - kWindow) : 0;

                out.window.assign(e.tokens.begin() + static_cast<long>(begin),
                                  e.tokens.begin() + static_cast<long>(target_pos));
                out.target = e.tokens[target_pos];
                out.image_angles = e.image_angles;
                out.has_image = true;
                return true;
            }

        private:
            static constexpr size_t kWindow = 32;

            struct Entry
            {
                std::vector<uint8_t> tokens;
                std::array<double, 8> image_angles{};
                size_t first_sample = 0;
                size_t sample_count = 0;
            };

            std::vector<Entry> entries_;
            std::vector<std::string> skipped_;
            size_t total_ = 0;
            size_t caption_count_ = 0;
            size_t image_count_ = 0;
            size_t cursor_entry_ = 0;
        };
    }
}