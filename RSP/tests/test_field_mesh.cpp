// 快子场切片 → heightmap 网格 单元测试
#include "test_framework.hpp"
#include "render/field_to_mesh.hpp"
#include "spacetime/SliceTopology.hpp"

using namespace rvdlrsp::render;
using namespace rvdl::spacetime;

QTEST(heightfield_vertex_and_index_count) {
    Grid g;
    g.dim = 2;
    g.n = {16, 8, 1};
    g.length = {8.0, 4.0, 1.0};
    std::vector<double> field(g.total(), 0.5);
    Mesh m = make_heightfield_mesh(g, field, 1.0, 1.0);
    QCHECK(m.vertices.size() == 16u * 8u);
    QCHECK(m.indices.size() == 2u * 15u * 7u * 3u);
}

QTEST(heightfield_height_encodes_field) {
    Grid g;
    g.dim = 2;
    g.n = {4, 4, 1};
    g.length = {1.0, 1.0, 1.0};
    std::vector<double> field(g.total(), 0.0);
    field[g.index(0, 0, 0)] = 1.0;
    Mesh m = make_heightfield_mesh(g, field, 1.0, 2.0);
    // 顶点 0 对应 (i=0,j=0)，其高度 = 场值 × height_scale = 2.0
    QCHECK(std::abs(m.vertices[0].position.y - 2.0) < 1e-6);
}

QTEST(heightfield_color_encodes_sign) {
    Grid g;
    g.dim = 2;
    g.n = {2, 2, 1};
    g.length = {1.0, 1.0, 1.0};
    std::vector<double> field = {1.0, -1.0, 0.0, 0.0};
    Mesh m = make_heightfield_mesh(g, field, 1.0, 1.0);
    // +1 → 红 (r>g 且 r>b)；-1 → 蓝 (b>r 且 b>g)
    QCHECK(m.vertices[0].r > m.vertices[0].b);
    QCHECK(m.vertices[1].b > m.vertices[1].r);
}
