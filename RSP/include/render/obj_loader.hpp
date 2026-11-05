#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <stdexcept>
#include "scene.hpp"

namespace rvdlrsp::render
{

    class ObjLoader
    {
    public:
        // 加载 OBJ 文件（支持 v / vt / vn / f；f 索引支持 v / v/vt / v//vn / v/vt/vn）
        static Mesh load(const std::string &path)
        {
            std::ifstream in(path);
            if (!in)
                throw std::runtime_error("OBJ: cannot open " + path);

            std::vector<qpc::Vec3> positions;
            std::vector<qpc::Vec3> normals;
            std::vector<std::pair<float, float>> texcoords;

            Mesh mesh;
            mesh.name = path;

            std::string line;
            while (std::getline(in, line))
            {
                std::istringstream ss(line);
                std::string tag;
                ss >> tag;
                if (tag == "v")
                {
                    double x, y, z;
                    ss >> x >> y >> z;
                    positions.push_back({x, y, z});
                }
                else if (tag == "vn")
                {
                    double x, y, z;
                    ss >> x >> y >> z;
                    normals.push_back({x, y, z});
                }
                else if (tag == "vt")
                {
                    double u, v;
                    ss >> u >> v;
                    texcoords.push_back({static_cast<float>(u), static_cast<float>(v)});
                }
                else if (tag == "f")
                {
                    std::vector<int> vidx, nidx, tidx;
                    std::string tok;
                    while (ss >> tok)
                    {
                        int v = 0, t = 0, n = 0;
                        if (std::sscanf(tok.c_str(), "%d/%d/%d", &v, &t, &n) == 3)
                        {
                            vidx.push_back(v);
                            tidx.push_back(t);
                            nidx.push_back(n);
                        }
                        else if (std::sscanf(tok.c_str(), "%d//%d", &v, &n) == 2)
                        {
                            vidx.push_back(v);
                            tidx.push_back(0);
                            nidx.push_back(n);
                        }
                        else if (std::sscanf(tok.c_str(), "%d/%d", &v, &t) == 2)
                        {
                            vidx.push_back(v);
                            tidx.push_back(t);
                            nidx.push_back(0);
                        }
                        else if (std::sscanf(tok.c_str(), "%d", &v) == 1)
                        {
                            vidx.push_back(v);
                            tidx.push_back(0);
                            nidx.push_back(0);
                        }
                    }
                    // 三角化（扇形）
                    for (size_t i = 1; i + 1 < vidx.size(); ++i)
                    {
                        mesh.indices.push_back(static_cast<uint32_t>(mesh.vertices.size()));
                        add_vertex(mesh, positions, normals, texcoords, vidx[0], tidx[0], nidx[0]);
                        mesh.indices.push_back(static_cast<uint32_t>(mesh.vertices.size()));
                        add_vertex(mesh, positions, normals, texcoords, vidx[i], tidx[i], nidx[i]);
                        mesh.indices.push_back(static_cast<uint32_t>(mesh.vertices.size()));
                        add_vertex(mesh, positions, normals, texcoords, vidx[i + 1], tidx[i + 1], nidx[i + 1]);
                    }
                }
            }
            return mesh;
        }

    private:
        static void add_vertex(Mesh &mesh,
                               const std::vector<qpc::Vec3> &positions,
                               const std::vector<qpc::Vec3> &normals,
                               const std::vector<std::pair<float, float>> &texcoords,
                               int v_idx, int t_idx, int n_idx)
        {
            Vertex v;
            int vi = (v_idx > 0) ? v_idx - 1 : static_cast<int>(positions.size()) + v_idx;
            if (vi >= 0 && vi < static_cast<int>(positions.size()))
                v.position = positions[vi];

            int ni = (n_idx > 0) ? n_idx - 1 : static_cast<int>(normals.size()) + n_idx;
            if (ni >= 0 && ni < static_cast<int>(normals.size()))
                v.normal = normals[ni];

            int ti = (t_idx > 0) ? t_idx - 1 : static_cast<int>(texcoords.size()) + t_idx;
            if (ti >= 0 && ti < static_cast<int>(texcoords.size()))
            {
                v.u = texcoords[ti].first;
                v.v = texcoords[ti].second;
            }

            v.r = 0.7f;
            v.g = 0.7f;
            v.b = 0.75f;
            mesh.vertices.push_back(v);
        }
    };
}