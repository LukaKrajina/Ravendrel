#pragma once
#include <vector>
#include <cstddef>
#include <cstdint>

namespace qlm
{

    struct IFPHead
    {
        int K = 4;
        int s = 2;
        std::vector<double> w = {0.5, 0.25, 0.15, 0.15};

        int future_chunk_index(int current_i, int k) const
        {
            return (current_i + 1) + 1 + (k - 1) * s;
        }

        double chunk_loss(const double *pred, uint8_t target_token, int k) const
        {
            double loss = 0.0;
            for (size_t j = 0; j < 8; ++j)
            {
                double x1 = 2.0 * ((target_token >> j) & 1) - 1.0;
                double diff = pred[j] - x1;
                loss += diff * diff;
            }
            loss /= 8.0;
            return (k < static_cast<int>(w.size())) ? w[k] * loss : 0.0;
        }

        double ifp_loss(const std::vector<std::vector<double>> &future_preds,
                        const std::vector<uint8_t> &future_targets) const
        {
            double total = 0.0;
            for (int k = 0; k < static_cast<int>(future_preds.size()) &&
                            k < static_cast<int>(future_targets.size()); ++k)
            {
                total += chunk_loss(future_preds[k].data(), future_targets[k], k);
            }
            return total;
        }
    };
}