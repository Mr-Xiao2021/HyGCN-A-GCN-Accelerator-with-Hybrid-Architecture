#ifndef HYGCN_PAPER_SIM_H
#define HYGCN_PAPER_SIM_H

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "graph.h"

enum class PipelineMode {
    SEQUENTIAL,
    LATENCY_AWARE,
    ENERGY_AWARE,
};

enum class CombinationMode {
    INDEPENDENT,
    COOPERATIVE,
};

enum class AggregationOp {
    SUM,
    MAX,
};

struct ArchitectureConfig {
    std::string profile;
    double frequency_ghz = 0.0;
    int block_size = 0;
    int num_simd = 0;
    int simd_width = 0;
    int combination_modules = 0;
    int arrays_per_module = 0;
    int array_width = 0;

    uint64_t input_buffer_bytes = 0;
    uint64_t edge_buffer_bytes = 0;
    uint64_t weight_buffer_bytes = 0;
    uint64_t output_buffer_bytes = 0;
    uint64_t aggregation_buffer_bytes = 0;

    uint64_t hbm_capacity_bytes = 0;
    double hbm_bandwidth_gbps = 0.0;
    double coordinated_efficiency = 0.0;
    double uncoordinated_efficiency = 0.0;
    int edram_latency_cycles = 0;
    int edram_transactions_per_cycle = 0;

    double simd_efficiency = 0.0;
    double independent_array_efficiency = 0.0;
    double cooperative_array_efficiency = 0.0;
    double memory_hidden_fraction = 0.0;
    double latency_overlap_fraction = 0.0;
    double energy_overlap_fraction = 0.0;
    int energy_batch_vertices = 0;
    int partition_vertices = 0;
    double sequential_spill_factor = 0.0;

    static ArchitectureConfig Load(const std::string& path);
    void Validate() const;
    double HbmBytesPerCycle() const;
};

struct FeatureFlags {
    bool sparsity_elimination = true;
    bool memory_coordination = true;
    PipelineMode pipeline = PipelineMode::LATENCY_AWARE;
    CombinationMode combination = CombinationMode::INDEPENDENT;
};

struct LayerShape {
    int input_features = 0;
    int output_features = 0;
};

struct EdgeShard {
    int shard_id = 0;
    int batch_id = 0;
    int dst_start = 0;
    int dst_end = 0;
    int interval_start = 0;
    int interval_end = 0;
    int shrunk_interval_end = 0;
    uint64_t edge_bytes = 0;
    uint64_t input_bytes = 0;
    std::vector<int> unique_neighbors;
};

enum class RequestClass {
    EDGE = 0,
    INPUT = 1,
    WEIGHT = 2,
    OUTPUT = 3,
};

struct MemoryRequest {
    int batch_id = 0;
    RequestClass request_class = RequestClass::EDGE;
    uint64_t bytes = 0;
    uint64_t address = 0;
    uint64_t enqueue_cycle = 0;
    uint64_t sequence = 0;
};

struct AddressDistribution {
    std::vector<uint64_t> channel_blocks;
    std::vector<uint64_t> bank_blocks;
};

struct HbmRegion {
    uint64_t start = 0;
    uint64_t bytes = 0;

    uint64_t End() const;
};

struct HbmLayout {
    HbmRegion input;
    HbmRegion output;
    HbmRegion weight;
    HbmRegion edge;
    HbmRegion intermediate;
};

struct CombinationSchedule {
    uint64_t row_tiles = 0;
    uint64_t inner_tiles = 0;
    uint64_t column_tiles = 0;
    uint64_t active_modules = 0;
    uint64_t batch_waves = 0;
    uint64_t weight_load_bytes = 0;
    uint64_t weight_cascade_bytes = 0;
    uint64_t output_columns_per_module = 0;
    uint64_t mac_operations = 0;
    uint64_t input_advance_cycles = 0;
    uint64_t pipeline_fill_cycles = 0;
    uint64_t compute_cycles = 0;
    uint64_t output_write_cycles = 0;
    uint64_t total_cycles = 0;
};

enum class BufferSegmentState {
    ALLOCATED,
    READY,
    CONSUMING,
};

struct BufferSegment {
    int batch_id = 0;
    uint64_t offset = 0;
    uint64_t bytes = 0;
    BufferSegmentState state = BufferSegmentState::ALLOCATED;
};

class AggregationBufferModel {
public:
    explicit AggregationBufferModel(uint64_t capacity_bytes);

    bool CanAllocate(uint64_t bytes) const;
    uint64_t Allocate(int batch_id, uint64_t bytes);
    void MarkReady(int batch_id);
    void StartConsume(int batch_id);
    void Reclaim(int batch_id);

    uint64_t CapacityBytes() const;
    uint64_t UsedBytes() const;
    uint64_t PeakBytes() const;
    const std::deque<BufferSegment>& Segments() const;

private:
    BufferSegment& Find(int batch_id);
    uint64_t FindAllocationOffset(uint64_t bytes) const;

    uint64_t capacity_bytes_ = 0;
    uint64_t allocation_head_ = 0;
    uint64_t used_bytes_ = 0;
    uint64_t peak_bytes_ = 0;
    std::deque<BufferSegment> segments_;
};

class MemoryCoordinatorModel {
public:
    static std::vector<MemoryRequest> Order(std::vector<MemoryRequest> requests,
                                            bool coordinated);
};

struct LayerMetrics {
    int layer = 0;
    int input_features = 0;
    int output_features = 0;
    AggregationOp aggregation_op = AggregationOp::SUM;
    uint64_t cycles = 0;
    uint64_t aggregation_cycles = 0;
    uint64_t aggregation_compute_cycles = 0;
    uint64_t aggregation_memory_cycles = 0;
    uint64_t combination_cycles = 0;
    uint64_t combination_weight_load_cycles = 0;
    uint64_t combination_input_cycles = 0;
    uint64_t combination_pipeline_fill_cycles = 0;
    uint64_t combination_compute_cycles = 0;
    uint64_t combination_output_cycles = 0;
    uint64_t combination_active_modules = 0;
    uint64_t combination_batch_waves = 0;
    uint64_t combination_output_columns_per_module = 0;
    uint64_t weight_cascade_bytes = 0;
    uint64_t memory_service_cycles = 0;
    uint64_t queue_wait_cycles = 0;
    uint64_t hbm_blocked_cycles = 0;
    uint64_t ae_finish_cycle = 0;
    uint64_t ce_start_cycle = 0;
    uint64_t ce_finish_cycle = 0;
    uint64_t edge_dram_bytes = 0;
    uint64_t input_dram_bytes = 0;
    uint64_t weight_dram_bytes = 0;
    uint64_t output_dram_bytes = 0;
    uint64_t intermediate_dram_bytes = 0;
    uint64_t aggregation_buffer_read_bytes = 0;
    uint64_t aggregation_buffer_write_bytes = 0;
    uint64_t mac_operations = 0;
    uint64_t add_operations = 0;
    uint64_t compare_operations = 0;
    uint64_t skipped_input_vertices = 0;
    uint64_t requested_input_vertices = 0;
    uint64_t shards = 0;
    uint64_t batches = 0;
    uint64_t aggregation_buffer_peak_bytes = 0;
    uint64_t aggregation_buffer_capacity_stalls = 0;
    uint64_t simd_idle_lane_cycles = 0;
    uint64_t simd_feature_chunks = 0;
    uint64_t simd_cores_per_vertex = 0;
    uint64_t simd_parallel_vertices = 0;
    uint64_t array_idle_lane_cycles = 0;
    std::array<uint64_t, 4> request_counts{};
    std::array<uint64_t, 4> request_bytes{};
    std::array<uint64_t, 4> request_wait_cycles{};
    std::vector<uint64_t> channel_blocks;
    std::vector<uint64_t> bank_blocks;
    double simd_utilization = 0.0;
    double array_utilization = 0.0;
    double bandwidth_utilization = 0.0;
    double channel_imbalance = 0.0;
    double bank_imbalance = 0.0;

    uint64_t TotalDramBytes() const;
};

struct ExperimentResult {
    std::string model;
    std::string dataset;
    std::string profile;
    std::string graph_digest;
    std::string config_digest;
    std::string binary_digest;
    uint64_t seed = 0;
    int selected_layer = -1;
    FeatureFlags flags;
    ArchitectureConfig architecture;
    std::vector<LayerMetrics> layers;

    uint64_t TotalCycles() const;
    uint64_t TotalDramBytes() const;
    uint64_t TotalInputDramBytes() const;
    double BandwidthUtilization() const;
};

class PaperSimulator {
public:
    explicit PaperSimulator(ArchitectureConfig architecture);

    ExperimentResult Run(const Graph& graph,
                         const std::string& model,
                         const std::string& dataset,
                         uint64_t seed,
                         const FeatureFlags& flags,
                         int selected_layer = -1) const;

    std::vector<EdgeShard> BuildShards(const Graph& graph,
                                       int feature_count,
                                       bool sparsity_elimination) const;

    static uint64_t OutputAddress(uint64_t output_start,
                                  int vertex,
                                  int output_chunk,
                                  uint64_t vertex_stride,
                                  uint64_t chunk_stride);

    static uint64_t EstimateSystolicCycles(int rows,
                                           int inner,
                                           int columns,
                                           const ArchitectureConfig& architecture,
                                           CombinationMode mode);

    static CombinationSchedule BuildCombinationSchedule(
        int rows,
        int inner,
        int columns,
        uint64_t batches,
        const ArchitectureConfig& architecture,
        CombinationMode mode);

    static HbmLayout BuildHbmLayout(uint64_t input_bytes,
                                    uint64_t output_bytes,
                                    uint64_t weight_bytes,
                                    uint64_t edge_bytes,
                                    uint64_t intermediate_bytes,
                                    uint64_t capacity_bytes);

    static AddressDistribution MapAddressDistribution(
        const std::vector<MemoryRequest>& requests,
        uint64_t block_size,
        std::size_t channels,
        std::size_t banks);

private:
    LayerMetrics RunLayer(const Graph& graph,
                          const LayerShape& shape,
                          int layer,
                          AggregationOp aggregation_op,
                          const FeatureFlags& flags) const;
    std::vector<LayerShape> GetLayerShapes(const Graph& graph,
                                           const std::string& model) const;
    ArchitectureConfig architecture_;
};

std::string ToString(PipelineMode mode);
std::string ToString(CombinationMode mode);
std::string ToString(AggregationOp operation);
PipelineMode ParsePipelineMode(const std::string& value);
CombinationMode ParseCombinationMode(const std::string& value);
bool ParseToggle(const std::string& value);

std::string DigestFile(const std::string& path);
void WriteExperimentJson(const ExperimentResult& result, const std::string& path);
void WriteExperimentCsv(const ExperimentResult& result, const std::string& path);

#endif
