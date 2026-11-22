#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace numqk
{
    namespace nqcmath
    {
        template <typename Scalar, int R, int C>
        class Matrix;

        template <typename Scalar, int R, int C>
        class Matrix
        {
        public:
            static constexpr int rows = R;
            static constexpr int cols = C;
            static constexpr size_t size() { return static_cast<size_t>(R) * static_cast<size_t>(C); }

            Scalar m[R * C];

            Matrix() { fill(Scalar(0)); }

            explicit Matrix(Scalar v) { fill(v); }

            void fill(Scalar v)
            {
                for (size_t i = 0; i < size(); ++i)
                    m[i] = v;
            }

            Scalar &operator()(int r, int c) { return m[static_cast<size_t>(r) * C + c]; }
            const Scalar &operator()(int r, int c) const { return m[static_cast<size_t>(r) * C + c]; }

            Scalar *data() { return m; }
            const Scalar *data() const { return m; }

            Matrix operator+(const Matrix &o) const
            {
                Matrix r;
                for (size_t i = 0; i < size(); ++i)
                    r.m[i] = m[i] + o.m[i];
                return r;
            }
            Matrix operator-(const Matrix &o) const
            {
                Matrix r;
                for (size_t i = 0; i < size(); ++i)
                    r.m[i] = m[i] - o.m[i];
                return r;
            }
            Matrix operator*(const Scalar &s) const
            {
                Matrix r;
                for (size_t i = 0; i < size(); ++i)
                    r.m[i] = m[i] * s;
                return r;
            }
            Matrix &operator+=(const Matrix &o)
            {
                for (size_t i = 0; i < size(); ++i)
                    m[i] += o.m[i];
                return *this;
            }
            Matrix &operator-=(const Matrix &o)
            {
                for (size_t i = 0; i < size(); ++i)
                    m[i] -= o.m[i];
                return *this;
            }
            Matrix &operator*=(const Scalar &s)
            {
                for (size_t i = 0; i < size(); ++i)
                    m[i] *= s;
                return *this;
            }

            template <int C2>
            Matrix<Scalar, R, C2> operator*(const Matrix<Scalar, C, C2> &o) const
            {
                Matrix<Scalar, R, C2> r;
                for (int i = 0; i < R; ++i)
                    for (int k = 0; k < C; ++k)
                    {
                        const Scalar a = (*this)(i, k);
                        if (a == Scalar(0))
                            continue;
                        for (int j = 0; j < C2; ++j)
                            r(i, j) += a * o(k, j);
                    }
                return r;
            }

            Matrix<Scalar, C, R> transpose() const
            {
                Matrix<Scalar, C, R> t;
                for (int i = 0; i < R; ++i)
                    for (int j = 0; j < C; ++j)
                        t(j, i) = (*this)(i, j);
                return t;
            }

            Scalar sum() const
            {
                Scalar s = Scalar(0);
                for (size_t i = 0; i < size(); ++i)
                    s += m[i];
                return s;
            }
        };

        namespace Parallel
        {
            inline int get_threads()
            {
                int t = static_cast<int>(std::thread::hardware_concurrency());
                return t < 2 ? 2 : t;
            }

            constexpr size_t SEQUENTIAL_THRESHOLD = 4096;

            constexpr size_t REDUCE_BLOCKS = 256;

            class ThreadPool
            {
            public:
                explicit ThreadPool(size_t nthreads) : nthreads_(nthreads < 1 ? 1 : nthreads)
                {
                    workers_.reserve(nthreads_ - 1);
                    for (size_t i = 0; i + 1 < nthreads_; ++i)
                        workers_.emplace_back([this] { worker_loop(); });
                }

                ~ThreadPool()
                {
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        stop_ = true;
                    }
                    cv_task_.notify_all();
                    for (auto &w : workers_)
                        if (w.joinable())
                            w.join();
                }

                ThreadPool(const ThreadPool &) = delete;
                ThreadPool &operator=(const ThreadPool &) = delete;

                size_t size() const { return nthreads_; }

                void run_tasks(size_t n, const std::function<void(size_t)> &fn)
                {
                    if (n == 0)
                        return;
                    if (n > 1)
                    {
                        {
                            std::lock_guard<std::mutex> lk(mtx_);
                            for (size_t i = 1; i < n; ++i)
                                tasks_.emplace_back([&fn, i] { fn(i); });
                            active_ += (n - 1);
                        }
                        cv_task_.notify_all();
                    }
                    fn(0);
                    wait_idle();
                }

                void for_each(size_t count, const std::function<void(size_t)> &fn,
                              size_t chunk = 0)
                {
                    if (count == 0)
                        return;
                    if (chunk == 0)
                    {
                        chunk = std::max<size_t>(1, count / (nthreads_ * 8));
                    }

                    std::atomic<size_t> cursor{0};
                    auto task = [&](size_t) {
                        for (;;)
                        {
                            const size_t begin = cursor.fetch_add(chunk, std::memory_order_relaxed);
                            if (begin >= count)
                                break;
                            const size_t end = std::min(count, begin + chunk);
                            for (size_t i = begin; i < end; ++i)
                                fn(i);
                        }
                    };
                    run_tasks(nthreads_, task);
                }

            private:
                void worker_loop()
                {
                    for (;;)
                    {
                        std::function<void()> task;
                        {
                            std::unique_lock<std::mutex> lk(mtx_);
                            cv_task_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
                            if (stop_ && tasks_.empty())
                                return;
                            task = std::move(tasks_.front());
                            tasks_.pop_front();
                        }
                        task();
                        {
                            std::lock_guard<std::mutex> lk(mtx_);
                            --active_;
                            if (active_ == 0)
                                cv_done_.notify_all();
                        }
                    }
                }

                void wait_idle()
                {
                    std::unique_lock<std::mutex> lk(mtx_);
                    cv_done_.wait(lk, [this] { return active_ == 0; });
                }

                size_t nthreads_ = 1;
                std::vector<std::thread> workers_;
                std::deque<std::function<void()>> tasks_;
                std::mutex mtx_;
                std::condition_variable cv_task_;
                std::condition_variable cv_done_;
                size_t active_ = 0;
                bool stop_ = false;
            };

            inline ThreadPool &pool()
            {
                static ThreadPool p(static_cast<size_t>(get_threads()));
                return p;
            }

            inline void parallel_for(size_t count, const std::function<void(size_t)> &fn,
                                     size_t chunk = 0)
            {
                if (count <= SEQUENTIAL_THRESHOLD)
                {
                    for (size_t i = 0; i < count; ++i)
                        fn(i);
                    return;
                }
                pool().for_each(count, fn, chunk);
            }

            template <typename T, typename Elem, typename Comb>
            T parallel_reduce(size_t count, T init, Elem &&elem, Comb &&comb)
            {
                if (count == 0)
                    return init;
                const size_t nblocks = std::min<size_t>(REDUCE_BLOCKS, count);
                const size_t block = (count + nblocks - 1) / nblocks;

                std::vector<T> partial(nblocks, init);
                std::atomic<size_t> cursor{0};
                auto task = [&](size_t) {
                    for (;;)
                    {
                        const size_t b = cursor.fetch_add(1, std::memory_order_relaxed);
                        if (b >= nblocks)
                            break;
                        const size_t begin = b * block;
                        const size_t end = std::min(count, begin + block);
                        T acc = init;
                        for (size_t i = begin; i < end; ++i)
                            acc = comb(acc, elem(i));
                        partial[b] = acc;
                    }
                };
                pool().run_tasks(static_cast<size_t>(get_threads()), task);

                T total = init;
                for (size_t b = 0; b < nblocks; ++b)
                    total = comb(total, partial[b]);
                return total;
            }

            template <typename S>
            static void matmul_blocked(const S *A, const S *B, S *C, size_t M, size_t K,
                                       size_t N, size_t i0, size_t j0)
            {
                constexpr size_t BM = 64;
                constexpr size_t BN = 64;
                constexpr size_t BK = 32;
                const size_t i_end = std::min(M, i0 + BM);
                const size_t j_end = std::min(N, j0 + BN);

                for (size_t kk = 0; kk < K; kk += BK)
                {
                    const size_t k_end = std::min(K, kk + BK);
                    for (size_t i = i0; i < i_end; ++i)
                    {
                        for (size_t k = kk; k < k_end; ++k)
                        {
                            const S a = A[i * K + k];
                            if (a == S(0))
                                continue;
                            const S *brow = B + k * N;
                            S *crow = C + i * N;
                            for (size_t j = j0; j < j_end; ++j)
                                crow[j] += a * brow[j];
                        }
                    }
                }
            }

            template <typename S>
            void matmul(const S *A, const S *B, S *C, size_t M, size_t K, size_t N)
            {
                if (M == 0 || K == 0 || N == 0)
                    return;

                constexpr size_t BM = 64;
                constexpr size_t BN = 64;
                constexpr size_t BK = 32;

                if (M * N <= SEQUENTIAL_THRESHOLD)
                {
                    matmul_blocked(A, B, C, M, K, N, 0, 0);
                    return;
                }

                const size_t nbi = (M + BM - 1) / BM;
                const size_t nbj = (N + BN - 1) / BN;
                const size_t nblocks = nbi * nbj;
                std::atomic<size_t> cursor{0};
                auto task = [&](size_t) {
                    for (;;)
                    {
                        const size_t b = cursor.fetch_add(1, std::memory_order_relaxed);
                        if (b >= nblocks)
                            break;
                        matmul_blocked(A, B, C, M, K, N, (b / nbj) * BM, (b % nbj) * BN);
                    }
                };
                pool().run_tasks(static_cast<size_t>(get_threads()), task);
            }

            template <typename Scalar, int R, int C>
            void parallel_elementwise(const Matrix<Scalar, R, C> &src1,
                                      const Matrix<Scalar, R, C> &src2,
                                      Matrix<Scalar, R, C> &out,
                                      std::function<void(size_t)> op)
            {
                const size_t total = static_cast<size_t>(R) * static_cast<size_t>(C);
                parallel_for(total, [&op](size_t i) { op(i); });
            }

            template <typename Scalar, int R, int C>
            void parallel_elementwise(const Matrix<Scalar, R, C> &src1,
                                      const Matrix<Scalar, R, C> &src2,
                                      Matrix<Scalar, R, C> &out,
                                      std::function<void(size_t, size_t, size_t)> op)
            {
                const size_t total = static_cast<size_t>(R) * static_cast<size_t>(C);
                parallel_for(total, [&op](size_t i)
                             {
                                 const size_t row = i / static_cast<size_t>(C);
                                 const size_t col = i % static_cast<size_t>(C);
                                 op(i, row, col);
                             });
            }

        }

        template <typename Scalar>
        class MatrixX
        {
        public:
            size_t rows_ = 0;
            size_t cols_ = 0;
            std::vector<Scalar> m;

            MatrixX() = default;

            MatrixX(size_t r, size_t c) : rows_(r), cols_(c), m(r * c, Scalar(0)) {}

            MatrixX(size_t r, size_t c, Scalar v) : rows_(r), cols_(c), m(r * c, v) {}

            size_t rows() const { return rows_; }
            size_t cols() const { return cols_; }
            size_t size() const { return m.size(); }
            bool empty() const { return m.empty(); }

            Scalar &operator()(size_t r, size_t c) { return m[r * cols_ + c]; }
            const Scalar &operator()(size_t r, size_t c) const { return m[r * cols_ + c]; }

            Scalar *data() { return m.data(); }
            const Scalar *data() const { return m.data(); }

            void fill(Scalar v) { std::fill(m.begin(), m.end(), v); }
            void set_zero() { fill(Scalar(0)); }

            MatrixX &operator+=(const MatrixX &o)
            {
                require_same(o);
                for (size_t i = 0; i < m.size(); ++i)
                    m[i] += o.m[i];
                return *this;
            }
            MatrixX &operator-=(const MatrixX &o)
            {
                require_same(o);
                for (size_t i = 0; i < m.size(); ++i)
                    m[i] -= o.m[i];
                return *this;
            }
            MatrixX &operator*=(const Scalar &s)
            {
                for (size_t i = 0; i < m.size(); ++i)
                    m[i] *= s;
                return *this;
            }
            MatrixX operator+(const MatrixX &o) const
            {
                MatrixX r(*this);
                r += o;
                return r;
            }
            MatrixX operator-(const MatrixX &o) const
            {
                MatrixX r(*this);
                r -= o;
                return r;
            }
            MatrixX operator*(const Scalar &s) const
            {
                MatrixX r(*this);
                r *= s;
                return r;
            }

            MatrixX operator*(const MatrixX &o) const
            {
                if (cols_ != o.rows_)
                    throw std::invalid_argument("nqcmath: matrix product shape mismatch");
                MatrixX r(rows_, o.cols_);
                Parallel::matmul(m.data(), o.m.data(), r.m.data(), rows_, cols_, o.cols_);
                return r;
            }

            MatrixX transpose() const
            {
                MatrixX t(cols_, rows_);
                for (size_t i = 0; i < rows_; ++i)
                    for (size_t j = 0; j < cols_; ++j)
                        t(j, i) = (*this)(i, j);
                return t;
            }

            Scalar sum() const
            {
                Scalar s = Scalar(0);
                for (size_t i = 0; i < m.size(); ++i)
                    s += m[i];
                return s;
            }

            double frobenius_norm() const
            {
                double s = 0.0;
                for (size_t i = 0; i < m.size(); ++i)
                    s += static_cast<double>(std::abs(m[i]));
                return std::sqrt(s);
            }

        private:
            void require_same(const MatrixX &o) const
            {
                if (rows_ != o.rows_ || cols_ != o.cols_)
                    throw std::invalid_argument("nqcmath: matrix shape mismatch");
            }
        };

        using MatrixXf = MatrixX<float>;
        using MatrixXd = MatrixX<double>;

        template <typename Scalar>
        class Vector
        {
        public:
            std::vector<Scalar> v;

            Vector() = default;
            explicit Vector(size_t n) : v(n, Scalar(0)) {}
            Vector(size_t n, Scalar s) : v(n, s) {}

            size_t size() const { return v.size(); }
            bool empty() const { return v.empty(); }
            Scalar &operator[](size_t i) { return v[i]; }
            const Scalar &operator[](size_t i) const { return v[i]; }
            Scalar *data() { return v.data(); }
            const Scalar *data() const { return v.data(); }
            void fill(Scalar s) { std::fill(v.begin(), v.end(), s); }
            Scalar sum() const
            {
                Scalar s = Scalar(0);
                for (size_t i = 0; i < v.size(); ++i)
                    s += v[i];
                return s;
            }
            double norm() const
            {
                double s = 0.0;
                for (size_t i = 0; i < v.size(); ++i)
                    s += static_cast<double>(std::abs(v[i]));
                return std::sqrt(s);
            }
        };

        using VectorXf = Vector<float>;
        using VectorXd = Vector<double>;
    }
}