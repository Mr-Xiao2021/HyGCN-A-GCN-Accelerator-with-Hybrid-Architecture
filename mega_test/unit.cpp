#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "adaptive_package.h"
#include "condense_edge.h"
#include "mega_sim.h"
#include "quantization.h"

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <typename Function>
void ExpectThrows(Function function, const std::string& message) {
    try {
        function();
        Check(false, message);
    } catch (const std::exception&) {
        Check(true, message);
    }
}

template <typename Function>
void RunNamedTest(const std::string& name, Function function) {
    const int before = failures;
    function();
    std::cout << name << '=' << (failures == before ? "PASS" : "FAIL") << '\n';
}

std::filesystem::path WriteManifest(const std::string& body,
                                    const std::string& name) {
    const auto directory = std::filesystem::temp_directory_path() /
                           "mega-quantization-tests";
    std::filesystem::create_directories(directory);
    const auto path = directory / name;
    std::ofstream stream(path);
    stream << body;
    return path;
}

std::string ValidManifest(const std::string& provenance = "diagnostic-heuristic",
                          const std::string& digest = "*") {
    return R"JSON({
  "schema_version": 1,
  "manifest_version": "fixture-v1",
  "dataset": "test",
  "model": "gcn",
  "graph_digest": ")JSON" + digest + R"JSON(",
  "provenance": ")JSON" + provenance + R"JSON(",
  "layers": [
    {
      "layer": 0,
      "feature_count": 4,
      "output_features": 3,
      "feature_density": 0.5,
      "weight_bits": 4,
      "weight_scales": [0.25, 0.5, 1.0],
      "degree_rules": [
        {"min_degree": 0, "max_degree": 2, "bitwidth": 2, "scale": 0.5},
        {"min_degree": 3, "max_degree": "max", "bitwidth": 4, "scale": 0.25}
      ]
    }
  ]
})JSON";
}

void TestQuantizationManifest() {
    const auto diagnostic_path = WriteManifest(ValidManifest(), "diagnostic.json");
    const auto diagnostic = mega::QuantizationManifest::Load(diagnostic_path.string());
    diagnostic.ValidateFor("test", "gcn", "fixture-graph");
    Check(!diagnostic.EligibleForRequiredBenchmark(),
          "diagnostic manifest is excluded from required benchmarks");
    Check(diagnostic.Layer(0).RuleForDegree(0).bitwidth == 2 &&
              diagnostic.Layer(0).RuleForDegree(99).bitwidth == 4,
          "degree rules select the expected bitwidths");

    const auto trained_path = WriteManifest(
        ValidManifest("locally-trained", "fixture-graph"), "trained.json");
    const auto trained = mega::QuantizationManifest::Load(trained_path.string());
    trained.ValidateFor("test", "gcn", "fixture-graph");
    Check(trained.EligibleForRequiredBenchmark(),
          "locally trained manifest is eligible for required benchmarks");
    ExpectThrows([&] { trained.ValidateFor("test", "gcn", "other"); },
                 "graph digest mismatch is rejected");

    auto missing = ValidManifest();
    const auto position = missing.find("\"weight_scales\"");
    missing.replace(position, std::string("\"weight_scales\"").size(),
                    "\"missing_weight_scales\"");
    const auto missing_path = WriteManifest(missing, "missing.json");
    ExpectThrows([&] { mega::QuantizationManifest::Load(missing_path.string()); },
                 "missing weight scales are rejected");

    auto invalid_bits = ValidManifest();
    const auto bits_position = invalid_bits.find("\"bitwidth\": 2");
    invalid_bits.replace(bits_position, std::string("\"bitwidth\": 2").size(),
                         "\"bitwidth\": 9");
    const auto invalid_path = WriteManifest(invalid_bits, "invalid-bits.json");
    ExpectThrows([&] { mega::QuantizationManifest::Load(invalid_path.string()); },
                 "feature bitwidth outside [1, 8] is rejected");

    const auto wildcard_required_path = WriteManifest(
        ValidManifest("paper-derived", "*"), "wildcard-required.json");
    ExpectThrows([&] {
        mega::QuantizationManifest::Load(wildcard_required_path.string());
    }, "required-eligible manifest cannot use a wildcard graph digest");
}

void TestReferenceQuantizer() {
    const auto path = WriteManifest(ValidManifest(), "quantizer.json");
    const auto manifest = mega::QuantizationManifest::Load(path.string());
    mega::ReferenceQuantizer quantizer(manifest);
    Check(quantizer.Quantize(0.24, 0, 0) == 0,
          "nearest rounding keeps a sub-half-scale value at zero");
    Check(quantizer.Quantize(0.26, 0, 0) == 1,
          "nearest rounding promotes a value above half scale");
    Check(quantizer.Quantize(20.0, 0, 0) == 1 &&
              quantizer.Quantize(-20.0, 0, 0) == -1,
          "2-bit symmetric quantization saturates at one");
    Check(quantizer.Quantize(-1.5, 0, 4) == -6 &&
              quantizer.Quantize(20.0, 0, 4) == 7,
          "4-bit quantization applies degree-specific scale and saturation");
    Check(quantizer.Dequantize(-6, 0, 4) == -1.5,
          "dequantization applies the selected scale");
    ExpectThrows([&] { quantizer.Dequantize(8, 0, 4); },
                 "dequantization rejects values outside the signed range");
    ExpectThrows([&] { quantizer.Quantize(1.0, 0, -1); },
                 "negative degrees are rejected");
}

void TestAdaptivePackageGolden() {
    const std::vector<mega::QuantizedNode> nodes = {
        {0, 2, {1, -1, 0, 1}},
    };
    const auto encoded = mega::AdaptivePackageCodec::Encode(nodes, 8);
    Check(encoded.packages.size() == 1 && encoded.packages[0].mode == 0 &&
              encoded.packages[0].bitwidth == 2 &&
              encoded.packages[0].bytes.size() == 8,
          "golden vector selects one 64-bit package");
    Check(encoded.packages[0].bytes[0] == 0x0b &&
              encoded.packages[0].bytes[1] == 0xa0,
          "golden package contains the exact header and signed payload bits");
    Check(encoded.bitmap == std::vector<uint8_t>({0xd0}) &&
              encoded.node_value_offsets == std::vector<uint64_t>({0, 3}),
          "golden bitmap and node boundaries are exact");
    Check(encoded.traffic.payload_bits == 6 &&
              encoded.traffic.header_bits == 5 &&
              encoded.traffic.padding_bits == 53 &&
              encoded.traffic.bitmap_bits == 4 &&
              encoded.traffic.boundary_bits == 64 &&
              encoded.traffic.logical_bytes == 17 &&
              encoded.traffic.dram_bytes == 24 &&
              encoded.traffic.dram_transactions == 3,
          "golden traffic accounts for payload, metadata, padding, and alignment");
    Check(mega::AdaptivePackageCodec::Decode(encoded)[0].values == nodes[0].values,
          "golden package decodes exactly");
}

void TestAdaptivePackageMixedRoundTrip() {
    std::vector<int32_t> dense(140, 1);
    std::vector<int32_t> sparse2(140, 0);
    std::vector<int32_t> sparse4(140, 0);
    for (std::size_t index = 0; index < sparse2.size(); index += 7) {
        sparse2[index] = index % 2 == 0 ? 1 : -1;
        sparse4[index] = index % 2 == 0 ? 3 : -3;
    }
    const std::vector<mega::QuantizedNode> nodes = {
        {0, 2, sparse2},
        {1, 2, std::vector<int32_t>(140, 0)},
        {2, 3, dense},
        {3, 4, sparse4},
    };
    const auto encoded64 = mega::AdaptivePackageCodec::Encode(nodes, 64, 7);
    const auto encoded128 = mega::AdaptivePackageCodec::Encode(nodes, 128, 7);
    const auto decoded = mega::AdaptivePackageCodec::Decode(encoded64);
    Check(decoded.size() == nodes.size(), "mixed stream preserves the node count");
    for (std::size_t node = 0; node < nodes.size(); ++node) {
        Check(decoded[node].node_id == nodes[node].node_id &&
                  decoded[node].bitwidth == nodes[node].bitwidth &&
                  decoded[node].values == nodes[node].values,
              "mixed sparse/dense stream round-trips node data");
    }
    bool saw_short = false;
    bool saw_medium = false;
    bool saw_long = false;
    for (const auto& package : encoded64.packages) {
        saw_short |= package.mode == 0;
        saw_medium |= package.mode == 1;
        saw_long |= package.mode == 2;
    }
    Check(saw_short && saw_medium && saw_long,
          "mixed stream exercises all three package modes");
    Check(encoded128.traffic.logical_bytes == encoded64.traffic.logical_bytes &&
              encoded128.traffic.dram_bytes >= encoded64.traffic.dram_bytes &&
              encoded128.traffic.dram_transactions <=
                  encoded64.traffic.dram_transactions,
          "transaction sensitivity preserves logical bytes and recomputes alignment");

    auto truncated = encoded64;
    truncated.packages.front().bytes.pop_back();
    ExpectThrows([&] { mega::AdaptivePackageCodec::Decode(truncated); },
                 "truncated package is rejected");
    auto bad_mode = encoded64;
    bad_mode.packages.front().bytes[0] |= 0xc0;
    ExpectThrows([&] { mega::AdaptivePackageCodec::Decode(bad_mode); },
                 "reserved package mode is rejected");
    auto bad_bitmap = encoded64;
    bad_bitmap.bitmap[0] ^= 0x80;
    ExpectThrows([&] { mega::AdaptivePackageCodec::Decode(bad_bitmap); },
                 "bitmap/boundary mutation is rejected");
    ExpectThrows([] {
        mega::AdaptivePackageCodec::Encode({{0, 2, {2}}}, 64);
    }, "out-of-range signed values are rejected");
}

Graph CondenseFixture() {
    Graph graph;
    graph.num_vertex = 8;
    graph.num_edge = 10;
    graph.num_class = 2;
    graph.len_feature = 4;
    graph.r_adj = {
        {0},
        {0, 1},
        {0, 1, 2},
        {3},
        {0, 4},
        {1, 5},
        {0, 1, 6},
        {7},
    };
    return graph;
}

void TestCondensePlanning() {
    const auto graph = CondenseFixture();
    const auto partition = mega::PartitionManifest::BuildContiguous(
        graph, 4, "fixture-digest");
    partition.Validate(graph, "fixture-digest");
    Check(partition.SubgraphCount() == 2 &&
              !partition.EligibleForRequiredBenchmark(),
          "diagnostic contiguous partition creates two non-required subgraphs");
    ExpectThrows([&] { partition.Validate(graph, "other"); },
                 "partition graph digest mismatch is rejected");
    const auto plan = mega::CondensePlan::Build(graph, partition);
    Check(plan.cross_edges == 4,
          "Condense planner counts every cross-subgraph edge");
    Check(plan.unique_source_references == 2 &&
              plan.subgraphs[1].unique_external_sources ==
                  std::vector<int>({0, 1}),
          "Condense planner deduplicates repeated external sources per target subgraph");
}

void TestCondenseExecution() {
    const auto graph = CondenseFixture();
    const auto partition = mega::PartitionManifest::BuildContiguous(
        graph, 4, "fixture-digest");
    const auto plan = mega::CondensePlan::Build(graph, partition);
    mega::CondenseConfig config;
    config.parallel_fifos = 16;
    config.fifo_entries = 8;
    config.sparse_buffer_bytes = 32 * 1024;
    config.transaction_bytes = 128;
    const std::vector<uint64_t> feature_bytes(graph.num_vertex, 64);
    const auto metrics = mega::CondenseEdgeModel::Run(
        graph, partition, plan, feature_bytes, config);
    Check(metrics.matched_sources == 2 && metrics.matches.size() == 2,
          "Condense executor matches every unique external source once");
    Check(metrics.baseline_cross_read_transactions == 2 &&
              metrics.condensed_cross_read_transactions == 1,
          "paper golden case merges Node0 and Node1 into one 128-byte read");
    Check(metrics.matches[0].sparse_address == 0 &&
              metrics.matches[1].sparse_address == 64,
          "matched features are laid out at contiguous sparse addresses");
    Check(metrics.sparse_spill_write_bytes == 0 &&
              metrics.CondensedDramBytes() == 128,
          "features fitting the Sparse Buffer avoid spill writes");
    Check(metrics.combination_buffer_write_bytes == 8 * 64 &&
              metrics.sparse_buffer_write_bytes == 2 * 64,
          "Combination and Sparse Buffer writes are independently conserved");

    auto spilling = config;
    spilling.sparse_buffer_bytes = 64;
    const auto spilled = mega::CondenseEdgeModel::Run(
        graph, partition, plan, feature_bytes, spilling);
    Check(spilled.sparse_spill_write_transactions > 0 &&
              spilled.CondensedDramBytes() ==
                  spilled.condensed_cross_read_bytes +
                      spilled.sparse_spill_write_bytes,
          "Sparse Buffer overflow produces auditable spill traffic");
}

void TestCondenseFifoRefill() {
    Graph graph;
    graph.num_vertex = 24;
    graph.num_class = 2;
    graph.len_feature = 4;
    graph.r_adj.resize(graph.num_vertex);
    for (int destination = 12; destination < 24; ++destination) {
        for (int source = 0; source < 12; ++source) {
            graph.r_adj[destination].push_back(source);
            ++graph.num_edge;
        }
    }
    const auto partition = mega::PartitionManifest::BuildContiguous(
        graph, 12, "refill-digest");
    const auto plan = mega::CondensePlan::Build(graph, partition);
    mega::CondenseConfig config;
    config.parallel_fifos = 1;
    config.fifo_entries = 8;
    config.sparse_buffer_bytes = 1024;
    config.transaction_bytes = 128;
    const auto metrics = mega::CondenseEdgeModel::Run(
        graph, partition, plan, std::vector<uint64_t>(24, 16), config);
    Check(plan.subgraphs[1].unique_external_sources.size() == 12 &&
              metrics.fifo_refill_entries == 12 &&
              metrics.fifo_refill_cycles == 12,
          "eID lists longer than eight entries refill through the bounded FIFO");
    Check(metrics.compare_cycles == 48,
          "one active FIFO wave compares each source once per target-subgraph wave");
}

void TestMegaCycleModel() {
    const auto manifest_path = WriteManifest(ValidManifest(), "mega-sim.json");
    const auto manifest = mega::QuantizationManifest::Load(manifest_path.string());
    const auto graph = CondenseFixture();
    const auto partition = mega::PartitionManifest::BuildContiguous(
        graph, 4, "fixture-digest");
    const auto architecture = mega::MegaArchitectureConfig::Load(
        "configs/MEGA_SMOKE.ini");
    Check(architecture.TotalBses() == 16 &&
              architecture.TotalBufferBytes() == 32 * 1024,
          "MEGA smoke profile exposes deterministic processing and buffer resources");
    mega::MegaSimulator simulator(architecture);
    const auto baseline = simulator.Run(
        graph, "gcn", "test", "fixture-digest", manifest, partition,
        mega::MegaVariant::M0_FP32_AXW, false);
    const auto bitmap = simulator.Run(
        graph, "gcn", "test", "fixture-digest", manifest, partition,
        mega::MegaVariant::M1_DEGREE_AWARE_BITMAP, false);
    const auto packaged = simulator.Run(
        graph, "gcn", "test", "fixture-digest", manifest, partition,
        mega::MegaVariant::M2_ADAPTIVE_PACKAGE, false);
    const auto condensed = simulator.Run(
        graph, "gcn", "test", "fixture-digest", manifest, partition,
        mega::MegaVariant::M3_CONDENSE_EDGE, false);
    Check(baseline.layers.size() == 1 && condensed.layers.size() == 1,
          "MEGA engine emits the selected manifest layer");
    Check(bitmap.layers[0].input_package_traffic.payload_bits <
              static_cast<uint64_t>(graph.num_vertex) *
                  manifest.Layer(0).feature_count * 32 &&
              bitmap.layers[0].combination_cycles <
                  baseline.layers[0].combination_cycles,
          "degree-aware storage reduces payload bits and bit-serial work");
    Check(packaged.layers[0].input_package_traffic.header_bits > 0 &&
              packaged.layers[0].input_package_traffic.boundary_bits > 0 &&
              packaged.layers[0].input_package_traffic.dram_bytes ==
                  packaged.layers[0].input_dram_bytes,
          "Adaptive-Package accounts for its header and boundary metadata");
    Check(condensed.layers[0].cross_partition_dram_bytes <=
              packaged.layers[0].cross_partition_dram_bytes &&
              condensed.layers[0].condense.condensed_cross_read_transactions <=
                  condensed.layers[0].condense.baseline_cross_read_transactions,
          "Condense-Edge does not increase cross-partition traffic");
    Check(condensed.TotalCycles() < baseline.TotalCycles(),
          "full MEGA reduces small-graph total cycles versus the 32-bit AXW baseline");
    Check(condensed.layers[0].condense.matched_sources == 2 &&
              condensed.layers[0].memory_service_cycles > 0 &&
              condensed.layers[0].total_dram_transactions *
                      architecture.transaction_bytes ==
                  condensed.layers[0].total_dram_bytes,
          "MEGA result retains Condense and HBM service evidence");
    Check(!condensed.required_eligible,
          "diagnostic quantization and partition are not publishable evidence");
    ExpectThrows([&] {
        simulator.Run(graph, "gcn", "test", "fixture-digest", manifest, partition,
                      mega::MegaVariant::M3_CONDENSE_EDGE, true);
    }, "required run rejects diagnostic quantization and partition inputs");
}

void TestMegaSensitivity() {
    auto low_bits_text = ValidManifest();
    const auto second_rule = low_bits_text.find("\"bitwidth\": 4");
    low_bits_text.replace(second_rule, std::string("\"bitwidth\": 4").size(),
                          "\"bitwidth\": 2");
    auto high_bits_text = ValidManifest();
    const auto first_rule = high_bits_text.find("\"bitwidth\": 2");
    high_bits_text.replace(first_rule, std::string("\"bitwidth\": 2").size(),
                           "\"bitwidth\": 4");
    auto sparse_text = low_bits_text;
    const auto density = sparse_text.find("\"feature_density\": 0.5");
    sparse_text.replace(density, std::string("\"feature_density\": 0.5").size(),
                        "\"feature_density\": 0.25");
    const auto low_bits = mega::QuantizationManifest::Load(
        WriteManifest(low_bits_text, "low-bits.json").string());
    const auto high_bits = mega::QuantizationManifest::Load(
        WriteManifest(high_bits_text, "high-bits.json").string());
    const auto sparse = mega::QuantizationManifest::Load(
        WriteManifest(sparse_text, "sparse.json").string());
    const auto graph = CondenseFixture();
    const auto partition = mega::PartitionManifest::BuildContiguous(
        graph, 4, "fixture-digest");
    const auto base_architecture = mega::MegaArchitectureConfig::Load(
        "configs/MEGA_SMOKE.ini");
    const auto low = mega::MegaSimulator(base_architecture).Run(
        graph, "gcn", "test", "fixture-digest", low_bits, partition,
        mega::MegaVariant::M2_ADAPTIVE_PACKAGE, false);
    const auto high = mega::MegaSimulator(base_architecture).Run(
        graph, "gcn", "test", "fixture-digest", high_bits, partition,
        mega::MegaVariant::M2_ADAPTIVE_PACKAGE, false);
    const auto sparse_result = mega::MegaSimulator(base_architecture).Run(
        graph, "gcn", "test", "fixture-digest", sparse, partition,
        mega::MegaVariant::M2_ADAPTIVE_PACKAGE, false);
    Check(low.layers[0].combination_cycles <= high.layers[0].combination_cycles &&
              low.layers[0].input_package_traffic.payload_bits <=
                  high.layers[0].input_package_traffic.payload_bits,
          "lower feature precision cannot increase bit-serial work or payload bits");
    Check(sparse_result.layers[0].nonzero_features <= low.layers[0].nonzero_features &&
              sparse_result.layers[0].combination_cycles <=
                  low.layers[0].combination_cycles,
          "lower feature density cannot increase nonzero work");

    auto slower_memory = base_architecture;
    slower_memory.hbm.hbm_read_row_hit_cycles += 100;
    slower_memory.hbm.hbm_read_row_miss_cycles += 100;
    slower_memory.hbm.hbm_read_row_conflict_cycles += 100;
    slower_memory.hbm.hbm_write_row_hit_cycles += 100;
    slower_memory.hbm.hbm_write_row_miss_cycles += 100;
    slower_memory.hbm.hbm_write_row_conflict_cycles += 100;
    const auto delayed = mega::MegaSimulator(slower_memory).Run(
        graph, "gcn", "test", "fixture-digest", low_bits, partition,
        mega::MegaVariant::M2_ADAPTIVE_PACKAGE, false);
    Check(delayed.layers[0].memory_service_cycles >=
              low.layers[0].memory_service_cycles &&
              delayed.TotalCycles() >= low.TotalCycles(),
          "higher HBM latency cannot reduce consumer completion time");

    auto fewer_units = base_architecture;
    fewer_units.aggregation_units /= 2;
    const auto constrained = mega::MegaSimulator(fewer_units).Run(
        graph, "gcn", "test", "fixture-digest", low_bits, partition,
        mega::MegaVariant::M2_ADAPTIVE_PACKAGE, false);
    Check(constrained.layers[0].aggregation_cycles >=
              low.layers[0].aggregation_cycles,
          "reducing aggregation resources cannot improve aggregation latency");
}

}  // namespace

int main() {
    RunNamedTest("quantization_manifest", TestQuantizationManifest);
    RunNamedTest("reference_quantizer", TestReferenceQuantizer);
    RunNamedTest("adaptive_package_golden", TestAdaptivePackageGolden);
    RunNamedTest("adaptive_package_roundtrip", TestAdaptivePackageMixedRoundTrip);
    RunNamedTest("condense_planning", TestCondensePlanning);
    RunNamedTest("condense_execution", TestCondenseExecution);
    RunNamedTest("condense_fifo_refill", TestCondenseFifoRefill);
    RunNamedTest("mega_cycle_model", TestMegaCycleModel);
    RunNamedTest("mega_sensitivity", TestMegaSensitivity);
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all MEGA unit tests passed\n";
    return 0;
}
