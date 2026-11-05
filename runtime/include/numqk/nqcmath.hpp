#pragma once
// ============================================================================
// nqcmath —— 高并发并行层普通数学库
//
// 说明与定位
// ----
// numqk 原本只有 Tensor + 自动微分(早期)。QLM（量子学习机）真正缺的是**稠密线性代数**:
// 量子几何张量是 p×p（p = 层数×(n+8) ≈ 144），自然梯度要伪逆，
// 而 Tensor::matmul 是朴素的 i-k-j 单线程三重循环 —— 整个训练循环是串行的。
// 所以：
//
//   · nqcmath::Matrix<Scalar,R,C>   定长矩阵（栈上，编译期维度）
//   · nqcmath::MatrixX<Scalar>      动态矩阵（MatrixXf = MatrixX<float>）
//   · nqcmath::Vector<T>            动态向量
//   · nqcmath::Parallel             并行层：常驻线程池 + 动态块调度 + 分块
//                                   matmul（缓存友好）
//
// 并行层是「常驻线程池」
// ----------------------------
// 严谨看,并行 for 每次调用都 spawn 一批 std::thread 再 join：在 1900fps
// 规模的 QLM 训练循环里，线程创建的开销比计算本身还大（一次 spawn ≈ 20-50µs，
// 而 144×144 的 matmul 只要几微秒）。这里的池是**进程内单例、惰性创建**，
// 线程常驻，用「原子游标 + 动态块」领取任务：
//   · 负载均衡：块不是静态切分，慢的块不会拖住整帧；
//   · 主线程也参与计算（不空等）；
//   · 细粒度操作不逐元素加锁（原子游标是每块一次，不是每元素一次）。
//
// 对于浮点归约的可复现性
// ------------------
// 浮点加法不满足结合律，所以「并行求和」与「串行求和」的末位必然可能不同。
// 本层的归约把块数**固定**为常量（不随线程数变化），块内串行、块间按块序
// 合并 —— 于是结果与机器核数无关、逐位可复现；与朴素串行求和的差异只在
// 末位浮点误差。
//
// 数值约定
// --------
// 行主序（与 numqk::Tensor 一致）：A(i,j) 存在 a[i*cols + j]。
// ============================================================================

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

        // ─────────────────────────────────────────────────────────────
        // Matrix<Scalar, R, C> —— 定长矩阵（编译期维度，栈上分配）
        // ─────────────────────────────────────────────────────────────
        //
        //  维度是类型的一部分，「矩阵乘法结果不匹配」这种错误在**编译期**
        // 就被抓住，而不是运行时才发现形状对不上。
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

            /// 矩阵乘（维度不匹配在编译期就被抓住：R×C · C×C2）
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

        // ─────────────────────────────────────────────────────────────
        // Parallel —— 高并发并行层
        // ─────────────────────────────────────────────────────────────
        namespace Parallel
        {
            /// 硬件线程数，最小 2（单线程退化会让「并行」路径永远不被测到）。
            inline int get_threads()
            {
                int t = static_cast<int>(std::thread::hardware_concurrency());
                return t < 2 ? 2 : t;
            }

            /// 小规模直接串行：线程调度开销比计算还贵,而且小规模没必要开多线程,增加延迟不符合原则。
            constexpr size_t SEQUENTIAL_THRESHOLD = 4096;

            /// 归约块数（**固定**，不随线程数变化）—— 见文件头的可复现性说明。
            constexpr size_t REDUCE_BLOCKS = 256;

            // ── 常驻线程池 ──────────────────────────────────────────────
            //
            // 一次 spawn 一批线程再 join 的写法，在 QLM 那种「每帧都算」的
            // 场景里是纯开销：一次 std::thread 创建约 20-50µs，而 144×144 的
            // 矩阵乘只有几微秒。池是**进程内单例、惰性创建**，线程常驻。
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

                /// 提交 n 个任务并等待全部完成。任务 0 由**调用线程**执行
                /// （不空等），1..n-1 交给池里的 worker。
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

                /// 并行 for：[0,count) 逐元素调用 fn(i)。
                ///
                /// 块不是静态切分：所有块共享一个**原子游标**，谁快谁多拿 ——
                /// 静态切分下最慢的块决定整帧的耗时（QLM 的矩阵乘里，不同
                /// 行的数值密度差别很大，静态切分会明显失衡）。
                void for_each(size_t count, const std::function<void(size_t)> &fn,
                              size_t chunk = 0)
                {
                    if (count == 0)
                        return;
                    if (chunk == 0)
                    {
                        // 目标块数 = 线程数 × 8：太多则调度开销占比上升，
                        // 太少则负载不均（一个慢块就能拖住整批）。
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

            /// 进程内单例（惰性创建）。Meyers 单例在 C++11 起是线程安全的。
            inline ThreadPool &pool()
            {
                static ThreadPool p(static_cast<size_t>(get_threads()));
                return p;
            }

            /// 并行 for：[0,count) 逐元素调用 fn(i)。
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

            /// 并行归约：elem(i) 出元素，comb 合并两个部分结果。
            ///
            /// 块数固定为 REDUCE_BLOCKS（不随线程数变），块内串行、块间按块序
            /// 合并 ⇒ 结果与机器核数无关、逐位可复现。
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

            // 命名空间没有访问控制 —— 下面这个辅助函数与上面的公开 API 同级。
            /// 一块矩阵乘（行块 i0、列块 j0）。
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

            /// 分块矩阵乘（缓存友好 + 并行）。
            ///
            /// 循环顺序是 **i-k-j**（不是 i-j-k）：B 的一行与 C 的一行都是连续
            /// 访问，A 的一个元素被整行复用 —— 这正是让分块有意义的顺序。
            /// 分块尺寸 64×64×32 让工作集落在 L2 里。
            template <typename S>
            void matmul(const S *A, const S *B, S *C, size_t M, size_t K, size_t N)
            {
                if (M == 0 || K == 0 || N == 0)
                    return;

                constexpr size_t BM = 64;
                constexpr size_t BN = 64;
                constexpr size_t BK = 32;

                // 小矩阵：spawn 线程不划算，串行（但仍用缓存友好的顺序）。
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

            /// 兼容旧签名的逐元素并行（早期原实现的三处错现以修复：
            /// ① `vector(n)` 预分配后又 emplace_back → 线程数变成 2n，
            ///    前 n 个是**空线程**，join 空线程是 UB；
            /// ② 分块起点 `t*chunk + (t==0?0:t)` 的偏移语义错了（off-by-one）；
            /// ③ op 只拿到 index，矩阵访问还得自己再算行列 —— 现在直接给
            ///    (row, col) 与索引，省掉调用方的重复除法）。
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

        // ─────────────────────────────────────────────────────────────
        // MatrixX<Scalar> —— 动态矩阵（行主序）
        // ─────────────────────────────────────────────────────────────
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

            /// 矩阵乘：形状不匹配**抛异常**（而不是静默截断）。
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

            /// Frobenius 范数
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

        // ─────────────────────────────────────────────────────────────
        // Vector<T> —— 动态向量
        // ─────────────────────────────────────────────────────────────
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