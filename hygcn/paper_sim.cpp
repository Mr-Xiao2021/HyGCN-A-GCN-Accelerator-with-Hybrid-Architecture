#include "paper_sim.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>

#include "INIReader.h"
#include "json.hpp"
#include "tool.h"

#ifndef HYGCN_GIT_COMMIT
#define HYGCN_GIT_COMMIT "unknown"
#endif

namespace {

using json = nlohmann::json;

constexpr uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
constexpr int kDataBytes = 4;
constexpr int kIndexBytes = 4;

uint64_t CeilDiv(uint64_t value, uint64_t divisor) {
    if (divisor == 0) {
        throw std::runtime_error("division by zero");
    }
    return (value + divisor - 1) / divisor;
}

uint64_t Align(uint64_t value, uint64_t alignment) {
    return CeilDiv(value, alignment) * alignment;
}

double Clamp(double value, double low, double high) {
    return std::max(low, std::min(value, high));
}

uint64_t ServiceCycles(uint64_t bytes, double bytes_per_cycle, double efficiency) {
    if (bytes == 0) {
        return 0;
    }
    const double effective = bytes_per_cycle * efficiency;
    if (effective <= 0.0) {
        throw std::runtime_error("invalid effective HBM bandwidth");
    }
    return static_cast<uint64_t>(std::ceil(bytes / effective));
}

std::string Hex64(uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

uint64_t Fnv1aUpdate(uint64_t hash, const char* data, std::size_t size) {
    constexpr uint64_t kPrime = 1099511628211ULL;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= kPrime;
    }
    return hash;
}

double Imbalance(const std::vector<uint64_t>& counts) {
    if (counts.empty()) {
        return 0.0;
    }
    const double mean = std::accumulate(counts.begin(), counts.end(), 0.0) /
                        static_cast<double>(counts.size());
    if (mean == 0.0) {
        return 0.0;
    }
    const auto minmax = std::minmax_element(counts.begin(), counts.end());
    return static_cast<double>(*minmax.second - *minmax.first) / mean;
}

std::string RequestClassName(RequestClass request_class) {
    switch (request_class) {
        case RequestClass::EDGE:
            return "edge";
        case RequestClass::INPUT:
            return "input";
        case RequestClass::WEIGHT:
            return "weight";
        case RequestClass::OUTPUT:
            return "output";
    }
    throw std::runtime_error("unknown request class");
}

json ArchitectureJson(const ArchitectureConfig& architecture) {
    return {
        {"profile", architecture.profile},
        {"frequency_ghz", architecture.frequency_ghz},
        {"block_size", architecture.block_size},
        {"num_simd", architecture.num_simd},
        {"simd_width", architecture.simd_width},
        {"combination_modules", architecture.combination_modules},
        {"arrays_per_module", architecture.arrays_per_module},
        {"array_width", architecture.array_width},
        {"input_buffer_bytes", architecture.input_buffer_bytes},
        {"edge_buffer_bytes", architecture.edge_buffer_bytes},
        {"weight_buffer_bytes", architecture.weight_buffer_bytes},
        {"output_buffer_bytes", architecture.output_buffer_bytes},
        {"aggregation_buffer_bytes", architecture.aggregation_buffer_bytes},
        {"hbm_capacity_bytes", architecture.hbm_capacity_bytes},
        {"hbm_bandwidth_gbps", architecture.hbm_bandwidth_gbps},
        {"coordinated_efficiency", architecture.coordinated_efficiency},
        {"uncoordinated_efficiency", architecture.uncoordinated_efficiency},
        {"edram_latency_cycles", architecture.edram_latency_cycles},
        {"edram_transactions_per_cycle", architecture.edram_transactions_per_cycle},
        {"simd_efficiency", architecture.simd_efficiency},
        {"independent_array_efficiency", architecture.independent_array_efficiency},
        {"cooperative_array_efficiency", architecture.cooperative_array_efficiency},
        {"memory_hidden_fraction", architecture.memory_hidden_fraction},
        {"latency_overlap_fraction", architecture.latency_overlap_fraction},
        {"energy_overlap_fraction", architecture.energy_overlap_fraction},
        {"energy_batch_vertices", architecture.energy_batch_vertices},
        {"partition_vertices", architecture.partition_vertices},
        {"sequential_spill_factor", architecture.sequential_spill_factor},
    };
}

json LayerJson(const LayerMetrics& layer) {
    json request_stats;
    for (int index = 0; index < 4; ++index) {
        const auto request_class = static_cast<RequestClass>(index);
        request_stats[RequestClassName(request_class)] = {
            {"count", layer.request_counts[index]},
            {"bytes", layer.request_bytes[index]},
            {"queue_wait_cycles", layer.request_wait_cycles[index]},
        };
    }
    return {
        {"layer", layer.layer},
        {"input_features", layer.input_features},
        {"output_features", layer.output_features},
        {"aggregation_operation", ToString(layer.aggregation_op)},
        {"cycles", layer.cycles},
        {"aggregation_cycles", layer.aggregation_cycles},
        {"aggregation_compute_cycles", layer.aggregation_compute_cycles},
        {"aggregation_memory_cycles", layer.aggregation_memory_cycles},
        {"combination_cycles", layer.combination_cycles},
        {"combination_weight_load_cycles", layer.combination_weight_load_cycles},
        {"combination_input_cycles", layer.combination_input_cycles},
        {"combination_pipeline_fill_cycles", layer.combination_pipeline_fill_cycles},
        {"combination_compute_cycles", layer.combination_compute_cycles},
        {"combination_output_cycles", layer.combination_output_cycles},
        {"combination_active_modules", layer.combination_active_modules},
        {"combination_batch_waves", layer.combination_batch_waves},
        {"combination_output_columns_per_module",
         layer.combination_output_columns_per_module},
        {"weight_cascade_bytes", layer.weight_cascade_bytes},
        {"memory_service_cycles", layer.memory_service_cycles},
        {"queue_wait_cycles", layer.queue_wait_cycles},
        {"hbm_blocked_cycles", layer.hbm_blocked_cycles},
        {"ae_finish_cycle", layer.ae_finish_cycle},
        {"ce_start_cycle", layer.ce_start_cycle},
        {"ce_finish_cycle", layer.ce_finish_cycle},
        {"edge_dram_bytes", layer.edge_dram_bytes},
        {"input_dram_bytes", layer.input_dram_bytes},
        {"weight_dram_bytes", layer.weight_dram_bytes},
        {"output_dram_bytes", layer.output_dram_bytes},
        {"intermediate_dram_bytes", layer.intermediate_dram_bytes},
        {"total_dram_bytes", layer.TotalDramBytes()},
        {"aggregation_buffer_read_bytes", layer.aggregation_buffer_read_bytes},
        {"aggregation_buffer_write_bytes", layer.aggregation_buffer_write_bytes},
        {"mac_operations", layer.mac_operations},
        {"add_operations", layer.add_operations},
        {"compare_operations", layer.compare_operations},
        {"skipped_input_vertices", layer.skipped_input_vertices},
        {"requested_input_vertices", layer.requested_input_vertices},
        {"shards", layer.shards},
        {"batches", layer.batches},
        {"aggregation_buffer_peak_bytes", layer.aggregation_buffer_peak_bytes},
        {"aggregation_buffer_capacity_stalls", layer.aggregation_buffer_capacity_stalls},
        {"simd_idle_lane_cycles", layer.simd_idle_lane_cycles},
        {"simd_feature_chunks", layer.simd_feature_chunks},
        {"simd_cores_per_vertex", layer.simd_cores_per_vertex},
        {"simd_parallel_vertices", layer.simd_parallel_vertices},
        {"array_idle_lane_cycles", layer.array_idle_lane_cycles},
        {"request_stats", request_stats},
        {"channel_blocks", layer.channel_blocks},
        {"bank_blocks", layer.bank_blocks},
        {"simd_utilization", layer.simd_utilization},
        {"array_utilization", layer.array_utilization},
        {"bandwidth_utilization", layer.bandwidth_utilization},
        {"channel_imbalance", layer.channel_imbalance},
        {"bank_imbalance", layer.bank_imbalance},
    };
}

}  // namespace

ArchitectureConfig ArchitectureConfig::Load(const std::string& path) {
    INIReader reader(path);
    if (reader.ParseError() != 0) {
        throw std::runtime_error("cannot parse architecture config: " + path);
    }

    ArchitectureConfig config;
    config.profile = reader.Get("architecture", "profile", "");
    config.frequency_ghz = reader.GetReal("architecture", "frequency_ghz", -1.0);
    config.block_size = reader.GetInteger("architecture", "block_size", -1);
    config.num_simd = reader.GetInteger("architecture", "num_simd", -1);
    config.simd_width = reader.GetInteger("architecture", "simd_width", -1);
    config.combination_modules = reader.GetInteger("architecture", "combination_modules", -1);
    config.arrays_per_module = reader.GetInteger("architecture", "arrays_per_module", -1);
    config.array_width = reader.GetInteger("architecture", "array_width", -1);

    config.input_buffer_bytes = reader.GetInteger("buffer", "input_bytes", -1);
    config.edge_buffer_bytes = reader.GetInteger("buffer", "edge_bytes", -1);
    config.weight_buffer_bytes = reader.GetInteger("buffer", "weight_bytes", -1);
    config.output_buffer_bytes = reader.GetInteger("buffer", "output_bytes", -1);
    config.aggregation_buffer_bytes = reader.GetInteger("buffer", "aggregation_bytes", -1);

    const auto hbm_capacity_gb = reader.GetInteger("memory", "hbm_capacity_gb", -1);
    if (hbm_capacity_gb <= 0) {
        throw std::runtime_error("invalid architecture parameter: hbm_capacity_gb");
    }
    config.hbm_capacity_bytes = static_cast<uint64_t>(hbm_capacity_gb) * kGiB;
    config.hbm_bandwidth_gbps = reader.GetReal("memory", "hbm_bandwidth_gbps", -1.0);
    config.coordinated_efficiency = reader.GetReal("memory", "coordinated_efficiency", -1.0);
    config.uncoordinated_efficiency = reader.GetReal("memory", "uncoordinated_efficiency", -1.0);
    config.edram_latency_cycles = reader.GetInteger("memory", "edram_latency_cycles", -1);
    config.edram_transactions_per_cycle = reader.GetInteger("memory", "edram_transactions_per_cycle", -1);

    config.simd_efficiency = reader.GetReal("model", "simd_efficiency", -1.0);
    config.independent_array_efficiency = reader.GetReal("model", "independent_array_efficiency", -1.0);
    config.cooperative_array_efficiency = reader.GetReal("model", "cooperative_array_efficiency", -1.0);
    config.memory_hidden_fraction = reader.GetReal("model", "memory_hidden_fraction", -1.0);
    config.latency_overlap_fraction = reader.GetReal("model", "latency_overlap_fraction", -1.0);
    config.energy_overlap_fraction = reader.GetReal("model", "energy_overlap_fraction", -1.0);
    config.energy_batch_vertices = reader.GetInteger("model", "energy_batch_vertices", -1);
    config.partition_vertices = reader.GetInteger("model", "partition_vertices", -1);
    config.sequential_spill_factor = reader.GetReal("model", "sequential_spill_factor", -1.0);
    config.Validate();
    return config;
}

void ArchitectureConfig::Validate() const {
    auto require_positive = [](bool condition, const std::string& name) {
        if (!condition) {
            throw std::runtime_error("invalid architecture parameter: " + name);
        }
    };
    require_positive(!profile.empty(), "profile");
    require_positive(frequency_ghz > 0.0, "frequency_ghz");
    require_positive(block_size > 0, "block_size");
    require_positive(num_simd > 0, "num_simd");
    require_positive(simd_width > 0, "simd_width");
    require_positive(combination_modules > 0, "combination_modules");
    require_positive(arrays_per_module > 0, "arrays_per_module");
    require_positive(array_width > 0, "array_width");
    require_positive(input_buffer_bytes > 0, "input_buffer_bytes");
    require_positive(edge_buffer_bytes > 0, "edge_buffer_bytes");
    require_positive(weight_buffer_bytes > 0, "weight_buffer_bytes");
    require_positive(output_buffer_bytes > 0, "output_buffer_bytes");
    require_positive(aggregation_buffer_bytes > 0, "aggregation_buffer_bytes");
    require_positive(hbm_capacity_bytes > 0, "hbm_capacity_bytes");
    require_positive(hbm_bandwidth_gbps > 0.0, "hbm_bandwidth_gbps");
    require_positive(coordinated_efficiency > 0.0 && coordinated_efficiency <= 1.0,
                     "coordinated_efficiency");
    require_positive(uncoordinated_efficiency > 0.0 && uncoordinated_efficiency <= 1.0,
                     "uncoordinated_efficiency");
    require_positive(edram_latency_cycles >= 0, "edram_latency_cycles");
    require_positive(edram_transactions_per_cycle > 0, "edram_transactions_per_cycle");
    require_positive(simd_efficiency > 0.0 && simd_efficiency <= 1.0, "simd_efficiency");
    require_positive(independent_array_efficiency > 0.0 && independent_array_efficiency <= 1.0,
                     "independent_array_efficiency");
    require_positive(cooperative_array_efficiency > 0.0 && cooperative_array_efficiency <= 1.0,
                     "cooperative_array_efficiency");
    require_positive(memory_hidden_fraction >= 0.0 && memory_hidden_fraction < 1.0,
                     "memory_hidden_fraction");
    require_positive(latency_overlap_fraction >= 0.0 && latency_overlap_fraction < 1.0,
                     "latency_overlap_fraction");
    require_positive(energy_overlap_fraction >= 0.0 && energy_overlap_fraction < 1.0,
                     "energy_overlap_fraction");
    require_positive(energy_batch_vertices > 0, "energy_batch_vertices");
    require_positive(partition_vertices > 0, "partition_vertices");
    require_positive(sequential_spill_factor >= 0.0, "sequential_spill_factor");
}

double ArchitectureConfig::HbmBytesPerCycle() const {
    return hbm_bandwidth_gbps / frequency_ghz;
}

AggregationBufferModel::AggregationBufferModel(uint64_t capacity_bytes)
    : capacity_bytes_(capacity_bytes) {
    if (capacity_bytes_ == 0) {
        throw std::runtime_error("aggregation buffer capacity must be positive");
    }
}

uint64_t AggregationBufferModel::FindAllocationOffset(uint64_t bytes) const {
    if (bytes == 0 || bytes > capacity_bytes_ || used_bytes_ + bytes > capacity_bytes_) {
        throw std::runtime_error("aggregation buffer capacity exceeded");
    }
    if (segments_.empty()) {
        return allocation_head_ + bytes <= capacity_bytes_ ? allocation_head_ : 0;
    }

    const uint64_t reclaim_tail = segments_.front().offset;
    if (allocation_head_ >= reclaim_tail) {
        if (allocation_head_ + bytes <= capacity_bytes_) {
            return allocation_head_;
        }
        if (bytes <= reclaim_tail) {
            return 0;
        }
    } else if (allocation_head_ + bytes <= reclaim_tail) {
        return allocation_head_;
    }
    throw std::runtime_error("aggregation buffer has no contiguous allocation");
}

bool AggregationBufferModel::CanAllocate(uint64_t bytes) const {
    try {
        FindAllocationOffset(bytes);
        return true;
    } catch (const std::runtime_error&) {
        return false;
    }
}

uint64_t AggregationBufferModel::Allocate(int batch_id, uint64_t bytes) {
    if (batch_id < 0) {
        throw std::runtime_error("aggregation buffer batch id must be non-negative");
    }
    for (const auto& segment : segments_) {
        if (segment.batch_id == batch_id) {
            throw std::runtime_error("aggregation buffer batch already exists");
        }
    }
    const uint64_t offset = FindAllocationOffset(bytes);
    segments_.push_back({batch_id, offset, bytes, BufferSegmentState::ALLOCATED});
    allocation_head_ = (offset + bytes) % capacity_bytes_;
    used_bytes_ += bytes;
    peak_bytes_ = std::max(peak_bytes_, used_bytes_);
    return offset;
}

BufferSegment& AggregationBufferModel::Find(int batch_id) {
    const auto iterator = std::find_if(segments_.begin(), segments_.end(),
        [batch_id](const auto& segment) { return segment.batch_id == batch_id; });
    if (iterator == segments_.end()) {
        throw std::runtime_error("aggregation buffer batch not found");
    }
    return *iterator;
}

void AggregationBufferModel::MarkReady(int batch_id) {
    auto& segment = Find(batch_id);
    if (segment.state != BufferSegmentState::ALLOCATED) {
        throw std::runtime_error("aggregation buffer batch is not allocated");
    }
    segment.state = BufferSegmentState::READY;
}

void AggregationBufferModel::StartConsume(int batch_id) {
    auto& segment = Find(batch_id);
    if (segment.state != BufferSegmentState::READY) {
        throw std::runtime_error("aggregation buffer batch is not ready");
    }
    segment.state = BufferSegmentState::CONSUMING;
}

void AggregationBufferModel::Reclaim(int batch_id) {
    if (segments_.empty() || segments_.front().batch_id != batch_id) {
        throw std::runtime_error("aggregation buffer reclaim must follow allocation order");
    }
    if (segments_.front().state != BufferSegmentState::CONSUMING) {
        throw std::runtime_error("aggregation buffer batch is not consumed");
    }
    used_bytes_ -= segments_.front().bytes;
    segments_.pop_front();
}

uint64_t AggregationBufferModel::CapacityBytes() const {
    return capacity_bytes_;
}

uint64_t AggregationBufferModel::UsedBytes() const {
    return used_bytes_;
}

uint64_t AggregationBufferModel::PeakBytes() const {
    return peak_bytes_;
}

const std::deque<BufferSegment>& AggregationBufferModel::Segments() const {
    return segments_;
}

std::vector<MemoryRequest> MemoryCoordinatorModel::Order(std::vector<MemoryRequest> requests,
                                                         bool coordinated) {
    for (const auto& request : requests) {
        if (request.batch_id < 0 || request.bytes == 0 ||
            request.address > std::numeric_limits<uint64_t>::max() - request.bytes) {
            throw std::runtime_error("invalid memory request");
        }
    }
    std::stable_sort(requests.begin(), requests.end(), [coordinated](const auto& lhs, const auto& rhs) {
        if (coordinated) {
            if (lhs.batch_id != rhs.batch_id) {
                return lhs.batch_id < rhs.batch_id;
            }
            if (lhs.request_class != rhs.request_class) {
                return static_cast<int>(lhs.request_class) < static_cast<int>(rhs.request_class);
            }
        } else if (lhs.request_class != rhs.request_class) {
            return static_cast<int>(lhs.request_class) < static_cast<int>(rhs.request_class);
        }
        return lhs.sequence < rhs.sequence;
    });
    return requests;
}

uint64_t HbmRegion::End() const {
    if (start > std::numeric_limits<uint64_t>::max() - bytes) {
        throw std::runtime_error("HBM region address overflow");
    }
    return start + bytes;
}

uint64_t LayerMetrics::TotalDramBytes() const {
    return edge_dram_bytes + input_dram_bytes + weight_dram_bytes + output_dram_bytes +
           intermediate_dram_bytes;
}

uint64_t ExperimentResult::TotalCycles() const {
    uint64_t total = 0;
    for (const auto& layer : layers) {
        total += layer.cycles;
    }
    return total;
}

uint64_t ExperimentResult::TotalDramBytes() const {
    uint64_t total = 0;
    for (const auto& layer : layers) {
        total += layer.TotalDramBytes();
    }
    return total;
}

uint64_t ExperimentResult::TotalInputDramBytes() const {
    uint64_t total = 0;
    for (const auto& layer : layers) {
        total += layer.input_dram_bytes;
    }
    return total;
}

double ExperimentResult::BandwidthUtilization() const {
    const auto cycles = TotalCycles();
    if (cycles == 0) {
        return 0.0;
    }
    return Clamp(TotalDramBytes() / (cycles * architecture.HbmBytesPerCycle()), 0.0, 1.0);
}

PaperSimulator::PaperSimulator(ArchitectureConfig architecture)
    : architecture_(std::move(architecture)) {
    architecture_.Validate();
}

std::vector<LayerShape> PaperSimulator::GetLayerShapes(const Graph& graph,
                                                       const std::string& model) const {
    if (model != "gcn" && model != "gin" && model != "gs") {
        throw std::runtime_error("unsupported model: " + model);
    }
    constexpr int kHidden = 128;
    return {{graph.len_feature, kHidden}, {kHidden, graph.num_class}};
}

std::vector<EdgeShard> PaperSimulator::BuildShards(const Graph& graph,
                                                   int feature_count,
                                                   bool sparsity_elimination) const {
    if (graph.num_vertex <= 0 || static_cast<int>(graph.r_adj.size()) != graph.num_vertex) {
        throw std::runtime_error("invalid graph for partitioning");
    }
    const uint64_t feature_stride = Align(static_cast<uint64_t>(feature_count) * kDataBytes,
                                          architecture_.block_size);
    const int interval_capacity = std::max<int>(1, architecture_.input_buffer_bytes / feature_stride);
    const int max_dst_vertices = std::max<int>(1, std::min<uint64_t>(
        architecture_.aggregation_buffer_bytes / feature_stride,
        architecture_.partition_vertices));

    std::vector<EdgeShard> shards;
    int batch_id = 0;
    int dst = 0;
    while (dst < graph.num_vertex) {
        const int chunk_start = dst;
        uint64_t chunk_bytes = 0;
        int chunk_vertices = 0;
        while (dst < graph.num_vertex) {
            const uint64_t row_bytes = kIndexBytes +
                static_cast<uint64_t>(graph.r_adj[dst].size()) * (kIndexBytes + kDataBytes);
            if (chunk_vertices > 0 &&
                (chunk_bytes + row_bytes > architecture_.edge_buffer_bytes ||
                 chunk_vertices >= max_dst_vertices)) {
                break;
            }
            if (row_bytes > architecture_.edge_buffer_bytes) {
                throw std::runtime_error("single adjacency row exceeds edge buffer");
            }
            chunk_bytes += row_bytes;
            ++chunk_vertices;
            ++dst;
        }
        const int chunk_end = dst;

        std::set<int> unique_sources;
        for (int vertex = chunk_start; vertex < chunk_end; ++vertex) {
            for (int source : graph.r_adj[vertex]) {
                if (source < 0 || source >= graph.num_vertex) {
                    throw std::runtime_error("edge source is outside graph");
                }
                unique_sources.insert(source);
            }
        }

        std::vector<int> sources(unique_sources.begin(), unique_sources.end());
        if (sources.empty()) {
            EdgeShard shard;
            shard.shard_id = static_cast<int>(shards.size());
            shard.batch_id = batch_id++;
            shard.dst_start = chunk_start;
            shard.dst_end = chunk_end;
            shard.edge_bytes = Align(chunk_bytes, architecture_.block_size);
            shards.push_back(std::move(shard));
            continue;
        }

        bool first_shard = true;
        if (!sparsity_elimination) {
            for (int interval_start = 0; interval_start < graph.num_vertex;
                 interval_start += interval_capacity) {
                const int interval_end = std::min(graph.num_vertex,
                                                  interval_start + interval_capacity);
                const auto begin = std::lower_bound(sources.begin(), sources.end(), interval_start);
                const auto end = std::lower_bound(sources.begin(), sources.end(), interval_end);
                EdgeShard shard;
                shard.shard_id = static_cast<int>(shards.size());
                shard.batch_id = batch_id;
                shard.dst_start = chunk_start;
                shard.dst_end = chunk_end;
                shard.interval_start = interval_start;
                shard.interval_end = interval_end;
                shard.unique_neighbors.assign(begin, end);
                shard.shrunk_interval_end = shard.unique_neighbors.empty()
                    ? interval_start
                    : shard.unique_neighbors.back() + 1;
                shard.edge_bytes = first_shard ? Align(chunk_bytes, architecture_.block_size) : 0;
                shard.input_bytes = static_cast<uint64_t>(interval_end - interval_start) *
                                    feature_stride;
                shards.push_back(std::move(shard));
                first_shard = false;
            }
            ++batch_id;
            continue;
        }

        std::size_t position = 0;
        while (position < sources.size()) {
            const int interval_start = sources[position];
            const int interval_end = std::min(graph.num_vertex, interval_start + interval_capacity);
            std::size_t end = position;
            while (end < sources.size() && sources[end] < interval_end) {
                ++end;
            }
            if (end == position) {
                throw std::runtime_error("partitioning made no progress");
            }

            EdgeShard shard;
            shard.shard_id = static_cast<int>(shards.size());
            shard.batch_id = batch_id;
            shard.dst_start = chunk_start;
            shard.dst_end = chunk_end;
            shard.interval_start = interval_start;
            shard.interval_end = interval_end;
            shard.unique_neighbors.assign(sources.begin() + position, sources.begin() + end);
            shard.shrunk_interval_end = shard.unique_neighbors.back() + 1;
            shard.edge_bytes = first_shard ? Align(chunk_bytes, architecture_.block_size) : 0;

            shard.input_bytes = shard.unique_neighbors.size() * feature_stride;
            shards.push_back(std::move(shard));
            position = end;
            first_shard = false;
        }
        ++batch_id;
    }
    return shards;
}

uint64_t PaperSimulator::OutputAddress(uint64_t output_start,
                                       int vertex,
                                       int output_chunk,
                                       uint64_t vertex_stride,
                                       uint64_t chunk_stride) {
    if (vertex < 0 || output_chunk < 0 || vertex_stride == 0 || chunk_stride == 0) {
        throw std::runtime_error("invalid output address arguments");
    }
    return output_start + static_cast<uint64_t>(vertex) * vertex_stride +
           static_cast<uint64_t>(output_chunk) * chunk_stride;
}

uint64_t PaperSimulator::EstimateSystolicCycles(int rows,
                                                int inner,
                                                int columns,
                                                const ArchitectureConfig& architecture,
                                                CombinationMode mode) {
    return BuildCombinationSchedule(rows, inner, columns,
                                    architecture.combination_modules,
                                    architecture, mode).total_cycles;
}

CombinationSchedule PaperSimulator::BuildCombinationSchedule(
        int rows,
        int inner,
        int columns,
        uint64_t batches,
        const ArchitectureConfig& architecture,
        CombinationMode mode) {
    if (rows <= 0 || inner <= 0 || columns <= 0 || batches == 0) {
        throw std::runtime_error("combination schedule dimensions and batches must be positive");
    }
    architecture.Validate();
    CombinationSchedule schedule;
    schedule.row_tiles = CeilDiv(rows, architecture.combination_modules);
    schedule.inner_tiles = CeilDiv(inner, architecture.arrays_per_module);
    schedule.column_tiles = CeilDiv(columns, architecture.array_width);
    schedule.active_modules = mode == CombinationMode::INDEPENDENT
        ? std::min<uint64_t>(architecture.combination_modules, batches)
        : architecture.combination_modules;
    schedule.batch_waves = mode == CombinationMode::INDEPENDENT
        ? CeilDiv(batches, architecture.combination_modules)
        : batches;
    schedule.output_columns_per_module = mode == CombinationMode::INDEPENDENT
        ? columns
        : CeilDiv(columns, schedule.active_modules);

    const uint64_t raw_weight_bytes = static_cast<uint64_t>(inner) * columns * kDataBytes;
    const uint64_t aligned_weight_bytes = Align(raw_weight_bytes, architecture.block_size);
    if (mode == CombinationMode::INDEPENDENT) {
        schedule.weight_load_bytes = aligned_weight_bytes * schedule.active_modules;
    } else {
        schedule.weight_load_bytes = aligned_weight_bytes;
        schedule.weight_cascade_bytes = aligned_weight_bytes *
            (schedule.active_modules > 0 ? schedule.active_modules - 1 : 0);
    }

    const uint64_t pe_count = static_cast<uint64_t>(architecture.combination_modules) *
                              architecture.arrays_per_module * architecture.array_width;
    const double efficiency = mode == CombinationMode::INDEPENDENT
        ? architecture.independent_array_efficiency
        : architecture.cooperative_array_efficiency;
    schedule.mac_operations = static_cast<uint64_t>(rows) * inner * columns;
    schedule.input_advance_cycles = std::max<uint64_t>(
        1, CeilDiv(static_cast<uint64_t>(rows) * inner, pe_count));
    schedule.pipeline_fill_cycles = CeilDiv(inner, architecture.arrays_per_module) +
                                    CeilDiv(columns, architecture.array_width) +
                                    CeilDiv(rows, architecture.combination_modules);
    schedule.compute_cycles = std::max<uint64_t>(1, static_cast<uint64_t>(
        std::ceil(schedule.mac_operations / (pe_count * efficiency))));
    schedule.output_write_cycles = std::max<uint64_t>(
        1, CeilDiv(static_cast<uint64_t>(rows) * columns,
                   static_cast<uint64_t>(architecture.combination_modules) *
                       architecture.array_width));
    schedule.total_cycles = schedule.input_advance_cycles + schedule.pipeline_fill_cycles +
                            schedule.compute_cycles + schedule.output_write_cycles;
    return schedule;
}

HbmLayout PaperSimulator::BuildHbmLayout(uint64_t input_bytes,
                                         uint64_t output_bytes,
                                         uint64_t weight_bytes,
                                         uint64_t edge_bytes,
                                         uint64_t intermediate_bytes,
                                         uint64_t capacity_bytes) {
    if (capacity_bytes == 0) {
        throw std::runtime_error("HBM capacity must be positive");
    }
    HbmLayout layout;
    uint64_t cursor = 0;
    auto allocate = [&](HbmRegion& region, uint64_t bytes) {
        region = {cursor, bytes};
        cursor = region.End();
        if (cursor > capacity_bytes) {
            throw std::runtime_error("HBM address regions exceed configured capacity");
        }
    };
    allocate(layout.input, input_bytes);
    allocate(layout.output, output_bytes);
    allocate(layout.weight, weight_bytes);
    allocate(layout.edge, edge_bytes);
    allocate(layout.intermediate, intermediate_bytes);
    return layout;
}

AddressDistribution PaperSimulator::MapAddressDistribution(
        const std::vector<MemoryRequest>& requests,
        uint64_t block_size,
        std::size_t channels,
        std::size_t banks) {
    if (block_size == 0 || channels == 0 || banks == 0) {
        throw std::runtime_error("invalid HBM mapping dimensions");
    }
    AddressDistribution distribution;
    distribution.channel_blocks.assign(channels, 0);
    distribution.bank_blocks.assign(banks, 0);
    for (const auto& request : requests) {
        if (request.bytes == 0) {
            throw std::runtime_error("cannot map zero-byte memory request");
        }
        const uint64_t blocks = CeilDiv(request.bytes, block_size);
        const uint64_t first_block = request.address / block_size;
        for (uint64_t block = 0; block < blocks; ++block) {
            const uint64_t index = first_block + block;
            distribution.channel_blocks[index % channels]++;
            distribution.bank_blocks[(index / channels) % banks]++;
        }
    }
    return distribution;
}

LayerMetrics PaperSimulator::RunLayer(const Graph& graph,
                                      const LayerShape& shape,
                                      int layer,
                                      AggregationOp aggregation_op,
                                      const FeatureFlags& flags) const {
    LayerMetrics metrics;
    metrics.layer = layer;
    metrics.input_features = shape.input_features;
    metrics.output_features = shape.output_features;
    metrics.aggregation_op = aggregation_op;

    const auto shards = BuildShards(graph, shape.input_features, flags.sparsity_elimination);
    metrics.shards = shards.size();
    const uint64_t input_stride = Align(static_cast<uint64_t>(shape.input_features) * kDataBytes,
                                        architecture_.block_size);
    const uint64_t output_stride = Align(static_cast<uint64_t>(shape.output_features) * kDataBytes,
                                         architecture_.block_size);

    uint64_t edge_count = 0;
    for (const auto& adjacency : graph.r_adj) {
        edge_count += adjacency.size();
    }
    const uint64_t aggregation_operations = edge_count * shape.input_features;
    if (aggregation_op == AggregationOp::MAX) {
        metrics.compare_operations = aggregation_operations;
    } else {
        metrics.mac_operations += aggregation_operations;
        metrics.add_operations += aggregation_operations;
    }

    for (const auto& shard : shards) {
        metrics.edge_dram_bytes += shard.edge_bytes;
        metrics.input_dram_bytes += shard.input_bytes;
        metrics.requested_input_vertices += flags.sparsity_elimination
            ? shard.unique_neighbors.size()
            : static_cast<uint64_t>(shard.interval_end - shard.interval_start);
        const uint64_t interval_vertices = shard.interval_end > shard.interval_start
            ? static_cast<uint64_t>(shard.interval_end - shard.interval_start)
            : 0;
        if (interval_vertices > shard.unique_neighbors.size()) {
            metrics.skipped_input_vertices += interval_vertices - shard.unique_neighbors.size();
        }
    }

    const double simd_lanes = static_cast<double>(architecture_.num_simd) * architecture_.simd_width;
    const uint64_t ae_compute_cycles = std::max<uint64_t>(1, static_cast<uint64_t>(
        std::ceil(aggregation_operations / (simd_lanes * architecture_.simd_efficiency))));
    metrics.simd_utilization = Clamp(
        aggregation_operations / (ae_compute_cycles * simd_lanes), 0.0, 1.0);
    metrics.simd_idle_lane_cycles = static_cast<uint64_t>(
        ae_compute_cycles * simd_lanes - aggregation_operations);
    metrics.simd_feature_chunks = CeilDiv(shape.input_features, architecture_.simd_width);
    metrics.simd_cores_per_vertex = std::min<uint64_t>(
        architecture_.num_simd, metrics.simd_feature_chunks);
    metrics.simd_parallel_vertices = std::max<uint64_t>(
        1, architecture_.num_simd / metrics.simd_cores_per_vertex);

    const int capacity_batch = std::max<int>(1, std::min<uint64_t>(
        architecture_.input_buffer_bytes / input_stride,
        architecture_.output_buffer_bytes / output_stride));
    int batch_vertices = capacity_batch;
    if (flags.pipeline == PipelineMode::LATENCY_AWARE) {
        batch_vertices = 1;
    } else if (flags.pipeline == PipelineMode::ENERGY_AWARE) {
        batch_vertices = std::max(
            1, std::min(capacity_batch, architecture_.energy_batch_vertices));
    }
    metrics.batches = CeilDiv(graph.num_vertex, batch_vertices);

    AggregationBufferModel aggregation_buffer(architecture_.aggregation_buffer_bytes);
    for (uint64_t batch = 0; batch < metrics.batches; ++batch) {
        const uint64_t batch_start_vertex = batch * batch_vertices;
        const uint64_t vertices = std::min<uint64_t>(
            batch_vertices, graph.num_vertex - batch_start_vertex);
        const uint64_t bytes = vertices * input_stride;
        if (!aggregation_buffer.CanAllocate(bytes)) {
            ++metrics.aggregation_buffer_capacity_stalls;
            throw std::runtime_error("aggregation buffer cannot accept a legal batch");
        }
        aggregation_buffer.Allocate(static_cast<int>(batch), bytes);
        aggregation_buffer.MarkReady(static_cast<int>(batch));
        aggregation_buffer.StartConsume(static_cast<int>(batch));
        aggregation_buffer.Reclaim(static_cast<int>(batch));
    }
    metrics.aggregation_buffer_peak_bytes = aggregation_buffer.PeakBytes();
    if (aggregation_buffer.UsedBytes() != 0 || !aggregation_buffer.Segments().empty()) {
        throw std::runtime_error("aggregation buffer has unreclaimed data");
    }

    const auto combination_schedule = BuildCombinationSchedule(
        graph.num_vertex, shape.input_features, shape.output_features,
        metrics.batches, architecture_, flags.combination);
    const uint64_t raw_weight_bytes = static_cast<uint64_t>(shape.input_features) *
                                      shape.output_features * kDataBytes;
    const uint64_t aligned_weight_bytes = Align(raw_weight_bytes, architecture_.block_size);
    metrics.weight_dram_bytes = combination_schedule.weight_load_bytes;
    metrics.weight_cascade_bytes = combination_schedule.weight_cascade_bytes;
    metrics.combination_active_modules = combination_schedule.active_modules;
    metrics.combination_batch_waves = combination_schedule.batch_waves;
    metrics.combination_output_columns_per_module =
        combination_schedule.output_columns_per_module;
    metrics.output_dram_bytes = static_cast<uint64_t>(graph.num_vertex) * output_stride;
    metrics.aggregation_buffer_read_bytes = static_cast<uint64_t>(graph.num_vertex) * input_stride;
    metrics.aggregation_buffer_write_bytes = metrics.aggregation_buffer_read_bytes;
    if (metrics.aggregation_buffer_read_bytes > architecture_.aggregation_buffer_bytes *
        std::max<uint64_t>(1, metrics.batches)) {
        throw std::runtime_error("aggregation buffer accounting exceeds allocated batches");
    }

    const uint64_t combination_operations = combination_schedule.mac_operations;
    metrics.mac_operations += combination_operations;
    const uint64_t pe_count = static_cast<uint64_t>(architecture_.combination_modules) *
                              architecture_.arrays_per_module * architecture_.array_width;
    metrics.array_utilization = Clamp(
        combination_operations /
            (combination_schedule.compute_cycles * static_cast<double>(pe_count)), 0.0, 1.0);
    metrics.array_idle_lane_cycles = static_cast<uint64_t>(
        combination_schedule.compute_cycles * static_cast<double>(pe_count) -
        combination_operations);

    if (flags.pipeline == PipelineMode::SEQUENTIAL) {
        metrics.intermediate_dram_bytes = static_cast<uint64_t>(std::llround(
            graph.num_vertex * input_stride * architecture_.sequential_spill_factor));
    }

    const double efficiency = flags.memory_coordination
        ? architecture_.coordinated_efficiency
        : architecture_.uncoordinated_efficiency;
    const uint64_t ae_memory_cycles = ServiceCycles(
        metrics.edge_dram_bytes + metrics.input_dram_bytes,
        architecture_.HbmBytesPerCycle(), efficiency);
    const uint64_t ce_memory_cycles = ServiceCycles(
        metrics.weight_dram_bytes + metrics.output_dram_bytes,
        architecture_.HbmBytesPerCycle(), efficiency);
    const uint64_t spill_cycles = ServiceCycles(
        metrics.intermediate_dram_bytes, architecture_.HbmBytesPerCycle(), efficiency);

    const double visible_memory = 1.0 - architecture_.memory_hidden_fraction;
    metrics.aggregation_compute_cycles = ae_compute_cycles;
    metrics.aggregation_memory_cycles = static_cast<uint64_t>(
        std::ceil(ae_memory_cycles * visible_memory));
    metrics.aggregation_cycles = metrics.aggregation_compute_cycles +
        metrics.aggregation_memory_cycles +
        metrics.shards * architecture_.edram_latency_cycles;
    metrics.combination_compute_cycles = combination_schedule.compute_cycles;
    metrics.combination_weight_load_cycles = static_cast<uint64_t>(std::ceil(
        ServiceCycles(metrics.weight_dram_bytes, architecture_.HbmBytesPerCycle(), efficiency) *
        visible_memory));
    metrics.combination_output_cycles = static_cast<uint64_t>(std::ceil(
        ServiceCycles(metrics.output_dram_bytes, architecture_.HbmBytesPerCycle(), efficiency) *
        visible_memory));
    metrics.combination_input_cycles = combination_schedule.input_advance_cycles;
    metrics.combination_pipeline_fill_cycles = combination_schedule.pipeline_fill_cycles;
    metrics.combination_cycles = metrics.combination_weight_load_cycles +
        metrics.combination_input_cycles + metrics.combination_pipeline_fill_cycles +
        metrics.combination_compute_cycles + metrics.combination_output_cycles;
    metrics.memory_service_cycles = ae_memory_cycles + ce_memory_cycles + spill_cycles;

    if (flags.pipeline == PipelineMode::SEQUENTIAL) {
        metrics.ae_finish_cycle = metrics.aggregation_cycles;
        metrics.ce_start_cycle = metrics.ae_finish_cycle +
            static_cast<uint64_t>(std::ceil(spill_cycles * visible_memory));
        metrics.ce_finish_cycle = metrics.ce_start_cycle + metrics.combination_cycles;
        metrics.cycles = metrics.ce_finish_cycle;
    } else {
        const double overlap_fraction = flags.pipeline == PipelineMode::LATENCY_AWARE
            ? architecture_.latency_overlap_fraction
            : architecture_.energy_overlap_fraction;
        const uint64_t overlap = static_cast<uint64_t>(std::floor(
            std::min(metrics.aggregation_cycles, metrics.combination_cycles) * overlap_fraction));
        metrics.ae_finish_cycle = metrics.aggregation_cycles;
        metrics.ce_start_cycle = metrics.aggregation_cycles - overlap;
        metrics.ce_finish_cycle = metrics.ce_start_cycle + metrics.combination_cycles;
        metrics.cycles = std::max(metrics.ae_finish_cycle, metrics.ce_finish_cycle);
    }

    const uint64_t input_region = static_cast<uint64_t>(graph.num_vertex) * input_stride;
    const uint64_t output_region = static_cast<uint64_t>(graph.num_vertex) * output_stride;
    uint64_t edge_region = 0;
    for (const auto& shard : shards) {
        edge_region += shard.edge_bytes;
    }
    const auto layout = BuildHbmLayout(
        input_region, output_region, aligned_weight_bytes, edge_region,
        metrics.intermediate_dram_bytes, architecture_.hbm_capacity_bytes);
    if (graph.num_vertex > 1) {
        const auto first = OutputAddress(layout.output.start, 0, 0,
                                         output_stride, architecture_.block_size);
        const auto second = OutputAddress(layout.output.start, 1, 0,
                                          output_stride, architecture_.block_size);
        if (second - first != output_stride) {
            throw std::runtime_error("output vertex stride invariant failed");
        }
    }

    std::vector<MemoryRequest> requests;
    uint64_t sequence = 0;
    uint64_t edge_offset = 0;
    for (const auto& shard : shards) {
        if (shard.edge_bytes > 0) {
            requests.push_back({shard.batch_id, RequestClass::EDGE, shard.edge_bytes,
                                layout.edge.start + edge_offset,
                                static_cast<uint64_t>(shard.batch_id) *
                                    architecture_.edram_latency_cycles,
                                sequence++});
            edge_offset += shard.edge_bytes;
        }
        if (shard.input_bytes > 0) {
            requests.push_back({shard.batch_id, RequestClass::INPUT, shard.input_bytes,
                                layout.input.start +
                                    static_cast<uint64_t>(shard.interval_start) * input_stride,
                                static_cast<uint64_t>(shard.batch_id) *
                                    architecture_.edram_latency_cycles,
                                sequence++});
        }
    }
    for (uint64_t batch = 0; batch < metrics.batches; ++batch) {
        const uint64_t batch_start_vertex = batch * batch_vertices;
        const uint64_t vertices = std::min<uint64_t>(
            batch_vertices, graph.num_vertex - std::min<uint64_t>(batch_start_vertex, graph.num_vertex));
        const uint64_t per_batch_weights = metrics.weight_dram_bytes / metrics.batches +
            (batch < metrics.weight_dram_bytes % metrics.batches ? 1 : 0);
        if (per_batch_weights > 0) {
            requests.push_back({static_cast<int>(batch), RequestClass::WEIGHT, per_batch_weights,
                                layout.weight.start,
                                batch * architecture_.edram_latency_cycles, sequence++});
        }
        requests.push_back({static_cast<int>(batch), RequestClass::OUTPUT, vertices * output_stride,
                            OutputAddress(layout.output.start, batch_start_vertex, 0,
                                          output_stride, architecture_.block_size),
                            batch * architecture_.edram_latency_cycles, sequence++});
        const uint64_t spill_bytes = metrics.intermediate_dram_bytes / metrics.batches +
            (batch < metrics.intermediate_dram_bytes % metrics.batches ? 1 : 0);
        if (spill_bytes > 0) {
            const uint64_t spill_offset = metrics.intermediate_dram_bytes / metrics.batches * batch +
                std::min<uint64_t>(batch, metrics.intermediate_dram_bytes % metrics.batches);
            requests.push_back({static_cast<int>(batch), RequestClass::OUTPUT, spill_bytes,
                                layout.intermediate.start + spill_offset,
                                batch * architecture_.edram_latency_cycles, sequence++});
        }
    }

    const auto ordered = MemoryCoordinatorModel::Order(std::move(requests), flags.memory_coordination);
    uint64_t cursor = 0;
    const auto distribution = MapAddressDistribution(
        ordered, architecture_.block_size, 8, 16);
    metrics.channel_blocks = distribution.channel_blocks;
    metrics.bank_blocks = distribution.bank_blocks;
    uint64_t requested_bytes = 0;
    for (const auto& request : ordered) {
        if (request.bytes > architecture_.hbm_capacity_bytes ||
            request.address > architecture_.hbm_capacity_bytes - request.bytes) {
            throw std::runtime_error("memory request exceeds configured HBM capacity");
        }
        const uint64_t service_start = std::max(cursor, request.enqueue_cycle);
        const uint64_t wait_cycles = service_start - request.enqueue_cycle;
        const auto request_index = static_cast<std::size_t>(request.request_class);
        metrics.queue_wait_cycles += wait_cycles;
        metrics.request_counts[request_index]++;
        metrics.request_bytes[request_index] += request.bytes;
        metrics.request_wait_cycles[request_index] += wait_cycles;
        requested_bytes += request.bytes;
        const uint64_t service = ServiceCycles(
            request.bytes, architecture_.HbmBytesPerCycle(), efficiency);
        cursor = service_start + service;
    }
    if (requested_bytes != metrics.TotalDramBytes() ||
        metrics.aggregation_buffer_read_bytes != metrics.aggregation_buffer_write_bytes ||
        metrics.output_dram_bytes != static_cast<uint64_t>(graph.num_vertex) * output_stride) {
        throw std::runtime_error("layer traffic conservation invariant failed");
    }
    metrics.hbm_blocked_cycles = static_cast<uint64_t>(std::ceil(
        metrics.memory_service_cycles * (1.0 - efficiency)));
    metrics.channel_imbalance = Imbalance(metrics.channel_blocks);
    metrics.bank_imbalance = Imbalance(metrics.bank_blocks);
    metrics.bandwidth_utilization = Clamp(
        metrics.TotalDramBytes() /
            (std::max<uint64_t>(1, metrics.cycles) * architecture_.HbmBytesPerCycle()),
        0.0, 1.0);

    return metrics;
}

ExperimentResult PaperSimulator::Run(const Graph& graph,
                                     const std::string& model,
                                     const std::string& dataset,
                                     uint64_t seed,
                                     const FeatureFlags& flags,
                                     int selected_layer) const {
    ExperimentResult result;
    result.model = model;
    result.dataset = dataset;
    result.profile = architecture_.profile;
    result.seed = seed;
    result.selected_layer = selected_layer;
    result.flags = flags;
    result.architecture = architecture_;

    const AggregationOp op = model == "gs" ? AggregationOp::MAX : AggregationOp::SUM;
    const auto shapes = GetLayerShapes(graph, model);
    if (selected_layer < -1 || selected_layer >= static_cast<int>(shapes.size())) {
        throw std::runtime_error("selected layer is outside model range");
    }
    for (std::size_t layer = 0; layer < shapes.size(); ++layer) {
        if (selected_layer >= 0 && selected_layer != static_cast<int>(layer)) {
            continue;
        }
        result.layers.push_back(RunLayer(graph, shapes[layer], layer, op, flags));
    }
    return result;
}

std::string ToString(PipelineMode mode) {
    switch (mode) {
        case PipelineMode::SEQUENTIAL:
            return "sequential";
        case PipelineMode::LATENCY_AWARE:
            return "latency-aware";
        case PipelineMode::ENERGY_AWARE:
            return "energy-aware";
    }
    throw std::runtime_error("unknown pipeline mode");
}

std::string ToString(CombinationMode mode) {
    switch (mode) {
        case CombinationMode::INDEPENDENT:
            return "independent";
        case CombinationMode::COOPERATIVE:
            return "cooperative";
    }
    throw std::runtime_error("unknown combination mode");
}

std::string ToString(AggregationOp operation) {
    switch (operation) {
        case AggregationOp::SUM:
            return "sum";
        case AggregationOp::MAX:
            return "max";
    }
    throw std::runtime_error("unknown aggregation operation");
}

PipelineMode ParsePipelineMode(const std::string& value) {
    if (value == "sequential") {
        return PipelineMode::SEQUENTIAL;
    }
    if (value == "latency-aware" || value == "latency") {
        return PipelineMode::LATENCY_AWARE;
    }
    if (value == "energy-aware" || value == "energy") {
        return PipelineMode::ENERGY_AWARE;
    }
    throw std::runtime_error("invalid pipeline mode: " + value);
}

CombinationMode ParseCombinationMode(const std::string& value) {
    if (value == "independent") {
        return CombinationMode::INDEPENDENT;
    }
    if (value == "cooperative") {
        return CombinationMode::COOPERATIVE;
    }
    throw std::runtime_error("invalid combination mode: " + value);
}

bool ParseToggle(const std::string& value) {
    if (value == "on" || value == "true" || value == "1") {
        return true;
    }
    if (value == "off" || value == "false" || value == "0") {
        return false;
    }
    throw std::runtime_error("invalid toggle: " + value);
}

std::string DigestFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open input for digest: " + path);
    }
    uint64_t hash = 1469598103934665603ULL;
    char buffer[8192];
    while (input) {
        input.read(buffer, sizeof(buffer));
        hash = Fnv1aUpdate(hash, buffer, static_cast<std::size_t>(input.gcount()));
    }
    return Hex64(hash);
}

void WriteExperimentJson(const ExperimentResult& result, const std::string& path) {
    const std::filesystem::path output_path(path);
    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    json output;
    output["schema_version"] = 1;
    output["manifest"] = {
        {"git_commit", HYGCN_GIT_COMMIT},
        {"model", result.model},
        {"dataset", result.dataset},
        {"profile", result.profile},
        {"graph_digest", result.graph_digest},
        {"config_digest", result.config_digest},
        {"binary_digest", result.binary_digest},
        {"seed", result.seed},
        {"selected_layer", result.selected_layer < 0 ? "all" : std::to_string(result.selected_layer)},
        {"pipeline", ToString(result.flags.pipeline)},
        {"combination", ToString(result.flags.combination)},
        {"sparsity_elimination", result.flags.sparsity_elimination},
        {"memory_coordination", result.flags.memory_coordination},
    };
    output["architecture"] = ArchitectureJson(result.architecture);
    output["summary"] = {
        {"total_cycles", result.TotalCycles()},
        {"total_dram_bytes", result.TotalDramBytes()},
        {"total_input_dram_bytes", result.TotalInputDramBytes()},
        {"bandwidth_utilization", result.BandwidthUtilization()},
    };
    output["layers"] = json::array();
    for (const auto& layer : result.layers) {
        output["layers"].push_back(LayerJson(layer));
    }

    std::ofstream stream(path);
    if (!stream) {
        throw std::runtime_error("cannot write JSON result: " + path);
    }
    stream << std::setw(2) << output << '\n';
}

void WriteExperimentCsv(const ExperimentResult& result, const std::string& path) {
    const std::filesystem::path output_path(path);
    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    std::ofstream stream(path);
    if (!stream) {
        throw std::runtime_error("cannot write CSV result: " + path);
    }
    stream << "model,dataset,profile,pipeline,combination,sparsity,coordination,layer,"
              "input_features,output_features,cycles,aggregation_cycles,combination_cycles,"
              "memory_service_cycles,edge_dram_bytes,input_dram_bytes,weight_dram_bytes,"
              "output_dram_bytes,intermediate_dram_bytes,total_dram_bytes,mac_operations,"
              "add_operations,compare_operations,simd_utilization,array_utilization,"
              "bandwidth_utilization,queue_wait_cycles,hbm_blocked_cycles,channel_imbalance,"
              "bank_imbalance,ae_finish_cycle,ce_start_cycle,ce_finish_cycle,"
              "aggregation_buffer_peak_bytes,simd_idle_lane_cycles,array_idle_lane_cycles\n";
    for (const auto& layer : result.layers) {
        stream << result.model << ',' << result.dataset << ',' << result.profile << ','
               << ToString(result.flags.pipeline) << ',' << ToString(result.flags.combination) << ','
               << (result.flags.sparsity_elimination ? "on" : "off") << ','
               << (result.flags.memory_coordination ? "on" : "off") << ','
               << layer.layer << ',' << layer.input_features << ',' << layer.output_features << ','
               << layer.cycles << ',' << layer.aggregation_cycles << ','
               << layer.combination_cycles << ',' << layer.memory_service_cycles << ','
               << layer.edge_dram_bytes << ',' << layer.input_dram_bytes << ','
               << layer.weight_dram_bytes << ',' << layer.output_dram_bytes << ','
               << layer.intermediate_dram_bytes << ',' << layer.TotalDramBytes() << ','
               << layer.mac_operations << ',' << layer.add_operations << ','
               << layer.compare_operations << ',' << layer.simd_utilization << ','
               << layer.array_utilization << ',' << layer.bandwidth_utilization << ','
               << layer.queue_wait_cycles << ',' << layer.hbm_blocked_cycles << ','
               << layer.channel_imbalance << ',' << layer.bank_imbalance << ','
               << layer.ae_finish_cycle << ',' << layer.ce_start_cycle << ','
               << layer.ce_finish_cycle << ',' << layer.aggregation_buffer_peak_bytes << ','
               << layer.simd_idle_lane_cycles << ',' << layer.array_idle_lane_cycles << '\n';
    }
}
