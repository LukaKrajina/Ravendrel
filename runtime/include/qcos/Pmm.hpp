#pragma once

#include "Arch.hpp"

namespace qcos
{
    class BitmapPmm
    {
    private:
        u64*  bitmap_       = nullptr;
        u64   base_         = 0;
        u64   total_pages_  = 0;
        u64   free_pages_   = 0;
        u64   bitmap_words_ = 0;

        void mark_used(u64 page) { bitmap_[page / 64] |= (1ull << (page % 64)); }
        void mark_free(u64 page) { bitmap_[page / 64] &= ~(1ull << (page % 64)); }
        bool is_used(u64 page) const { return ((bitmap_[page / 64] >> (page % 64)) & 1ull) != 0; }

    public:
        void init(u64 base, u64 size_bytes)
        {
            base_        = base;
            total_pages_ = size_bytes / PAGE_SIZE;
            bitmap_words_ = (total_pages_ + 63) / 64;
            bitmap_      = reinterpret_cast<u64*>(base_);

            for (u64 i = 0; i < bitmap_words_; ++i) bitmap_[i] = ~0ull;
            free_pages_ = 0;

            const u64 used_pages = (bitmap_words_ * 8 + PAGE_SIZE - 1) / PAGE_SIZE;
            for (u64 p = used_pages; p < total_pages_; ++p)
            {
                mark_free(p);
                ++free_pages_;
            }
        }

        u64 alloc(u64 count = 1)
        {
            if (count == 0 || count > free_pages_) return 0;

            u64 run = 0;
            for (u64 p = 0; p < total_pages_; ++p)
            {
                if (is_used(p)) { run = 0; continue; }
                if (++run == count)
                {
                    const u64 start = p - count + 1;
                    for (u64 q = start; q <= p; ++q) mark_used(q);
                    free_pages_ -= count;
                    return base_ + start * PAGE_SIZE;
                }
            }
            return 0;
        }

        void free(u64 addr, u64 count = 1)
        {
            if (addr < base_) return;
            const u64 start = (addr - base_) / PAGE_SIZE;
            for (u64 q = 0; q < count && start + q < total_pages_; ++q)
            {
                if (is_used(start + q)) { mark_free(start + q); ++free_pages_; }
            }
        }

        u64 free_page_count()  const { return free_pages_;  }
        u64 total_page_count() const { return total_pages_; }
        u64 base()             const { return base_;        }
    };
}