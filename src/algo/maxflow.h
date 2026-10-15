#pragma once

#include <deque>
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace algo
{

    class MaxflowGraph
    {
    public:
        MaxflowGraph (size_t nodes, size_t edges);

        void add_tweights (size_t i, float cap_source, float cap_sink);
        void add_edge (size_t i, size_t j, float cap, float rev_cap);
        double maxflow ();
        bool in_source (size_t i) const;

    private:
        void augment (size_t m, std::deque<uint32_t>* orphans);
        void adopt (size_t i, uint32_t time, std::deque<uint32_t>* queue, std::deque<uint32_t>* orphans);

        std::vector<uint32_t> first_;
        std::vector<float> tr_cap_;
        std::vector<uint32_t> parent_;
        std::vector<uint8_t> is_sink_;
        std::vector<uint32_t> ts_;
        std::vector<uint32_t> dist_;
        std::vector<uint8_t> active_;
        std::vector<uint32_t> head_;
        std::vector<uint32_t> next_;
        std::vector<float> r_cap_;
        double flow_;
    };

} // namespace algo
