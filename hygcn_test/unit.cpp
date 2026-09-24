#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "dataflow/event.h"
#include "hardware/spm.h"
#include "paper_sim.h"

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

uint64_t Sum(const std::array<uint64_t, 4>& values) {
    return std::accumulate(values.begin(), values.end(), uint64_t{0});
}

Graph SmallGraph() {
    Graph graph;
    graph.num_vertex = 8;
    graph.num_edge = 12;
    graph.num_class = 2;
    graph.len_feature = 16;
    graph.r_adj = {
        {0, 1},
        {1, 3},
        {2, 5},
        {3, 7},
        {0},
        {2},
        {4},
        {6},
    };
    return graph;
}

void TestConfig() {
    const auto paper = ArchitectureConfig::Load("configs/HYGCN_PAPER.ini");
    Check(paper.num_simd == 32, "paper profile has 32 SIMD cores");
    Check(paper.simd_width == 16, "paper profile has SIMD16 cores");
    Check(paper.combination_modules == 8, "paper profile has 8 combination modules");
    Check(paper.arrays_per_module == 4, "paper profile has 4 arrays per module");
    Check(paper.array_width == 128, "paper profile array width is 128");
    Check(paper.aggregation_buffer_bytes == 16ULL * 1024 * 1024,
          "paper profile has 16 MiB aggregation buffer");
    Check(std::abs(paper.HbmBytesPerCycle() - 256.0) < 1e-9,
          "paper profile exposes 256 bytes per cycle HBM bandwidth");

    auto invalid = paper;
    invalid.num_simd = 0;
    ExpectThrows([&] { invalid.Validate(); }, "invalid architecture values are rejected");
    ExpectThrows([] { ArchitectureConfig::Load("configs/does-not-exist.ini"); },
                 "missing architecture config is rejected");
}

void TestPartitionAndSparsity() {
    const auto config = ArchitectureConfig::Load("configs/HYGCN_SMOKE.ini");
    PaperSimulator simulator(config);
    const auto graph = SmallGraph();
    const auto sparse = simulator.BuildShards(graph, graph.len_feature, true);
    const auto dense = simulator.BuildShards(graph, graph.len_feature, false);
    Check(!sparse.empty(), "partition creates shards");
    Check(dense.size() >= sparse.size(),
          "static interval baseline retains at least as many shards as window sliding");
    uint64_t sparse_bytes = 0;
    uint64_t dense_bytes = 0;
    uint64_t edge_bytes = 0;
    for (const auto& shard : sparse) {
        sparse_bytes += shard.input_bytes;
        edge_bytes += shard.edge_bytes;
    }
    for (const auto& shard : dense) {
        dense_bytes += shard.input_bytes;
    }
    Check(sparse_bytes <= dense_bytes, "sparsity elimination does not increase input traffic");
    Check(edge_bytes > 0 && edge_bytes <= config.edge_buffer_bytes,
          "edge chunk uses actual bounded bytes");

    Check(sparse.size() == 1, "small graph produces one golden shard");
    Check(sparse[0].shard_id == 0 && sparse[0].dst_start == 0 && sparse[0].dst_end == 8,
          "golden shard covers expected destination vertices");
    Check(sparse[0].interval_start == 0 && sparse[0].interval_end == 8 &&
              sparse[0].shrunk_interval_end == 8,
          "golden shard records interval and shrinking bounds");
    Check(sparse[0].unique_neighbors.size() == 8 && sparse[0].edge_bytes == 128 &&
              sparse[0].input_bytes == 512,
          "golden shard records exact neighbors and encoded bytes");
}

Graph EdgeBoundaryGraph(int neighbors) {
    Graph graph;
    graph.num_vertex = 8;
    graph.num_edge = neighbors;
    graph.num_class = 2;
    graph.len_feature = 16;
    graph.r_adj.resize(graph.num_vertex);
    for (int source = 0; source < neighbors; ++source) {
        graph.r_adj[0].push_back(source);
    }
    return graph;
}

void TestEdgeChunkBoundaries() {
    auto config = ArchitectureConfig::Load("configs/HYGCN_SMOKE.ini");
    config.block_size = 4;
    config.edge_buffer_bytes = 60;
    config.partition_vertices = 1;
    config.Validate();
    PaperSimulator simulator(config);

    const auto below = simulator.BuildShards(EdgeBoundaryGraph(6), 16, true);
    const auto exact = simulator.BuildShards(EdgeBoundaryGraph(7), 16, true);
    Check(below.front().edge_bytes == 52, "edge chunk below capacity uses actual bytes");
    Check(exact.front().edge_bytes == 60, "edge chunk at capacity uses actual bytes");
    ExpectThrows([&] { simulator.BuildShards(EdgeBoundaryGraph(8), 16, true); },
                 "edge row above capacity is rejected");
}

void TestOutputAddress() {
    const uint64_t first = PaperSimulator::OutputAddress(4096, 3, 0, 512, 64);
    const uint64_t second = PaperSimulator::OutputAddress(4096, 4, 0, 512, 64);
    const uint64_t next_chunk = PaperSimulator::OutputAddress(4096, 3, 1, 512, 64);
    Check(second - first == 512, "consecutive output vertices use the vertex stride");
    Check(next_chunk - first == 64, "output chunks use the chunk stride");
}

void TestSystolicModel() {
    const auto config = ArchitectureConfig::Load("configs/HYGCN_PAPER.ini");
    const auto independent = PaperSimulator::EstimateSystolicCycles(
        257, 1433, 129, config, CombinationMode::INDEPENDENT);
    const auto cooperative = PaperSimulator::EstimateSystolicCycles(
        257, 1433, 129, config, CombinationMode::COOPERATIVE);
    Check(independent > 0, "independent systolic estimate is positive");
    Check(cooperative > 0, "cooperative systolic estimate is positive");
    Check(cooperative <= independent, "cooperative mode benefits from higher modeled utilization");

    auto ideal = config;
    ideal.independent_array_efficiency = 1.0;
    ideal.cooperative_array_efficiency = 1.0;
    const auto hand_computed = PaperSimulator::EstimateSystolicCycles(
        8, 4, 128, ideal, CombinationMode::INDEPENDENT);
    Check(hand_computed == 6,
          "small systolic case matches input, compute, fill, and output cycles");

    const auto non_divisible = PaperSimulator::BuildCombinationSchedule(
        257, 1433, 129, 9, config, CombinationMode::INDEPENDENT);
    Check(non_divisible.row_tiles == 33 && non_divisible.inner_tiles == 359 &&
              non_divisible.column_tiles == 2,
          "combination scheduler tiles non-divisible matrix dimensions");
    Check(non_divisible.active_modules == 8 && non_divisible.batch_waves == 2,
          "independent scheduler respects module concurrency and batch waves");

    auto ideal_schedule_config = ideal;
    const auto independent_schedule = PaperSimulator::BuildCombinationSchedule(
        8, 4, 128, 8, ideal_schedule_config, CombinationMode::INDEPENDENT);
    const auto cooperative_schedule = PaperSimulator::BuildCombinationSchedule(
        8, 4, 128, 8, ideal_schedule_config, CombinationMode::COOPERATIVE);
    Check(independent_schedule.weight_load_bytes == 8 * 2048 &&
              independent_schedule.weight_cascade_bytes == 0,
          "independent hand case loads one weight copy per active module");
    Check(cooperative_schedule.weight_load_bytes == 2048 &&
              cooperative_schedule.weight_cascade_bytes == 7 * 2048 &&
              cooperative_schedule.output_columns_per_module == 16,
          "cooperative hand case records one HBM load and seven cascades");
    Check(independent_schedule.mac_operations == cooperative_schedule.mac_operations &&
              independent_schedule.total_cycles == cooperative_schedule.total_cycles,
          "ideal hand case preserves work across combination policies");
}

void TestHbmLayoutAndMapping() {
    const auto layout = PaperSimulator::BuildHbmLayout(128, 256, 64, 192, 32, 672);
    Check(layout.input.start == 0 && layout.input.End() == layout.output.start,
          "HBM input and output regions are contiguous and non-overlapping");
    Check(layout.output.End() == layout.weight.start &&
              layout.weight.End() == layout.edge.start &&
              layout.edge.End() == layout.intermediate.start &&
              layout.intermediate.End() == 672,
          "all HBM regions are ordered without overlap");
    ExpectThrows([] { PaperSimulator::BuildHbmLayout(128, 256, 64, 192, 33, 672); },
                 "HBM layout rejects a one-byte capacity overflow");

    const std::vector<MemoryRequest> requests = {
        {0, RequestClass::EDGE, 64, 0, 0, 0},
        {0, RequestClass::INPUT, 128, 64, 0, 1},
        {0, RequestClass::OUTPUT, 64, 192, 0, 2},
    };
    const auto distribution = PaperSimulator::MapAddressDistribution(requests, 64, 2, 2);
    Check(distribution.channel_blocks == std::vector<uint64_t>({2, 2}),
          "synthetic address stream maps evenly across two channels");
    Check(distribution.bank_blocks == std::vector<uint64_t>({2, 2}),
          "synthetic address stream maps evenly across two banks");
}

void TestCoordinator() {
    std::vector<MemoryRequest> requests = {
        {1, RequestClass::EDGE, 64, 0, 0, 3},
        {0, RequestClass::OUTPUT, 64, 0, 0, 2},
        {0, RequestClass::INPUT, 64, 0, 0, 1},
        {0, RequestClass::EDGE, 64, 0, 0, 0},
    };
    const auto ordered = MemoryCoordinatorModel::Order(requests, true);
    Check(ordered[0].batch_id == 0 && ordered[0].request_class == RequestClass::EDGE,
          "coordinator prioritizes edge inside the oldest batch");
    Check(ordered[1].batch_id == 0 && ordered[1].request_class == RequestClass::INPUT,
          "coordinator prioritizes input after edge");
    Check(ordered[2].batch_id == 0 && ordered[2].request_class == RequestClass::OUTPUT,
          "current batch output precedes next batch edge");

    const auto uncoordinated = MemoryCoordinatorModel::Order(requests, false);
    Check(uncoordinated[0].request_class == RequestClass::EDGE &&
              uncoordinated[1].request_class == RequestClass::EDGE,
          "uncoordinated baseline uses global class priority");
    ExpectThrows([] {
        MemoryCoordinatorModel::Order({{0, RequestClass::EDGE, 0, 0, 0, 0}}, true);
    }, "zero-byte memory requests are rejected");
}

void TestAggregationBuffer() {
    AggregationBufferModel buffer(100);
    Check(buffer.Allocate(0, 60) == 0, "aggregation buffer allocates from the head");
    Check(buffer.Allocate(1, 30) == 60, "aggregation buffer advances allocation pointer");
    Check(!buffer.CanAllocate(20), "aggregation buffer blocks over-capacity allocation");
    buffer.MarkReady(0);
    buffer.StartConsume(0);
    buffer.Reclaim(0);
    Check(buffer.Allocate(2, 40) == 0, "aggregation buffer wraps after reclaim");
    Check(buffer.PeakBytes() == 90, "aggregation buffer records peak occupancy");
    ExpectThrows([&] { buffer.Reclaim(2); }, "aggregation buffer enforces FIFO reclaim");
    buffer.MarkReady(1);
    buffer.StartConsume(1);
    buffer.Reclaim(1);
    buffer.MarkReady(2);
    buffer.StartConsume(2);
    buffer.Reclaim(2);
    Check(buffer.UsedBytes() == 0 && buffer.Segments().empty(),
          "aggregation buffer reclaims all consumed segments");
    ExpectThrows([] { AggregationBufferModel invalid(0); },
                 "zero-sized aggregation buffer is rejected");
}

void TestEventAndSpmGuards() {
    Event::sz_block = 64;
    uint64_t address = 64;
    Event event(address, 128, EventDirType::DRAM, EventType::READ,
                EventDataType::INPUT_FEATURE);
    event.ValidateAddressRange(192);
    ExpectThrows([&] { event.ValidateAddressRange(191); },
                 "event address range overflow is rejected");
    ExpectThrows([&] { event.ValidateComplete(); },
                 "unfinished event transactions are detected");
    event.IssueTrans();
    event.ConfirmTrans();
    event.IssueTrans();
    event.ConfirmTrans();
    event.ValidateComplete();
    ExpectThrows([&] { event.IssueTrans(); }, "event issue overflow is rejected");
    ExpectThrows([&] { event.ConfirmTrans(); }, "event confirmation overflow is rejected");

    uint64_t second_address = 0;
    Event second(second_address, 64, EventDirType::DRAM, EventType::READ,
                 EventDataType::EDGE);
    ExpectThrows([&] { second.ConfirmTrans(); },
                 "event cannot confirm an unissued transaction");

    SPM spm(64);
    ExpectThrows([&] { spm.AddEvents(-1); }, "SPM rejects negative event counts");
    ExpectThrows([&] { spm.ConfirmEvent(); }, "SPM rejects event counter underflow");
    SPMA spma(64);
    ExpectThrows([&] { spma.AddEvents(-1); }, "SPMA rejects negative event counts");
    ExpectThrows([&] { spma.ConfirmEvent(); }, "SPMA rejects event counter underflow");
}

void TestAggregationOperations() {
    const auto config = ArchitectureConfig::Load("configs/HYGCN_SMOKE.ini");
    PaperSimulator simulator(config);
    FeatureFlags flags;
    const auto graph = SmallGraph();
    const auto sum = simulator.Run(graph, "gcn", "small", 7, flags);
    const auto maximum = simulator.Run(graph, "gs", "small", 7, flags);
    Check(sum.layers[0].add_operations > 0 && sum.layers[0].compare_operations == 0,
          "GCN records SUM aggregation operations");
    Check(maximum.layers[0].compare_operations > 0 && maximum.layers[0].add_operations == 0,
          "GraphSAGE records MAX aggregation operations");
    Check(sum.layers[0].aggregation_op == AggregationOp::SUM &&
              maximum.layers[0].aggregation_op == AggregationOp::MAX,
          "aggregation cycle model records the selected operation type");
    Check(maximum.layers[0].mac_operations < sum.layers[0].mac_operations,
          "MAX aggregation does not count aggregation work as MACs");
}

void TestSimulationDeterminism() {
    const auto config = ArchitectureConfig::Load("configs/HYGCN_SMOKE.ini");
    PaperSimulator simulator(config);
    FeatureFlags flags;
    const auto graph = SmallGraph();
    const auto first = simulator.Run(graph, "gcn", "small", 7, flags);
    const auto second = simulator.Run(graph, "gcn", "small", 7, flags);
    Check(first.TotalCycles() == second.TotalCycles(), "same seed produces identical cycles");
    Check(first.TotalDramBytes() == second.TotalDramBytes(),
          "same seed produces identical DRAM traffic");
    Check(first.layers.size() == 2, "GCN simulation emits two layers");
    for (const auto& layer : first.layers) {
        Check(layer.cycles > 0, "layer cycles are positive");
        Check(layer.TotalDramBytes() > 0, "layer DRAM traffic is positive");
        Check(layer.simd_utilization >= 0.0 && layer.simd_utilization <= 1.0,
              "SIMD utilization is bounded");
        Check(layer.array_utilization >= 0.0 && layer.array_utilization <= 1.0,
              "array utilization is bounded");
        Check(Sum(layer.request_bytes) == layer.TotalDramBytes(),
              "layer request bytes conserve total DRAM traffic");
        Check(layer.aggregation_buffer_read_bytes == layer.aggregation_buffer_write_bytes,
              "aggregation buffer reads and writes are conserved");
        Check(layer.ae_finish_cycle <= layer.cycles && layer.ce_finish_cycle <= layer.cycles,
              "engine finish cycles remain inside the layer runtime");
        Check(layer.channel_blocks.size() == 8 && layer.bank_blocks.size() == 16,
              "HBM mapping distributions are reported");
    }

    const auto selected = simulator.Run(graph, "gcn", "small", 7, flags, 1);
    Check(selected.layers.size() == 1 && selected.layers[0].layer == 1,
          "selected layer execution emits only the requested layer");
    ExpectThrows([&] { simulator.Run(graph, "gcn", "small", 7, flags, 2); },
                 "out-of-range layer selection is rejected");
}

void TestPolicyMatrix() {
    const auto config = ArchitectureConfig::Load("configs/HYGCN_SMOKE.ini");
    PaperSimulator simulator(config);
    const auto graph = SmallGraph();
    std::vector<ExperimentResult> results;
    for (const auto pipeline : {PipelineMode::SEQUENTIAL, PipelineMode::LATENCY_AWARE,
                                PipelineMode::ENERGY_AWARE}) {
        for (const auto combination : {CombinationMode::INDEPENDENT,
                                       CombinationMode::COOPERATIVE}) {
            FeatureFlags flags;
            flags.pipeline = pipeline;
            flags.combination = combination;
            auto result = simulator.Run(graph, "gcn", "small", 11, flags);
            Check(result.TotalCycles() > 0 && result.layers.size() == 2,
                  "all pipeline and combination policy pairs complete");
            for (const auto& layer : result.layers) {
                Check(Sum(layer.request_bytes) == layer.TotalDramBytes(),
                      "policy pair preserves traffic conservation");
                if (pipeline == PipelineMode::SEQUENTIAL) {
                    Check(layer.ce_start_cycle >= layer.ae_finish_cycle,
                          "sequential CE starts after AE finishes");
                } else {
                    Check(layer.ce_start_cycle <= layer.ae_finish_cycle,
                          "pipelined CE starts no later than AE completion");
                }
            }
            results.push_back(std::move(result));
        }
    }

    FeatureFlags independent_flags;
    independent_flags.combination = CombinationMode::INDEPENDENT;
    FeatureFlags cooperative_flags;
    cooperative_flags.combination = CombinationMode::COOPERATIVE;
    const auto independent = simulator.Run(graph, "gcn", "small", 13, independent_flags);
    const auto cooperative = simulator.Run(graph, "gcn", "small", 13, cooperative_flags);
    for (std::size_t layer = 0; layer < independent.layers.size(); ++layer) {
        Check(independent.layers[layer].mac_operations == cooperative.layers[layer].mac_operations,
              "combination policies preserve effective MAC operations");
        Check(independent.layers[layer].output_dram_bytes ==
                  cooperative.layers[layer].output_dram_bytes,
              "combination policies preserve output bytes");
        Check(independent.layers[layer].weight_dram_bytes >=
                  cooperative.layers[layer].weight_dram_bytes,
              "cooperative mode does not increase weight traffic");
    }
}

void TestFeatureDispersionAndAddressBounds() {
    const auto base = ArchitectureConfig::Load("configs/HYGCN_PAPER.ini");
    PaperSimulator simulator(base);
    FeatureFlags flags;
    auto low = SmallGraph();
    low.len_feature = 4;
    auto high = SmallGraph();
    high.len_feature = 1024;
    const auto low_result = simulator.Run(low, "gcn", "low", 1, flags, 0);
    const auto high_result = simulator.Run(high, "gcn", "high", 1, flags, 0);
    Check(high_result.layers[0].aggregation_compute_cycles >
              low_result.layers[0].aggregation_compute_cycles,
          "high-dimensional features distribute across more SIMD work");
    Check(high_result.layers[0].simd_utilization > low_result.layers[0].simd_utilization,
          "Vertex-Disperse improves lane use on high-dimensional features");
    Check(high_result.layers[0].simd_cores_per_vertex == 32 &&
              high_result.layers[0].simd_parallel_vertices == 1,
          "high-dimensional vertex disperses across all 32 SIMD16 cores");
    Check(low_result.layers[0].simd_cores_per_vertex == 1 &&
              low_result.layers[0].simd_parallel_vertices == 32,
          "low-dimensional workload executes multiple vertices in parallel");

    auto tiny_hbm = base;
    tiny_hbm.hbm_capacity_bytes = 1024;
    PaperSimulator bounded(tiny_hbm);
    ExpectThrows([&] { bounded.Run(SmallGraph(), "gcn", "small", 1, flags); },
                 "overlapping or out-of-range HBM regions are rejected");
}

void TestPipelineBatching() {
    const auto config = ArchitectureConfig::Load("configs/HYGCN_SMOKE.ini");
    PaperSimulator simulator(config);
    Graph graph;
    graph.num_vertex = 40;
    graph.num_edge = 40;
    graph.num_class = 2;
    graph.len_feature = 16;
    graph.r_adj.resize(graph.num_vertex);
    for (int vertex = 0; vertex < graph.num_vertex; ++vertex) {
        graph.r_adj[vertex] = {vertex};
    }
    FeatureFlags latency;
    latency.pipeline = PipelineMode::LATENCY_AWARE;
    FeatureFlags energy;
    energy.pipeline = PipelineMode::ENERGY_AWARE;
    FeatureFlags sequential;
    sequential.pipeline = PipelineMode::SEQUENTIAL;
    const auto latency_result = simulator.Run(graph, "gcn", "batch", 1, latency, 0);
    const auto energy_result = simulator.Run(graph, "gcn", "batch", 1, energy, 0);
    const auto sequential_result = simulator.Run(graph, "gcn", "batch", 1, sequential, 0);
    Check(latency_result.layers[0].batches > energy_result.layers[0].batches,
          "latency-aware starts CE from smaller legal batches");
    Check(energy_result.layers[0].batches >= sequential_result.layers[0].batches,
          "energy-aware batches remain within buffer capacity");
    Check(sequential_result.layers[0].ce_start_cycle >=
              sequential_result.layers[0].ae_finish_cycle,
          "sequential pipeline waits for AE completion");
}

}  // namespace

int main() {
    try {
        TestConfig();
        TestPartitionAndSparsity();
        TestEdgeChunkBoundaries();
        TestOutputAddress();
        TestSystolicModel();
        TestHbmLayoutAndMapping();
        TestCoordinator();
        TestAggregationBuffer();
        TestEventAndSpmGuards();
        TestAggregationOperations();
        TestSimulationDeterminism();
        TestPolicyMatrix();
        TestFeatureDispersionAndAddressBounds();
        TestPipelineBatching();
    } catch (const std::exception& error) {
        std::cerr << "UNCAUGHT: " << error.what() << '\n';
        return 2;
    }
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all unit tests passed\n";
    return 0;
}
