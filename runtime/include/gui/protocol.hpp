#pragma once
#include <vector>
#include <string>
#include <complex>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <cstring>

namespace qgui
{

    inline constexpr const char *CMD_PING = "PING";
    inline constexpr const char *CMD_GET_SNAPSHOT = "GET_SNAPSHOT";

    struct GateRecord
    {
        char name[16] = {0};
        int target = -1;
        int control = -1;
        uint64_t step = 0;
    };

    struct MeasurementRecord
    {
        int qubit = -1;
        int result = -1;
        uint64_t seq = 0;
    };

    struct ObjectRecord
    {
        std::string type;
        std::vector<int> ids;
        double theta = 0.0;   // 任意基极角（basis_state）
        double phi = 0.0;     // 任意基方位角
    };

    struct StateSnapshot
    {
        uint64_t generation = 0;
        uint32_t num_qubits = 0;
        std::vector<std::complex<double>> amplitudes; // 长度为 2^num_qubits
        std::vector<GateRecord> gates;
        std::vector<MeasurementRecord> measurements;
        std::vector<ObjectRecord> objects;
        std::string backend_name;
    };

    // ------------------------------------------------------------------
    // 线格式（基于行）：
    //   响应：SNAPSHOT <生成> <量子比特数>
    //   Q <实部> <虚部>                     # 每个基态一条
    //   G <名称> <目标> <控制> <步长>
    //   M <量子比特> <结果> <序列>
    //   O <类型> <id0> <id1> ...
    //   B <后端名称>
    //   END_SNAPSHOT
    // ------------------------------------------------------------------
    inline std::string serialize(const StateSnapshot &s)
    {
        std::ostringstream os;
        os << "RESPONSE: SNAPSHOT " << s.generation << " " << s.num_qubits << "\n";
        os << std::setprecision(17);
        for (const auto &a : s.amplitudes)
        {
            os << "Q " << a.real() << " " << a.imag() << "\n";
        }
        for (const auto &g : s.gates)
        {
            os << "G " << g.name << " " << g.target << " " << g.control << " "
               << g.step << "\n";
        }
        for (const auto &m : s.measurements)
        {
            os << "M " << m.qubit << " " << m.result << " " << m.seq << "\n";
        }
        for (const auto &o : s.objects)
        {
            os << "O " << o.type;
            for (int id : o.ids)
                os << " " << id;
            os << "\n";
            if (o.theta != 0.0 || o.phi != 0.0)
                os << "P " << o.theta << " " << o.phi << "\n";
        }
        os << "B " << s.backend_name << "\n";
        os << "END_SNAPSHOT\n";
        return os.str();
    }

    inline bool deserialize(const std::string &data, StateSnapshot &out)
    {
        out = StateSnapshot{};
        std::istringstream is(data);
        std::string line;
        bool in_snapshot = false;

        while (std::getline(is, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line == "END_SNAPSHOT")
                return in_snapshot;

            if (line.rfind("RESPONSE: SNAPSHOT", 0) == 0)
            {
                in_snapshot = true;
                std::istringstream ls(line);
                std::string a, b;
                ls >> a >> b >> out.generation >> out.num_qubits;
                continue;
            }
            if (!in_snapshot || line.empty())
                continue;

            std::istringstream ls(line);
            char tag = '\0';
            ls >> tag;
            switch (tag)
            {
            case 'Q':
            {
                double re = 0.0, im = 0.0;
                ls >> re >> im;
                out.amplitudes.emplace_back(re, im);
                break;
            }
            case 'G':
            {
                GateRecord g{};
                std::string name;
                ls >> name;
                std::snprintf(g.name, sizeof(g.name), "%s", name.c_str());
                ls >> g.target >> g.control >> g.step;
                out.gates.push_back(g);
                break;
            }
            case 'M':
            {
                MeasurementRecord m{};
                ls >> m.qubit >> m.result >> m.seq;
                out.measurements.push_back(m);
                break;
            }
            case 'O':
            {
                ObjectRecord o;
                ls >> o.type;
                int id = 0;
                while (ls >> id)
                    o.ids.push_back(id);
                out.objects.push_back(std::move(o));
                break;
            }
            case 'P':
            {
                if (!out.objects.empty())
                    ls >> out.objects.back().theta >> out.objects.back().phi;
                break;
            }
            case 'B':
            {
                std::getline(ls, out.backend_name);
                size_t p = out.backend_name.find_first_not_of(" \t");
                if (p != std::string::npos)
                    out.backend_name = out.backend_name.substr(p);
                else
                    out.backend_name.clear();
                break;
            }
            default:
                break;
            }
        }
        return in_snapshot;
    }
}