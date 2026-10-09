#ifndef MEGA_SIM_H
#define MEGA_SIM_H

#include <cstdint>
#include <string>
#include <vector>

#include "adaptive_package.h"
#include "condense_edge.h"
#include "paper_sim.h"
#include "quantization.h"

namespace mega {

enum class MegaVariant {
    M0_FP32_AXW,
    M1_DEGREE_AWARE_BITMAP,
    M2_ADAPTIVE_PACKAGE,
    M3_CONDENSE_EDGE,
};

struct MegaArchitectureConfig {
    std::string profile;
    double frequency_ghz = 0.0;
    std::string hbm_profile_path;
    uint64_t transaction_bytes = 0;
    int combination_tiles = 0;
    int cpes_per_tile = 0;
    int bses_per_cpe = 0;
    int aggregation_units = 0;
    int encoder_qn_units = 0;
    int decoder_values_per_cycle = 0;
    uint64_t input_buffer_bytes = 0;
    uint64_t edge_buffer_bytes = 0;
    uint64_t weight_buffer_bytes = 0;
    uint64_t combination_buffer_bytes = 0;
    uint64_t aggregation_buffer_bytes = 0;
    uint64_t sparse_buffer_bytes = 0;
    int parallel_fifos = 0;
    int fifo_entries = 0;
    int partition_vertices = 0;
    ArchitectureConfig hbm;

    static MegaArchitectureConfig Load(const std::string& path);
    void Validate() const;
    uint64_t TotalBufferBytes() const;
    uint64_t TotalBses() const;
};

struct MegaLayerMetrics {
    int layer = 0;
    int input_features = 0;
    int output_features = 0;
    uint64_t vertices = 0;
    uint64_t edges = 0;
    uint64_t nonzero_features = 0;
    uint64_t feature_bit_work = 0;
    double average_feature_bits = 0.0;
    uint64_t input_logical_bytes = 0;
    uint64_t weight_logical_bytes = 0;
    uint64_t edge_logical_bytes = 0;
    uint64_t cross_partition_logical_bytes = 0;
    uint64_t output_logical_bytes = 0;
    uint64_t total_logical_bytes = 0;
    uint64_t input_dram_bytes = 0;
    uint64_t weight_dram_bytes = 0;
    uint64_t edge_dram_bytes = 0;
    uint64_t cross_partition_dram_bytes = 0;
    uint64_t output_dram_bytes = 0;
    uint64_t total_dram_bytes = 0;
    uint64_t input_dram_transactions = 0;
    uint64_t weight_dram_transactions = 0;
    uint64_t edge_dram_transactions = 0;
    uint64_t cross_partition_dram_transactions = 0;
    uint64_t output_dram_transactions = 0;
    uint64_t total_dram_transactions = 0;
    uint64_t preload_memory_cycles = 0;
    uint64_t cross_partition_memory_cycles = 0;
    uint64_t output_memory_cycles = 0;
    uint64_t decoder_cycles = 0;
    uint64_t combination_cycles = 0;
    uint64_t condense_cycles = 0;
    uint64_t aggregation_cycles = 0;
    uint64_t encoder_cycles = 0;
    uint64_t memory_service_cycles = 0;
    uint64_t total_cycles = 0;
    PackageTraffic input_package_traffic;
    PackageTraffic output_package_traffic;
    CondenseMetrics condense;
};

struct MegaExperimentResult {
    std::string model;
    std::string dataset;
    std::string profile;
    std::string binary_digest;
    std::string graph_digest;
    std::string config_digest;
    std::string quantization_digest;
    std::string quantization_manifest_version;
    std::string feature_value_source;
    std::string partition_source;
    std::string partition_manifest_version;
    std::string partition_parameters;
    std::string partition_digest;
    MegaVariant variant = MegaVariant::M3_CONDENSE_EDGE;
    QuantizationProvenance quantization_provenance =
        QuantizationProvenance::DIAGNOSTIC_HEURISTIC;
    bool required_eligible = false;
    MegaArchitectureConfig architecture;
    std::vector<MegaLayerMetrics> layers;

    uint64_t TotalCycles() const;
    uint64_t TotalDramBytes() const;
    uint64_t TotalDramTransactions() const;
};

class MegaSimulator {
public:
    explicit MegaSimulator(MegaArchitectureConfig architecture);

    MegaExperimentResult Run(const Graph& graph,
                             const std::string& model,
                             const std::string& dataset,
                             const std::string& graph_digest,
                             const QuantizationManifest& quantization,
                             const PartitionManifest& partition,
                             MegaVariant variant,
                             bool require_publishable,
                             int selected_layer = -1) const;

private:
    MegaLayerMetrics RunLayer(const Graph& graph,
                              const LayerQuantization& quantization,
                              const PartitionManifest& partition,
                              int layer,
                              MegaVariant variant) const;
    MegaArchitectureConfig architecture_;
};

std::string ToString(MegaVariant variant);
MegaVariant ParseMegaVariant(const std::string& value);
void WriteMegaJson(const MegaExperimentResult& result, const std::string& path);
void WriteMegaCsv(const MegaExperimentResult& result, const std::string& path);

}  // namespace mega

#endif
