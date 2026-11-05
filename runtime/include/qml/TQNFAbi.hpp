#pragma once
//
// TQNFAbi.hpp —— TQNF 拓扑量子神经场语言层 ABI 桥接
//
// 把 numqk/QuantumGeometry.hpp 的 DLA 可训练性诊断暴露为 rvdl 语言层可调用的
// extern "C" 函数。对应 TQNF 设计法则「拓扑-代数收缩律」（Ragone 等）。
//
//   rvdl_dla_dim(spec, n_qubits) -> int
//     spec      逗号分隔的 Pauli 串（如 "X,Z" / "XX,ZZ"），每串长度 = n_qubits，
//               字符 ∈ {I,X,Y,Z}（大小写均可）
//     n_qubits  每个 Pauli 串作用的 qubit 数
//     返回      动力学李代数（DLA）维数，维数越小越可训练（规避贫瘠高原）
//
// 纯 numqk 依赖，后端无关。
//
#include "../qhal/Export.hpp"
#include "../numqk/QuantumGeometry.hpp"
#include <string>
#include <vector>
#include <sstream>
#include <cctype>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

namespace qml
{
    // 解析 "X,Z" → { pauli_x(), pauli_z() }（或 "XX,ZZ" → n 比特 Pauli 串）
    inline std::vector<numqk::CTensor> parse_pauli_generators(const std::string &spec, int n_qubits)
    {
        std::vector<numqk::CTensor> gens;
        if (n_qubits <= 0)
            return gens;

        std::stringstream ss(spec);
        std::string tok;
        while (std::getline(ss, tok, ','))
        {
            // 去除首尾空白
            size_t b = tok.find_first_not_of(" \t\r\n");
            size_t e = tok.find_last_not_of(" \t\r\n");
            if (b == std::string::npos)
                continue;
            tok = tok.substr(b, e - b + 1);

            if (static_cast<int>(tok.size()) != n_qubits)
                continue; // 长度不符的串跳过

            std::vector<int> spec_v(static_cast<size_t>(n_qubits));
            for (int i = 0; i < n_qubits; ++i)
            {
                char c = static_cast<char>(std::toupper(static_cast<unsigned char>(tok[i])));
                int p = 0; // I
                if (c == 'X')
                    p = 1;
                else if (c == 'Y')
                    p = 2;
                else if (c == 'Z')
                    p = 3;
                spec_v[static_cast<size_t>(i)] = p;
            }
            gens.push_back(numqk::n_qubit_pauli(spec_v));
        }
        return gens;
    }
}

extern "C"
{
    RVDL_HOST_EXPORT int rvdl_dla_dim(const char *spec, int n_qubits);
}

#if defined(RVDL_RT_BUILD)
int rvdl_dla_dim(const char *spec, int n_qubits)
{
    auto gens = qml::parse_pauli_generators(spec ? spec : "", n_qubits);
    if (gens.empty())
        return 0;
    return static_cast<int>(numqk::dla_dimension(gens));
}
#endif