#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <vector>

namespace qhal
{

    using qcomplex = std::complex<double>;

    namespace qgf_detail
    {
        inline constexpr double kPi = 3.14159265358979323846;

        inline void jacobi_eigen(std::vector<double> a, size_t n,
                                 std::vector<double> &evals)
        {
            evals.assign(n, 0.0);
            if (n == 0)
                return;
            std::vector<double> vecs(n * n, 0.0);
            for (size_t i = 0; i < n; ++i)
                vecs[i * n + i] = 1.0;

            for (int sweep = 0; sweep < 100; ++sweep)
            {
                double off = 0.0;
                for (size_t p = 0; p < n; ++p)
                    for (size_t q = p + 1; q < n; ++q)
                        off += a[p * n + q] * a[p * n + q];
                if (off < 1e-26)
                    break;
                for (size_t p = 0; p < n; ++p)
                {
                    for (size_t q = p + 1; q < n; ++q)
                    {
                        const double apq = a[p * n + q];
                        if (std::fabs(apq) < 1e-18)
                            continue;
                        const double theta = (a[q * n + q] - a[p * n + p]) / (2.0 * apq);
                        const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                                         (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                        const double c = 1.0 / std::sqrt(t * t + 1.0);
                        const double s = t * c;
                        for (size_t k = 0; k < n; ++k)
                        {
                            const double akp = a[k * n + p];
                            const double akq = a[k * n + q];
                            a[k * n + p] = c * akp - s * akq;
                            a[k * n + q] = s * akp + c * akq;
                        }
                        for (size_t k = 0; k < n; ++k)
                        {
                            const double apk = a[p * n + k];
                            const double aqk = a[q * n + k];
                            a[p * n + k] = c * apk - s * aqk;
                            a[q * n + k] = s * apk + c * aqk;
                        }
                        for (size_t k = 0; k < n; ++k)
                        {
                            const double vkp = vecs[k * n + p];
                            const double vkq = vecs[k * n + q];
                            vecs[k * n + p] = c * vkp - s * vkq;
                            vecs[k * n + q] = s * vkp + c * vkq;
                        }
                    }
                }
            }
            for (size_t i = 0; i < n; ++i)
                evals[i] = a[i * n + i];
        }

        inline std::vector<double> schmidt_spectrum(const std::vector<qcomplex> &amps,
                                                    uint32_t num_qubits,
                                                    uint32_t first_qubit,
                                                    uint32_t qubit_count)
        {
            std::vector<double> out;
            if (qubit_count == 0 || num_qubits == 0 ||
                first_qubit + qubit_count > num_qubits)
                return out;
            const size_t dim = amps.size();
            const size_t sub_dim = size_t(1) << qubit_count;
            if (dim < sub_dim || dim % sub_dim != 0)
                return out;
            const size_t other = dim / sub_dim;

            std::vector<qcomplex> m(sub_dim * other, qcomplex(0.0, 0.0));
            for (size_t i = 0; i < dim; ++i)
            {
                size_t row = 0, col = 0;
                uint32_t rbit = 0, cbit = 0;
                for (uint32_t b = 0; b < num_qubits; ++b)
                {
                    const size_t set = (i >> b) & size_t(1);
                    if (b >= first_qubit && b < first_qubit + qubit_count)
                    {
                        row |= set << rbit;
                        ++rbit;
                    }
                    else
                    {
                        col |= set << cbit;
                        ++cbit;
                    }
                }
                m[row * other + col] = amps[i];
            }

            const bool transpose = sub_dim < other;
            const size_t side = transpose ? sub_dim : other;
            const size_t inner = transpose ? other : sub_dim;
            std::vector<double> gram(side * side, 0.0);
            for (size_t p = 0; p < side; ++p)
            {
                for (size_t q = p; q < side; ++q)
                {
                    double acc = 0.0;
                    for (size_t k = 0; k < inner; ++k)
                    {
                        const qcomplex lp = transpose ? m[p * other + k] : m[k * side + p];
                        const qcomplex lq = transpose ? m[q * other + k] : m[k * side + q];
                        acc += (lp * std::conj(lq)).real();
                    }
                    gram[p * side + q] = acc;
                    gram[q * side + p] = acc;
                }
            }

            std::vector<double> evals;
            jacobi_eigen(std::move(gram), side, evals);
            out.reserve(evals.size());
            for (double e : evals)
                out.push_back(std::sqrt(std::max(0.0, e)));
            const double lead = out.empty() ? 0.0 : *std::max_element(out.begin(), out.end());
            const double tol = lead * 1e-9;
            out.erase(std::remove_if(out.begin(), out.end(),
                                     [tol](double v) { return v < tol; }),
                      out.end());
            std::sort(out.begin(), out.end(), std::greater<double>());
            return out;
        }
    }

    enum class QgfProjection : int32_t
    {
        LocalBloch = 0,
        Quaternionic = 1,
        EntanglementSphere = 2,
        Nested = 3,
        Hypersphere = 4,
        Wigner = 5,
        Density = 6,
    };

    inline const char *qgf_projection_name(QgfProjection p)
    {
        switch (p)
        {
        case QgfProjection::LocalBloch:        return "local_bloch";
        case QgfProjection::Quaternionic:       return "quaternionic";
        case QgfProjection::EntanglementSphere: return "entanglement_sphere";
        case QgfProjection::Nested:             return "nested";
        case QgfProjection::Hypersphere:        return "hypersphere";
        case QgfProjection::Wigner:             return "wigner";
        case QgfProjection::Density:            return "density";
        }
        return "unknown";
    }

    inline double qgf_state_real_dims(uint32_t num_qubits)
    {
        if (num_qubits == 0)
            return 0.0;
        const double d = std::ldexp(1.0, static_cast<int>(num_qubits));
        return 2.0 * (d - 1.0);
    }

    inline double qgf_projection_target_dims(QgfProjection p, uint32_t num_qubits)
    {
        const double n = static_cast<double>(num_qubits);
        const double src = qgf_state_real_dims(num_qubits);
        auto capped = [src](double drawn) { return std::min(drawn, src); };
        switch (p)
        {
        case QgfProjection::LocalBloch:
            return capped(3.0 * n);
        case QgfProjection::Quaternionic:
            return capped(3.0);
        case QgfProjection::EntanglementSphere:
            return capped(n >= 2.0 ? 9.0 : 3.0);
        case QgfProjection::Nested:
            return std::min(3.0, src);
        case QgfProjection::Hypersphere:
            return std::min(3.0, src);
        case QgfProjection::Wigner:
            return std::min(2.0, src);
        case QgfProjection::Density:
            return src;
        }
        return 0.0;
    }

    struct QuaternionicCoords
    {
        double theta = 0.0;
        double phi = 0.0;
        double quaternionic = 0.0;
        double global_phase = 0.0;
        bool valid = false;
    };

    class QuantumFrame
    {
    public:
        static constexpr uint32_t kMaxQubits = 24;

        QuantumFrame() { set_amplitudes({}, 1); }

        void set_amplitudes(std::vector<qcomplex> amps, uint32_t num_qubits)
        {
            num_qubits_ = std::clamp<uint32_t>(num_qubits, 1, kMaxQubits);
            const size_t want = hilbert_dim(num_qubits_);
            if (amps.size() > want)
                amps.resize(want);
            amps.resize(want, qcomplex(0.0, 0.0));
            amps_ = std::move(amps);
        }

        uint32_t num_qubits() const { return num_qubits_; }
        size_t hilbert_dim() const { return amps_.size(); }

        const std::vector<qcomplex> &amplitudes() const { return amps_; }
        const qcomplex &amplitude(size_t basis) const { return amps_[basis]; }

        double normalization_residual() const
        {
            return std::fabs(1.0 - total_norm());
        }

        double collision_probability() const
        {
            double acc = 0.0;
            for (const auto &a : amps_)
                acc += std::norm(a) * std::norm(a);
            return acc;
        }

        double linear_entropy() const
        {
            const double n = total_norm();
            if (n <= 0.0)
                return 0.0;
            return std::clamp(1.0 - collision_probability() / (n * n), 0.0, 1.0);
        }

        double probability(size_t basis) const
        {
            return basis < amps_.size() ? std::norm(amps_[basis]) : 0.0;
        }

        double phase(size_t basis) const
        {
            if (basis >= amps_.size())
                return 0.0;
            return std::arg(amps_[basis]);
        }

        void local_bloch(uint32_t qubit, double &rx, double &ry, double &rz) const
        {
            const size_t q = qubit < num_qubits_ ? qubit : 0;
            const size_t mask = size_t(1) << q;
            double sx = 0.0, sy = 0.0, sz = 0.0;
            for (size_t i = 0; i < amps_.size(); ++i)
            {
                const size_t j = i ^ mask;
                if (j < i)
                    continue;
                const qcomplex z = amps_[i] * std::conj(amps_[j]);
                sx += 2.0 * z.real();
                sy += -2.0 * z.imag();
                sz += std::norm(amps_[i]) - std::norm(amps_[j]);
            }
            const double n = total_norm();
            const double s = n > 0.0 ? 1.0 / n : 0.0;
            rx = sx * s;
            ry = sy * s;
            rz = sz * s;
        }

        void local_bloch_polar(uint32_t qubit, double &theta, double &phi) const
        {
            double x, y, z;
            local_bloch(qubit, x, y, z);
            const double r = std::sqrt(x * x + y * y + z * z);
            theta = std::acos(std::clamp(z, -1.0, 1.0));
            phi = (r > 1e-12) ? std::atan2(y, x) : 0.0;
        }

        QuaternionicCoords quaternionic_coords() const
        {
            QuaternionicCoords q{};
            if (num_qubits_ != 2 || amps_.size() < 4)
                return q;
            const qcomplex a = amps_[0], b = amps_[1];
            const qcomplex c = amps_[2], d = amps_[3];
            const double n = total_norm();
            if (n <= 0.0)
                return q;

            const qcomplex cross = a * d - b * c;
            q.quaternionic = std::abs(cross) / (n * n);

            const double conc = 2.0 * q.quaternionic;
            const double theta_s = 0.5 * std::asin(std::clamp(conc, -1.0, 1.0));
            q.theta = std::clamp(theta_s, 0.0, qgf_detail::kPi * 0.5);

            q.phi = (std::abs(c) > 1e-15) ? std::arg(a * std::conj(c))
                                          : ((std::abs(b) > 1e-15) ? std::arg(b * std::conj(d)) : 0.0);
            q.global_phase = std::arg(a);
            q.valid = true;
            return q;
        }

        double concurrence() const
        {
            const QuaternionicCoords q = quaternionic_coords();
            return q.valid ? 2.0 * q.quaternionic : 0.0;
        }

        bool separable_across(uint32_t first_qubit, uint32_t qubit_count) const
        {
            if (qubit_count == 0)
                return true;
            if (first_qubit + qubit_count > num_qubits_)
                return true;
            if (qubit_count >= num_qubits_)
            {
                return true;
            }
            const std::vector<double> lam =
                qgf_detail::schmidt_spectrum(amps_, num_qubits_, first_qubit, qubit_count);
            if (lam.empty())
                return false;
            return lam.size() <= 1;
        }

        bool entanglement_exists(uint32_t qa = 0, uint32_t qb = 1) const
        {
            if (num_qubits_ <= 1)
                return false;
            if (num_qubits_ == 2)
                return concurrence() > 1e-9;
            const uint32_t lo = std::min(qa, qb);
            const uint32_t hi = std::max(qa, qb);
            return !separable_across(lo, hi - lo + 1);
        }

        double pair_entanglement(uint32_t qa = 0, uint32_t qb = 1) const
        {
            if (num_qubits_ != 2 || qa == qb || qa >= num_qubits_ || qb >= num_qubits_)
                return 0.0;
            return concurrence();
        }

        double von_neumann_entropy(uint32_t first_qubit, uint32_t qubit_count) const
        {
            if (qubit_count == 0 || first_qubit + qubit_count > num_qubits_)
                return 0.0;
            const double n = total_norm();
            if (n <= 0.0)
                return 0.0;

            if (qubit_count >= num_qubits_)
                return 0.0;

            const std::vector<double> schmidt =
                qgf_detail::schmidt_spectrum(amps_, num_qubits_, first_qubit, qubit_count);
            double h = 0.0;
            for (double lam : schmidt)
            {
                const double p = lam * lam / (n * n);
                if (p > 1e-15)
                    h -= p * std::log2(p);
            }
            return std::max(0.0, h);
        }

        static double dirichlet(double x, int peaks)
        {
            if (peaks < 1)
                return 1.0;
            const double num = std::sin((peaks + 0.5) * x);
            const double den = std::sin(0.5 * x);
            if (std::fabs(den) < 1e-12)
            {
                return 2.0 * peaks + 1.0;
            }
            return num / den;
        }

        qcomplex amplitude_kernel(size_t i, size_t j, int peaks) const
        {
            if (i >= amps_.size() || j >= amps_.size())
                return qcomplex(0.0, 0.0);
            const qcomplex z = amps_[i] * std::conj(amps_[j]);
            const double x = qgf_detail::kPi * static_cast<double>(i + j) /
                             static_cast<double>(std::max<size_t>(peaks, 1));
            return dirichlet(x, peaks) * z;
        }

        static uint32_t nested_shell_count(uint32_t num_qubits)
        {
            if (num_qubits <= 1)
                return 1;
            return (size_t(1) << (num_qubits - 1));
        }

    private:
        static size_t hilbert_dim(uint32_t n)
        {
            return size_t(1) << std::min<uint32_t>(n, kMaxQubits);
        }

        double total_norm() const
        {
            double acc = 0.0;
            for (const auto &a : amps_)
                acc += std::norm(a);
            return std::sqrt(acc);
        }

        uint32_t num_qubits_ = 1;
        std::vector<qcomplex> amps_;
    };

    struct QgfBar
    {
        double birth = 0.0;
        double death = 0.0;
        double persist = 0.0;

        double persistence() const { return death - birth; }
    };

    class EntanglementBarcode
    {
    public:
        static void from_schmidt(const std::vector<double> &schmidt, std::vector<QgfBar> &out)
        {
            out.clear();
            if (schmidt.empty())
                return;
            std::vector<double> lam = schmidt;
            std::sort(lam.begin(), lam.end());
            out.push_back({1.0, lam.back(), 1.0 - lam.back()});
            if (lam.size() >= 2)
                out.push_back({lam[1], lam[0], lam[0] - lam[1]});
        }

        static std::vector<double> schmidt_spectrum(const QuantumFrame &f,
                                                    uint32_t first_qubit,
                                                    uint32_t qubits)
        {
            return qgf_detail::schmidt_spectrum(f.amplitudes(), f.num_qubits(),
                                                first_qubit, qubits);
        }

        static void of_frame(const QuantumFrame &f, uint32_t first_qubit, uint32_t qubits,
                             std::vector<QgfBar> &out)
        {
            from_schmidt(schmidt_spectrum(f, first_qubit, qubits), out);
        }

        static bool same_class(const std::vector<QgfBar> &a, const std::vector<QgfBar> &b,
                               double tol = 1e-6)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::fabs(a[i].birth - b[i].birth) > tol ||
                    std::fabs(a[i].death - b[i].death) > tol)
                    return false;
            return true;
        }
    };

}