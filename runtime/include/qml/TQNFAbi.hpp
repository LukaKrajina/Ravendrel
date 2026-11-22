#pragma once
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
    inline std::vector<numqk::CTensor> parse_pauli_generators(const std::string &spec, int n_qubits)
    {
        std::vector<numqk::CTensor> gens;
        if (n_qubits <= 0)
            return gens;

        std::stringstream ss(spec);
        std::string tok;
        while (std::getline(ss, tok, ','))
        {
            size_t b = tok.find_first_not_of(" \t\r\n");
            size_t e = tok.find_last_not_of(" \t\r\n");
            if (b == std::string::npos)
                continue;
            tok = tok.substr(b, e - b + 1);

            if (static_cast<int>(tok.size()) != n_qubits)
                continue;

            std::vector<int> spec_v(static_cast<size_t>(n_qubits));
            for (int i = 0; i < n_qubits; ++i)
            {
                char c = static_cast<char>(std::toupper(static_cast<unsigned char>(tok[i])));
                int p = 0;
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