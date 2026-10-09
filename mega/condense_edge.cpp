#include "condense_edge.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

#include "json.hpp"

namespace mega {
namespace {

using json = nlohmann::json;

uint64_t CeilDiv(uint64_t value, uint64_t divisor) {
    if (divisor == 0) {
        throw std::runtime_error("division by zero");
    }
    return value / divisor + (value % divisor != 0 ? 1 : 0);
}

uint64_t Align(uint64_t value, uint64_t alignment) {
    return value == 0 ? 0 : CeilDiv(value, alignment) * alignment;
}

template <typename Type>
Type Require(const json& object, const char* name) {
    if (!object.contains(name)) {
        throw std::runtime_error(std::string("partition manifest is missing ") + name);
    }
    return object.at(name).get<Type>();
}

}  // namespace

PartitionManifest PartitionManifest::Load(const std::string& path) {
    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error("cannot open partition manifest: " + path);
    }
    json input;
    stream >> input;
    PartitionManifest manifest;
    manifest.schema_version = Require<int>(input, "schema_version");
    manifest.manifest_version = Require<std::string>(input, "manifest_version");
    manifest.graph_digest = Require<std::string>(input, "graph_digest");
    manifest.tool = Require<std::string>(input, "tool");
    manifest.parameters = Require<std::string>(input, "parameters");
    manifest.node_to_subgraph = Require<std::vector<int>>(input, "node_to_subgraph");
    return manifest;
}

PartitionManifest PartitionManifest::BuildContiguous(
        const Graph& graph, int vertices_per_subgraph,
        const std::string& graph_digest) {
    if (graph.num_vertex <= 0 || vertices_per_subgraph <= 0 || graph_digest.empty()) {
        throw std::runtime_error("invalid contiguous partition inputs");
    }
    PartitionManifest manifest;
    manifest.schema_version = 1;
    manifest.manifest_version = "diagnostic-contiguous-v1";
    manifest.graph_digest = graph_digest;
    manifest.tool = "deterministic-contiguous";
    manifest.parameters = "vertices_per_subgraph=" +
                          std::to_string(vertices_per_subgraph);
    manifest.node_to_subgraph.resize(graph.num_vertex);
    for (int vertex = 0; vertex < graph.num_vertex; ++vertex) {
        manifest.node_to_subgraph[vertex] = vertex / vertices_per_subgraph;
    }
    manifest.Validate(graph, graph_digest);
    return manifest;
}

void PartitionManifest::Validate(const Graph& graph,
                                 const std::string& expected_digest) const {
    if (schema_version != 1 || manifest_version.empty() || graph_digest.empty() ||
        tool.empty() || parameters.empty()) {
        throw std::runtime_error("invalid partition manifest identity fields");
    }
    if (graph_digest != expected_digest) {
        throw std::runtime_error("partition manifest graph digest mismatch");
    }
    if (graph.num_vertex <= 0 ||
        node_to_subgraph.size() != static_cast<std::size_t>(graph.num_vertex)) {
        throw std::runtime_error("partition manifest vertex count mismatch");
    }
    std::set<int> subgraphs;
    for (int subgraph : node_to_subgraph) {
        if (subgraph < 0) {
            throw std::runtime_error("partition IDs cannot be negative");
        }
        subgraphs.insert(subgraph);
    }
    if (subgraphs.empty() || *subgraphs.begin() != 0 ||
        *subgraphs.rbegin() != static_cast<int>(subgraphs.size()) - 1) {
        throw std::runtime_error("partition IDs must be contiguous from zero");
    }
}

int PartitionManifest::SubgraphCount() const {
    if (node_to_subgraph.empty()) {
        return 0;
    }
    return *std::max_element(node_to_subgraph.begin(), node_to_subgraph.end()) + 1;
}

bool PartitionManifest::EligibleForRequiredBenchmark() const {
    return tool == "metis";
}

std::string PartitionManifest::Digest() const {
    uint64_t digest = 1469598103934665603ULL;
    auto update = [&](const std::string& value) {
        for (unsigned char byte : value) {
            digest ^= byte;
            digest *= 1099511628211ULL;
        }
        digest ^= 0xffU;
        digest *= 1099511628211ULL;
    };
    update(std::to_string(schema_version));
    update(manifest_version);
    update(graph_digest);
    update(tool);
    update(parameters);
    for (int subgraph : node_to_subgraph) {
        update(std::to_string(subgraph));
    }
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << digest;
    return stream.str();
}

CondensePlan CondensePlan::Build(const Graph& graph,
                                 const PartitionManifest& partition) {
    if (graph.num_vertex <= 0 ||
        graph.r_adj.size() != static_cast<std::size_t>(graph.num_vertex)) {
        throw std::runtime_error("invalid graph for Condense-Edge planning");
    }
    if (partition.node_to_subgraph.size() != static_cast<std::size_t>(graph.num_vertex)) {
        throw std::runtime_error("partition size does not match graph");
    }
    CondensePlan plan;
    plan.subgraphs.resize(partition.SubgraphCount());
    std::vector<std::set<int>> unique_sources(plan.subgraphs.size());
    for (std::size_t subgraph = 0; subgraph < plan.subgraphs.size(); ++subgraph) {
        plan.subgraphs[subgraph].subgraph_id = static_cast<int>(subgraph);
    }
    for (int destination = 0; destination < graph.num_vertex; ++destination) {
        const int target = partition.node_to_subgraph[destination];
        for (int source : graph.r_adj[destination]) {
            if (source < 0 || source >= graph.num_vertex) {
                throw std::runtime_error("edge source is outside graph");
            }
            if (partition.node_to_subgraph[source] == target) {
                continue;
            }
            ++plan.cross_edges;
            ++plan.subgraphs[target].cross_edges;
            unique_sources[target].insert(source);
        }
    }
    for (std::size_t subgraph = 0; subgraph < plan.subgraphs.size(); ++subgraph) {
        plan.subgraphs[subgraph].unique_external_sources.assign(
            unique_sources[subgraph].begin(), unique_sources[subgraph].end());
        plan.unique_source_references += unique_sources[subgraph].size();
    }
    return plan;
}

uint64_t CondenseMetrics::CondensedDramBytes() const {
    return condensed_cross_read_bytes + sparse_spill_write_bytes;
}

uint64_t CondenseMetrics::CondensedDramTransactions() const {
    return condensed_cross_read_transactions + sparse_spill_write_transactions;
}

CondenseMetrics CondenseEdgeModel::Run(
        const Graph& graph,
        const PartitionManifest& partition,
        const CondensePlan& plan,
        const std::vector<uint64_t>& encoded_feature_bytes,
        const CondenseConfig& config) {
    if (config.parallel_fifos <= 0 || config.fifo_entries <= 0 ||
        config.sparse_buffer_bytes == 0 || config.transaction_bytes == 0) {
        throw std::runtime_error("invalid Condense-Edge hardware configuration");
    }
    if (encoded_feature_bytes.size() != static_cast<std::size_t>(graph.num_vertex) ||
        partition.node_to_subgraph.size() != encoded_feature_bytes.size() ||
        plan.subgraphs.size() != static_cast<std::size_t>(partition.SubgraphCount())) {
        throw std::runtime_error("Condense-Edge inputs do not share a graph shape");
    }
    for (uint64_t bytes : encoded_feature_bytes) {
        if (bytes == 0) {
            throw std::runtime_error("encoded node feature size must be positive");
        }
    }

    CondenseMetrics metrics;
    metrics.cross_edges = plan.cross_edges;
    metrics.unique_source_references = plan.unique_source_references;
    for (uint64_t bytes : encoded_feature_bytes) {
        metrics.combination_buffer_write_bytes += bytes;
    }

    const int subgraph_count = partition.SubgraphCount();
    std::vector<uint64_t> region_bytes(subgraph_count, 0);
    std::vector<uint64_t> region_offsets(subgraph_count, 0);
    std::vector<uint64_t> stream_bytes(subgraph_count, 0);

    for (int wave_start = 0; wave_start < subgraph_count;
         wave_start += config.parallel_fifos) {
        const int wave_end = std::min(subgraph_count,
                                      wave_start + config.parallel_fifos);
        const int active = wave_end - wave_start;
        const uint64_t region_capacity = std::max<uint64_t>(
            1, config.sparse_buffer_bytes / static_cast<uint64_t>(active));
        std::vector<std::deque<int>> fifos(active);
        std::vector<std::size_t> positions(active, 0);
        std::vector<uint64_t> pending_refill(active, 0);

        auto refill = [&](int local) {
            const auto& sources = plan.subgraphs[wave_start + local].unique_external_sources;
            const std::size_t begin = positions[local];
            const std::size_t end = std::min(
                sources.size(), begin + static_cast<std::size_t>(config.fifo_entries));
            for (std::size_t index = begin; index < end; ++index) {
                fifos[local].push_back(sources[index]);
            }
            positions[local] = end;
            pending_refill[local] += end - begin;
            metrics.fifo_refill_entries += end - begin;
        };
        for (int local = 0; local < active; ++local) {
            refill(local);
        }
        metrics.fifo_refill_cycles += *std::max_element(
            pending_refill.begin(), pending_refill.end());

        for (int source = 0; source < graph.num_vertex; ++source) {
            ++metrics.compare_cycles;
            for (int local = 0; local < active; ++local) {
                if (fifos[local].empty() &&
                    positions[local] < plan.subgraphs[wave_start + local]
                                           .unique_external_sources.size()) {
                    const auto before = metrics.fifo_refill_entries;
                    refill(local);
                    metrics.fifo_refill_cycles +=
                        metrics.fifo_refill_entries - before;
                }
                if (fifos[local].empty() || fifos[local].front() != source) {
                    continue;
                }
                fifos[local].pop_front();
                const int target = wave_start + local;
                const uint64_t bytes = encoded_feature_bytes[source];
                ++metrics.matched_sources;
                metrics.sparse_buffer_write_bytes += bytes;
                metrics.baseline_cross_read_bytes += Align(
                    bytes, config.transaction_bytes);
                metrics.baseline_cross_read_transactions += CeilDiv(
                    bytes, config.transaction_bytes);
                metrics.matches.push_back({source, target, region_offsets[target], bytes});
                region_offsets[target] += bytes;
                stream_bytes[target] += bytes;

                if (region_bytes[target] != 0 &&
                    region_bytes[target] + bytes > region_capacity) {
                    const uint64_t spill = Align(region_bytes[target],
                                                 config.transaction_bytes);
                    metrics.sparse_spill_write_bytes += spill;
                    metrics.sparse_spill_write_transactions +=
                        spill / config.transaction_bytes;
                    region_bytes[target] = 0;
                }
                if (bytes > region_capacity) {
                    const uint64_t spill = Align(bytes, config.transaction_bytes);
                    metrics.sparse_spill_write_bytes += spill;
                    metrics.sparse_spill_write_transactions +=
                        spill / config.transaction_bytes;
                } else {
                    region_bytes[target] += bytes;
                }
            }
        }
        for (int local = 0; local < active; ++local) {
            if (!fifos[local].empty() ||
                positions[local] != plan.subgraphs[wave_start + local]
                                       .unique_external_sources.size()) {
                throw std::runtime_error("Condense-Edge did not consume every eID");
            }
        }
    }

    for (uint64_t bytes : stream_bytes) {
        if (bytes == 0) {
            continue;
        }
        const uint64_t transferred = Align(bytes, config.transaction_bytes);
        metrics.condensed_cross_read_bytes += transferred;
        metrics.condensed_cross_read_transactions +=
            transferred / config.transaction_bytes;
    }
    metrics.condense_cycles = metrics.compare_cycles + metrics.fifo_refill_cycles;
    if (metrics.matched_sources != plan.unique_source_references ||
        metrics.matches.size() != plan.unique_source_references) {
        throw std::runtime_error("Condense-Edge source conservation failed");
    }
    return metrics;
}

}  // namespace mega
