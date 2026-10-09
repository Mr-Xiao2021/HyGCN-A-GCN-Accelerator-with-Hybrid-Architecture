#ifndef MEGA_CONDENSE_EDGE_H
#define MEGA_CONDENSE_EDGE_H

#include <cstdint>
#include <string>
#include <vector>

#include "graph.h"

namespace mega {

struct PartitionManifest {
    int schema_version = 0;
    std::string manifest_version;
    std::string graph_digest;
    std::string tool;
    std::string parameters;
    std::vector<int> node_to_subgraph;

    static PartitionManifest Load(const std::string& path);
    static PartitionManifest BuildContiguous(const Graph& graph,
                                             int vertices_per_subgraph,
                                             const std::string& graph_digest);
    void Validate(const Graph& graph, const std::string& expected_digest) const;
    int SubgraphCount() const;
    bool EligibleForRequiredBenchmark() const;
    std::string Digest() const;
};

struct CondenseSubgraphPlan {
    int subgraph_id = 0;
    uint64_t cross_edges = 0;
    std::vector<int> unique_external_sources;
};

struct CondensePlan {
    std::vector<CondenseSubgraphPlan> subgraphs;
    uint64_t cross_edges = 0;
    uint64_t unique_source_references = 0;

    static CondensePlan Build(const Graph& graph,
                              const PartitionManifest& partition);
};

struct CondenseConfig {
    int parallel_fifos = 16;
    int fifo_entries = 8;
    uint64_t sparse_buffer_bytes = 32 * 1024;
    uint64_t transaction_bytes = 128;
};

struct CondenseMatchTrace {
    int source_node = 0;
    int target_subgraph = 0;
    uint64_t sparse_address = 0;
    uint64_t feature_bytes = 0;
};

struct CondenseMetrics {
    uint64_t cross_edges = 0;
    uint64_t unique_source_references = 0;
    uint64_t matched_sources = 0;
    uint64_t fifo_refill_entries = 0;
    uint64_t fifo_refill_cycles = 0;
    uint64_t compare_cycles = 0;
    uint64_t condense_cycles = 0;
    uint64_t combination_buffer_write_bytes = 0;
    uint64_t sparse_buffer_write_bytes = 0;
    uint64_t sparse_spill_write_bytes = 0;
    uint64_t baseline_cross_read_bytes = 0;
    uint64_t condensed_cross_read_bytes = 0;
    uint64_t baseline_cross_read_transactions = 0;
    uint64_t condensed_cross_read_transactions = 0;
    uint64_t sparse_spill_write_transactions = 0;
    std::vector<CondenseMatchTrace> matches;

    uint64_t CondensedDramBytes() const;
    uint64_t CondensedDramTransactions() const;
};

class CondenseEdgeModel {
public:
    static CondenseMetrics Run(const Graph& graph,
                               const PartitionManifest& partition,
                               const CondensePlan& plan,
                               const std::vector<uint64_t>& encoded_feature_bytes,
                               const CondenseConfig& config);
};

}  // namespace mega

#endif
