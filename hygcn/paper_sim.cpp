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
constexpr std::size_t kCommandTraceChunkEvents = 4096;

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

void AppendVarint(std::string& output, uint64_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7fU);
        value >>= 7U;
        if (value != 0) {
            byte |= 0x80U;
        }
        output.push_back(static_cast<char>(byte));
    } while (value != 0);
}

std::string Base64Encode(const std::string& input) {
    constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < input.size(); offset += 3) {
        const uint32_t first = static_cast<unsigned char>(input[offset]);
        const uint32_t second = offset + 1 < input.size()
            ? static_cast<unsigned char>(input[offset + 1]) : 0;
        const uint32_t third = offset + 2 < input.size()
            ? static_cast<unsigned char>(input[offset + 2]) : 0;
        const uint32_t value = (first << 16U) | (second << 8U) | third;
        output.push_back(kAlphabet[(value >> 18U) & 0x3fU]);
        output.push_back(kAlphabet[(value >> 12U) & 0x3fU]);
        output.push_back(offset + 1 < input.size()
            ? kAlphabet[(value >> 6U) & 0x3fU] : '=');
        output.push_back(offset + 2 < input.size()
            ? kAlphabet[value & 0x3fU] : '=');
    }
    return output;
}

void IncrementHistogram(json& histogram, uint64_t value) {
    const std::string key = std::to_string(value);
    histogram[key] = histogram.value(key, uint64_t{0}) + 1;
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

std::string MemoryCommandName(MemoryCommandType command) {
    switch (command) {
        case MemoryCommandType::PRECHARGE:
            return "PRE";
        case MemoryCommandType::ACTIVATE:
            return "ACT";
        case MemoryCommandType::READ:
            return "READ";
        case MemoryCommandType::WRITE:
            return "WRITE";
    }
    throw std::runtime_error("unknown memory command");
}

constexpr std::size_t kCoordinatorPorts = 4;

std::size_t CoordinatorPort(RequestClass request_class) {
    switch (request_class) {
        case RequestClass::EDGE:
            return 0;
        case RequestClass::INPUT:
        case RequestClass::INTERMEDIATE_READ:
            return 1;
        case RequestClass::WEIGHT:
            return 2;
        case RequestClass::OUTPUT:
        case RequestClass::INTERMEDIATE_WRITE:
            return 3;
    }
    throw std::runtime_error("unknown request class");
}

bool IsWriteRequest(RequestClass request_class) {
    return request_class == RequestClass::OUTPUT ||
           request_class == RequestClass::INTERMEDIATE_WRITE;
}

uint64_t CoordinatorIssueBlocksPerCycle(const ArchitectureConfig& architecture) {
    return std::max<uint64_t>(
        1, static_cast<uint64_t>(std::floor(
               architecture.HbmBytesPerCycle() / architecture.block_size)));
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
        {"hbm_read_queue_entries_per_channel",
         architecture.hbm_read_queue_entries_per_channel},
        {"hbm_write_buffer_entries_per_channel",
         architecture.hbm_write_buffer_entries_per_channel},
        {"hbm_command_queue_entries_per_bank",
         architecture.hbm_command_queue_entries_per_bank},
        {"coordinator_issue_blocks_per_cycle",
         CoordinatorIssueBlocksPerCycle(architecture)},
        {"coordinator_fifo_active_windows",
         architecture.coordinator_fifo_active_windows},
        {"coordinator_fifo_window_blocks",
         architecture.hbm_row_bytes / architecture.block_size},
        {"hbm_read_row_hit_cycles", architecture.hbm_read_row_hit_cycles},
        {"hbm_read_row_miss_cycles", architecture.hbm_read_row_miss_cycles},
        {"hbm_read_row_conflict_cycles", architecture.hbm_read_row_conflict_cycles},
        {"hbm_write_row_hit_cycles", architecture.hbm_write_row_hit_cycles},
        {"hbm_write_row_miss_cycles", architecture.hbm_write_row_miss_cycles},
        {"hbm_write_row_conflict_cycles", architecture.hbm_write_row_conflict_cycles},
        {"hbm_activate_to_read_cycles", architecture.hbm_activate_to_read_cycles},
        {"hbm_activate_to_write_cycles", architecture.hbm_activate_to_write_cycles},
        {"hbm_read_to_read_cycles", architecture.hbm_read_to_read_cycles},
        {"hbm_write_to_write_cycles", architecture.hbm_write_to_write_cycles},
        {"hbm_read_to_write_cycles", architecture.hbm_read_to_write_cycles},
        {"hbm_write_to_read_cycles", architecture.hbm_write_to_read_cycles},
        {"hbm_read_to_precharge_cycles", architecture.hbm_read_to_precharge_cycles},
        {"hbm_write_to_precharge_cycles", architecture.hbm_write_to_precharge_cycles},
        {"hbm_activate_to_precharge_cycles",
         architecture.hbm_activate_to_precharge_cycles},
        {"hbm_precharge_to_activate_cycles",
         architecture.hbm_precharge_to_activate_cycles},
        {"hbm_activate_to_activate_cycles",
         architecture.hbm_activate_to_activate_cycles},
        {"hbm_command_issue_interval_cycles",
         architecture.hbm_command_issue_interval_cycles},
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
            {"row_buffer_hits", layer.row_buffer_hits_by_class[index]},
            {"row_buffer_misses", layer.row_buffer_misses_by_class[index]},
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
            {"base_producer_ready_cycle", trace.base_producer_ready_cycle},
            {"base_enqueue_cycle", trace.base_enqueue_cycle},
            {"producer_delay_cycles", trace.producer_delay_cycles},
            {"producer_ready_cycle", trace.producer_ready_cycle},
            {"enqueue_cycle", trace.enqueue_cycle},
            {"first_admission_cycle", trace.first_admission_cycle},
            {"last_admission_cycle", trace.last_admission_cycle},
            {"first_issue_cycle", trace.first_issue_cycle},
            {"completion_cycle", trace.completion_cycle},
            {"precharge_commands", trace.precharge_commands},
            {"activate_commands", trace.activate_commands},
            {"first_precharge_cycle", trace.first_precharge_cycle},
            {"last_precharge_cycle", trace.last_precharge_cycle},
            {"first_activate_cycle", trace.first_activate_cycle},
            {"last_activate_cycle", trace.last_activate_cycle},
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
            {"base_producer_ready_cycle", trace.base_producer_ready_cycle},
            {"base_enqueue_cycle", trace.base_enqueue_cycle},
            {"producer_delay_cycles", trace.producer_delay_cycles},
            {"producer_ready_cycle", trace.producer_ready_cycle},
            {"enqueue_cycle", trace.enqueue_cycle},
            {"first_admission_cycle", trace.first_admission_cycle},
            {"last_admission_cycle", trace.last_admission_cycle},
            {"first_issue_cycle", trace.first_issue_cycle},
            {"completion_cycle", trace.completion_cycle},
            {"precharge_commands", trace.precharge_commands},
            {"activate_commands", trace.activate_commands},
            {"first_precharge_cycle", trace.first_precharge_cycle},
            {"last_precharge_cycle", trace.last_precharge_cycle},
            {"first_activate_cycle", trace.first_activate_cycle},
            {"last_activate_cycle", trace.last_activate_cycle},
            {"producer_sequence", trace.producer_sequence.has_value()
                ? json(*trace.producer_sequence) : json(nullptr)},
        });
    }
    json command_samples = json::array();
    for (const auto& trace : layer.command_trace_samples) {
        command_samples.push_back({
            {"cycle", trace.cycle},
            {"sequence", trace.sequence},
            {"block_offset", trace.block_offset},
            {"channel", trace.channel},
            {"bank", trace.bank},
            {"row", trace.row},
            {"command", MemoryCommandName(trace.command)},
        });
    }
    json command_chunks = json::array();
    for (const auto& chunk : layer.command_trace_chunks) {
        command_chunks.push_back({
            {"event_count", chunk.event_count},
            {"payload_base64", Base64Encode(chunk.payload)},
        });
    }
    json command_trace = {
        {"representation", "command_delta_varint_base64_v2"},
        {"fields", {"cycle_delta", "sequence", "block_offset", "channel",
                    "bank", "row", "command"}},
        {"chunk_event_limit", kCommandTraceChunkEvents},
        {"trace_chunks", command_chunks},
        {"event_count", layer.command_trace_event_count},
        {"checksum_fnv1a64", Hex64(layer.command_trace_checksum)},
        {"command_lane_violations", layer.command_lane_violations},
        {"samples", command_samples},
    };
    json admission_edge_samples = json::array();
    json admission_histograms = {
        {"admitted_blocks", json::object()},
        {"admitted_read_blocks", json::object()},
        {"admitted_write_blocks", json::object()},
        {"total_read_occupancy_after", json::object()},
        {"total_write_occupancy_after", json::object()},
        {"max_channel_read_occupancy_after", json::object()},
        {"max_channel_write_occupancy_after", json::object()},
    };
    uint64_t admitted_blocks = 0;
    uint64_t admitted_read_blocks = 0;
    uint64_t admitted_write_blocks = 0;
    uint64_t inferred_dispatched_read_blocks = 0;
    uint64_t inferred_dispatched_write_blocks = 0;
    uint64_t total_read_occupancy_before = 0;
    uint64_t total_read_occupancy_after = 0;
    uint64_t total_write_occupancy_before = 0;
    uint64_t total_write_occupancy_after = 0;
    uint64_t max_blocks_admitted_per_cycle = 0;
    uint64_t max_total_read_occupancy = 0;
    uint64_t max_total_write_occupancy = 0;
    uint64_t max_channel_read_occupancy = 0;
    uint64_t max_channel_write_occupancy = 0;
    uint64_t admission_checksum = 1469598103934665603ULL;
    const std::size_t trace_channels =
        layer.max_channel_read_queue_occupancy.size();
    std::array<uint16_t, kMaxTraceHbmChannels> previous_read_occupancy{};
    std::array<uint16_t, kMaxTraceHbmChannels> previous_write_occupancy{};
    std::size_t admission_event_count = 0;
    for (std::size_t index = 0; index < layer.transaction_admission_traces.size(); ++index) {
        const auto& trace = layer.transaction_admission_traces[index];
        uint64_t trace_read_before = 0;
        uint64_t trace_read_after = 0;
        uint64_t trace_write_before = 0;
        uint64_t trace_write_after = 0;
        uint64_t trace_max_read_after = 0;
        uint64_t trace_max_write_after = 0;
        uint64_t trace_admitted_read = 0;
        uint64_t trace_admitted_write = 0;
        json read_before = json::array();
        json read_after = json::array();
        json write_before = json::array();
        json write_after = json::array();
        json block_identities = json::array();
        for (std::size_t channel = 0; channel < trace_channels; ++channel) {
            const uint64_t rb = trace.channel_read_occupancy_before[channel];
            const uint64_t ra = trace.channel_read_occupancy_after[channel];
            const uint64_t wb = trace.channel_write_occupancy_before[channel];
            const uint64_t wa = trace.channel_write_occupancy_after[channel];
            if (rb > previous_read_occupancy[channel] ||
                wb > previous_write_occupancy[channel] || ra < rb || wa < wb) {
                throw std::runtime_error("directional occupancy trace is inconsistent");
            }
            inferred_dispatched_read_blocks += previous_read_occupancy[channel] - rb;
            inferred_dispatched_write_blocks += previous_write_occupancy[channel] - wb;
            trace_admitted_read += ra - rb;
            trace_admitted_write += wa - wb;
            trace_read_before += rb;
            trace_read_after += ra;
            trace_write_before += wb;
            trace_write_after += wa;
            trace_max_read_after = std::max(trace_max_read_after, ra);
            trace_max_write_after = std::max(trace_max_write_after, wa);
            previous_read_occupancy[channel] = static_cast<uint16_t>(ra);
            previous_write_occupancy[channel] = static_cast<uint16_t>(wa);
            read_before.push_back(rb);
            read_after.push_back(ra);
            write_before.push_back(wb);
            write_after.push_back(wa);
        }
        if (trace_admitted_read != trace.admitted_read_blocks ||
            trace_admitted_write != trace.admitted_write_blocks ||
            trace.admitted_blocks !=
                trace.admitted_read_blocks + trace.admitted_write_blocks ||
            trace.admitted_blocks > trace.block_identities.size() ||
            (trace.terminal_snapshot && trace.admitted_blocks != 0)) {
            throw std::runtime_error("directional admission trace delta differs");
        }
        for (std::size_t identity_index = 0;
             identity_index < trace.admitted_blocks; ++identity_index) {
            const auto& identity = trace.block_identities[identity_index];
            block_identities.push_back({
                {"sequence", identity.sequence},
                {"block_offset", identity.block_offset},
            });
        }
        admission_event_count += trace.terminal_snapshot ? 0 : 1;
        IncrementHistogram(admission_histograms["admitted_blocks"],
                           trace.admitted_blocks);
        IncrementHistogram(admission_histograms["admitted_read_blocks"],
                           trace.admitted_read_blocks);
        IncrementHistogram(admission_histograms["admitted_write_blocks"],
                           trace.admitted_write_blocks);
        IncrementHistogram(admission_histograms["total_read_occupancy_after"],
                           trace_read_after);
        IncrementHistogram(admission_histograms["total_write_occupancy_after"],
                           trace_write_after);
        IncrementHistogram(admission_histograms["max_channel_read_occupancy_after"],
                           trace_max_read_after);
        IncrementHistogram(admission_histograms["max_channel_write_occupancy_after"],
                           trace_max_write_after);
        admitted_blocks += trace.admitted_blocks;
        admitted_read_blocks += trace.admitted_read_blocks;
        admitted_write_blocks += trace.admitted_write_blocks;
        total_read_occupancy_before += trace_read_before;
        total_read_occupancy_after += trace_read_after;
        total_write_occupancy_before += trace_write_before;
        total_write_occupancy_after += trace_write_after;
        max_blocks_admitted_per_cycle = std::max(
            max_blocks_admitted_per_cycle, trace.admitted_blocks);
        max_total_read_occupancy = std::max(
            max_total_read_occupancy, trace_read_after);
        max_total_write_occupancy = std::max(
            max_total_write_occupancy, trace_write_after);
        max_channel_read_occupancy = std::max(
            max_channel_read_occupancy, trace_max_read_after);
        max_channel_write_occupancy = std::max(
            max_channel_write_occupancy, trace_max_write_after);
        for (const uint64_t value : {trace.cycle,
                 static_cast<uint64_t>(trace.terminal_snapshot),
                 trace.admitted_blocks, trace.admitted_read_blocks,
                 trace.admitted_write_blocks}) {
            admission_checksum ^= value;
            admission_checksum *= 1099511628211ULL;
        }
        for (std::size_t identity_index = 0;
             identity_index < trace.admitted_blocks; ++identity_index) {
            const auto& identity = trace.block_identities[identity_index];
            for (const uint64_t value : {
                     identity.sequence, identity.block_offset}) {
                admission_checksum ^= value;
                admission_checksum *= 1099511628211ULL;
            }
        }
        for (std::size_t channel = 0; channel < trace_channels; ++channel) {
            for (const uint64_t value : {
                     static_cast<uint64_t>(trace.channel_read_occupancy_before[channel]),
                     static_cast<uint64_t>(trace.channel_read_occupancy_after[channel]),
                     static_cast<uint64_t>(trace.channel_write_occupancy_before[channel]),
                     static_cast<uint64_t>(trace.channel_write_occupancy_after[channel])}) {
                admission_checksum ^= value;
                admission_checksum *= 1099511628211ULL;
            }
        }
        if (index < 16 || index + 16 >= layer.transaction_admission_traces.size()) {
            admission_edge_samples.push_back({
                {"cycle", trace.cycle},
                {"terminal_snapshot", trace.terminal_snapshot},
                {"admitted_blocks", trace.admitted_blocks},
                {"admitted_read_blocks", trace.admitted_read_blocks},
                {"admitted_write_blocks", trace.admitted_write_blocks},
                {"admitted_block_identities", block_identities},
                {"channel_read_occupancy_before", read_before},
                {"channel_read_occupancy_after", read_after},
                {"channel_write_occupancy_before", write_before},
                {"channel_write_occupancy_after", write_after},
            });
        }
    }
    constexpr std::size_t kAdmissionChunkEvents = 4096;
    json admission_chunks = json::array();
    for (std::size_t offset = 0;
         offset < layer.transaction_admission_traces.size();
         offset += kAdmissionChunkEvents) {
        const std::size_t end = std::min(
            offset + kAdmissionChunkEvents,
            layer.transaction_admission_traces.size());
        std::string payload;
        payload.reserve((end - offset) * (8 + 4 * trace_channels));
        uint64_t previous_cycle = 0;
        for (std::size_t index = offset; index < end; ++index) {
            const auto& trace = layer.transaction_admission_traces[index];
            AppendVarint(payload, index == offset
                ? trace.cycle : trace.cycle - previous_cycle);
            previous_cycle = trace.cycle;
            for (const uint64_t value : {
                     static_cast<uint64_t>(trace.terminal_snapshot),
                     trace.admitted_blocks, trace.admitted_read_blocks,
                     trace.admitted_write_blocks}) {
                AppendVarint(payload, value);
            }
            AppendVarint(payload, trace.admitted_blocks);
            for (std::size_t identity_index = 0;
                 identity_index < trace.admitted_blocks; ++identity_index) {
                const auto& identity = trace.block_identities[identity_index];
                AppendVarint(payload, identity.sequence);
                AppendVarint(payload, identity.block_offset);
            }
            for (const auto* values : {
                     &trace.channel_read_occupancy_before,
                     &trace.channel_read_occupancy_after,
                     &trace.channel_write_occupancy_before,
                     &trace.channel_write_occupancy_after}) {
                for (std::size_t channel = 0; channel < trace_channels; ++channel) {
                    AppendVarint(payload, (*values)[channel]);
                }
            }
        }
        admission_chunks.push_back({
            {"event_count", end - offset},
            {"payload_base64", Base64Encode(payload)},
        });
    }
    json admission_summary = {
        {"representation", "directional_occupancy_identity_delta_varint_base64_v3"},
        {"fields", {"cycle_delta", "terminal_snapshot", "admitted_blocks",
                    "admitted_read_blocks", "admitted_write_blocks",
                    "identity_count",
                    "admitted_block_identities[sequence,block_offset]",
                    "channel_read_occupancy_before[]",
                    "channel_read_occupancy_after[]",
                    "channel_write_occupancy_before[]",
                    "channel_write_occupancy_after[]"}},
        {"channel_count", trace_channels},
        {"chunk_event_limit", kAdmissionChunkEvents},
        {"trace_chunks", admission_chunks},
        {"event_count", layer.transaction_admission_traces.size()},
        {"admission_event_count", admission_event_count},
        {"first_cycle", layer.transaction_admission_traces.empty()
            ? 0 : layer.transaction_admission_traces.front().cycle},
        {"last_cycle", layer.transaction_admission_traces.empty()
            ? 0 : layer.transaction_admission_traces.back().cycle},
        {"weighted_totals", {
            {"admitted_blocks", admitted_blocks},
            {"admitted_read_blocks", admitted_read_blocks},
            {"admitted_write_blocks", admitted_write_blocks},
            {"inferred_dispatched_read_blocks", inferred_dispatched_read_blocks},
            {"inferred_dispatched_write_blocks", inferred_dispatched_write_blocks},
            {"total_read_occupancy_before", total_read_occupancy_before},
            {"total_read_occupancy_after", total_read_occupancy_after},
            {"total_write_occupancy_before", total_write_occupancy_before},
            {"total_write_occupancy_after", total_write_occupancy_after},
        }},
        {"histograms", admission_histograms},
        {"actual_max", {
            {"blocks_admitted_per_cycle", max_blocks_admitted_per_cycle},
            {"total_read_occupancy", max_total_read_occupancy},
            {"total_write_occupancy", max_total_write_occupancy},
            {"channel_read_occupancy", max_channel_read_occupancy},
            {"channel_write_occupancy", max_channel_write_occupancy},
        }},
        {"max_channel_read_queue_occupancy",
         layer.max_channel_read_queue_occupancy},
        {"max_channel_write_buffer_occupancy",
         layer.max_channel_write_buffer_occupancy},
        {"trace_checksum_fnv1a64", Hex64(admission_checksum)},
        {"edge_samples", admission_edge_samples},
    };
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
        {"bandwidth_utilization", layer.bandwidth_utilization},
        {"active_bandwidth_utilization", layer.active_bandwidth_utilization},
        {"queue_wait_cycles", layer.queue_wait_cycles},
        {"hbm_blocked_cycles", layer.hbm_blocked_cycles},
        {"row_buffer_hits", layer.row_buffer_hits},
        {"row_buffer_misses", layer.row_buffer_misses},
        {"priority_reorders", layer.priority_reorders},
        {"read_to_write_switches", layer.read_to_write_switches},
        {"write_to_read_switches", layer.write_to_read_switches},
        {"direction_switch_stall_cycles", layer.direction_switch_stall_cycles},
        {"precharge_commands", layer.precharge_commands},
        {"activate_commands", layer.activate_commands},
        {"read_commands", layer.read_commands},
        {"write_commands", layer.write_commands},
        {"command_trace", command_trace},
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
        {"transaction_admission_trace", admission_summary},
        {"channel_blocks", layer.channel_blocks},
        {"bank_blocks", layer.bank_blocks},
        {"simd_utilization", layer.simd_utilization},
        {"array_utilization", layer.array_utilization},
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
    config.hbm_read_queue_entries_per_channel = reader.GetInteger(
        "memory", "hbm_read_queue_entries_per_channel", -1);
    config.hbm_write_buffer_entries_per_channel = reader.GetInteger(
        "memory", "hbm_write_buffer_entries_per_channel", -1);
    config.hbm_command_queue_entries_per_bank = reader.GetInteger(
        "memory", "hbm_command_queue_entries_per_bank", -1);
    config.hbm_read_row_hit_cycles = reader.GetInteger(
        "memory", "hbm_read_row_hit_cycles", -1);
    config.hbm_read_row_miss_cycles = reader.GetInteger(
        "memory", "hbm_read_row_miss_cycles", -1);
    config.hbm_read_row_conflict_cycles = reader.GetInteger(
        "memory", "hbm_read_row_conflict_cycles", -1);
    config.hbm_write_row_hit_cycles = reader.GetInteger(
        "memory", "hbm_write_row_hit_cycles", -1);
    config.hbm_write_row_miss_cycles = reader.GetInteger(
        "memory", "hbm_write_row_miss_cycles", -1);
    config.hbm_write_row_conflict_cycles = reader.GetInteger(
        "memory", "hbm_write_row_conflict_cycles", -1);
    config.hbm_activate_to_read_cycles = reader.GetInteger(
        "memory", "hbm_activate_to_read_cycles", -1);
    config.hbm_activate_to_write_cycles = reader.GetInteger(
        "memory", "hbm_activate_to_write_cycles", -1);
    config.hbm_read_to_read_cycles = reader.GetInteger(
        "memory", "hbm_read_to_read_cycles", -1);
    config.hbm_write_to_write_cycles = reader.GetInteger(
        "memory", "hbm_write_to_write_cycles", -1);
    config.hbm_read_to_write_cycles = reader.GetInteger(
        "memory", "hbm_read_to_write_cycles", -1);
    config.hbm_write_to_read_cycles = reader.GetInteger(
        "memory", "hbm_write_to_read_cycles", -1);
    config.hbm_read_to_precharge_cycles = reader.GetInteger(
        "memory", "hbm_read_to_precharge_cycles", -1);
    config.hbm_write_to_precharge_cycles = reader.GetInteger(
        "memory", "hbm_write_to_precharge_cycles", -1);
    config.hbm_activate_to_precharge_cycles = reader.GetInteger(
        "memory", "hbm_activate_to_precharge_cycles", -1);
    config.hbm_precharge_to_activate_cycles = reader.GetInteger(
        "memory", "hbm_precharge_to_activate_cycles", -1);
    config.hbm_activate_to_activate_cycles = reader.GetInteger(
        "memory", "hbm_activate_to_activate_cycles", -1);
    config.hbm_command_issue_interval_cycles = reader.GetInteger(
        "memory", "hbm_command_issue_interval_cycles", -1);
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
    config.coordinator_fifo_active_windows = reader.GetInteger(
        "model", "coordinator_fifo_active_windows", -1);
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
    require_positive(hbm_read_queue_entries_per_channel > 0,
                     "hbm_read_queue_entries_per_channel");
    require_positive(hbm_write_buffer_entries_per_channel > 0,
                     "hbm_write_buffer_entries_per_channel");
    require_positive(hbm_command_queue_entries_per_bank > 0,
                     "hbm_command_queue_entries_per_bank");
    require_positive(hbm_read_row_hit_cycles > 0, "hbm_read_row_hit_cycles");
    require_positive(hbm_read_row_miss_cycles > hbm_read_row_hit_cycles,
                     "hbm_read_row_miss_cycles");
    require_positive(hbm_read_row_conflict_cycles > hbm_read_row_miss_cycles,
                     "hbm_read_row_conflict_cycles");
    require_positive(hbm_write_row_hit_cycles > 0, "hbm_write_row_hit_cycles");
    require_positive(hbm_write_row_miss_cycles > hbm_write_row_hit_cycles,
                     "hbm_write_row_miss_cycles");
    require_positive(hbm_write_row_conflict_cycles > hbm_write_row_miss_cycles,
                     "hbm_write_row_conflict_cycles");
    require_positive(hbm_activate_to_read_cycles > 0,
                     "hbm_activate_to_read_cycles");
    require_positive(hbm_activate_to_write_cycles > 0,
                     "hbm_activate_to_write_cycles");
    require_positive(hbm_read_to_read_cycles > 0, "hbm_read_to_read_cycles");
    require_positive(hbm_write_to_write_cycles > 0, "hbm_write_to_write_cycles");
    require_positive(hbm_read_to_write_cycles > 0, "hbm_read_to_write_cycles");
    require_positive(hbm_write_to_read_cycles > 0, "hbm_write_to_read_cycles");
    require_positive(hbm_read_to_precharge_cycles > 0,
                     "hbm_read_to_precharge_cycles");
    require_positive(hbm_write_to_precharge_cycles > 0,
                     "hbm_write_to_precharge_cycles");
    require_positive(hbm_activate_to_precharge_cycles > 0,
                     "hbm_activate_to_precharge_cycles");
    require_positive(hbm_precharge_to_activate_cycles > 0,
                     "hbm_precharge_to_activate_cycles");
    require_positive(hbm_activate_to_activate_cycles >=
                         hbm_activate_to_precharge_cycles +
                         hbm_precharge_to_activate_cycles,
                     "hbm_activate_to_activate_cycles");
    require_positive(hbm_command_issue_interval_cycles > 0,
                     "hbm_command_issue_interval_cycles");
    require_positive(hbm_read_row_miss_cycles ==
                         hbm_activate_to_read_cycles + hbm_read_row_hit_cycles,
                     "hbm_read_row_miss_cycles command decomposition");
    require_positive(hbm_read_row_conflict_cycles ==
                         hbm_precharge_to_activate_cycles +
                         hbm_activate_to_read_cycles + hbm_read_row_hit_cycles,
                     "hbm_read_row_conflict_cycles command decomposition");
    require_positive(hbm_write_row_miss_cycles ==
                         hbm_activate_to_write_cycles + hbm_write_row_hit_cycles,
                     "hbm_write_row_miss_cycles command decomposition");
    require_positive(hbm_write_row_conflict_cycles ==
                         hbm_precharge_to_activate_cycles +
                         hbm_activate_to_write_cycles + hbm_write_row_hit_cycles,
                     "hbm_write_row_conflict_cycles command decomposition");
    require_positive(hbm_channels <= static_cast<int>(kMaxTraceHbmChannels),
                     "hbm_channels trace capacity");
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
    require_positive(coordinator_fifo_active_windows >= 0,
                     "coordinator_fifo_active_windows");
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
    result.max_channel_read_queue_occupancy.assign(architecture.hbm_channels, 0);
    result.max_channel_write_buffer_occupancy.assign(architecture.hbm_channels, 0);
    if (requests.empty()) {
        return result;
    }

    struct BankState {
        uint64_t next_activate_cycle = 0;
        uint64_t next_precharge_cycle = 0;
        uint64_t next_read_cycle = 0;
        uint64_t next_write_cycle = 0;
        uint64_t open_row = 0;
        bool row_open = false;
    };

    struct ChannelState {
        uint64_t next_read_cycle = 0;
        uint64_t next_write_cycle = 0;
        uint64_t next_command_cycle = 0;
        uint64_t last_issue_cycle = 0;
        int last_direction = -1;
    };

    struct RequestState {
        MemoryRequest request;
        uint64_t first_block = 0;
        uint64_t block_count = 0;
        uint64_t admitted_blocks = 0;
        uint64_t issued_blocks = 0;
        uint64_t first_issue = std::numeric_limits<uint64_t>::max();
        uint64_t first_admission = std::numeric_limits<uint64_t>::max();
        uint64_t last_admission = 0;
        uint64_t completion = 0;
        uint64_t effective_producer_ready = 0;
        uint64_t effective_enqueue = 0;
        uint64_t precharge_commands = 0;
        uint64_t activate_commands = 0;
        uint64_t first_precharge = std::numeric_limits<uint64_t>::max();
        uint64_t last_precharge = 0;
        uint64_t first_activate = std::numeric_limits<uint64_t>::max();
        uint64_t last_activate = 0;
    };

    struct Candidate {
        std::size_t request_index = 0;
        uint64_t start_cycle = 0;
        uint64_t producer_ready_cycle = 0;
        uint64_t enqueue_cycle = 0;
        uint64_t admission_cycle = std::numeric_limits<uint64_t>::max();
        uint64_t controller_dispatch_cycle = std::numeric_limits<uint64_t>::max();
        uint64_t admission_order = std::numeric_limits<uint64_t>::max();
        uint64_t block = 0;
        uint64_t block_offset = 0;
        uint64_t command_ready_cycle = 0;
        uint64_t precharge_cycle = std::numeric_limits<uint64_t>::max();
        uint64_t activate_cycle = std::numeric_limits<uint64_t>::max();
        std::size_t channel = 0;
        std::size_t bank = 0;
        uint64_t row = 0;
        bool is_write = false;
        bool row_hit = false;
    };

    const uint64_t blocks_per_row = architecture.hbm_row_bytes / architecture.block_size;
    const uint64_t bytes_per_channel_cycle =
        std::max<double>(1.0, architecture.HbmBytesPerCycle() / architecture.hbm_channels);
    const uint64_t transfer_cycles = std::max<uint64_t>(
        1, static_cast<uint64_t>(std::ceil(architecture.block_size / bytes_per_channel_cycle)));
    const std::size_t coordinator_issue_blocks = static_cast<std::size_t>(
        CoordinatorIssueBlocksPerCycle(architecture));
    std::vector<ChannelState> channels(architecture.hbm_channels);
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
        if (lhs.admission_order != rhs.admission_order) {
            return lhs.admission_order < rhs.admission_order;
        }
        if (lhs_request.sequence != rhs_request.sequence) {
            return lhs_request.sequence < rhs_request.sequence;
        }
        return lhs.block_offset < rhs.block_offset;
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
        if (lhs.row != rhs.row) {
            return lhs.row < rhs.row;
        }
        if (lhs.block != rhs.block) {
            return lhs.block < rhs.block;
        }
        if (lhs_request.sequence != rhs_request.sequence) {
            return lhs_request.sequence < rhs_request.sequence;
        }
        return lhs.block_offset < rhs.block_offset;
    };

    std::vector<std::vector<std::size_t>> dependents(states.size());
    for (std::size_t index = 0; index < states.size(); ++index) {
        const auto& producer_sequence = states[index].request.producer_sequence;
        if (producer_sequence.has_value()) {
            dependents[sequence_to_index.at(*producer_sequence)].push_back(index);
        }
    }

    auto make_candidate = [&](std::size_t index, uint64_t block_offset) {
        const auto& state = states[index];
        if (block_offset >= state.block_count) {
            throw std::runtime_error("completed request cannot be scheduled");
        }
        uint64_t producer_ready = state.request.producer_ready_cycle;
        if (state.request.producer_sequence.has_value()) {
            const auto& producer = states[sequence_to_index.at(
                *state.request.producer_sequence)];
            if (producer.issued_blocks < producer.block_count) {
                throw std::runtime_error("dependent request released before producer completion");
            }
            producer_ready = std::max(
                producer_ready,
                producer.completion + state.request.producer_delay_cycles);
        }
        const uint64_t enqueue = std::max(state.request.enqueue_cycle, producer_ready);
        auto candidate = map_block(state.first_block + block_offset);
        candidate.request_index = index;
        candidate.block_offset = block_offset;
        candidate.is_write = IsWriteRequest(state.request.request_class);
        candidate.producer_ready_cycle = producer_ready;
        candidate.enqueue_cycle = enqueue;
        candidate.start_cycle = enqueue;
        return candidate;
    };
    struct BankQueue {
        enum class ActiveStage {
            NONE,
            ACTIVATE,
            DATA,
        };

        using Comparator = std::function<bool(const Candidate&, const Candidate&)>;
        using Queue = std::priority_queue<Candidate, std::vector<Candidate>, Comparator>;
        using Set = std::set<Candidate, Comparator>;

        BankQueue(const Comparator& future_after, const Comparator& available_less)
            : future_reads(future_after), future_writes(future_after),
              available(available_less),
              available_less(available_less) {}

        Queue& Future(bool is_write) {
            return is_write ? future_writes : future_reads;
        }

        Queue future_reads;
        Queue future_writes;
        Set available;
        std::map<uint64_t, Set> available_rows;
        Comparator available_less;
        std::optional<Candidate> active;
        ActiveStage active_stage = ActiveStage::NONE;
    };
    struct CommandChoice {
        Candidate candidate;
        std::size_t flat_bank = 0;
        int source = 0;
        MemoryCommandType command = MemoryCommandType::READ;
        uint64_t issue_cycle = 0;
        bool valid = false;
    };

    auto future_after = [&](const Candidate& lhs, const Candidate& rhs) {
        if (lhs.controller_dispatch_cycle != rhs.controller_dispatch_cycle) {
            return lhs.controller_dispatch_cycle > rhs.controller_dispatch_cycle;
        }
        return priority_less(rhs, lhs);
    };
    auto available_less = [&](const Candidate& lhs, const Candidate& rhs) {
        return priority_less(lhs, rhs);
    };
    auto fifo_after = [&](const Candidate& lhs, const Candidate& rhs) {
        return fifo_less(rhs, lhs);
    };
    auto pending_future_after = [&](const Candidate& lhs, const Candidate& rhs) {
        if (lhs.enqueue_cycle != rhs.enqueue_cycle) {
            return lhs.enqueue_cycle > rhs.enqueue_cycle;
        }
        const auto& lhs_request = states[lhs.request_index].request;
        const auto& rhs_request = states[rhs.request_index].request;
        if (lhs_request.sequence != rhs_request.sequence) {
            return lhs_request.sequence > rhs_request.sequence;
        }
        return lhs.block_offset > rhs.block_offset;
    };
    auto pending_priority_less = [&](const Candidate& lhs, const Candidate& rhs) {
        const auto& lhs_request = states[lhs.request_index].request;
        const auto& rhs_request = states[rhs.request_index].request;
        if (lhs_request.batch_id != rhs_request.batch_id) {
            return lhs_request.batch_id < rhs_request.batch_id;
        }
        if (lhs_request.request_class != rhs_request.request_class) {
            return static_cast<int>(lhs_request.request_class) <
                   static_cast<int>(rhs_request.request_class);
        }
        if (lhs.row != rhs.row) {
            return lhs.row < rhs.row;
        }
        if (lhs.block != rhs.block) {
            return lhs.block < rhs.block;
        }
        if (lhs_request.sequence != rhs_request.sequence) {
            return lhs_request.sequence < rhs_request.sequence;
        }
        return lhs.block_offset < rhs.block_offset;
    };
    auto pending_fifo_less = [&](const Candidate& lhs, const Candidate& rhs) {
        const auto& lhs_request = states[lhs.request_index].request;
        const auto& rhs_request = states[rhs.request_index].request;
        if (lhs_request.sequence != rhs_request.sequence) {
            return lhs_request.sequence < rhs_request.sequence;
        }
        return lhs.block_offset < rhs.block_offset;
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
    using PendingFuture = std::priority_queue<
        Candidate, std::vector<Candidate>, decltype(pending_future_after)>;
    using PendingSet = std::set<Candidate, std::function<bool(
        const Candidate&, const Candidate&)>>;
    PendingFuture pending_future(pending_future_after);
    PendingSet pending_priority(pending_priority_less);
    std::vector<PendingSet> pending_fifo_ports;
    pending_fifo_ports.reserve(kCoordinatorPorts);
    for (std::size_t port = 0; port < kCoordinatorPorts; ++port) {
        pending_fifo_ports.emplace_back(pending_fifo_less);
    }
    std::set<std::pair<std::size_t, uint64_t>> issued_candidates;
    std::vector<std::size_t> outstanding_read_blocks(architecture.hbm_channels, 0);
    std::vector<std::size_t> outstanding_write_blocks(architecture.hbm_channels, 0);
    std::vector<std::deque<Candidate>> controller_read_queues(architecture.hbm_channels);
    std::vector<std::deque<Candidate>> controller_write_queues(architecture.hbm_channels);
    std::vector<std::size_t> write_drain_remaining(architecture.hbm_channels, 0);
    std::vector<uint64_t> next_controller_dispatch_cycle(architecture.hbm_channels, 0);
    std::vector<std::size_t> command_queue_occupancy(total_banks, 0);
    std::vector<std::pair<uint64_t, uint64_t>> block_active_intervals;
    block_active_intervals.reserve(static_cast<std::size_t>(remaining_blocks));
    std::vector<bool> saw_channel_command(architecture.hbm_channels, false);
    std::vector<uint64_t> last_channel_command_cycle(architecture.hbm_channels, 0);
    bool saw_command = false;
    uint64_t last_command_cycle = 0;
    std::vector<MemoryCommandTrace> first_command_samples;
    std::deque<MemoryCommandTrace> last_command_samples;
    constexpr std::size_t kCommandEdgeSamples = 32;
    uint64_t command_chunk_previous_cycle = 0;
    result.command_trace_checksum = 1469598103934665603ULL;
    auto record_command = [&](const Candidate& candidate,
                              MemoryCommandType command,
                              uint64_t cycle,
                              uint64_t row) {
        if (saw_command && cycle < last_command_cycle) {
            throw std::runtime_error("memory command trace is not time ordered");
        }
        saw_command = true;
        last_command_cycle = cycle;
        if (saw_channel_command[candidate.channel] &&
            cycle < last_channel_command_cycle[candidate.channel] +
                static_cast<uint64_t>(
                    architecture.hbm_command_issue_interval_cycles)) {
            ++result.command_lane_violations;
            throw std::runtime_error("channel command lane overlap");
        }
        saw_channel_command[candidate.channel] = true;
        last_channel_command_cycle[candidate.channel] = cycle;
        const MemoryCommandTrace trace{
            cycle,
            states[candidate.request_index].request.sequence,
            candidate.block_offset,
            candidate.channel,
            candidate.bank,
            row,
            command,
        };
        for (const uint64_t value : {
                 trace.cycle, trace.sequence, trace.block_offset,
                 static_cast<uint64_t>(trace.channel),
                 static_cast<uint64_t>(trace.bank),
                 trace.row,
                 static_cast<uint64_t>(trace.command)}) {
            result.command_trace_checksum ^= value;
            result.command_trace_checksum *= 1099511628211ULL;
        }
        ++result.command_trace_event_count;
        if (result.command_trace_chunks.empty() ||
            result.command_trace_chunks.back().event_count >=
                kCommandTraceChunkEvents) {
            result.command_trace_chunks.push_back({});
            command_chunk_previous_cycle = 0;
        }
        auto& chunk = result.command_trace_chunks.back();
        AppendVarint(chunk.payload, chunk.event_count == 0
            ? trace.cycle : trace.cycle - command_chunk_previous_cycle);
        command_chunk_previous_cycle = trace.cycle;
        for (const uint64_t value : {
                 trace.sequence, trace.block_offset,
                 static_cast<uint64_t>(trace.channel),
                 static_cast<uint64_t>(trace.bank), trace.row,
                 static_cast<uint64_t>(trace.command)}) {
            AppendVarint(chunk.payload, value);
        }
        ++chunk.event_count;
        if (first_command_samples.size() < kCommandEdgeSamples) {
            first_command_samples.push_back(trace);
        } else {
            if (last_command_samples.size() == kCommandEdgeSamples) {
                last_command_samples.pop_front();
            }
            last_command_samples.push_back(trace);
        }
    };
    std::size_t fifo_next_port = 0;
    std::map<std::pair<std::size_t, uint64_t>, std::size_t> fifo_active_windows;
    uint64_t next_admission_order = 0;
    uint64_t simulation_cycle = 0;
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
    auto enqueue_bank_candidate = [&](Candidate candidate) {
        const std::size_t flat_bank =
            candidate.channel * architecture.hbm_banks_per_channel + candidate.bank;
        push_available(bank_queues[flat_bank], candidate);
        active_fifo.push(candidate);
    };
    auto peek_bank = [&](std::size_t flat_bank) {
        auto& queue = bank_queues[flat_bank];
        CommandChoice choice;
        choice.flat_bank = flat_bank;
        if (queue.active.has_value()) {
            choice.candidate = *queue.active;
            choice.command = queue.active_stage == BankQueue::ActiveStage::ACTIVATE
                ? MemoryCommandType::ACTIVATE
                : (choice.candidate.is_write
                    ? MemoryCommandType::WRITE : MemoryCommandType::READ);
            choice.valid = true;
        } else if (!queue.available.empty()) {
            choice.candidate = *queue.available.begin();
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
                            choice.source = 2;
                        }
                    }
                }
            }
            const auto& bank_state = banks[flat_bank];
            const bool row_hit = bank_state.row_open &&
                bank_state.open_row == choice.candidate.row;
            choice.candidate.row_hit = row_hit;
            choice.command = row_hit
                ? (choice.candidate.is_write
                    ? MemoryCommandType::WRITE : MemoryCommandType::READ)
                : (bank_state.row_open
                    ? MemoryCommandType::PRECHARGE
                    : MemoryCommandType::ACTIVATE);
        }
        if (!choice.valid) {
            return choice;
        }
        const auto& bank_state = banks[flat_bank];
        const auto& channel_state = channels[choice.candidate.channel];
        const uint64_t dispatch_cycle = choice.candidate.controller_dispatch_cycle;
        switch (choice.command) {
            case MemoryCommandType::PRECHARGE:
                choice.issue_cycle = std::max(
                    {dispatch_cycle, bank_state.next_precharge_cycle,
                     channel_state.next_command_cycle});
                break;
            case MemoryCommandType::ACTIVATE:
                choice.issue_cycle = std::max(
                    {dispatch_cycle, bank_state.next_activate_cycle,
                     channel_state.next_command_cycle});
                break;
            case MemoryCommandType::READ:
                choice.candidate.command_ready_cycle = std::max(
                    {dispatch_cycle, bank_state.next_read_cycle,
                     channel_state.next_command_cycle});
                choice.issue_cycle = std::max(
                    choice.candidate.command_ready_cycle,
                    channel_state.next_read_cycle);
                break;
            case MemoryCommandType::WRITE:
                choice.candidate.command_ready_cycle = std::max(
                    {dispatch_cycle, bank_state.next_write_cycle,
                     channel_state.next_command_cycle});
                choice.issue_cycle = std::max(
                    choice.candidate.command_ready_cycle,
                    channel_state.next_write_cycle);
                break;
        }
        choice.candidate.start_cycle = choice.issue_cycle;
        return choice;
    };
    auto choice_less = [&](const CommandChoice& lhs, const CommandChoice& rhs) {
        if (!lhs.valid) {
            return false;
        }
        if (!rhs.valid) {
            return true;
        }
        if (lhs.issue_cycle != rhs.issue_cycle) {
            return lhs.issue_cycle < rhs.issue_cycle;
        }
        return priority_less(lhs.candidate, rhs.candidate);
    };

    std::vector<CommandChoice> channel_choices(architecture.hbm_channels);
    auto recompute_channel = [&](std::size_t channel) {
        CommandChoice best;
        const std::size_t first_bank =
            channel * architecture.hbm_banks_per_channel;
        for (std::size_t bank = 0;
             bank < static_cast<std::size_t>(architecture.hbm_banks_per_channel);
            ++bank) {
            const CommandChoice candidate = peek_bank(first_bank + bank);
            if (choice_less(candidate, best)) {
                best = candidate;
            }
        }
        channel_choices[channel] = best;
    };

    auto push_pending = [&](const Candidate& candidate, uint64_t visible_cycle) {
        if (candidate.enqueue_cycle <= visible_cycle) {
            if (priority == MemoryPriorityMode::BATCH_CLASS) {
                pending_priority.insert(candidate);
            } else {
                pending_fifo_ports[CoordinatorPort(
                    states[candidate.request_index].request.request_class)].insert(candidate);
            }
        } else {
            pending_future.push(candidate);
        }
    };
    auto promote_pending = [&](uint64_t visible_cycle) {
        while (!pending_future.empty() &&
               pending_future.top().enqueue_cycle <= visible_cycle) {
            const Candidate candidate = pending_future.top();
            pending_future.pop();
            if (priority == MemoryPriorityMode::BATCH_CLASS) {
                pending_priority.insert(candidate);
            } else {
                pending_fifo_ports[CoordinatorPort(
                    states[candidate.request_index].request.request_class)].insert(candidate);
            }
        }
    };
    auto pending_available_count = [&]() {
        std::size_t count = pending_priority.size();
        for (const auto& port : pending_fifo_ports) {
            count += port.size();
        }
        return count;
    };
    for (std::size_t index = 0; index < states.size(); ++index) {
        if (!states[index].request.producer_sequence.has_value()) {
            push_pending(make_candidate(index, 0), 0);
        }
    }
    const std::size_t read_queue_capacity = static_cast<std::size_t>(
        architecture.hbm_read_queue_entries_per_channel);
    const std::size_t write_buffer_capacity = static_cast<std::size_t>(
        architecture.hbm_write_buffer_entries_per_channel);
    const std::size_t command_queue_capacity = static_cast<std::size_t>(
        architecture.hbm_command_queue_entries_per_bank);
    struct PendingChoice {
        Candidate candidate;
        std::size_t port = 0;
        PendingSet::iterator iterator;
        bool valid = false;
    };
    auto transaction_queue_has_capacity = [&](const Candidate& candidate) {
        return candidate.is_write
            ? outstanding_write_blocks[candidate.channel] < write_buffer_capacity
            : outstanding_read_blocks[candidate.channel] < read_queue_capacity;
    };
    auto select_pending = [&](std::optional<std::size_t> fixed_fifo_port) {
        PendingChoice choice;
        if (priority == MemoryPriorityMode::BATCH_CLASS) {
            if (pending_priority.empty()) {
                return choice;
            }
            const auto& head = *pending_priority.begin();
            const auto& head_request = states[head.request_index].request;
            for (auto iterator = pending_priority.begin();
                 iterator != pending_priority.end(); ++iterator) {
                const auto& request = states[iterator->request_index].request;
                if (request.batch_id != head_request.batch_id ||
                    request.request_class != head_request.request_class ||
                    iterator->row != head.row) {
                    break;
                }
                if (transaction_queue_has_capacity(*iterator)) {
                    choice.candidate = *iterator;
                    choice.iterator = iterator;
                    choice.valid = true;
                    return choice;
                }
            }
            return choice;
        }
        const std::size_t port_count = fixed_fifo_port.has_value()
            ? 1 : kCoordinatorPorts;
        for (std::size_t distance = 0; distance < port_count; ++distance) {
            const std::size_t port = fixed_fifo_port.value_or(
                (fifo_next_port + distance) % kCoordinatorPorts);
            if (pending_fifo_ports[port].empty()) {
                continue;
            }
            const auto iterator = pending_fifo_ports[port].begin();
            const uint64_t logical_window = iterator->block / blocks_per_row;
            const auto window_key = std::make_pair(port, logical_window);
            if (architecture.coordinator_fifo_active_windows > 0 &&
                fifo_active_windows.count(window_key) == 0 &&
                fifo_active_windows.size() >= static_cast<std::size_t>(
                    architecture.coordinator_fifo_active_windows)) {
                continue;
            }
            if (!transaction_queue_has_capacity(*iterator)) {
                continue;
            }
            choice.candidate = *iterator;
            choice.port = port;
            choice.iterator = iterator;
            choice.valid = true;
            return choice;
        }
        return choice;
    };
    uint64_t next_admission_cycle = 0;
    auto next_admission_event = [&]() -> std::optional<uint64_t> {
        if (pending_available_count() != 0) {
            return std::max(next_admission_cycle, simulation_cycle);
        }
        if (!pending_future.empty()) {
            return std::max({next_admission_cycle,
                             pending_future.top().enqueue_cycle,
                             simulation_cycle});
        }
        return std::nullopt;
    };
    auto admit_transaction_blocks = [&](uint64_t cycle,
                                        std::vector<bool>& affected_channels) {
        promote_pending(cycle);
        TransactionAdmissionTrace trace;
        trace.cycle = cycle;
        for (std::size_t channel = 0;
             channel < outstanding_read_blocks.size(); ++channel) {
            trace.channel_read_occupancy_before[channel] = static_cast<uint16_t>(
                outstanding_read_blocks[channel]);
            trace.channel_write_occupancy_before[channel] = static_cast<uint16_t>(
                outstanding_write_blocks[channel]);
        }
        std::size_t admitted = 0;
        std::size_t admitted_reads = 0;
        std::size_t admitted_writes = 0;
        std::optional<std::size_t> fifo_admission_port;
        if (priority == MemoryPriorityMode::FIFO) {
            const PendingChoice first = select_pending(std::nullopt);
            if (first.valid) {
                fifo_admission_port = first.port;
            }
        }
        while (admitted < coordinator_issue_blocks) {
            PendingChoice choice = select_pending(fifo_admission_port);
            if (!choice.valid) {
                break;
            }
            if (priority == MemoryPriorityMode::BATCH_CLASS) {
                pending_priority.erase(choice.iterator);
            } else {
                pending_fifo_ports[choice.port].erase(choice.iterator);
            }
            Candidate candidate = choice.candidate;
            if (priority == MemoryPriorityMode::FIFO &&
                architecture.coordinator_fifo_active_windows > 0) {
                const uint64_t logical_window = candidate.block / blocks_per_row;
                ++fifo_active_windows[{choice.port, logical_window}];
            }
            candidate.admission_cycle = cycle;
            candidate.admission_order = next_admission_order++;
            if (candidate.is_write) {
                controller_write_queues[candidate.channel].push_back(candidate);
                ++outstanding_write_blocks[candidate.channel];
                ++admitted_writes;
            } else {
                controller_read_queues[candidate.channel].push_back(candidate);
                ++outstanding_read_blocks[candidate.channel];
                ++admitted_reads;
            }
            auto& state = states[candidate.request_index];
            if (admitted >= trace.block_identities.size()) {
                throw std::runtime_error("admission trace identity capacity exceeded");
            }
            trace.block_identities[admitted] = {
                state.request.sequence,
                candidate.block_offset,
            };
            state.first_admission = std::min(state.first_admission, cycle);
            state.last_admission = std::max(state.last_admission, cycle);
            ++state.admitted_blocks;
            ++admitted;
            if (state.admitted_blocks < state.block_count) {
                push_pending(make_candidate(candidate.request_index,
                                            state.admitted_blocks),
                             cycle);
            }
        }
        if (fifo_admission_port.has_value() && admitted != 0) {
            fifo_next_port = (*fifo_admission_port + 1) % kCoordinatorPorts;
        }
        if (admitted != 0) {
            trace.admitted_blocks = admitted;
            trace.admitted_read_blocks = admitted_reads;
            trace.admitted_write_blocks = admitted_writes;
            for (std::size_t channel = 0;
                 channel < outstanding_read_blocks.size(); ++channel) {
                if (outstanding_read_blocks[channel] > read_queue_capacity) {
                    throw std::runtime_error("read transaction queue capacity exceeded");
                }
                if (outstanding_write_blocks[channel] > write_buffer_capacity) {
                    throw std::runtime_error("write buffer capacity exceeded");
                }
                trace.channel_read_occupancy_after[channel] = static_cast<uint16_t>(
                    outstanding_read_blocks[channel]);
                trace.channel_write_occupancy_after[channel] = static_cast<uint16_t>(
                    outstanding_write_blocks[channel]);
                result.max_channel_read_queue_occupancy[channel] = std::max<uint64_t>(
                    result.max_channel_read_queue_occupancy[channel],
                    outstanding_read_blocks[channel]);
                result.max_channel_write_buffer_occupancy[channel] = std::max<uint64_t>(
                    result.max_channel_write_buffer_occupancy[channel],
                    outstanding_write_blocks[channel]);
            }
            result.admission_traces.push_back(std::move(trace));
            next_admission_cycle = cycle + 1;
        }
        return admitted;
    };
    auto channel_command_queue_empty = [&](std::size_t channel) {
        const std::size_t first_bank =
            channel * architecture.hbm_banks_per_channel;
        return std::all_of(
            command_queue_occupancy.begin() + static_cast<std::ptrdiff_t>(first_bank),
            command_queue_occupancy.begin() + static_cast<std::ptrdiff_t>(
                first_bank + architecture.hbm_banks_per_channel),
            [](std::size_t occupancy) { return occupancy == 0; });
    };
    auto refresh_write_drain = [&](std::size_t channel) {
        auto& remaining = write_drain_remaining[channel];
        const auto& writes = controller_write_queues[channel];
        const auto& reads = controller_read_queues[channel];
        if (remaining != 0 && writes.empty()) {
            remaining = 0;
        }
        if (remaining == 0 && !writes.empty() &&
            (writes.size() >= write_buffer_capacity || reads.empty() ||
             (writes.size() > 8 && channel_command_queue_empty(channel)))) {
            remaining = writes.size();
        }
    };
    struct ControllerChoice {
        std::size_t index = 0;
        bool is_write = false;
        bool valid = false;
    };
    auto controller_choice = [&](std::size_t channel, uint64_t cycle) {
        refresh_write_drain(channel);
        ControllerChoice selected;
        selected.is_write = write_drain_remaining[channel] != 0;
        const auto& queue = selected.is_write
            ? controller_write_queues[channel] : controller_read_queues[channel];
        if (queue.empty() || queue.front().admission_cycle > cycle) {
            return selected;
        }
        const auto& candidate = queue.front();
        const std::size_t flat_bank =
            candidate.channel * architecture.hbm_banks_per_channel + candidate.bank;
        if (command_queue_occupancy[flat_bank] < command_queue_capacity) {
            selected.valid = true;
        }
        return selected;
    };
    auto next_controller_event = [&]() -> std::optional<uint64_t> {
        std::optional<uint64_t> earliest;
        for (std::size_t channel = 0;
             channel < controller_read_queues.size(); ++channel) {
            refresh_write_drain(channel);
            const bool is_write = write_drain_remaining[channel] != 0;
            const auto& queue = is_write
                ? controller_write_queues[channel] : controller_read_queues[channel];
            if (queue.empty()) {
                continue;
            }
            const auto& candidate = queue.front();
            const std::size_t flat_bank =
                candidate.channel * architecture.hbm_banks_per_channel + candidate.bank;
            if (command_queue_occupancy[flat_bank] >= command_queue_capacity) {
                continue;
            }
            const uint64_t cycle = std::max(
                {next_controller_dispatch_cycle[channel], candidate.admission_cycle,
                 simulation_cycle});
            if (!earliest.has_value() || cycle < *earliest) {
                earliest = cycle;
            }
        }
        return earliest;
    };
    auto dispatch_controllers = [&](uint64_t cycle,
                                    std::vector<bool>& affected_channels) {
        std::size_t dispatched = 0;
        for (std::size_t channel = 0;
             channel < controller_read_queues.size(); ++channel) {
            if (next_controller_dispatch_cycle[channel] > cycle) {
                continue;
            }
            const auto selected = controller_choice(channel, cycle);
            if (!selected.valid) {
                continue;
            }
            auto& queue = selected.is_write
                ? controller_write_queues[channel] : controller_read_queues[channel];
            Candidate candidate = queue[selected.index];
            queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(selected.index));
            auto& outstanding = selected.is_write
                ? outstanding_write_blocks[channel] : outstanding_read_blocks[channel];
            if (outstanding == 0) {
                throw std::runtime_error("directional transaction queue occupancy underflow");
            }
            --outstanding;
            if (selected.is_write && write_drain_remaining[channel] != 0) {
                --write_drain_remaining[channel];
            }
            candidate.controller_dispatch_cycle = cycle;
            const std::size_t flat_bank =
                candidate.channel * architecture.hbm_banks_per_channel + candidate.bank;
            ++command_queue_occupancy[flat_bank];
            enqueue_bank_candidate(candidate);
            affected_channels[channel] = true;
            next_controller_dispatch_cycle[channel] = cycle + transfer_cycles;
            ++dispatched;
        }
        return dispatched;
    };
    for (std::size_t channel = 0; channel < channel_choices.size(); ++channel) {
        recompute_channel(channel);
    }

    auto erase_available_candidate = [&](BankQueue& queue,
                                         const CommandChoice& choice,
                                         const Candidate& candidate) {
        if (choice.source != 1 && choice.source != 2) {
            throw std::runtime_error("unknown bank choice source");
        }
        if (queue.available.erase(candidate) != 1) {
            throw std::runtime_error("available command candidate is inconsistent");
        }
        auto row = queue.available_rows.find(candidate.row);
        if (row == queue.available_rows.end() || row->second.erase(candidate) != 1) {
            throw std::runtime_error("available row index is inconsistent");
        }
        if (row->second.empty()) {
            queue.available_rows.erase(row);
        }
    };

    while (remaining_blocks > 0) {
        CommandChoice selected_choice;
        for (const auto& channel_choice : channel_choices) {
            if (choice_less(channel_choice, selected_choice)) {
                selected_choice = channel_choice;
            }
        }
        if (selected_choice.valid &&
            selected_choice.issue_cycle < simulation_cycle) {
            selected_choice.issue_cycle = simulation_cycle;
            selected_choice.candidate.start_cycle = simulation_cycle;
        }
        const auto admission_event = next_admission_event();
        const auto controller_event = next_controller_event();
        const uint64_t bank_event = selected_choice.valid
            ? selected_choice.issue_cycle
            : std::numeric_limits<uint64_t>::max();
        const uint64_t controller_cycle = controller_event.value_or(
            std::numeric_limits<uint64_t>::max());
        if (admission_event.has_value() &&
            *admission_event <= controller_cycle && *admission_event <= bank_event) {
            simulation_cycle = *admission_event;
            std::vector<bool> affected_channels(architecture.hbm_channels, false);
            const std::size_t admitted = admit_transaction_blocks(
                *admission_event, affected_channels);
            if (admitted != 0) {
                for (std::size_t channel = 0; channel < affected_channels.size(); ++channel) {
                    if (affected_channels[channel]) {
                        recompute_channel(channel);
                    }
                }
                continue;
            }
            const uint64_t state_change_cycle = std::min(
                controller_cycle, bank_event);
            if (state_change_cycle == std::numeric_limits<uint64_t>::max()) {
                throw std::runtime_error(
                    "transaction admission is blocked without a future state change");
            }
            next_admission_cycle = std::max(
                next_admission_cycle, state_change_cycle);
        }
        if (controller_event.has_value() && *controller_event <= bank_event) {
            simulation_cycle = *controller_event;
            std::vector<bool> affected_channels(architecture.hbm_channels, false);
            const std::size_t dispatched = dispatch_controllers(
                *controller_event, affected_channels);
            if (dispatched == 0) {
                throw std::runtime_error("controller dispatch event made no progress");
            }
            for (std::size_t channel = 0; channel < affected_channels.size(); ++channel) {
                if (affected_channels[channel]) {
                    recompute_channel(channel);
                }
            }
            continue;
        }
        if (!selected_choice.valid) {
            std::size_t bank_available = 0;
            std::size_t bank_future = 0;
            std::size_t bank_active = 0;
            std::size_t valid_channels = 0;
            for (const auto& queue : bank_queues) {
                bank_available += queue.available.size();
                bank_future += queue.future_reads.size() + queue.future_writes.size();
                bank_active += queue.active.has_value() ? 1 : 0;
            }
            for (const auto& choice : channel_choices) {
                valid_channels += choice.valid ? 1 : 0;
            }
            const std::size_t total_outstanding = std::accumulate(
                outstanding_read_blocks.begin(), outstanding_read_blocks.end(),
                std::size_t{0}) + std::accumulate(
                outstanding_write_blocks.begin(), outstanding_write_blocks.end(),
                std::size_t{0});
            const std::size_t controller_pending = std::accumulate(
                controller_read_queues.begin(), controller_read_queues.end(),
                std::size_t{0},
                [](std::size_t count, const auto& queue) { return count + queue.size(); }) +
                std::accumulate(
                controller_write_queues.begin(), controller_write_queues.end(),
                std::size_t{0},
                [](std::size_t count, const auto& queue) { return count + queue.size(); });
            const std::size_t command_pending = std::accumulate(
                command_queue_occupancy.begin(), command_queue_occupancy.end(),
                std::size_t{0});
            throw std::runtime_error(
                "memory request dependency cycle: remaining=" +
                std::to_string(remaining_blocks) + " outstanding=" +
                std::to_string(total_outstanding) + " pending_available=" +
                std::to_string(pending_available_count()) + " pending_future=" +
                std::to_string(pending_future.size()) + " controller_pending=" +
                std::to_string(controller_pending) + " command_pending=" +
                std::to_string(command_pending) + " bank_available=" +
                std::to_string(bank_available) + " bank_active=" +
                std::to_string(bank_active) + " bank_future=" +
                std::to_string(bank_future) + " valid_channels=" +
                std::to_string(valid_channels));
        }
        Candidate selected = selected_choice.candidate;
        selected.start_cycle = selected_choice.issue_cycle;
        auto& selected_queue = bank_queues[selected_choice.flat_bank];
        auto& state = states[selected.request_index];
        auto& bank_state = banks[selected_choice.flat_bank];
        auto& channel_state = channels[selected.channel];
        const uint64_t command_cycle = selected_choice.issue_cycle;
        simulation_cycle = command_cycle;
        const uint64_t command_row =
            selected_choice.command == MemoryCommandType::PRECHARGE
                ? bank_state.open_row : selected.row;
        record_command(selected, selected_choice.command, command_cycle, command_row);
        channel_state.next_command_cycle = command_cycle +
            architecture.hbm_command_issue_interval_cycles;

        if (selected_choice.command == MemoryCommandType::PRECHARGE) {
            if (selected_queue.active.has_value() || !bank_state.row_open ||
                bank_state.open_row == selected.row) {
                throw std::runtime_error("invalid PRE command plan");
            }
            erase_available_candidate(selected_queue, selected_choice, selected);
            selected.precharge_cycle = command_cycle;
            selected.row_hit = false;
            selected_queue.active = selected;
            selected_queue.active_stage = BankQueue::ActiveStage::ACTIVATE;
            bank_state.row_open = false;
            bank_state.next_activate_cycle = std::max(
                bank_state.next_activate_cycle,
                command_cycle + architecture.hbm_precharge_to_activate_cycles);
            ++result.precharge_commands;
            ++state.precharge_commands;
            state.first_precharge = std::min(state.first_precharge, command_cycle);
            state.last_precharge = std::max(state.last_precharge, command_cycle);
            recompute_channel(selected.channel);
            continue;
        }

        if (selected_choice.command == MemoryCommandType::ACTIVATE) {
            if (bank_state.row_open) {
                throw std::runtime_error("ACT command requires a closed bank");
            }
            if (!selected_queue.active.has_value()) {
                erase_available_candidate(selected_queue, selected_choice, selected);
                selected.row_hit = false;
                selected_queue.active = selected;
            } else if (selected_queue.active_stage !=
                       BankQueue::ActiveStage::ACTIVATE) {
                throw std::runtime_error("invalid active ACT command stage");
            }
            selected_queue.active->activate_cycle = command_cycle;
            selected_queue.active_stage = BankQueue::ActiveStage::DATA;
            bank_state.open_row = selected.row;
            bank_state.row_open = true;
            bank_state.next_read_cycle = std::max(
                bank_state.next_read_cycle,
                command_cycle + architecture.hbm_activate_to_read_cycles);
            bank_state.next_write_cycle = std::max(
                bank_state.next_write_cycle,
                command_cycle + architecture.hbm_activate_to_write_cycles);
            bank_state.next_precharge_cycle = std::max(
                bank_state.next_precharge_cycle,
                command_cycle + architecture.hbm_activate_to_precharge_cycles);
            bank_state.next_activate_cycle = std::max(
                bank_state.next_activate_cycle,
                command_cycle + architecture.hbm_activate_to_activate_cycles);
            ++result.activate_commands;
            ++state.activate_commands;
            state.first_activate = std::min(state.first_activate, command_cycle);
            state.last_activate = std::max(state.last_activate, command_cycle);
            recompute_channel(selected.channel);
            continue;
        }

        if (selected_queue.active.has_value()) {
            if (selected_queue.active_stage != BankQueue::ActiveStage::DATA ||
                selected_queue.active->request_index != selected.request_index ||
                selected_queue.active->block_offset != selected.block_offset) {
                throw std::runtime_error("invalid active data command stage");
            }
            selected = *selected_queue.active;
            selected.start_cycle = command_cycle;
            selected.command_ready_cycle =
                selected_choice.candidate.command_ready_cycle;
        } else {
            if (!selected.row_hit || !bank_state.row_open ||
                bank_state.open_row != selected.row) {
                throw std::runtime_error("direct data command is not a row hit");
            }
            erase_available_candidate(selected_queue, selected_choice, selected);
        }
        if (command_queue_occupancy[selected_choice.flat_bank] == 0) {
            throw std::runtime_error("command queue occupancy underflow");
        }
        --command_queue_occupancy[selected_choice.flat_bank];

        while (!active_fifo.empty() &&
               issued_candidates.count({active_fifo.top().request_index,
                                        active_fifo.top().block_offset}) != 0) {
            active_fifo.pop();
        }
        if (priority == MemoryPriorityMode::BATCH_CLASS &&
            state.first_issue == std::numeric_limits<uint64_t>::max() &&
            !active_fifo.empty() &&
            active_fifo.top().request_index != selected.request_index) {
            if (active_fifo.top().enqueue_cycle <= selected.start_cycle) {
                ++result.priority_reorders;
            }
        }
        state.effective_producer_ready = std::max(
            state.effective_producer_ready, selected.producer_ready_cycle);
        state.effective_enqueue = std::max(
            state.effective_enqueue, selected.enqueue_cycle);
        state.first_issue = std::min(state.first_issue, command_cycle);
        if (selected.row_hit) {
            ++result.row_buffer_hits;
            ++result.row_buffer_hits_by_class[static_cast<std::size_t>(
                state.request.request_class)];
        } else {
            ++result.row_buffer_misses;
            ++result.row_buffer_misses_by_class[static_cast<std::size_t>(
                state.request.request_class)];
        }
        const uint64_t data_latency = selected.is_write
            ? architecture.hbm_write_row_hit_cycles
            : architecture.hbm_read_row_hit_cycles;
        const uint64_t completion = command_cycle + data_latency + transfer_cycles;
        block_active_intervals.push_back({command_cycle, completion});
        if (channel_state.last_direction != -1 &&
            channel_state.last_direction != static_cast<int>(selected.is_write)) {
            if (command_cycle > selected.command_ready_cycle) {
                result.direction_switch_stall_cycles +=
                    command_cycle - selected.command_ready_cycle;
            }
            if (selected.is_write) {
                ++result.read_to_write_switches;
            } else {
                ++result.write_to_read_switches;
            }
        }
        if (selected.is_write) {
            ++result.write_commands;
            channel_state.next_write_cycle = std::max(
                channel_state.next_write_cycle,
                command_cycle + architecture.hbm_write_to_write_cycles);
            channel_state.next_read_cycle = std::max(
                channel_state.next_read_cycle,
                command_cycle + architecture.hbm_write_to_read_cycles);
            bank_state.next_precharge_cycle = std::max(
                bank_state.next_precharge_cycle,
                command_cycle + architecture.hbm_write_to_precharge_cycles);
        } else {
            ++result.read_commands;
            channel_state.next_read_cycle = std::max(
                channel_state.next_read_cycle,
                command_cycle + architecture.hbm_read_to_read_cycles);
            channel_state.next_write_cycle = std::max(
                channel_state.next_write_cycle,
                command_cycle + architecture.hbm_read_to_write_cycles);
            bank_state.next_precharge_cycle = std::max(
                bank_state.next_precharge_cycle,
                command_cycle + architecture.hbm_read_to_precharge_cycles);
        }
        channel_state.last_issue_cycle = command_cycle;
        channel_state.last_direction = static_cast<int>(selected.is_write);
        selected_queue.active.reset();
        selected_queue.active_stage = BankQueue::ActiveStage::NONE;
        state.completion = std::max(state.completion, completion);
        ++state.issued_blocks;
        if (priority == MemoryPriorityMode::FIFO &&
            architecture.coordinator_fifo_active_windows > 0) {
            const std::size_t port = CoordinatorPort(state.request.request_class);
            const auto window_key = std::make_pair(
                port, selected.block / blocks_per_row);
            const auto active_window = fifo_active_windows.find(window_key);
            if (active_window == fifo_active_windows.end() ||
                active_window->second == 0) {
                throw std::runtime_error("FIFO port window occupancy underflow");
            }
            if (--active_window->second == 0) {
                fifo_active_windows.erase(active_window);
            }
        }
        issued_candidates.insert({selected.request_index, selected.block_offset});
        --remaining_blocks;
        ++result.channel_blocks[selected.channel];
        ++result.bank_blocks[selected.bank];

        std::vector<bool> affected_channels(architecture.hbm_channels, false);
        affected_channels[selected.channel] = true;
        if (state.issued_blocks == state.block_count) {
            for (std::size_t dependent : dependents[selected.request_index]) {
                push_pending(make_candidate(dependent, 0), command_cycle);
            }
        }
        for (std::size_t channel = 0; channel < affected_channels.size(); ++channel) {
            if (affected_channels[channel]) {
                recompute_channel(channel);
            }
        }
    }

    result.command_trace_samples = std::move(first_command_samples);
    result.command_trace_samples.insert(
        result.command_trace_samples.end(),
        last_command_samples.begin(), last_command_samples.end());
    if (result.command_trace_event_count != result.precharge_commands +
            result.activate_commands + result.read_commands + result.write_commands) {
        throw std::runtime_error("command trace count differs from command totals");
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
            request.producer_ready_cycle,
            request.enqueue_cycle,
            request.producer_delay_cycles,
            state.effective_producer_ready,
            state.effective_enqueue,
            state.first_admission,
            state.last_admission,
            state.first_issue,
            state.completion,
            state.precharge_commands,
            state.activate_commands,
            state.precharge_commands == 0 ? 0 : state.first_precharge,
            state.last_precharge,
            state.activate_commands == 0 ? 0 : state.first_activate,
            state.last_activate,
            request.sequence,
            request.producer_sequence,
        });
        result.cycles = std::max(result.cycles, state.completion);
    }
    if (!result.admission_traces.empty()) {
        TransactionAdmissionTrace terminal;
        terminal.cycle = std::max(
            result.cycles, result.admission_traces.back().cycle + 1);
        terminal.terminal_snapshot = true;
        result.admission_traces.push_back(std::move(terminal));
    }
    std::stable_sort(result.request_traces.begin(), result.request_traces.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.sequence < rhs.sequence; });
    std::sort(block_active_intervals.begin(), block_active_intervals.end());
    uint64_t active_start = block_active_intervals.front().first;
    uint64_t active_end = block_active_intervals.front().second;
    for (std::size_t index = 1; index < block_active_intervals.size(); ++index) {
        const auto& interval = block_active_intervals[index];
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
        memory_cycles += layer.memory_service_cycles;
    }
    if (memory_cycles == 0) {
        return 0.0;
    }
    return Clamp(TotalDramBytes() /
                     (memory_cycles * architecture.HbmBytesPerCycle()),
                 0.0, 1.0);
}

double ExperimentResult::ActiveBandwidthUtilization() const {
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
        metrics.read_to_write_switches = memory_timing.read_to_write_switches;
        metrics.write_to_read_switches = memory_timing.write_to_read_switches;
        metrics.direction_switch_stall_cycles = memory_timing.direction_switch_stall_cycles;
        metrics.precharge_commands = memory_timing.precharge_commands;
        metrics.activate_commands = memory_timing.activate_commands;
        metrics.read_commands = memory_timing.read_commands;
        metrics.write_commands = memory_timing.write_commands;
        metrics.command_trace_event_count = memory_timing.command_trace_event_count;
        metrics.command_trace_checksum = memory_timing.command_trace_checksum;
        metrics.command_lane_violations = memory_timing.command_lane_violations;
        metrics.request_counts = memory_timing.request_counts;
        metrics.request_bytes = memory_timing.request_bytes;
        metrics.request_wait_cycles = memory_timing.request_wait_cycles;
        metrics.row_buffer_hits_by_class = memory_timing.row_buffer_hits_by_class;
        metrics.row_buffer_misses_by_class = memory_timing.row_buffer_misses_by_class;
        metrics.channel_blocks = memory_timing.channel_blocks;
        metrics.bank_blocks = memory_timing.bank_blocks;
        metrics.memory_request_traces = memory_timing.request_traces;
        metrics.command_trace_samples = memory_timing.command_trace_samples;
        metrics.command_trace_chunks = memory_timing.command_trace_chunks;
        metrics.transaction_admission_traces = memory_timing.admission_traces;
        metrics.max_channel_read_queue_occupancy =
            memory_timing.max_channel_read_queue_occupancy;
        metrics.max_channel_write_buffer_occupancy =
            memory_timing.max_channel_write_buffer_occupancy;
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
                (std::max<uint64_t>(1, metrics.memory_service_cycles) *
                 architecture_.HbmBytesPerCycle()),
            0.0, 1.0);
        metrics.active_bandwidth_utilization = Clamp(
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
            (std::max<uint64_t>(1, metrics.memory_service_cycles) *
             architecture_.HbmBytesPerCycle()),
        0.0, 1.0);
    metrics.active_bandwidth_utilization = Clamp(
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
        {"active_bandwidth_utilization", result.ActiveBandwidthUtilization()},
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
              "bandwidth_utilization,active_bandwidth_utilization,queue_wait_cycles,"
              "hbm_blocked_cycles,channel_imbalance,"
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
               << layer.active_bandwidth_utilization << ','
               << layer.queue_wait_cycles << ',' << layer.hbm_blocked_cycles << ','
               << layer.channel_imbalance << ',' << layer.bank_imbalance << ','
               << layer.ae_finish_cycle << ',' << layer.ce_start_cycle << ','
               << layer.ce_finish_cycle << ',' << layer.aggregation_buffer_peak_bytes << ','
               << layer.simd_idle_lane_cycles << ',' << layer.array_idle_lane_cycles << '\n';
    }
}
