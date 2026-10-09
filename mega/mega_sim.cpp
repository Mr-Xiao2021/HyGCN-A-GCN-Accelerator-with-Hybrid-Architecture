#include "mega_sim.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <stdexcept>

#include "INIReader.h"
#include "json.hpp"

#ifndef HYGCN_GIT_COMMIT
#define HYGCN_GIT_COMMIT "unknown"
#endif

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

uint64_t Mix(uint64_t value) {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::vector<QuantizedNode> GenerateNodes(const Graph& graph,
                                         const LayerQuantization& layer) {
    std::vector<QuantizedNode> nodes;
    nodes.reserve(graph.num_vertex);
    const uint64_t density_threshold = static_cast<uint64_t>(
        std::llround(layer.feature_density * 1'000'000.0));
    for (int vertex = 0; vertex < graph.num_vertex; ++vertex) {
        const int degree = static_cast<int>(graph.r_adj[vertex].size());
        const auto& rule = layer.RuleForDegree(degree);
        QuantizedNode node;
        node.node_id = vertex;
        node.bitwidth = rule.bitwidth;
        node.values.assign(layer.feature_count, 0);
        const int32_t maximum = (int32_t{1} << (rule.bitwidth - 1)) - 1;
        if (maximum > 0) {
            for (int feature = 0; feature < layer.feature_count; ++feature) {
                const uint64_t hash = Mix(
                    (static_cast<uint64_t>(layer.layer + 1) << 56U) ^
                    (static_cast<uint64_t>(vertex) << 24U) ^
                    static_cast<uint64_t>(feature));
                if (hash % 1'000'000ULL >= density_threshold) {
                    continue;
                }
                const int32_t magnitude = 1 + static_cast<int32_t>(hash % maximum);
                node.values[feature] = (hash & (1ULL << 63U)) != 0
                    ? -magnitude : magnitude;
            }
        }
        nodes.push_back(std::move(node));
    }
    return nodes;
}

uint64_t NonzeroCount(const std::vector<QuantizedNode>& nodes) {
    uint64_t count = 0;
    for (const auto& node : nodes) {
        count += std::count_if(node.values.begin(), node.values.end(),
                               [](int32_t value) { return value != 0; });
    }
    return count;
}

uint64_t FeatureBitWork(const std::vector<QuantizedNode>& nodes) {
    uint64_t bits = 0;
    for (const auto& node : nodes) {
        const uint64_t nonzero = std::count_if(
            node.values.begin(), node.values.end(),
            [](int32_t value) { return value != 0; });
        bits += nonzero * node.bitwidth;
    }
    return bits;
}

PackageTraffic BitmapTraffic(const std::vector<QuantizedNode>& nodes,
                             uint64_t transaction_bytes,
                             int stored_value_bits,
                             std::size_t scale_entries) {
    if (nodes.empty() || transaction_bytes == 0 || stored_value_bits <= 0) {
        throw std::runtime_error("invalid bitmap traffic inputs");
    }
    PackageTraffic traffic;
    const uint64_t nonzero = NonzeroCount(nodes);
    traffic.payload_bits = nonzero * stored_value_bits;
    traffic.bitmap_bits = static_cast<uint64_t>(nodes.size()) *
                          nodes.front().values.size();
    traffic.scale_bits = scale_entries * 32;
    const uint64_t payload_bytes = CeilDiv(traffic.payload_bits, 8);
    const uint64_t bitmap_bytes = CeilDiv(traffic.bitmap_bits, 8);
    const uint64_t scale_bytes = CeilDiv(traffic.scale_bits, 8);
    traffic.logical_bytes = payload_bytes + bitmap_bytes + scale_bytes;
    traffic.dram_bytes = Align(payload_bytes, transaction_bytes) +
                         Align(bitmap_bytes, transaction_bytes) +
                         (scale_bytes == 0 ? 0 : Align(scale_bytes, transaction_bytes));
    traffic.dram_transactions = traffic.dram_bytes / transaction_bytes;
    return traffic;
}

MemoryTimingResult SimulateStage(
        const ArchitectureConfig& hbm,
        const std::vector<std::pair<RequestClass, uint64_t>>& streams) {
    std::vector<MemoryRequest> requests;
    uint64_t address = 0;
    uint64_t sequence = 0;
    for (const auto& [request_class, bytes] : streams) {
        if (bytes == 0) {
            continue;
        }
        const uint64_t aligned = Align(bytes, hbm.block_size);
        requests.push_back({0, request_class, aligned, address, 0, sequence++});
        address += aligned;
    }
    if (requests.empty()) {
        return {};
    }
    if (address > hbm.hbm_capacity_bytes) {
        throw std::runtime_error("MEGA memory stage exceeds configured HBM capacity");
    }
    return MemoryCoordinatorModel::Simulate(
        requests, hbm, MemoryPriorityMode::BATCH_CLASS,
        AddressMappingMode::LOW_BITS);
}

uint64_t EdgeLogicalBytes(const Graph& graph, bool quantized) {
    const uint64_t index_bytes = static_cast<uint64_t>(graph.num_edge) * 4;
    const uint64_t value_bits = static_cast<uint64_t>(graph.num_edge) *
                                (quantized ? 4 : 32);
    return index_bytes + CeilDiv(value_bits, 8);
}

json PackageTrafficJson(const PackageTraffic& traffic) {
    return {
        {"payload_bits", traffic.payload_bits},
        {"header_bits", traffic.header_bits},
        {"padding_bits", traffic.padding_bits},
        {"bitmap_bits", traffic.bitmap_bits},
        {"boundary_bits", traffic.boundary_bits},
        {"scale_bits", traffic.scale_bits},
        {"logical_bytes", traffic.logical_bytes},
        {"dram_bytes", traffic.dram_bytes},
        {"dram_transactions", traffic.dram_transactions},
    };
}

json CondenseJson(const CondenseMetrics& metrics) {
    return {
        {"cross_edges", metrics.cross_edges},
        {"unique_source_references", metrics.unique_source_references},
        {"matched_sources", metrics.matched_sources},
        {"fifo_refill_entries", metrics.fifo_refill_entries},
        {"fifo_refill_cycles", metrics.fifo_refill_cycles},
        {"compare_cycles", metrics.compare_cycles},
        {"condense_cycles", metrics.condense_cycles},
        {"combination_buffer_write_bytes", metrics.combination_buffer_write_bytes},
        {"sparse_buffer_write_bytes", metrics.sparse_buffer_write_bytes},
        {"sparse_spill_write_bytes", metrics.sparse_spill_write_bytes},
        {"baseline_cross_read_bytes", metrics.baseline_cross_read_bytes},
        {"condensed_cross_read_bytes", metrics.condensed_cross_read_bytes},
        {"baseline_cross_read_transactions", metrics.baseline_cross_read_transactions},
        {"condensed_cross_read_transactions", metrics.condensed_cross_read_transactions},
        {"sparse_spill_write_transactions", metrics.sparse_spill_write_transactions},
    };
}

}  // namespace

MegaArchitectureConfig MegaArchitectureConfig::Load(const std::string& path) {
    INIReader reader(path);
    if (reader.ParseError() != 0) {
        throw std::runtime_error("cannot parse MEGA architecture config: " + path);
    }
    MegaArchitectureConfig config;
    config.profile = reader.Get("architecture", "profile", "");
    config.frequency_ghz = reader.GetReal("architecture", "frequency_ghz", -1.0);
    config.hbm_profile_path = reader.Get("architecture", "hbm_profile", "");
    config.transaction_bytes = reader.GetInteger(
        "architecture", "transaction_bytes", -1);
    config.combination_tiles = reader.GetInteger("processing", "combination_tiles", -1);
    config.cpes_per_tile = reader.GetInteger("processing", "cpes_per_tile", -1);
    config.bses_per_cpe = reader.GetInteger("processing", "bses_per_cpe", -1);
    config.aggregation_units = reader.GetInteger("processing", "aggregation_units", -1);
    config.encoder_qn_units = reader.GetInteger("processing", "encoder_qn_units", -1);
    config.decoder_values_per_cycle = reader.GetInteger(
        "processing", "decoder_values_per_cycle", -1);
    config.input_buffer_bytes = reader.GetInteger("buffer", "input_bytes", -1);
    config.edge_buffer_bytes = reader.GetInteger("buffer", "edge_bytes", -1);
    config.weight_buffer_bytes = reader.GetInteger("buffer", "weight_bytes", -1);
    config.combination_buffer_bytes = reader.GetInteger(
        "buffer", "combination_bytes", -1);
    config.aggregation_buffer_bytes = reader.GetInteger(
        "buffer", "aggregation_bytes", -1);
    config.sparse_buffer_bytes = reader.GetInteger("buffer", "sparse_bytes", -1);
    config.parallel_fifos = reader.GetInteger("condense", "parallel_fifos", -1);
    config.fifo_entries = reader.GetInteger("condense", "fifo_entries", -1);
    config.partition_vertices = reader.GetInteger("condense", "partition_vertices", -1);
    if (config.hbm_profile_path.empty()) {
        throw std::runtime_error("MEGA config is missing hbm_profile");
    }
    config.hbm = ArchitectureConfig::Load(config.hbm_profile_path);
    config.Validate();
    return config;
}

void MegaArchitectureConfig::Validate() const {
    if (profile.empty() || frequency_ghz <= 0.0 || transaction_bytes == 0 ||
        combination_tiles <= 0 || cpes_per_tile <= 0 || bses_per_cpe <= 0 ||
        aggregation_units <= 0 || encoder_qn_units <= 0 ||
        decoder_values_per_cycle <= 0 || input_buffer_bytes == 0 ||
        edge_buffer_bytes == 0 || weight_buffer_bytes == 0 ||
        combination_buffer_bytes == 0 || aggregation_buffer_bytes == 0 ||
        sparse_buffer_bytes == 0 || parallel_fifos <= 0 || fifo_entries <= 0 ||
        partition_vertices <= 0) {
        throw std::runtime_error("MEGA architecture values must be positive");
    }
    if (transaction_bytes % hbm.block_size != 0) {
        throw std::runtime_error("MEGA transaction size must be a multiple of HBM block size");
    }
    hbm.Validate();
    if (profile == "mega-paper") {
        if (frequency_ghz != 1.0 || hbm.hbm_bandwidth_gbps != 256.0 ||
            TotalBufferBytes() != 392ULL * 1024 || TotalBses() != 4ULL * 8 * 32 ||
            aggregation_units != 256 || parallel_fifos != 16 || fifo_entries != 8) {
            throw std::runtime_error("MEGA paper profile does not match the paper configuration");
        }
    }
}

uint64_t MegaArchitectureConfig::TotalBufferBytes() const {
    return input_buffer_bytes + edge_buffer_bytes + weight_buffer_bytes +
           combination_buffer_bytes + aggregation_buffer_bytes + sparse_buffer_bytes;
}

uint64_t MegaArchitectureConfig::TotalBses() const {
    return static_cast<uint64_t>(combination_tiles) * cpes_per_tile * bses_per_cpe;
}

uint64_t MegaExperimentResult::TotalCycles() const {
    return std::accumulate(layers.begin(), layers.end(), uint64_t{0},
                           [](uint64_t total, const auto& layer) {
                               return total + layer.total_cycles;
                           });
}

uint64_t MegaExperimentResult::TotalDramBytes() const {
    return std::accumulate(layers.begin(), layers.end(), uint64_t{0},
                           [](uint64_t total, const auto& layer) {
                               return total + layer.total_dram_bytes;
                           });
}

uint64_t MegaExperimentResult::TotalDramTransactions() const {
    return std::accumulate(layers.begin(), layers.end(), uint64_t{0},
                           [](uint64_t total, const auto& layer) {
                               return total + layer.total_dram_transactions;
                           });
}

MegaSimulator::MegaSimulator(MegaArchitectureConfig architecture)
    : architecture_(std::move(architecture)) {
    architecture_.Validate();
}

MegaExperimentResult MegaSimulator::Run(
        const Graph& graph,
        const std::string& model,
        const std::string& dataset,
        const std::string& graph_digest,
        const QuantizationManifest& quantization,
        const PartitionManifest& partition,
        MegaVariant variant,
        bool require_publishable,
        int selected_layer) const {
    quantization.ValidateFor(dataset, model, graph_digest);
    partition.Validate(graph, graph_digest);
    const bool exact_feature_payload_available = false;
    if (require_publishable &&
        (!quantization.EligibleForRequiredBenchmark() ||
         !partition.EligibleForRequiredBenchmark() ||
         !exact_feature_payload_available)) {
        throw std::runtime_error(
            "required MEGA benchmark needs an exact quantized feature payload, "
            "paper-derived/locally-trained quantization, and a METIS partition");
    }
    if (selected_layer < -1 || selected_layer >= static_cast<int>(quantization.layers.size())) {
        throw std::runtime_error("selected MEGA layer is outside the quantization manifest");
    }
    MegaExperimentResult result;
    result.model = model;
    result.dataset = dataset;
    result.profile = architecture_.profile;
    result.graph_digest = graph_digest;
    result.variant = variant;
    result.quantization_provenance = quantization.provenance;
    result.quantization_manifest_version = quantization.manifest_version;
    result.feature_value_source = "deterministic-density-surrogate-v1";
    result.partition_source = partition.tool;
    result.partition_manifest_version = partition.manifest_version;
    result.partition_parameters = partition.parameters;
    result.partition_digest = partition.Digest();
    result.required_eligible = quantization.EligibleForRequiredBenchmark() &&
                               partition.EligibleForRequiredBenchmark() &&
                               exact_feature_payload_available;
    result.architecture = architecture_;
    for (const auto& layer : quantization.layers) {
        if (selected_layer >= 0 && layer.layer != selected_layer) {
            continue;
        }
        result.layers.push_back(RunLayer(
            graph, layer, partition, layer.layer, variant));
    }
    return result;
}

MegaLayerMetrics MegaSimulator::RunLayer(
        const Graph& graph,
        const LayerQuantization& quantization,
        const PartitionManifest& partition,
        int layer,
        MegaVariant variant) const {
    MegaLayerMetrics metrics;
    metrics.layer = layer;
    metrics.input_features = quantization.feature_count;
    metrics.output_features = quantization.output_features;
    metrics.vertices = graph.num_vertex;
    metrics.edges = graph.num_edge;

    const auto input_nodes = GenerateNodes(graph, quantization);
    auto output_quantization = quantization;
    output_quantization.feature_count = quantization.output_features;
    output_quantization.feature_density = std::max(0.05, quantization.feature_density);
    const auto output_nodes = GenerateNodes(graph, output_quantization);
    metrics.nonzero_features = NonzeroCount(input_nodes);
    metrics.feature_bit_work = FeatureBitWork(input_nodes);
    metrics.average_feature_bits = metrics.nonzero_features == 0 ? 0.0 :
        static_cast<double>(metrics.feature_bit_work) / metrics.nonzero_features;

    const std::size_t scale_entries = quantization.degree_rules.size();
    if (variant == MegaVariant::M0_FP32_AXW) {
        metrics.input_logical_bytes =
            static_cast<uint64_t>(graph.num_vertex) * quantization.feature_count * 4;
        metrics.output_logical_bytes =
            static_cast<uint64_t>(graph.num_vertex) * quantization.output_features * 4;
        metrics.input_dram_bytes = Align(metrics.input_logical_bytes,
                                         architecture_.transaction_bytes);
        metrics.output_dram_bytes = Align(metrics.output_logical_bytes,
                                          architecture_.transaction_bytes);
    } else if (variant == MegaVariant::M1_DEGREE_AWARE_BITMAP) {
        metrics.input_package_traffic = BitmapTraffic(
            input_nodes, architecture_.transaction_bytes, 8, scale_entries);
        metrics.output_package_traffic = BitmapTraffic(
            output_nodes, architecture_.transaction_bytes, 8, scale_entries);
        metrics.input_dram_bytes = metrics.input_package_traffic.dram_bytes;
        metrics.output_dram_bytes = metrics.output_package_traffic.dram_bytes;
        metrics.input_logical_bytes = metrics.input_package_traffic.logical_bytes;
        metrics.output_logical_bytes = metrics.output_package_traffic.logical_bytes;
    } else {
        metrics.input_package_traffic = AdaptivePackageCodec::Encode(
            input_nodes, architecture_.transaction_bytes, scale_entries).traffic;
        metrics.output_package_traffic = AdaptivePackageCodec::Encode(
            output_nodes, architecture_.transaction_bytes, scale_entries).traffic;
        metrics.input_dram_bytes = metrics.input_package_traffic.dram_bytes;
        metrics.output_dram_bytes = metrics.output_package_traffic.dram_bytes;
        metrics.input_logical_bytes = metrics.input_package_traffic.logical_bytes;
        metrics.output_logical_bytes = metrics.output_package_traffic.logical_bytes;
    }

    const int weight_bits = variant == MegaVariant::M0_FP32_AXW ? 32 : 4;
    const uint64_t weight_payload = CeilDiv(
        static_cast<uint64_t>(quantization.feature_count) *
            quantization.output_features * weight_bits,
        8);
    const uint64_t weight_scale_bytes = variant == MegaVariant::M0_FP32_AXW
        ? 0 : static_cast<uint64_t>(quantization.output_features) * 4;
    metrics.weight_logical_bytes = weight_payload + weight_scale_bytes;
    metrics.weight_dram_bytes = Align(weight_payload, architecture_.transaction_bytes) +
                                Align(weight_scale_bytes, architecture_.transaction_bytes);
    metrics.edge_logical_bytes = EdgeLogicalBytes(
        graph, variant != MegaVariant::M0_FP32_AXW);
    metrics.edge_dram_bytes = Align(metrics.edge_logical_bytes,
                                    architecture_.transaction_bytes);

    const auto plan = CondensePlan::Build(graph, partition);
    const uint64_t aggregation_feature_bytes = Align(
        CeilDiv(static_cast<uint64_t>(quantization.output_features) *
                    (variant == MegaVariant::M0_FP32_AXW ? 32 : 4),
                8),
        1);
    std::vector<uint64_t> node_feature_bytes(graph.num_vertex,
                                             aggregation_feature_bytes);
    CondenseConfig condense_config;
    condense_config.parallel_fifos = architecture_.parallel_fifos;
    condense_config.fifo_entries = architecture_.fifo_entries;
    condense_config.sparse_buffer_bytes = architecture_.sparse_buffer_bytes;
    condense_config.transaction_bytes = architecture_.transaction_bytes;
    metrics.condense = CondenseEdgeModel::Run(
        graph, partition, plan, node_feature_bytes, condense_config);
    metrics.cross_partition_dram_bytes =
        variant == MegaVariant::M3_CONDENSE_EDGE
            ? metrics.condense.CondensedDramBytes()
            : metrics.condense.baseline_cross_read_bytes;
    metrics.cross_partition_logical_bytes =
        metrics.condense.sparse_buffer_write_bytes +
        (variant == MegaVariant::M3_CONDENSE_EDGE
             ? metrics.condense.sparse_spill_write_bytes : 0);

    const uint64_t input_values = static_cast<uint64_t>(graph.num_vertex) *
                                  quantization.feature_count;
    if (variant == MegaVariant::M0_FP32_AXW) {
        metrics.decoder_cycles = 0;
        const uint64_t bit_work = input_values * 32ULL *
                                  quantization.output_features * 8ULL;
        metrics.combination_cycles = CeilDiv(bit_work, architecture_.TotalBses()) +
                                     quantization.feature_count +
                                     quantization.output_features;
    } else {
        metrics.decoder_cycles = CeilDiv(
            metrics.nonzero_features, architecture_.decoder_values_per_cycle) +
            (variant == MegaVariant::M1_DEGREE_AWARE_BITMAP
                 ? 0 : metrics.input_package_traffic.header_bits / 5);
        const uint64_t bit_work = metrics.feature_bit_work *
                                  quantization.output_features;
        metrics.combination_cycles = CeilDiv(bit_work, architecture_.TotalBses()) +
                                     quantization.output_features;
    }
    const uint64_t precision_factor =
        variant == MegaVariant::M0_FP32_AXW ? 8 : 1;
    metrics.aggregation_cycles = CeilDiv(
        static_cast<uint64_t>(graph.num_edge) * quantization.output_features *
            precision_factor,
        architecture_.aggregation_units);
    metrics.encoder_cycles = variant == MegaVariant::M0_FP32_AXW ? 0 : CeilDiv(
        static_cast<uint64_t>(graph.num_vertex) * quantization.output_features,
        architecture_.encoder_qn_units);
    metrics.condense_cycles = variant == MegaVariant::M3_CONDENSE_EDGE
        ? metrics.condense.condense_cycles : 0;

    const auto preload = SimulateStage(architecture_.hbm, {
        {RequestClass::INPUT, metrics.input_dram_bytes},
        {RequestClass::WEIGHT, metrics.weight_dram_bytes},
        {RequestClass::EDGE, metrics.edge_dram_bytes},
    });
    std::vector<std::pair<RequestClass, uint64_t>> cross_streams;
    if (variant == MegaVariant::M3_CONDENSE_EDGE) {
        cross_streams.push_back({RequestClass::INTERMEDIATE_WRITE,
                                 metrics.condense.sparse_spill_write_bytes});
        cross_streams.push_back({RequestClass::INTERMEDIATE_READ,
                                 metrics.condense.condensed_cross_read_bytes});
    } else {
        cross_streams.push_back({RequestClass::INPUT,
                                 metrics.cross_partition_dram_bytes});
    }
    const auto cross = SimulateStage(architecture_.hbm, cross_streams);
    const auto output = SimulateStage(architecture_.hbm, {
        {RequestClass::OUTPUT, metrics.output_dram_bytes},
    });
    metrics.preload_memory_cycles = preload.cycles;
    metrics.cross_partition_memory_cycles = cross.cycles;
    metrics.output_memory_cycles = output.cycles;
    metrics.memory_service_cycles = preload.cycles + cross.cycles + output.cycles;
    metrics.total_logical_bytes = metrics.input_logical_bytes +
        metrics.weight_logical_bytes + metrics.edge_logical_bytes +
        metrics.cross_partition_logical_bytes + metrics.output_logical_bytes;
    metrics.total_dram_bytes = metrics.input_dram_bytes + metrics.weight_dram_bytes +
        metrics.edge_dram_bytes + metrics.cross_partition_dram_bytes +
        metrics.output_dram_bytes;
    metrics.input_dram_transactions =
        metrics.input_dram_bytes / architecture_.transaction_bytes;
    metrics.weight_dram_transactions =
        metrics.weight_dram_bytes / architecture_.transaction_bytes;
    metrics.edge_dram_transactions =
        metrics.edge_dram_bytes / architecture_.transaction_bytes;
    metrics.cross_partition_dram_transactions =
        metrics.cross_partition_dram_bytes / architecture_.transaction_bytes;
    metrics.output_dram_transactions =
        metrics.output_dram_bytes / architecture_.transaction_bytes;
    metrics.total_dram_transactions = metrics.input_dram_transactions +
        metrics.weight_dram_transactions + metrics.edge_dram_transactions +
        metrics.cross_partition_dram_transactions + metrics.output_dram_transactions;
    metrics.total_cycles = metrics.memory_service_cycles + metrics.decoder_cycles +
        metrics.combination_cycles + metrics.condense_cycles +
        metrics.aggregation_cycles + metrics.encoder_cycles;
    return metrics;
}

std::string ToString(MegaVariant variant) {
    switch (variant) {
        case MegaVariant::M0_FP32_AXW:
            return "m0-fp32-axw";
        case MegaVariant::M1_DEGREE_AWARE_BITMAP:
            return "m1-degree-aware-bitmap";
        case MegaVariant::M2_ADAPTIVE_PACKAGE:
            return "m2-adaptive-package";
        case MegaVariant::M3_CONDENSE_EDGE:
            return "m3-condense-edge";
    }
    throw std::runtime_error("unknown MEGA variant");
}

MegaVariant ParseMegaVariant(const std::string& value) {
    if (value == "m0" || value == "m0-fp32-axw") {
        return MegaVariant::M0_FP32_AXW;
    }
    if (value == "m1" || value == "m1-degree-aware-bitmap") {
        return MegaVariant::M1_DEGREE_AWARE_BITMAP;
    }
    if (value == "m2" || value == "m2-adaptive-package") {
        return MegaVariant::M2_ADAPTIVE_PACKAGE;
    }
    if (value == "m3" || value == "m3-condense-edge") {
        return MegaVariant::M3_CONDENSE_EDGE;
    }
    throw std::runtime_error("invalid MEGA variant: " + value);
}

void WriteMegaJson(const MegaExperimentResult& result, const std::string& path) {
    const std::filesystem::path output_path(path);
    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    json output;
    output["schema_version"] = 1;
    output["manifest"] = {
        {"engine", "mega"},
        {"git_commit", HYGCN_GIT_COMMIT},
        {"binary_digest", result.binary_digest},
        {"model", result.model},
        {"dataset", result.dataset},
        {"profile", result.profile},
        {"variant", ToString(result.variant)},
        {"graph_digest", result.graph_digest},
        {"config_digest", result.config_digest},
        {"quantization_digest", result.quantization_digest},
        {"quantization_manifest_version", result.quantization_manifest_version},
        {"quantization_provenance", ToString(result.quantization_provenance)},
        {"feature_value_source", result.feature_value_source},
        {"partition_source", result.partition_source},
        {"partition_manifest_version", result.partition_manifest_version},
        {"partition_parameters", result.partition_parameters},
        {"partition_digest", result.partition_digest},
        {"required_eligible", result.required_eligible},
    };
    output["architecture"] = {
        {"frequency_ghz", result.architecture.frequency_ghz},
        {"hbm_bandwidth_gbps", result.architecture.hbm.hbm_bandwidth_gbps},
        {"transaction_bytes", result.architecture.transaction_bytes},
        {"total_buffer_bytes", result.architecture.TotalBufferBytes()},
        {"buffers", {
            {"input", result.architecture.input_buffer_bytes},
            {"edge", result.architecture.edge_buffer_bytes},
            {"weight", result.architecture.weight_buffer_bytes},
            {"combination", result.architecture.combination_buffer_bytes},
            {"aggregation", result.architecture.aggregation_buffer_bytes},
            {"sparse", result.architecture.sparse_buffer_bytes},
        }},
        {"combination_tiles", result.architecture.combination_tiles},
        {"cpes_per_tile", result.architecture.cpes_per_tile},
        {"bses_per_cpe", result.architecture.bses_per_cpe},
        {"total_bses", result.architecture.TotalBses()},
        {"aggregation_units", result.architecture.aggregation_units},
        {"encoder_qn_units", result.architecture.encoder_qn_units},
        {"decoder_values_per_cycle", result.architecture.decoder_values_per_cycle},
        {"parallel_fifos", result.architecture.parallel_fifos},
        {"fifo_entries", result.architecture.fifo_entries},
        {"partition_vertices", result.architecture.partition_vertices},
        {"adaptive_package_bits", {64, 128, 192}},
        {"hbm", {
            {"block_size", result.architecture.hbm.block_size},
            {"channels", result.architecture.hbm.hbm_channels},
            {"banks_per_channel", result.architecture.hbm.hbm_banks_per_channel},
            {"row_bytes", result.architecture.hbm.hbm_row_bytes},
            {"read_queue_entries_per_channel",
             result.architecture.hbm.hbm_read_queue_entries_per_channel},
            {"write_buffer_entries_per_channel",
             result.architecture.hbm.hbm_write_buffer_entries_per_channel},
        }},
    };
    output["summary"] = {
        {"total_cycles", result.TotalCycles()},
        {"total_dram_bytes", result.TotalDramBytes()},
        {"total_dram_transactions", result.TotalDramTransactions()},
        {"conclusion_level", result.required_eligible
            ? "required-eligible" : "diagnostic-mechanism"},
    };
    output["layers"] = json::array();
    for (const auto& layer : result.layers) {
        output["layers"].push_back({
            {"layer", layer.layer},
            {"input_features", layer.input_features},
            {"output_features", layer.output_features},
            {"vertices", layer.vertices},
            {"edges", layer.edges},
            {"nonzero_features", layer.nonzero_features},
            {"feature_bit_work", layer.feature_bit_work},
            {"average_feature_bits", layer.average_feature_bits},
            {"logical_bytes", {
                {"input", layer.input_logical_bytes},
                {"weight", layer.weight_logical_bytes},
                {"edge", layer.edge_logical_bytes},
                {"cross_partition", layer.cross_partition_logical_bytes},
                {"output", layer.output_logical_bytes},
                {"total", layer.total_logical_bytes},
            }},
            {"dram_bytes", {
                {"input", layer.input_dram_bytes},
                {"weight", layer.weight_dram_bytes},
                {"edge", layer.edge_dram_bytes},
                {"cross_partition", layer.cross_partition_dram_bytes},
                {"output", layer.output_dram_bytes},
                {"total", layer.total_dram_bytes},
            }},
            {"dram_transactions", {
                {"input", layer.input_dram_transactions},
                {"weight", layer.weight_dram_transactions},
                {"edge", layer.edge_dram_transactions},
                {"cross_partition", layer.cross_partition_dram_transactions},
                {"output", layer.output_dram_transactions},
                {"total", layer.total_dram_transactions},
            }},
            {"cycles", {
                {"preload_memory", layer.preload_memory_cycles},
                {"cross_partition_memory", layer.cross_partition_memory_cycles},
                {"output_memory", layer.output_memory_cycles},
                {"memory_service", layer.memory_service_cycles},
                {"decoder", layer.decoder_cycles},
                {"combination", layer.combination_cycles},
                {"condense", layer.condense_cycles},
                {"aggregation", layer.aggregation_cycles},
                {"encoder", layer.encoder_cycles},
                {"total", layer.total_cycles},
            }},
            {"input_format", PackageTrafficJson(layer.input_package_traffic)},
            {"output_format", PackageTrafficJson(layer.output_package_traffic)},
            {"condense", CondenseJson(layer.condense)},
        });
    }
    std::ofstream stream(path);
    if (!stream) {
        throw std::runtime_error("cannot write MEGA JSON result: " + path);
    }
    stream << std::setw(2) << output << '\n';
}

void WriteMegaCsv(const MegaExperimentResult& result, const std::string& path) {
    const std::filesystem::path output_path(path);
    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    std::ofstream stream(path);
    if (!stream) {
        throw std::runtime_error("cannot write MEGA CSV result: " + path);
    }
    stream << "model,dataset,profile,variant,required_eligible,layer,input_features,"
              "output_features,nonzero_features,average_feature_bits,total_logical_bytes,input_dram_bytes,"
              "weight_dram_bytes,edge_dram_bytes,cross_partition_dram_bytes,"
              "output_dram_bytes,total_dram_bytes,memory_service_cycles,decoder_cycles,"
              "combination_cycles,condense_cycles,aggregation_cycles,encoder_cycles,total_cycles\n";
    for (const auto& layer : result.layers) {
        stream << result.model << ',' << result.dataset << ',' << result.profile << ','
               << ToString(result.variant) << ',' << result.required_eligible << ','
               << layer.layer << ',' << layer.input_features << ','
               << layer.output_features << ',' << layer.nonzero_features << ','
               << layer.average_feature_bits << ',' << layer.total_logical_bytes << ','
               << layer.input_dram_bytes << ','
               << layer.weight_dram_bytes << ',' << layer.edge_dram_bytes << ','
               << layer.cross_partition_dram_bytes << ',' << layer.output_dram_bytes << ','
               << layer.total_dram_bytes << ',' << layer.memory_service_cycles << ','
               << layer.decoder_cycles << ',' << layer.combination_cycles << ','
               << layer.condense_cycles << ',' << layer.aggregation_cycles << ','
               << layer.encoder_cycles << ',' << layer.total_cycles << '\n';
    }
}

}  // namespace mega
