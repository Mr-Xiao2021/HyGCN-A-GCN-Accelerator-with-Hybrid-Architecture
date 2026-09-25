#include "paper_sim.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <queue>
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
        case RequestClass::INTERMEDIATE_WRITE:
            return "intermediate_write";
        case RequestClass::INTERMEDIATE_READ:
            return "intermediate_read";
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
        {"input_ping_pong_regions", architecture.input_ping_pong_regions},
        {"edge_ping_pong_regions", architecture.edge_ping_pong_regions},
        {"aggregation_ping_pong_regions", architecture.aggregation_ping_pong_regions},
        {"input_window_capacity_bytes", architecture.InputWindowCapacityBytes()},
        {"edge_shard_capacity_bytes", architecture.EdgeShardCapacityBytes()},
        {"aggregation_shard_capacity_bytes", architecture.AggregationShardCapacityBytes()},
        {"hbm_capacity_bytes", architecture.hbm_capacity_bytes},
        {"hbm_bandwidth_gbps", architecture.hbm_bandwidth_gbps},
        {"hbm_channels", architecture.hbm_channels},
        {"hbm_banks_per_channel", architecture.hbm_banks_per_channel},
        {"hbm_row_bytes", architecture.hbm_row_bytes},
        {"hbm_row_hit_cycles", architecture.hbm_row_hit_cycles},
        {"hbm_row_miss_cycles", architecture.hbm_row_miss_cycles},
        {"edram_latency_cycles", architecture.edram_latency_cycles},
        {"edram_transactions_per_cycle", architecture.edram_transactions_per_cycle},
        {"simd_efficiency", architecture.simd_efficiency},
        {"independent_array_efficiency", architecture.independent_array_efficiency},
        {"cooperative_array_efficiency", architecture.cooperative_array_efficiency},
        {"energy_batch_vertices", architecture.energy_batch_vertices},
        {"batch_launch_interval_cycles", architecture.batch_launch_interval_cycles},
        {"neighbor_index_ready_cycles", architecture.neighbor_index_ready_cycles},
        {"row_first_bank_interleave", architecture.row_first_bank_interleave},
        {"sequential_spill_alignment", architecture.sequential_spill_alignment},
    };
}

json LayerJson(const LayerMetrics& layer) {
    json request_stats;
    for (std::size_t index = 0; index < kRequestClassCount; ++index) {
        const auto request_class = static_cast<RequestClass>(index);
        request_stats[RequestClassName(request_class)] = {
            {"count", layer.request_counts[index]},
            {"bytes", layer.request_bytes[index]},
            {"queue_wait_cycles", layer.request_wait_cycles[index]},
        };
    }
    json producer_requests = json::array();
    for (const auto& trace : layer.producer_request_traces) {
        producer_requests.push_back({
            {"sequence", trace.sequence},
            {"batch_id", trace.batch_id},
            {"request_class", RequestClassName(trace.request_class)},
            {"address", trace.address},
            {"bytes", trace.bytes},
            {"producer_ready_cycle", trace.producer_ready_cycle},
            {"enqueue_cycle", trace.enqueue_cycle},
            {"first_issue_cycle", trace.first_issue_cycle},
            {"completion_cycle", trace.completion_cycle},
            {"producer_sequence", trace.producer_sequence.has_value()
                ? json(*trace.producer_sequence) : json(nullptr)},
        });
    }
    json memory_requests = json::array();
    for (const auto& trace : layer.memory_request_traces) {
        memory_requests.push_back({
            {"sequence", trace.sequence},
            {"batch_id", trace.batch_id},
            {"request_class", RequestClassName(trace.request_class)},
            {"address", trace.address},
            {"bytes", trace.bytes},
            {"producer_ready_cycle", trace.producer_ready_cycle},
            {"enqueue_cycle", trace.enqueue_cycle},
            {"first_issue_cycle", trace.first_issue_cycle},
            {"completion_cycle", trace.completion_cycle},
            {"producer_sequence", trace.producer_sequence.has_value()
                ? json(*trace.producer_sequence) : json(nullptr)},
        });
    }
    json input_windows = json::array();
    for (const auto& trace : layer.input_window_traces) {
        input_windows.push_back({
            {"batch_id", trace.batch_id},
            {"interval_start", trace.interval_start},
            {"interval_end", trace.interval_end},
            {"window_start", trace.window_start},
            {"window_end", trace.window_end},
            {"address", trace.address},
            {"bytes", trace.bytes},
            {"transactions", trace.transactions},
            {"unique_vertices", trace.unique_vertices},
            {"internal_holes", trace.internal_holes},
        });
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
        {"memory_active_cycles", layer.memory_active_cycles},
        {"queue_wait_cycles", layer.queue_wait_cycles},
        {"hbm_blocked_cycles", layer.hbm_blocked_cycles},
        {"row_buffer_hits", layer.row_buffer_hits},
        {"row_buffer_misses", layer.row_buffer_misses},
        {"priority_reorders", layer.priority_reorders},
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
        {"input_window_requests", input_windows},
        {"memory_requests", memory_requests},
        {"producer_dependent_requests", producer_requests},
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
    config.hbm_channels = reader.GetInteger("memory", "hbm_channels", -1);
    config.hbm_banks_per_channel = reader.GetInteger("memory", "hbm_banks_per_channel", -1);
    config.hbm_row_bytes = reader.GetInteger("memory", "hbm_row_bytes", -1);
    config.hbm_row_hit_cycles = reader.GetInteger("memory", "hbm_row_hit_cycles", -1);
    config.hbm_row_miss_cycles = reader.GetInteger("memory", "hbm_row_miss_cycles", -1);
    config.edram_latency_cycles = reader.GetInteger("memory", "edram_latency_cycles", -1);
    config.edram_transactions_per_cycle = reader.GetInteger("memory", "edram_transactions_per_cycle", -1);

    config.simd_efficiency = reader.GetReal("model", "simd_efficiency", -1.0);
    config.independent_array_efficiency = reader.GetReal("model", "independent_array_efficiency", -1.0);
    config.cooperative_array_efficiency = reader.GetReal("model", "cooperative_array_efficiency", -1.0);
    config.energy_batch_vertices = reader.GetInteger("model", "energy_batch_vertices", -1);
    config.input_ping_pong_regions = reader.GetInteger(
        "model", "input_ping_pong_regions", -1);
    config.edge_ping_pong_regions = reader.GetInteger(
        "model", "edge_ping_pong_regions", -1);
    config.aggregation_ping_pong_regions = reader.GetInteger(
        "model", "aggregation_ping_pong_regions", -1);
    config.aggregation_shard_capacity_bytes = reader.GetInteger(
        "model", "aggregation_shard_capacity_bytes", -1);
    config.batch_launch_interval_cycles = reader.GetInteger(
        "model", "batch_launch_interval_cycles", -1);
    config.neighbor_index_ready_cycles = reader.GetInteger(
        "model", "neighbor_index_ready_cycles", -1);
    config.row_first_bank_interleave = reader.GetInteger(
        "model", "row_first_bank_interleave", -1);
    config.sequential_spill_alignment = reader.Get(
        "model", "sequential_spill_alignment", "");
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
    require_positive(input_ping_pong_regions > 0, "input_ping_pong_regions");
    require_positive(edge_ping_pong_regions > 0, "edge_ping_pong_regions");
    require_positive(aggregation_ping_pong_regions > 0,
                     "aggregation_ping_pong_regions");
    require_positive(aggregation_shard_capacity_bytes > 0,
                     "aggregation_shard_capacity_bytes");
    require_positive(input_buffer_bytes % input_ping_pong_regions == 0,
                     "input_buffer_bytes ping-pong alignment");
    require_positive(edge_buffer_bytes % edge_ping_pong_regions == 0,
                     "edge_buffer_bytes ping-pong alignment");
    require_positive(
        aggregation_buffer_bytes % aggregation_ping_pong_regions == 0,
        "aggregation_buffer_bytes ping-pong alignment");
    require_positive(
        aggregation_shard_capacity_bytes <=
            aggregation_buffer_bytes /
                static_cast<uint64_t>(aggregation_ping_pong_regions),
        "aggregation_shard_capacity_bytes ping-pong bound");
    require_positive(hbm_capacity_bytes > 0, "hbm_capacity_bytes");
    require_positive(hbm_bandwidth_gbps > 0.0, "hbm_bandwidth_gbps");
    require_positive(hbm_channels > 0, "hbm_channels");
    require_positive(hbm_banks_per_channel > 0, "hbm_banks_per_channel");
    require_positive(hbm_row_bytes >= static_cast<uint64_t>(block_size) &&
                         hbm_row_bytes % block_size == 0,
                     "hbm_row_bytes");
    require_positive(hbm_row_hit_cycles > 0, "hbm_row_hit_cycles");
    require_positive(hbm_row_miss_cycles > hbm_row_hit_cycles,
                     "hbm_row_miss_cycles");
    require_positive(edram_latency_cycles >= 0, "edram_latency_cycles");
    require_positive(edram_transactions_per_cycle > 0, "edram_transactions_per_cycle");
    require_positive(simd_efficiency > 0.0 && simd_efficiency <= 1.0, "simd_efficiency");
    require_positive(independent_array_efficiency > 0.0 && independent_array_efficiency <= 1.0,
                     "independent_array_efficiency");
    require_positive(cooperative_array_efficiency > 0.0 && cooperative_array_efficiency <= 1.0,
                     "cooperative_array_efficiency");
    require_positive(energy_batch_vertices > 0, "energy_batch_vertices");
    require_positive(batch_launch_interval_cycles >= 0,
                     "batch_launch_interval_cycles");
    require_positive(neighbor_index_ready_cycles >= 0,
                     "neighbor_index_ready_cycles");
    require_positive(row_first_bank_interleave > 0 &&
                         row_first_bank_interleave <= hbm_banks_per_channel &&
                         hbm_banks_per_channel % row_first_bank_interleave == 0,
                     "row_first_bank_interleave");
    require_positive(sequential_spill_alignment == "block",
                     "sequential_spill_alignment");
    require_positive(InputWindowCapacityBytes() >= static_cast<uint64_t>(block_size),
                     "input_window_capacity_bytes");
    require_positive(EdgeShardCapacityBytes() >= static_cast<uint64_t>(block_size),
                     "edge_shard_capacity_bytes");
    require_positive(AggregationShardCapacityBytes() >= static_cast<uint64_t>(block_size),
                     "aggregation_shard_capacity_bytes");
}

double ArchitectureConfig::HbmBytesPerCycle() const {
    return hbm_bandwidth_gbps / frequency_ghz;
}

uint64_t ArchitectureConfig::InputWindowCapacityBytes() const {
    return input_buffer_bytes / input_ping_pong_regions;
}

uint64_t ArchitectureConfig::EdgeShardCapacityBytes() const {
    return edge_buffer_bytes / edge_ping_pong_regions;
}

uint64_t ArchitectureConfig::AggregationShardCapacityBytes() const {
    return aggregation_shard_capacity_bytes;
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
                                                         MemoryPriorityMode priority) {
    for (const auto& request : requests) {
        if (request.batch_id < 0 || request.bytes == 0 ||
            request.address > std::numeric_limits<uint64_t>::max() - request.bytes ||
            (!request.producer_sequence.has_value() &&
             request.enqueue_cycle < request.producer_ready_cycle)) {
            throw std::runtime_error("invalid memory request");
        }
    }
    std::stable_sort(requests.begin(), requests.end(), [priority](const auto& lhs, const auto& rhs) {
        if (lhs.enqueue_cycle != rhs.enqueue_cycle) {
            return lhs.enqueue_cycle < rhs.enqueue_cycle;
        }
        if (priority == MemoryPriorityMode::BATCH_CLASS) {
            if (lhs.batch_id != rhs.batch_id) {
                return lhs.batch_id < rhs.batch_id;
            }
            if (lhs.request_class != rhs.request_class) {
                return static_cast<int>(lhs.request_class) < static_cast<int>(rhs.request_class);
            }
        }
        return lhs.sequence < rhs.sequence;
    });
    return requests;
}

MemoryTimingResult MemoryCoordinatorModel::Simulate(
        const std::vector<MemoryRequest>& requests,
        const ArchitectureConfig& architecture,
        MemoryPriorityMode priority,
        AddressMappingMode mapping) {
    architecture.Validate();
    MemoryTimingResult result;
    result.channel_blocks.assign(architecture.hbm_channels, 0);
    result.bank_blocks.assign(architecture.hbm_banks_per_channel, 0);
    if (requests.empty()) {
        return result;
    }

    struct BankState {
        uint64_t ready_cycle = 0;
        uint64_t open_row = 0;
        bool row_open = false;
    };

    struct RequestState {
        MemoryRequest request;
        uint64_t first_block = 0;
        uint64_t block_count = 0;
        uint64_t next_block = 0;
        uint64_t first_issue = std::numeric_limits<uint64_t>::max();
        uint64_t completion = 0;
        uint64_t effective_producer_ready = 0;
        uint64_t effective_enqueue = 0;
    };

    struct Candidate {
        std::size_t request_index = 0;
        uint64_t start_cycle = 0;
        uint64_t producer_ready_cycle = 0;
        uint64_t enqueue_cycle = 0;
        uint64_t block = 0;
        std::size_t channel = 0;
        std::size_t bank = 0;
        uint64_t row = 0;
    };

    const uint64_t blocks_per_row = architecture.hbm_row_bytes / architecture.block_size;
    const uint64_t bytes_per_channel_cycle =
        std::max<double>(1.0, architecture.HbmBytesPerCycle() / architecture.hbm_channels);
    const uint64_t transfer_cycles = std::max<uint64_t>(
        1, static_cast<uint64_t>(std::ceil(architecture.block_size / bytes_per_channel_cycle)));
    std::vector<uint64_t> channel_issue_cycle(architecture.hbm_channels, 0);
    std::vector<BankState> banks(
        static_cast<std::size_t>(architecture.hbm_channels) *
        architecture.hbm_banks_per_channel);
    std::vector<RequestState> states;
    states.reserve(requests.size());
    std::map<uint64_t, std::size_t> sequence_to_index;
    uint64_t remaining_blocks = 0;
    for (const auto& request : requests) {
        if (request.batch_id < 0 || request.bytes == 0 ||
            request.address > std::numeric_limits<uint64_t>::max() - request.bytes ||
            (!request.producer_sequence.has_value() &&
             request.enqueue_cycle < request.producer_ready_cycle) ||
            !sequence_to_index.emplace(request.sequence, states.size()).second) {
            throw std::runtime_error("invalid memory request");
        }
        const uint64_t block_count = CeilDiv(request.bytes, architecture.block_size);
        states.push_back({request, request.address / architecture.block_size, block_count});
        remaining_blocks += block_count;
    }
    for (const auto& state : states) {
        if (!state.request.producer_sequence.has_value()) {
            continue;
        }
        const auto producer = sequence_to_index.find(*state.request.producer_sequence);
        if (producer == sequence_to_index.end() ||
            *state.request.producer_sequence == state.request.sequence) {
            throw std::runtime_error("invalid memory request dependency");
        }
    }

    auto map_block = [&](uint64_t block) {
        Candidate candidate;
        candidate.block = block;
        if (mapping == AddressMappingMode::LOW_BITS) {
            candidate.channel = static_cast<std::size_t>(block % architecture.hbm_channels);
            candidate.bank = static_cast<std::size_t>(
                (block / architecture.hbm_channels) % architecture.hbm_banks_per_channel);
            candidate.row = block /
                (static_cast<uint64_t>(architecture.hbm_channels) *
                 architecture.hbm_banks_per_channel * blocks_per_row);
        } else {
            const uint64_t bank_interleave =
                static_cast<uint64_t>(architecture.row_first_bank_interleave);
            const uint64_t interleaved_block = block / bank_interleave;
            candidate.channel = static_cast<std::size_t>(
                (interleaved_block / blocks_per_row) % architecture.hbm_channels);
            const uint64_t bank_group =
                (interleaved_block /
                 (blocks_per_row * architecture.hbm_channels)) %
                (architecture.hbm_banks_per_channel / bank_interleave);
            candidate.bank = static_cast<std::size_t>(
                bank_group * bank_interleave + block % bank_interleave);
            candidate.row = interleaved_block /
                (blocks_per_row * architecture.hbm_channels *
                 (architecture.hbm_banks_per_channel / bank_interleave));
        }
        return candidate;
    };

    auto fifo_less = [&](const Candidate& lhs, const Candidate& rhs) {
        const auto& lhs_request = states[lhs.request_index].request;
        const auto& rhs_request = states[rhs.request_index].request;
        if (lhs.enqueue_cycle != rhs.enqueue_cycle) {
            return lhs.enqueue_cycle < rhs.enqueue_cycle;
        }
        return lhs_request.sequence < rhs_request.sequence;
    };
    auto priority_less = [&](const Candidate& lhs, const Candidate& rhs) {
        const auto& lhs_request = states[lhs.request_index].request;
        const auto& rhs_request = states[rhs.request_index].request;
        if (priority == MemoryPriorityMode::FIFO) {
            return fifo_less(lhs, rhs);
        }
        if (lhs_request.batch_id != rhs_request.batch_id) {
            return lhs_request.batch_id < rhs_request.batch_id;
        }
        if (lhs_request.request_class != rhs_request.request_class) {
            return static_cast<int>(lhs_request.request_class) <
                   static_cast<int>(rhs_request.request_class);
        }
        return lhs_request.sequence < rhs_request.sequence;
    };

    std::vector<std::vector<std::size_t>> dependents(states.size());
    for (std::size_t index = 0; index < states.size(); ++index) {
        const auto& producer_sequence = states[index].request.producer_sequence;
        if (producer_sequence.has_value()) {
            dependents[sequence_to_index.at(*producer_sequence)].push_back(index);
        }
    }

    auto make_candidate = [&](std::size_t index) {
        const auto& state = states[index];
        if (state.next_block >= state.block_count) {
            throw std::runtime_error("completed request cannot be scheduled");
        }
        uint64_t producer_ready = state.request.producer_ready_cycle;
        if (state.request.producer_sequence.has_value()) {
            const auto& producer = states[sequence_to_index.at(
                *state.request.producer_sequence)];
            if (producer.next_block < producer.block_count) {
                throw std::runtime_error("dependent request released before producer completion");
            }
            producer_ready = std::max(
                producer_ready,
                producer.completion + state.request.producer_delay_cycles);
        }
        const uint64_t enqueue = std::max(state.request.enqueue_cycle, producer_ready);
        auto candidate = map_block(state.first_block + state.next_block);
        candidate.request_index = index;
        candidate.producer_ready_cycle = producer_ready;
        candidate.enqueue_cycle = enqueue;
        const auto& bank_state = banks[
            candidate.channel * architecture.hbm_banks_per_channel + candidate.bank];
        candidate.start_cycle = std::max(
            {enqueue, channel_issue_cycle[candidate.channel], bank_state.ready_cycle});
        return candidate;
    };
    struct BankQueue {
        using Comparator = std::function<bool(const Candidate&, const Candidate&)>;
        using Queue = std::priority_queue<Candidate, std::vector<Candidate>, Comparator>;
        using Set = std::set<Candidate, Comparator>;

        BankQueue(const Comparator& future_after, const Comparator& available_less)
            : future(future_after), available(available_less),
              available_less(available_less) {}

        Queue future;
        Set available;
        std::map<uint64_t, Set> available_rows;
        Comparator available_less;
    };
    struct BankChoice {
        Candidate candidate;
        std::size_t flat_bank = 0;
        int source = 0;
        bool valid = false;
    };

    auto future_after = [&](const Candidate& lhs, const Candidate& rhs) {
        if (lhs.enqueue_cycle != rhs.enqueue_cycle) {
            return lhs.enqueue_cycle > rhs.enqueue_cycle;
        }
        return priority_less(rhs, lhs);
    };
    auto available_less = [&](const Candidate& lhs, const Candidate& rhs) {
        return priority_less(lhs, rhs);
    };
    auto fifo_after = [&](const Candidate& lhs, const Candidate& rhs) {
        return fifo_less(rhs, lhs);
    };
    const std::size_t total_banks =
        static_cast<std::size_t>(architecture.hbm_channels) *
        architecture.hbm_banks_per_channel;
    std::vector<BankQueue> bank_queues;
    bank_queues.reserve(total_banks);
    for (std::size_t bank = 0; bank < total_banks; ++bank) {
        bank_queues.emplace_back(future_after, available_less);
    }
    std::priority_queue<Candidate, std::vector<Candidate>, decltype(fifo_after)> active_fifo(
        fifo_after);

    auto resource_ready = [&](std::size_t flat_bank) {
        const std::size_t channel =
            flat_bank / architecture.hbm_banks_per_channel;
        return std::max(channel_issue_cycle[channel], banks[flat_bank].ready_cycle);
    };
    auto push_available = [&](BankQueue& queue, const Candidate& candidate) {
        queue.available.insert(candidate);
        auto row = queue.available_rows.find(candidate.row);
        if (row == queue.available_rows.end()) {
            row = queue.available_rows.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(candidate.row),
                std::forward_as_tuple(queue.available_less)).first;
        }
        row->second.insert(candidate);
    };
    auto enqueue_candidate = [&](const Candidate& candidate, bool activate_request) {
        const std::size_t flat_bank =
            candidate.channel * architecture.hbm_banks_per_channel + candidate.bank;
        if (candidate.enqueue_cycle <= resource_ready(flat_bank)) {
            push_available(bank_queues[flat_bank], candidate);
        } else {
            bank_queues[flat_bank].future.push(candidate);
        }
        if (activate_request) {
            active_fifo.push(candidate);
        }
    };
    auto peek_bank = [&](std::size_t flat_bank) {
        auto& queue = bank_queues[flat_bank];
        const uint64_t ready_cycle = resource_ready(flat_bank);
        while (!queue.future.empty() &&
               queue.future.top().enqueue_cycle <= ready_cycle) {
            push_available(queue, queue.future.top());
            queue.future.pop();
        }
        BankChoice choice;
        choice.flat_bank = flat_bank;
        if (!queue.available.empty()) {
            choice.candidate = *queue.available.begin();
            choice.candidate.start_cycle = ready_cycle;
            choice.source = 1;
            choice.valid = true;
            if (priority == MemoryPriorityMode::BATCH_CLASS &&
                banks[flat_bank].row_open) {
                const auto row = queue.available_rows.find(banks[flat_bank].open_row);
                if (row != queue.available_rows.end()) {
                    auto& row_queue = row->second;
                    if (!row_queue.empty()) {
                        const auto& primary_request =
                            states[choice.candidate.request_index].request;
                        const auto& row_request =
                            states[row_queue.begin()->request_index].request;
                        if (primary_request.batch_id == row_request.batch_id &&
                            primary_request.request_class == row_request.request_class) {
                            choice.candidate = *row_queue.begin();
                            choice.candidate.start_cycle = ready_cycle;
                            choice.source = 2;
                        }
                    }
                }
            }
        } else if (!queue.future.empty()) {
            choice.candidate = queue.future.top();
            choice.candidate.start_cycle = queue.future.top().enqueue_cycle;
            choice.valid = true;
        }
        return choice;
    };
    auto choice_less = [&](const BankChoice& lhs, const BankChoice& rhs) {
        if (!lhs.valid) {
            return false;
        }
        if (!rhs.valid) {
            return true;
        }
        if (priority == MemoryPriorityMode::FIFO) {
            if (fifo_less(lhs.candidate, rhs.candidate)) {
                return true;
            }
            if (fifo_less(rhs.candidate, lhs.candidate)) {
                return false;
            }
        }
        if (lhs.candidate.start_cycle != rhs.candidate.start_cycle) {
            return lhs.candidate.start_cycle < rhs.candidate.start_cycle;
        }
        return priority_less(lhs.candidate, rhs.candidate);
    };

    std::vector<BankChoice> channel_choices(architecture.hbm_channels);
    auto recompute_channel = [&](std::size_t channel) {
        BankChoice best;
        const std::size_t first_bank =
            channel * architecture.hbm_banks_per_channel;
        for (std::size_t bank = 0;
             bank < static_cast<std::size_t>(architecture.hbm_banks_per_channel);
             ++bank) {
            const BankChoice candidate = peek_bank(first_bank + bank);
            if (choice_less(candidate, best)) {
                best = candidate;
            }
        }
        channel_choices[channel] = best;
    };

    for (std::size_t index = 0; index < states.size(); ++index) {
        if (!states[index].request.producer_sequence.has_value()) {
            enqueue_candidate(make_candidate(index), true);
        }
    }
    for (std::size_t channel = 0; channel < channel_choices.size(); ++channel) {
        recompute_channel(channel);
    }

    while (remaining_blocks > 0) {
        BankChoice selected_choice;
        for (const auto& channel_choice : channel_choices) {
            if (choice_less(channel_choice, selected_choice)) {
                selected_choice = channel_choice;
            }
        }
        if (!selected_choice.valid) {
            throw std::runtime_error("memory request dependency cycle");
        }
        const Candidate selected = selected_choice.candidate;
        auto& selected_queue = bank_queues[selected_choice.flat_bank];
        if (selected_choice.source == 1 || selected_choice.source == 2) {
            selected_queue.available.erase(selected);
            auto row = selected_queue.available_rows.find(selected.row);
            if (row == selected_queue.available_rows.end() ||
                row->second.erase(selected) != 1) {
                throw std::runtime_error("available row index is inconsistent");
            }
            if (row->second.empty()) {
                selected_queue.available_rows.erase(row);
            }
        } else {
            selected_queue.future.pop();
        }

        auto& state = states[selected.request_index];
        while (!active_fifo.empty() &&
               states[active_fifo.top().request_index].next_block >=
                   states[active_fifo.top().request_index].block_count) {
            active_fifo.pop();
        }
        if (priority == MemoryPriorityMode::BATCH_CLASS &&
            state.first_issue == std::numeric_limits<uint64_t>::max() &&
            !active_fifo.empty() &&
            active_fifo.top().request_index != selected.request_index) {
            const Candidate fifo_candidate = make_candidate(
                active_fifo.top().request_index);
            if (fifo_candidate.start_cycle <= selected.start_cycle) {
                ++result.priority_reorders;
            }
        }
        auto& bank_state = banks[selected_choice.flat_bank];
        state.effective_producer_ready = std::max(
            state.effective_producer_ready, selected.producer_ready_cycle);
        state.effective_enqueue = std::max(
            state.effective_enqueue, selected.enqueue_cycle);
        state.first_issue = std::min(state.first_issue, selected.start_cycle);
        const bool row_hit = bank_state.row_open && bank_state.open_row == selected.row;
        const uint64_t access_cycles = row_hit
            ? architecture.hbm_row_hit_cycles
            : architecture.hbm_row_miss_cycles;
        if (row_hit) {
            ++result.row_buffer_hits;
        } else {
            ++result.row_buffer_misses;
        }
        const uint64_t completion = selected.start_cycle + access_cycles + transfer_cycles;
        channel_issue_cycle[selected.channel] = selected.start_cycle + transfer_cycles;
        bank_state.ready_cycle = completion;
        bank_state.open_row = selected.row;
        bank_state.row_open = true;
        state.completion = std::max(state.completion, completion);
        ++state.next_block;
        --remaining_blocks;
        ++result.channel_blocks[selected.channel];
        ++result.bank_blocks[selected.bank];

        std::vector<bool> affected_channels(architecture.hbm_channels, false);
        affected_channels[selected.channel] = true;
        if (state.next_block < state.block_count) {
            const Candidate next = make_candidate(selected.request_index);
            enqueue_candidate(next, false);
            affected_channels[next.channel] = true;
        } else {
            for (std::size_t dependent : dependents[selected.request_index]) {
                const Candidate released = make_candidate(dependent);
                enqueue_candidate(released, true);
                affected_channels[released.channel] = true;
            }
        }
        for (std::size_t channel = 0; channel < affected_channels.size(); ++channel) {
            if (affected_channels[channel]) {
                recompute_channel(channel);
            }
        }
    }

    for (const auto& state : states) {
        const auto& request = state.request;
        const auto class_index = static_cast<std::size_t>(request.request_class);
        const uint64_t wait = state.first_issue - state.effective_enqueue;
        result.queue_wait_cycles += wait;
        result.blocked_cycles += wait;
        ++result.request_counts[class_index];
        result.request_bytes[class_index] += request.bytes;
        result.request_wait_cycles[class_index] += wait;
        result.class_completion_cycles[class_index] = std::max(
            result.class_completion_cycles[class_index], state.completion);
        auto& batch_completion = result.batch_completion_cycles[request.batch_id];
        batch_completion[class_index] = std::max(
            batch_completion[class_index], state.completion);
        result.request_traces.push_back({
            request.batch_id,
            request.request_class,
            request.bytes,
            request.address,
            state.effective_producer_ready,
            state.effective_enqueue,
            state.first_issue,
            state.completion,
            request.sequence,
            request.producer_sequence,
        });
        result.cycles = std::max(result.cycles, state.completion);
    }
    std::stable_sort(result.request_traces.begin(), result.request_traces.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.sequence < rhs.sequence; });
    std::vector<std::pair<uint64_t, uint64_t>> active_intervals;
    active_intervals.reserve(result.request_traces.size());
    for (const auto& trace : result.request_traces) {
        active_intervals.push_back({trace.first_issue_cycle, trace.completion_cycle});
    }
    std::sort(active_intervals.begin(), active_intervals.end());
    uint64_t active_start = active_intervals.front().first;
    uint64_t active_end = active_intervals.front().second;
    for (std::size_t index = 1; index < active_intervals.size(); ++index) {
        const auto& interval = active_intervals[index];
        if (interval.first <= active_end) {
            active_end = std::max(active_end, interval.second);
            continue;
        }
        result.active_cycles += active_end - active_start;
        active_start = interval.first;
        active_end = interval.second;
    }
    result.active_cycles += active_end - active_start;
    return result;
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

uint64_t ExperimentResult::TotalAggregationCycles() const {
    uint64_t total = 0;
    for (const auto& layer : layers) {
        total += layer.aggregation_cycles;
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

uint64_t ExperimentResult::TotalAggregationDramBytes() const {
    uint64_t total = 0;
    for (const auto& layer : layers) {
        total += layer.edge_dram_bytes + layer.input_dram_bytes;
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
    uint64_t memory_cycles = 0;
    for (const auto& layer : layers) {
        memory_cycles += layer.memory_active_cycles;
    }
    if (memory_cycles == 0) {
        return 0.0;
    }
    return Clamp(TotalDramBytes() /
                     (memory_cycles * architecture.HbmBytesPerCycle()),
                 0.0, 1.0);
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
    const uint64_t input_partition_bytes = architecture_.InputWindowCapacityBytes();
    const uint64_t edge_partition_bytes = architecture_.EdgeShardCapacityBytes();
    const uint64_t aggregation_partition_bytes =
        architecture_.AggregationShardCapacityBytes();
    const int interval_capacity = std::max<int>(1, input_partition_bytes / feature_stride);
    const uint64_t aggregation_capacity = aggregation_partition_bytes;
    const int max_dst_vertices = std::max<int>(1, aggregation_capacity / feature_stride);

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
                (chunk_bytes + row_bytes > edge_partition_bytes ||
                 chunk_vertices >= max_dst_vertices)) {
                break;
            }
            if (row_bytes > edge_partition_bytes) {
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

            shard.input_bytes = static_cast<uint64_t>(
                shard.shrunk_interval_end - shard.interval_start) * feature_stride;
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
        ? std::min<uint64_t>(architecture.combination_modules,
                             static_cast<uint64_t>(rows))
        : architecture.combination_modules;
    schedule.batch_waves = mode == CombinationMode::INDEPENDENT
        ? CeilDiv(batches, architecture.combination_modules)
        : batches;
    schedule.output_columns_per_module = mode == CombinationMode::INDEPENDENT
        ? columns
        : CeilDiv(columns, schedule.active_modules);

    const uint64_t raw_weight_bytes = static_cast<uint64_t>(inner) * columns * kDataBytes;
    const uint64_t aligned_weight_bytes = Align(raw_weight_bytes, architecture.block_size);
    schedule.weight_load_bytes = aligned_weight_bytes;
    if (mode == CombinationMode::INDEPENDENT) {
        schedule.weight_cascade_bytes = 0;
    } else {
        schedule.weight_cascade_bytes = aligned_weight_bytes *
            (schedule.active_modules > 0 ? schedule.active_modules - 1 : 0);
    }

    const uint64_t pe_count = schedule.active_modules *
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

    struct BatchInfo {
        int dst_start = std::numeric_limits<int>::max();
        int dst_end = 0;
        uint64_t shard_count = 0;
        uint64_t edge_count = 0;
        uint64_t aggregation_bytes = 0;
        uint64_t spill_bytes = 0;
    };

    int raw_batch_count = 0;
    for (const auto& shard : shards) {
        raw_batch_count = std::max(raw_batch_count, shard.batch_id + 1);
    }
    if (raw_batch_count == 0) {
        throw std::runtime_error("partitioning produced no execution batches");
    }
    std::vector<BatchInfo> batch_info(raw_batch_count);
    for (const auto& shard : shards) {
        auto& batch = batch_info[shard.batch_id];
        batch.dst_start = std::min(batch.dst_start, shard.dst_start);
        batch.dst_end = std::max(batch.dst_end, shard.dst_end);
        ++batch.shard_count;
    }
    for (auto& batch : batch_info) {
        if (batch.dst_start < 0 || batch.dst_end <= batch.dst_start ||
            batch.dst_end > graph.num_vertex) {
            throw std::runtime_error("invalid execution batch bounds");
        }
        for (int vertex = batch.dst_start; vertex < batch.dst_end; ++vertex) {
            batch.edge_count += graph.r_adj[vertex].size();
        }
        batch.aggregation_bytes =
            static_cast<uint64_t>(batch.dst_end - batch.dst_start) * input_stride;
        const uint64_t batch_capacity = architecture_.AggregationShardCapacityBytes();
        if (batch.aggregation_bytes > batch_capacity) {
            throw std::runtime_error("execution batch exceeds aggregation buffer allocation");
        }
        batch.spill_bytes = Align(batch.aggregation_bytes, architecture_.block_size);
    }

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
            ? static_cast<uint64_t>(shard.shrunk_interval_end - shard.interval_start)
            : static_cast<uint64_t>(shard.interval_end - shard.interval_start);
        const uint64_t requested_vertices = flags.sparsity_elimination
            ? static_cast<uint64_t>(shard.shrunk_interval_end - shard.interval_start)
            : static_cast<uint64_t>(shard.interval_end - shard.interval_start);
        const uint64_t interval_vertices = static_cast<uint64_t>(
            shard.interval_end - shard.interval_start);
        if (interval_vertices > requested_vertices) {
            metrics.skipped_input_vertices += interval_vertices - requested_vertices;
        }
    }

    const double simd_lanes = static_cast<double>(architecture_.num_simd) * architecture_.simd_width;
    std::vector<uint64_t> batch_ae_compute(raw_batch_count, 0);
    for (int batch = 0; batch < raw_batch_count; ++batch) {
        const uint64_t operations = batch_info[batch].edge_count * shape.input_features;
        batch_ae_compute[batch] = std::max<uint64_t>(1, static_cast<uint64_t>(
            std::ceil(operations / (simd_lanes * architecture_.simd_efficiency))));
        metrics.aggregation_compute_cycles += batch_ae_compute[batch];
    }
    metrics.simd_utilization = Clamp(
        aggregation_operations / (metrics.aggregation_compute_cycles * simd_lanes), 0.0, 1.0);
    metrics.simd_idle_lane_cycles = static_cast<uint64_t>(
        metrics.aggregation_compute_cycles * simd_lanes - aggregation_operations);
    metrics.simd_feature_chunks = CeilDiv(shape.input_features, architecture_.simd_width);
    metrics.simd_cores_per_vertex = std::min<uint64_t>(
        architecture_.num_simd, metrics.simd_feature_chunks);
    metrics.simd_parallel_vertices = std::max<uint64_t>(
        1, architecture_.num_simd / metrics.simd_cores_per_vertex);

    uint64_t aligned_weight_bytes = 0;
    uint64_t combination_operations = 0;
    metrics.aggregation_buffer_write_bytes =
        static_cast<uint64_t>(graph.num_vertex) * input_stride;
    if (!flags.aggregation_only) {
        const uint64_t raw_weight_bytes = static_cast<uint64_t>(shape.input_features) *
                                          shape.output_features * kDataBytes;
        aligned_weight_bytes = Align(raw_weight_bytes, architecture_.block_size);
        metrics.weight_dram_bytes = aligned_weight_bytes;
        metrics.weight_cascade_bytes = flags.combination == CombinationMode::COOPERATIVE
            ? aligned_weight_bytes * (architecture_.combination_modules - 1)
            : 0;
        metrics.output_dram_bytes = static_cast<uint64_t>(graph.num_vertex) * output_stride;
        metrics.aggregation_buffer_read_bytes = metrics.aggregation_buffer_write_bytes;
        combination_operations = static_cast<uint64_t>(graph.num_vertex) *
                                 shape.input_features * shape.output_features;
        metrics.mac_operations += combination_operations;
        if (flags.pipeline == PipelineMode::SEQUENTIAL) {
            for (const auto& batch : batch_info) {
                metrics.intermediate_dram_bytes += 2 * batch.spill_bytes;
            }
        }
    }

    const uint64_t input_region = static_cast<uint64_t>(graph.num_vertex) * input_stride;
    const uint64_t output_region = flags.aggregation_only
        ? 0
        : static_cast<uint64_t>(graph.num_vertex) * output_stride;
    uint64_t edge_region = 0;
    for (const auto& shard : shards) {
        edge_region += shard.edge_bytes;
    }
    uint64_t intermediate_region = 0;
    if (!flags.aggregation_only && flags.pipeline == PipelineMode::SEQUENTIAL) {
        for (const auto& batch : batch_info) {
            intermediate_region += batch.spill_bytes;
        }
    }
    const auto layout = BuildHbmLayout(
        input_region, output_region, aligned_weight_bytes, edge_region,
        intermediate_region, architecture_.hbm_capacity_bytes);
    if (!flags.aggregation_only && graph.num_vertex > 1) {
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
    std::vector<std::optional<uint64_t>> edge_sequences(raw_batch_count);
    for (const auto& shard : shards) {
        if (shard.edge_bytes > 0) {
            const uint64_t edge_sequence = sequence++;
            edge_sequences[shard.batch_id] = edge_sequence;
            requests.push_back({shard.batch_id, RequestClass::EDGE, shard.edge_bytes,
                                layout.edge.start + edge_offset,
                                static_cast<uint64_t>(shard.batch_id) *
                                    architecture_.batch_launch_interval_cycles,
                                edge_sequence});
            edge_offset += shard.edge_bytes;
        }
    }
    for (const auto& shard : shards) {
        if (shard.input_bytes > 0) {
            if (!edge_sequences[shard.batch_id].has_value()) {
                throw std::runtime_error("input window is missing its edge producer");
            }
            const int window_end = flags.sparsity_elimination
                ? shard.shrunk_interval_end
                : shard.interval_end;
            const uint64_t window_vertices = static_cast<uint64_t>(
                window_end - shard.interval_start);
            const uint64_t unique_vertices = shard.unique_neighbors.size();
            const uint64_t address = layout.input.start +
                static_cast<uint64_t>(shard.interval_start) * input_stride;
            requests.push_back({
                shard.batch_id,
                RequestClass::INPUT,
                shard.input_bytes,
                address,
                static_cast<uint64_t>(shard.batch_id) *
                    architecture_.batch_launch_interval_cycles,
                sequence++,
                0,
                edge_sequences[shard.batch_id],
                static_cast<uint64_t>(architecture_.neighbor_index_ready_cycles),
            });
            metrics.input_window_traces.push_back({
                shard.batch_id,
                shard.interval_start,
                shard.interval_end,
                shard.interval_start,
                window_end,
                address,
                shard.input_bytes,
                CeilDiv(shard.input_bytes, architecture_.block_size),
                unique_vertices,
                window_vertices > unique_vertices ? window_vertices - unique_vertices : 0,
            });
        }
    }
    if (!flags.aggregation_only) {
        requests.push_back({0, RequestClass::WEIGHT, metrics.weight_dram_bytes,
                            layout.weight.start, 0, sequence++});
    }

    auto validate_requests = [&]() {
        for (const auto& request : requests) {
            if (request.bytes > architecture_.hbm_capacity_bytes ||
                request.address > architecture_.hbm_capacity_bytes - request.bytes) {
                throw std::runtime_error("memory request exceeds configured HBM capacity");
            }
            if (!request.producer_sequence.has_value() &&
                request.enqueue_cycle < request.producer_ready_cycle) {
                throw std::runtime_error("memory request enqueued before producer readiness");
            }
        }
    };
    auto simulate = [&]() {
        validate_requests();
        return MemoryCoordinatorModel::Simulate(
            requests, architecture_, flags.memory_priority, flags.address_mapping);
    };
    auto trace_completion = [](const MemoryTimingResult& timing, uint64_t request_sequence) {
        const auto iterator = std::find_if(
            timing.request_traces.begin(), timing.request_traces.end(),
            [request_sequence](const auto& trace) {
                return trace.sequence == request_sequence;
            });
        if (iterator == timing.request_traces.end()) {
            throw std::runtime_error("memory request trace not found");
        }
        return iterator->completion_cycle;
    };

    const auto prefetch_timing = simulate();
    const uint64_t weight_ready = prefetch_timing.class_completion_cycles[
        static_cast<std::size_t>(RequestClass::WEIGHT)];
    std::vector<uint64_t> batch_ae_finish(raw_batch_count, 0);
    uint64_t ae_cursor = 0;
    for (int batch = 0; batch < raw_batch_count; ++batch) {
        const auto completion = prefetch_timing.batch_completion_cycles.find(batch);
        uint64_t memory_ready = 0;
        if (completion != prefetch_timing.batch_completion_cycles.end()) {
            memory_ready = std::max(
                completion->second[static_cast<std::size_t>(RequestClass::EDGE)],
                completion->second[static_cast<std::size_t>(RequestClass::INPUT)]);
        }
        ae_cursor = std::max(ae_cursor, memory_ready) + batch_ae_compute[batch] +
            batch_info[batch].shard_count * architecture_.edram_latency_cycles;
        batch_ae_finish[batch] = ae_cursor;
    }
    metrics.ae_finish_cycle = ae_cursor;
    metrics.aggregation_cycles = metrics.ae_finish_cycle;

    auto record_memory_timing = [&](const MemoryTimingResult& memory_timing) {
        metrics.memory_service_cycles = memory_timing.cycles;
        metrics.memory_active_cycles = memory_timing.active_cycles;
        metrics.queue_wait_cycles = memory_timing.queue_wait_cycles;
        metrics.hbm_blocked_cycles = memory_timing.blocked_cycles;
        metrics.row_buffer_hits = memory_timing.row_buffer_hits;
        metrics.row_buffer_misses = memory_timing.row_buffer_misses;
        metrics.priority_reorders = memory_timing.priority_reorders;
        metrics.request_counts = memory_timing.request_counts;
        metrics.request_bytes = memory_timing.request_bytes;
        metrics.request_wait_cycles = memory_timing.request_wait_cycles;
        metrics.channel_blocks = memory_timing.channel_blocks;
        metrics.bank_blocks = memory_timing.bank_blocks;
        metrics.memory_request_traces = memory_timing.request_traces;
        metrics.aggregation_memory_cycles = std::max(
            memory_timing.class_completion_cycles[static_cast<std::size_t>(RequestClass::EDGE)],
            memory_timing.class_completion_cycles[static_cast<std::size_t>(RequestClass::INPUT)]);
        metrics.combination_weight_load_cycles =
            memory_timing.class_completion_cycles[static_cast<std::size_t>(RequestClass::WEIGHT)];
        metrics.combination_output_cycles =
            memory_timing.class_completion_cycles[static_cast<std::size_t>(RequestClass::OUTPUT)];
        for (const auto& trace : memory_timing.request_traces) {
            if (trace.producer_sequence.has_value() ||
                trace.request_class == RequestClass::OUTPUT ||
                trace.request_class == RequestClass::INTERMEDIATE_WRITE ||
                trace.request_class == RequestClass::INTERMEDIATE_READ) {
                if (trace.enqueue_cycle < trace.producer_ready_cycle ||
                    trace.first_issue_cycle < trace.enqueue_cycle) {
                    throw std::runtime_error("producer-dependent request violated causality");
                }
                metrics.producer_request_traces.push_back(trace);
            }
        }
    };

    if (flags.aggregation_only) {
        metrics.batches = raw_batch_count;
        for (const auto& batch : batch_info) {
            metrics.aggregation_buffer_peak_bytes = std::max(
                metrics.aggregation_buffer_peak_bytes, batch.aggregation_bytes);
        }
        record_memory_timing(prefetch_timing);
        const uint64_t requested_bytes = std::accumulate(
            metrics.request_bytes.begin(), metrics.request_bytes.end(), uint64_t{0});
        if (requested_bytes != metrics.TotalDramBytes() ||
            metrics.TotalDramBytes() != metrics.edge_dram_bytes + metrics.input_dram_bytes ||
            metrics.weight_dram_bytes != 0 || metrics.output_dram_bytes != 0 ||
            metrics.intermediate_dram_bytes != 0) {
            throw std::runtime_error("aggregation-only traffic invariant failed");
        }
        metrics.channel_imbalance = Imbalance(metrics.channel_blocks);
        metrics.bank_imbalance = Imbalance(metrics.bank_blocks);
        metrics.bandwidth_utilization = Clamp(
            metrics.TotalDramBytes() /
                (std::max<uint64_t>(1, metrics.memory_active_cycles) *
                 architecture_.HbmBytesPerCycle()),
            0.0, 1.0);
        metrics.cycles = metrics.ae_finish_cycle;
        return metrics;
    }

    uint64_t array_capacity_cycles = 0;
    auto record_schedule = [&](const CombinationSchedule& schedule) {
        metrics.combination_input_cycles += schedule.input_advance_cycles;
        metrics.combination_pipeline_fill_cycles += schedule.pipeline_fill_cycles;
        metrics.combination_compute_cycles += schedule.compute_cycles;
        metrics.combination_active_modules = std::max(
            metrics.combination_active_modules, schedule.active_modules);
        metrics.combination_batch_waves += schedule.batch_waves;
        metrics.combination_output_columns_per_module = std::max(
            metrics.combination_output_columns_per_module,
            schedule.output_columns_per_module);
        array_capacity_cycles += schedule.compute_cycles * schedule.active_modules *
            architecture_.arrays_per_module * architecture_.array_width;
    };

    if (flags.pipeline == PipelineMode::SEQUENTIAL) {
        metrics.batches = 1;
        uint64_t intermediate_offset = 0;
        std::vector<uint64_t> write_sequences;
        for (int batch = 0; batch < raw_batch_count; ++batch) {
            const auto& info = batch_info[batch];
            requests.push_back({
                batch,
                RequestClass::INTERMEDIATE_WRITE,
                info.spill_bytes,
                layout.intermediate.start + intermediate_offset,
                metrics.ae_finish_cycle,
                sequence,
                batch_ae_finish[batch],
            });
            write_sequences.push_back(sequence++);
            intermediate_offset += info.spill_bytes;
            metrics.aggregation_buffer_peak_bytes = std::max(
                metrics.aggregation_buffer_peak_bytes, info.aggregation_bytes);
        }
        const auto write_timing = simulate();
        uint64_t all_writes_complete = 0;
        std::vector<uint64_t> write_completions;
        for (uint64_t write_sequence : write_sequences) {
            const uint64_t completion = trace_completion(write_timing, write_sequence);
            write_completions.push_back(completion);
            all_writes_complete = std::max(all_writes_complete, completion);
        }

        intermediate_offset = 0;
        std::vector<uint64_t> read_sequences;
        for (int batch = 0; batch < raw_batch_count; ++batch) {
            const auto& info = batch_info[batch];
            requests.push_back({
                batch,
                RequestClass::INTERMEDIATE_READ,
                info.spill_bytes,
                layout.intermediate.start + intermediate_offset,
                all_writes_complete,
                sequence,
                write_completions[batch],
                write_sequences[batch],
                0,
            });
            read_sequences.push_back(sequence++);
            intermediate_offset += info.spill_bytes;
        }
        const auto read_timing = simulate();
        uint64_t all_reads_complete = 0;
        for (uint64_t read_sequence : read_sequences) {
            all_reads_complete = std::max(
                all_reads_complete, trace_completion(read_timing, read_sequence));
        }

        const auto schedule = BuildCombinationSchedule(
            graph.num_vertex, shape.input_features, shape.output_features,
            raw_batch_count, architecture_, flags.combination);
        record_schedule(schedule);
        metrics.ce_start_cycle = std::max({metrics.ae_finish_cycle,
                                           all_reads_complete,
                                           weight_ready});
        metrics.ce_finish_cycle = metrics.ce_start_cycle + schedule.total_cycles;
        for (int batch = 0; batch < raw_batch_count; ++batch) {
            const auto& info = batch_info[batch];
            const uint64_t vertices = info.dst_end - info.dst_start;
            requests.push_back({
                batch,
                RequestClass::OUTPUT,
                vertices * output_stride,
                OutputAddress(layout.output.start, info.dst_start, 0,
                              output_stride, architecture_.block_size),
                metrics.ce_finish_cycle,
                sequence++,
                metrics.ce_finish_cycle,
            });
        }
    } else {
        std::vector<std::pair<int, int>> groups;
        if (flags.pipeline == PipelineMode::LATENCY_AWARE) {
            for (int batch = 0; batch < raw_batch_count; ++batch) {
                groups.push_back({batch, batch + 1});
            }
        } else {
            int begin = 0;
            while (begin < raw_batch_count) {
                int end = begin;
                uint64_t vertices = 0;
                uint64_t bytes = 0;
                while (end < raw_batch_count) {
                    const auto& info = batch_info[end];
                    const uint64_t next_vertices = info.dst_end - info.dst_start;
                    if (end > begin &&
                        (vertices + next_vertices >
                             static_cast<uint64_t>(architecture_.energy_batch_vertices) ||
                         bytes + info.aggregation_bytes >
                             architecture_.aggregation_buffer_bytes)) {
                        break;
                    }
                    vertices += next_vertices;
                    bytes += info.aggregation_bytes;
                    ++end;
                }
                groups.push_back({begin, end});
                begin = end;
            }
        }
        metrics.batches = groups.size();

        using Release = std::pair<uint64_t, uint64_t>;
        std::priority_queue<Release, std::vector<Release>, std::greater<Release>> releases;
        std::vector<uint64_t> module_ready(architecture_.combination_modules, 0);
        uint64_t cooperative_ready = 0;
        uint64_t buffer_used = 0;
        uint64_t first_ce_start = std::numeric_limits<uint64_t>::max();
        uint64_t last_ce_finish = 0;
        uint64_t fifo_release_cycle = 0;

        auto reclaim_until = [&](uint64_t cycle) {
            while (!releases.empty() && releases.top().first <= cycle) {
                if (releases.top().second > buffer_used) {
                    throw std::runtime_error("aggregation buffer release underflow");
                }
                buffer_used -= releases.top().second;
                releases.pop();
            }
        };

        uint64_t scheduled_ae_cursor = 0;
        for (const auto& group : groups) {
            uint64_t group_bytes = 0;
            uint64_t group_vertices = 0;
            uint64_t group_ready = scheduled_ae_cursor;
            for (int batch = group.first; batch < group.second; ++batch) {
                uint64_t finish = std::max(scheduled_ae_cursor, batch_ae_finish[batch]);
                reclaim_until(finish);
                while (buffer_used + batch_info[batch].aggregation_bytes >
                       architecture_.aggregation_buffer_bytes) {
                    if (releases.empty()) {
                        throw std::runtime_error("aggregation buffer has no reclaimable batch");
                    }
                    finish = std::max(finish, releases.top().first);
                    ++metrics.aggregation_buffer_capacity_stalls;
                    reclaim_until(finish);
                }
                buffer_used += batch_info[batch].aggregation_bytes;
                metrics.aggregation_buffer_peak_bytes = std::max(
                    metrics.aggregation_buffer_peak_bytes, buffer_used);
                scheduled_ae_cursor = finish;
                group_ready = finish;
                group_bytes += batch_info[batch].aggregation_bytes;
                group_vertices += batch_info[batch].dst_end - batch_info[batch].dst_start;
            }

            const auto schedule = BuildCombinationSchedule(
                static_cast<int>(group_vertices), shape.input_features, shape.output_features,
                static_cast<uint64_t>(group.second - group.first),
                architecture_, flags.combination);
            record_schedule(schedule);
            uint64_t ce_start = 0;
            if (flags.combination == CombinationMode::COOPERATIVE) {
                ce_start = std::max({group_ready, cooperative_ready, weight_ready});
            } else {
                std::vector<std::size_t> module_order(module_ready.size());
                std::iota(module_order.begin(), module_order.end(), 0);
                std::stable_sort(module_order.begin(), module_order.end(),
                    [&](std::size_t lhs, std::size_t rhs) {
                        return module_ready[lhs] < module_ready[rhs];
                    });
                ce_start = std::max(group_ready, weight_ready);
                for (std::size_t index = 0; index < schedule.active_modules; ++index) {
                    ce_start = std::max(ce_start, module_ready[module_order[index]]);
                }
                const uint64_t finish = ce_start + schedule.total_cycles;
                for (std::size_t index = 0; index < schedule.active_modules; ++index) {
                    module_ready[module_order[index]] = finish;
                }
            }
            const uint64_t ce_finish = ce_start + schedule.total_cycles;
            if (flags.combination == CombinationMode::COOPERATIVE) {
                cooperative_ready = ce_finish;
            }
            first_ce_start = std::min(first_ce_start, ce_start);
            last_ce_finish = std::max(last_ce_finish, ce_finish);
            fifo_release_cycle = std::max(fifo_release_cycle, ce_finish);
            releases.push({fifo_release_cycle, group_bytes});

            const auto& first_batch = batch_info[group.first];
            const auto& last_batch = batch_info[group.second - 1];
            requests.push_back({
                group.first,
                RequestClass::OUTPUT,
                group_vertices * output_stride,
                OutputAddress(layout.output.start, first_batch.dst_start, 0,
                              output_stride, architecture_.block_size),
                ce_finish,
                sequence++,
                ce_finish,
            });
            if (last_batch.dst_end - first_batch.dst_start !=
                static_cast<int>(group_vertices)) {
                throw std::runtime_error("pipeline group output is not contiguous");
            }
        }
        metrics.ae_finish_cycle = std::max(metrics.ae_finish_cycle, scheduled_ae_cursor);
        metrics.aggregation_cycles = metrics.ae_finish_cycle;
        metrics.ce_start_cycle = first_ce_start;
        metrics.ce_finish_cycle = last_ce_finish;
    }

    const auto memory_timing = simulate();
    record_memory_timing(memory_timing);

    metrics.aggregation_cycles = metrics.ae_finish_cycle;
    metrics.combination_cycles = metrics.ce_finish_cycle - metrics.ce_start_cycle;
    metrics.array_utilization = array_capacity_cycles == 0 ? 0.0 : Clamp(
        combination_operations / static_cast<double>(array_capacity_cycles), 0.0, 1.0);
    metrics.array_idle_lane_cycles = array_capacity_cycles > combination_operations
        ? array_capacity_cycles - combination_operations
        : 0;

    const uint64_t requested_bytes = std::accumulate(
        metrics.request_bytes.begin(), metrics.request_bytes.end(), uint64_t{0});
    if (requested_bytes != metrics.TotalDramBytes() ||
        metrics.aggregation_buffer_read_bytes != metrics.aggregation_buffer_write_bytes ||
        metrics.output_dram_bytes != static_cast<uint64_t>(graph.num_vertex) * output_stride) {
        throw std::runtime_error("layer traffic conservation invariant failed");
    }
    metrics.channel_imbalance = Imbalance(metrics.channel_blocks);
    metrics.bank_imbalance = Imbalance(metrics.bank_blocks);
    metrics.bandwidth_utilization = Clamp(
        metrics.TotalDramBytes() /
            (std::max<uint64_t>(1, metrics.memory_active_cycles) *
             architecture_.HbmBytesPerCycle()),
        0.0, 1.0);
    metrics.cycles = std::max(metrics.ce_finish_cycle, metrics.memory_service_cycles);

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

std::string ToString(MemoryPriorityMode mode) {
    switch (mode) {
        case MemoryPriorityMode::FIFO:
            return "fifo";
        case MemoryPriorityMode::BATCH_CLASS:
            return "batch-class";
    }
    throw std::runtime_error("unknown memory priority mode");
}

std::string ToString(AddressMappingMode mode) {
    switch (mode) {
        case AddressMappingMode::ROW_FIRST:
            return "row-first";
        case AddressMappingMode::LOW_BITS:
            return "low-bits";
    }
    throw std::runtime_error("unknown address mapping mode");
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

MemoryPriorityMode ParseMemoryPriorityMode(const std::string& value) {
    if (value == "fifo") {
        return MemoryPriorityMode::FIFO;
    }
    if (value == "batch-class" || value == "batch") {
        return MemoryPriorityMode::BATCH_CLASS;
    }
    throw std::runtime_error("invalid memory priority mode: " + value);
}

AddressMappingMode ParseAddressMappingMode(const std::string& value) {
    if (value == "row-first" || value == "row") {
        return AddressMappingMode::ROW_FIRST;
    }
    if (value == "low-bits" || value == "low") {
        return AddressMappingMode::LOW_BITS;
    }
    throw std::runtime_error("invalid address mapping mode: " + value);
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
        {"memory_priority", ToString(result.flags.memory_priority)},
        {"address_mapping", ToString(result.flags.address_mapping)},
        {"memory_coordination",
         result.flags.memory_priority == MemoryPriorityMode::BATCH_CLASS &&
             result.flags.address_mapping == AddressMappingMode::LOW_BITS},
        {"aggregation_only", result.flags.aggregation_only},
    };
    output["architecture"] = ArchitectureJson(result.architecture);
    output["summary"] = {
        {"total_cycles", result.TotalCycles()},
        {"total_aggregation_cycles", result.TotalAggregationCycles()},
        {"total_dram_bytes", result.TotalDramBytes()},
        {"total_aggregation_dram_bytes", result.TotalAggregationDramBytes()},
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
    stream << "model,dataset,profile,scope,pipeline,combination,sparsity,priority,mapping,layer,"
              "input_features,output_features,cycles,aggregation_cycles,combination_cycles,"
              "memory_service_cycles,memory_active_cycles,edge_dram_bytes,input_dram_bytes,weight_dram_bytes,"
              "output_dram_bytes,intermediate_dram_bytes,total_dram_bytes,mac_operations,"
              "add_operations,compare_operations,simd_utilization,array_utilization,"
              "bandwidth_utilization,queue_wait_cycles,hbm_blocked_cycles,channel_imbalance,"
              "bank_imbalance,ae_finish_cycle,ce_start_cycle,ce_finish_cycle,"
              "aggregation_buffer_peak_bytes,simd_idle_lane_cycles,array_idle_lane_cycles\n";
    for (const auto& layer : result.layers) {
        stream << result.model << ',' << result.dataset << ',' << result.profile << ','
               << (result.flags.aggregation_only ? "aggregation" : "full") << ','
               << ToString(result.flags.pipeline) << ',' << ToString(result.flags.combination) << ','
               << (result.flags.sparsity_elimination ? "on" : "off") << ','
               << ToString(result.flags.memory_priority) << ','
               << ToString(result.flags.address_mapping) << ','
               << layer.layer << ',' << layer.input_features << ',' << layer.output_features << ','
               << layer.cycles << ',' << layer.aggregation_cycles << ','
               << layer.combination_cycles << ',' << layer.memory_service_cycles << ','
               << layer.memory_active_cycles << ','
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
