#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
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

template <std::size_t Size>
uint64_t Sum(const std::array<uint64_t, Size>& values) {
    return std::accumulate(values.begin(), values.end(), uint64_t{0});
}

template <typename Function>
void RunNamedTest(const std::string& name, Function function) {
    const int failures_before = failures;
    function();
    std::cout << name << '=' << (failures == failures_before ? "PASS" : "FAIL") << '\n';
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
    Check(paper.InputWindowCapacityBytes() == 64ULL * 1024,
          "paper input ping-pong window exposes 64 KiB per resident window");
    Check(paper.EdgeShardCapacityBytes() == 1ULL * 1024 * 1024,
          "paper edge ping-pong region exposes 1 MiB per resident shard");
    Check(paper.AggregationShardCapacityBytes() == 5ULL * 1024 * 1024,
          "paper graph partition exposes the audited 5 MiB shard cap");
    Check(paper.input_ping_pong_regions == 2 && paper.edge_ping_pong_regions == 2 &&
              paper.aggregation_ping_pong_regions == 2,
          "paper profile audits all ping-pong region counts");
    Check(paper.aggregation_shard_capacity_bytes == 5ULL * 1024 * 1024 &&
              paper.row_first_bank_interleave == 2,
          "paper profile audits aggregation partitioning and row-first bank lanes");
    Check(paper.batch_launch_interval_cycles == 1 &&
              paper.neighbor_index_ready_cycles == 2 &&
              paper.sequential_spill_alignment == "block",
          "paper profile audits request release and spill behavior");
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

    auto non_contiguous = SmallGraph();
    non_contiguous.num_edge = non_contiguous.num_vertex * 4;
    for (auto& adjacency : non_contiguous.r_adj) {
        adjacency = {0, 2, 4, 6};
    }
    FeatureFlags sparse_flags;
    sparse_flags.aggregation_only = true;
    FeatureFlags dense_flags;
    dense_flags.aggregation_only = true;
    dense_flags.sparsity_elimination = false;
    const auto sparse_result = simulator.Run(
        non_contiguous, "gcn", "non-contiguous", 1, sparse_flags, 0);
    const auto dense_result = simulator.Run(
        non_contiguous, "gcn", "non-contiguous", 1, dense_flags, 0);
    const auto input_index = static_cast<std::size_t>(RequestClass::INPUT);
    Check(sparse_result.layers[0].request_counts[input_index] == 1,
          "non-contiguous neighbors issue one continuous shrunk window request");
    Check(dense_result.layers[0].request_counts[input_index] == 1,
          "dense interval baseline issues one contiguous input request");
    Check(sparse_result.layers[0].input_dram_bytes == 7 * 64,
          "window shrinking retains holes between first and last referenced vertices");
    Check(sparse_result.layers[0].request_bytes[input_index] ==
              sparse_result.layers[0].input_dram_bytes,
          "sparse input request addresses and traffic use the same byte accounting");
    Check(sparse_result.layers[0].input_window_traces.size() == 1,
          "golden sparse graph records one continuous window trace");
    const auto& window = sparse_result.layers[0].input_window_traces.front();
    Check(window.window_start == 0 && window.window_end == 7 &&
              window.interval_start == 0 && window.interval_end == 8,
          "golden sparse window shrinks {0,2,4,6} to [0,7)");
    Check(window.address == 0 && window.bytes == 448 && window.transactions == 7,
          "golden sparse window records address, 448-byte span, and seven transactions");
    Check(window.unique_vertices == 4 && window.internal_holes == 3,
          "golden sparse window preserves three internal holes");
    Check(sparse_result.layers[0].weight_dram_bytes == 0 &&
              sparse_result.layers[0].output_dram_bytes == 0 &&
              sparse_result.layers[0].intermediate_dram_bytes == 0 &&
              sparse_result.layers[0].ce_start_cycle == 0 &&
              sparse_result.layers[0].ce_finish_cycle == 0,
          "aggregation-only scope excludes Weight, Output, intermediate traffic, and CE");
    std::cout << "window_evidence={\"neighbors\":[0,2,4,6],\"window\":[0,7],"
              << "\"address\":" << window.address << ",\"bytes\":" << window.bytes
              << ",\"internal_holes\":" << window.internal_holes
              << ",\"transactions\":" << window.transactions << "}\n";
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
    config.edge_buffer_bytes = 120;
    config.aggregation_buffer_bytes = 256;
    config.aggregation_shard_capacity_bytes = 64;
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
    const auto one_batch_parallel = PaperSimulator::BuildCombinationSchedule(
        64, 128, 128, 1, ideal_schedule_config, CombinationMode::INDEPENDENT);
    const auto cooperative_schedule = PaperSimulator::BuildCombinationSchedule(
        8, 4, 128, 8, ideal_schedule_config, CombinationMode::COOPERATIVE);
    Check(independent_schedule.weight_load_bytes == 2048 &&
              independent_schedule.weight_cascade_bytes == 0,
          "independent hand case loads weights once from HBM and reuses the weight buffer");
    Check(cooperative_schedule.weight_load_bytes == 2048 &&
              cooperative_schedule.weight_cascade_bytes == 7 * 2048 &&
              cooperative_schedule.output_columns_per_module == 16,
          "cooperative hand case records one HBM load and seven cascades");
    Check(independent_schedule.mac_operations == cooperative_schedule.mac_operations &&
              independent_schedule.total_cycles == cooperative_schedule.total_cycles,
          "ideal hand case preserves work across combination policies");
    Check(one_batch_parallel.active_modules == 8 && one_batch_parallel.batch_waves == 1,
          "one legal batch distributes vertex groups across all eight combination modules");
    std::cout << "combination_parallelism_evidence={\"batches\":1,\"rows\":64,"
              << "\"active_modules\":" << one_batch_parallel.active_modules
              << ",\"batch_waves\":" << one_batch_parallel.batch_waves << "}\n";
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

    auto mapping_config = ArchitectureConfig::Load("configs/HYGCN_SMOKE.ini");
    const std::vector<MemoryRequest> contiguous = {
        {0, RequestClass::INPUT, 8192, 0, 0, 0},
    };
    const auto low_bits = MemoryCoordinatorModel::Simulate(
        contiguous, mapping_config, MemoryPriorityMode::FIFO,
        AddressMappingMode::LOW_BITS);
    const auto row_first = MemoryCoordinatorModel::Simulate(
        contiguous, mapping_config, MemoryPriorityMode::FIFO,
        AddressMappingMode::ROW_FIRST);
    Check(low_bits.channel_blocks == std::vector<uint64_t>({32, 32, 32, 32}),
          "paper low-bit mapping stripes a contiguous request across channels");
    Check(row_first.channel_blocks == std::vector<uint64_t>({32, 32, 32, 32}),
          "row-first baseline stripes complete rows across channels");
    Check(row_first.bank_blocks == std::vector<uint64_t>({64, 64, 0, 0}),
          "versioned row-first baseline exposes its two-lane bank approximation");
}

void TestCoordinator() {
    std::vector<MemoryRequest> requests = {
        {1, RequestClass::EDGE, 64, 0, 0, 3},
        {0, RequestClass::OUTPUT, 64, 0, 0, 2},
        {0, RequestClass::INPUT, 64, 0, 0, 1},
        {0, RequestClass::EDGE, 64, 0, 0, 0},
    };
    const auto ordered = MemoryCoordinatorModel::Order(
        requests, MemoryPriorityMode::BATCH_CLASS);
    Check(ordered[0].batch_id == 0 && ordered[0].request_class == RequestClass::EDGE,
          "coordinator prioritizes edge inside the oldest batch");
    Check(ordered[1].batch_id == 0 && ordered[1].request_class == RequestClass::INPUT,
          "coordinator prioritizes input after edge");
    Check(ordered[2].batch_id == 0 && ordered[2].request_class == RequestClass::OUTPUT,
          "current batch output precedes next batch edge");

    const auto uncoordinated = MemoryCoordinatorModel::Order(
        requests, MemoryPriorityMode::FIFO);
    Check(uncoordinated[0].sequence == 0 && uncoordinated[1].sequence == 1,
          "uncoordinated baseline preserves FIFO request order");
    ExpectThrows([] {
        MemoryCoordinatorModel::Order(
            {{0, RequestClass::EDGE, 0, 0, 0, 0}}, MemoryPriorityMode::BATCH_CLASS);
    }, "zero-byte memory requests are rejected");
    ExpectThrows([] {
        MemoryCoordinatorModel::Order(
            {{0, RequestClass::OUTPUT, 64, 0, 9, 0, 10}},
            MemoryPriorityMode::BATCH_CLASS);
    }, "requests cannot enqueue before their producer is ready");

    auto config = ArchitectureConfig::Load("configs/HYGCN_SMOKE.ini");
    config.hbm_channels = 1;
    config.hbm_banks_per_channel = 1;
    config.row_first_bank_interleave = 1;
    config.hbm_row_bytes = 256;
    config.block_size = 64;
    config.Validate();
    const std::vector<MemoryRequest> alternating = {
        {1, RequestClass::EDGE, 64, 256, 0, 0},
        {0, RequestClass::OUTPUT, 64, 0, 0, 1},
        {1, RequestClass::INPUT, 64, 320, 0, 2},
        {0, RequestClass::INPUT, 64, 64, 0, 3},
        {1, RequestClass::OUTPUT, 64, 384, 0, 4},
        {0, RequestClass::EDGE, 64, 128, 0, 5},
    };
    const auto fifo_timing = MemoryCoordinatorModel::Simulate(
        alternating, config, MemoryPriorityMode::FIFO, AddressMappingMode::LOW_BITS);
    const auto coordinated_timing = MemoryCoordinatorModel::Simulate(
        alternating, config, MemoryPriorityMode::BATCH_CLASS,
        AddressMappingMode::LOW_BITS);
    Check(coordinated_timing.cycles < fifo_timing.cycles,
          "coordinator ordering reduces row conflicts in the request timing model");
    Check(coordinated_timing.row_buffer_hits > fifo_timing.row_buffer_hits,
          "coordinator timing reports additional row-buffer hits");

    const std::vector<MemoryRequest> dynamic = {
        {0, RequestClass::EDGE, 128, 0, 0, 0, 0},
        {1, RequestClass::EDGE, 512, 256, 0, 1, 0},
        {0, RequestClass::INPUT, 128, 128, 0, 2, 0, 0, 0},
    };
    const auto dynamic_fifo = MemoryCoordinatorModel::Simulate(
        dynamic, config, MemoryPriorityMode::FIFO, AddressMappingMode::LOW_BITS);
    const auto dynamic_priority = MemoryCoordinatorModel::Simulate(
        dynamic, config, MemoryPriorityMode::BATCH_CLASS,
        AddressMappingMode::LOW_BITS);
    auto trace = [](const MemoryTimingResult& timing, uint64_t sequence) {
        const auto iterator = std::find_if(
            timing.request_traces.begin(), timing.request_traces.end(),
            [sequence](const auto& item) { return item.sequence == sequence; });
        if (iterator == timing.request_traces.end()) {
            throw std::runtime_error("dynamic priority trace not found");
        }
        return *iterator;
    };
    const auto priority_edge0 = trace(dynamic_priority, 0);
    const auto priority_edge1 = trace(dynamic_priority, 1);
    const auto priority_input0 = trace(dynamic_priority, 2);
    const auto fifo_input0 = trace(dynamic_fifo, 2);
    Check(priority_input0.producer_ready_cycle == priority_edge0.completion_cycle &&
              priority_input0.enqueue_cycle == priority_input0.producer_ready_cycle,
          "Input dynamically enqueues after its Edge producer completes");
    Check(priority_input0.first_issue_cycle < fifo_input0.first_issue_cycle &&
              priority_input0.completion_cycle < priority_edge1.completion_cycle,
          "batch priority advances current-batch Input ahead of queued next-batch Edge");
    Check(dynamic_priority.priority_reorders > 0,
          "dynamic priority records an observable arbitration reorder");
    Check(dynamic_priority.active_cycles > 0 &&
              dynamic_priority.active_cycles <= dynamic_priority.cycles,
          "HBM active cycles exclude producer-idle gaps from bandwidth utilization");
    std::cout << "priority_trace_evidence={\"edge0_complete\":"
              << priority_edge0.completion_cycle << ",\"input0_enqueue\":"
              << priority_input0.enqueue_cycle << ",\"priority_input_issue\":"
              << priority_input0.first_issue_cycle << ",\"fifo_input_issue\":"
              << fifo_input0.first_issue_cycle << ",\"priority_reorders\":"
              << dynamic_priority.priority_reorders << "}\n";
}

void TestFragmentationInvariant() {
    auto config = ArchitectureConfig::Load("configs/HYGCN_SMOKE.ini");
    config.block_size = 64;
    config.hbm_channels = 8;
    config.hbm_banks_per_channel = 4;
    config.hbm_row_bytes = 256;
    config.Validate();

    const std::vector<MemoryRequest> coalesced = {
        {0, RequestClass::INPUT, 8192, 4096, 7, 0},
    };
    std::vector<MemoryRequest> fragmented;
    for (uint64_t block = 0; block < 128; ++block) {
        fragmented.push_back({
            0,
            RequestClass::INPUT,
            64,
            4096 + block * 64,
            7,
            block,
        });
    }
    const auto whole = MemoryCoordinatorModel::Simulate(
        coalesced, config, MemoryPriorityMode::BATCH_CLASS,
        AddressMappingMode::LOW_BITS);
    const auto split = MemoryCoordinatorModel::Simulate(
        fragmented, config, MemoryPriorityMode::BATCH_CLASS,
        AddressMappingMode::LOW_BITS);
    Check(whole.cycles == split.cycles,
          "same block stream completion is invariant to request fragmentation");
    Check(whole.active_cycles == split.active_cycles,
          "same block stream active service time is invariant to request fragmentation");
    Check(whole.row_buffer_hits == split.row_buffer_hits &&
              whole.row_buffer_misses == split.row_buffer_misses,
          "same block stream preserves row hit and miss counts after fragmentation");
    Check(whole.channel_blocks == split.channel_blocks &&
              whole.bank_blocks == split.bank_blocks,
          "same block stream preserves channel and bank transaction counts");
    std::cout << "fragmentation_evidence={\"blocks\":128,\"coalesced_cycles\":"
              << whole.cycles << ",\"fragmented_cycles\":" << split.cycles
              << ",\"row_hits\":" << whole.row_buffer_hits
              << ",\"row_misses\":" << whole.row_buffer_misses << "}\n";
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
        Check(layer.channel_blocks.size() == static_cast<std::size_t>(config.hbm_channels) &&
                  layer.bank_blocks.size() ==
                      static_cast<std::size_t>(config.hbm_banks_per_channel),
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
                } else if (layer.batches > 1) {
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
        Check(independent.layers[layer].weight_dram_bytes ==
                  cooperative.layers[layer].weight_dram_bytes,
              "both combination policies load weights once from HBM");
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
    auto config = ArchitectureConfig::Load("configs/HYGCN_SMOKE.ini");
    config.aggregation_buffer_bytes = 1024;
    config.aggregation_shard_capacity_bytes = 512;
    config.Validate();
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

void TestProducerDependencies() {
    auto config = ArchitectureConfig::Load("configs/HYGCN_SMOKE.ini");
    config.aggregation_buffer_bytes = 1024;
    config.aggregation_shard_capacity_bytes = 512;
    config.Validate();
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
    const auto pipelined = simulator.Run(graph, "gcn", "producer", 1, latency, 0);
    uint64_t output_requests = 0;
    for (const auto& trace : pipelined.layers[0].producer_request_traces) {
        if (trace.request_class != RequestClass::OUTPUT) {
            continue;
        }
        ++output_requests;
        Check(trace.producer_ready_cycle == trace.enqueue_cycle,
              "output enqueues exactly when its CE producer is ready");
        Check(trace.first_issue_cycle >= trace.enqueue_cycle,
              "output memory issue does not precede producer readiness");
    }
    Check(output_requests == pipelined.layers[0].batches,
          "each pipelined CE batch emits one producer-dependent output request");

    FeatureFlags sequential;
    sequential.pipeline = PipelineMode::SEQUENTIAL;
    const auto spilled = simulator.Run(graph, "gcn", "producer", 1, sequential, 0);
    std::map<int, MemoryRequestTrace> writes;
    std::map<int, MemoryRequestTrace> reads;
    uint64_t latest_read_completion = 0;
    uint64_t written_bytes = 0;
    for (const auto& trace : spilled.layers[0].producer_request_traces) {
        if (trace.request_class == RequestClass::INTERMEDIATE_WRITE) {
            writes[trace.batch_id] = trace;
            written_bytes += trace.bytes;
            Check(trace.producer_ready_cycle <= trace.enqueue_cycle &&
                      trace.enqueue_cycle == spilled.layers[0].ae_finish_cycle,
                  "sequential intermediate writes wait for the AE phase boundary");
        } else if (trace.request_class == RequestClass::INTERMEDIATE_READ) {
            reads[trace.batch_id] = trace;
            latest_read_completion = std::max(latest_read_completion, trace.completion_cycle);
        } else if (trace.request_class == RequestClass::OUTPUT) {
            Check(trace.producer_ready_cycle == spilled.layers[0].ce_finish_cycle &&
                      trace.enqueue_cycle == trace.producer_ready_cycle,
                  "sequential output waits for CE completion");
        }
    }
    Check(writes.size() == reads.size() && !writes.empty(),
          "sequential execution records matched intermediate writes and reads");
    for (const auto& [batch, write] : writes) {
        const auto read = reads.find(batch);
        Check(read != reads.end(), "each intermediate write has a dependent read");
        if (read == reads.end()) {
            continue;
        }
        Check(read->second.address == write.address && read->second.bytes == write.bytes,
              "intermediate read accesses the bytes produced by its write");
        Check(read->second.producer_ready_cycle == write.completion_cycle &&
                  read->second.enqueue_cycle >= write.completion_cycle,
              "intermediate read observes write completion RAW dependency");
    }
    Check(spilled.layers[0].ce_start_cycle == latest_read_completion,
          "sequential CE starts at the unified memory timeline read completion");
    const uint64_t expected_producer_bytes =
        static_cast<uint64_t>(graph.num_vertex) * graph.len_feature * sizeof(float);
    Check(written_bytes == expected_producer_bytes &&
              spilled.layers[0].intermediate_dram_bytes == 2 * expected_producer_bytes,
          "sequential spill reads and writes only block-aligned producer bytes");
    std::cout << "producer_dependency_evidence={\"output_requests\":"
              << output_requests << ",\"intermediate_pairs\":" << writes.size()
              << ",\"latest_read_completion\":" << latest_read_completion
              << ",\"ce_start\":" << spilled.layers[0].ce_start_cycle << "}\n";
}

}  // namespace

int main() {
    try {
        RunNamedTest("config", TestConfig);
        RunNamedTest("F01_window_sliding_shrinking", TestPartitionAndSparsity);
        RunNamedTest("edge_chunk_boundaries", TestEdgeChunkBoundaries);
        RunNamedTest("output_address", TestOutputAddress);
        RunNamedTest("systolic_model", TestSystolicModel);
        RunNamedTest("hbm_layout_mapping", TestHbmLayoutAndMapping);
        RunNamedTest("coordinator_ordering", TestCoordinator);
        RunNamedTest("F02_fragmentation_invariance", TestFragmentationInvariant);
        RunNamedTest("aggregation_buffer", TestAggregationBuffer);
        RunNamedTest("event_spm_guards", TestEventAndSpmGuards);
        RunNamedTest("aggregation_operations", TestAggregationOperations);
        RunNamedTest("simulation_determinism", TestSimulationDeterminism);
        RunNamedTest("policy_matrix", TestPolicyMatrix);
        RunNamedTest("feature_dispersion_address_bounds", TestFeatureDispersionAndAddressBounds);
        RunNamedTest("pipeline_batching", TestPipelineBatching);
        RunNamedTest("F03_producer_and_RAW_dependencies", TestProducerDependencies);
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
