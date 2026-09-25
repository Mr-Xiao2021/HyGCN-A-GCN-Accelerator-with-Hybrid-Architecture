#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "config.h"
#include "hygcn.h"
#include "json.hpp"
#include "paper_sim.h"

#ifndef HYGCN_GIT_COMMIT
#define HYGCN_GIT_COMMIT "unknown"
#endif

namespace {

using json = nlohmann::json;

struct Options {
    std::string engine = "paper";
    std::string profile = "smoke";
    std::string profile_path;
    std::string model = "gcn";
    std::string dataset = "test";
    std::string graph_dir = "gcn_dataset";
    std::string output_dir = "res";
    uint64_t seed = 1;
    int layer = -1;
    FeatureFlags flags;
    bool quiet = false;
};

void PrintUsage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --engine paper|legacy\n"
        << "  --profile paper|legacy|smoke\n"
        << "  --profile-path PATH\n"
        << "  --model gcn|gin|gs\n"
        << "  --dataset NAME\n"
        << "  --layer all|0|1\n"
        << "  --scope full|aggregation\n"
        << "  --graph-dir PATH\n"
        << "  --pipeline sequential|latency-aware|energy-aware\n"
        << "  --combination independent|cooperative\n"
        << "  --sparsity on|off\n"
        << "  --coordination on|off (sets priority and mapping together)\n"
        << "  --priority fifo|batch-class\n"
        << "  --mapping row-first|low-bits\n"
        << "  --seed N\n"
        << "  --output-dir PATH\n"
        << "  --quiet\n";
}

std::string RequireValue(int argc, char** argv, int& index) {
    if (index + 1 >= argc) {
        throw std::runtime_error(std::string("missing value for ") + argv[index]);
    }
    return argv[++index];
}

int ParseLayer(const std::string& value) {
    if (value == "all") {
        return -1;
    }
    try {
        std::size_t consumed = 0;
        const int layer = std::stoi(value, &consumed);
        if (consumed != value.size() || layer < 0 || layer > 1) {
            throw std::runtime_error("invalid layer: expected all, 0, or 1");
        }
        return layer;
    } catch (const std::invalid_argument&) {
        throw std::runtime_error("invalid layer: expected all, 0, or 1");
    } catch (const std::out_of_range&) {
        throw std::runtime_error("invalid layer: expected all, 0, or 1");
    }
}

Options ParseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            PrintUsage(argv[0]);
            std::exit(0);
        } else if (argument == "--engine") {
            options.engine = RequireValue(argc, argv, i);
        } else if (argument == "--profile") {
            options.profile = RequireValue(argc, argv, i);
        } else if (argument == "--profile-path") {
            options.profile_path = RequireValue(argc, argv, i);
        } else if (argument == "--model") {
            options.model = RequireValue(argc, argv, i);
        } else if (argument == "--dataset") {
            options.dataset = RequireValue(argc, argv, i);
        } else if (argument == "--layer") {
            options.layer = ParseLayer(RequireValue(argc, argv, i));
        } else if (argument == "--scope") {
            const auto scope = RequireValue(argc, argv, i);
            if (scope == "full") {
                options.flags.aggregation_only = false;
            } else if (scope == "aggregation") {
                options.flags.aggregation_only = true;
            } else {
                throw std::runtime_error("invalid scope: expected full or aggregation");
            }
        } else if (argument == "--graph-dir") {
            options.graph_dir = RequireValue(argc, argv, i);
        } else if (argument == "--pipeline") {
            options.flags.pipeline = ParsePipelineMode(RequireValue(argc, argv, i));
        } else if (argument == "--combination") {
            options.flags.combination = ParseCombinationMode(RequireValue(argc, argv, i));
        } else if (argument == "--sparsity") {
            options.flags.sparsity_elimination = ParseToggle(RequireValue(argc, argv, i));
        } else if (argument == "--coordination") {
            const bool enabled = ParseToggle(RequireValue(argc, argv, i));
            options.flags.memory_priority = enabled
                ? MemoryPriorityMode::BATCH_CLASS
                : MemoryPriorityMode::FIFO;
            options.flags.address_mapping = enabled
                ? AddressMappingMode::LOW_BITS
                : AddressMappingMode::ROW_FIRST;
        } else if (argument == "--priority") {
            options.flags.memory_priority = ParseMemoryPriorityMode(
                RequireValue(argc, argv, i));
        } else if (argument == "--mapping") {
            options.flags.address_mapping = ParseAddressMappingMode(
                RequireValue(argc, argv, i));
        } else if (argument == "--seed") {
            options.seed = std::stoull(RequireValue(argc, argv, i));
        } else if (argument == "--output-dir") {
            options.output_dir = RequireValue(argc, argv, i);
        } else if (argument == "--quiet") {
            options.quiet = true;
        } else {
            throw std::runtime_error("unknown option: " + argument);
        }
    }
    if (options.engine != "paper" && options.engine != "legacy") {
        throw std::runtime_error("invalid engine: " + options.engine);
    }
    return options;
}

std::string ProfilePath(const Options& options) {
    if (!options.profile_path.empty()) {
        return options.profile_path;
    }
    if (options.profile == "paper") {
        return "configs/HYGCN_PAPER.ini";
    }
    if (options.profile == "legacy") {
        return "configs/HYGCN_LEGACY.ini";
    }
    if (options.profile == "smoke") {
        return "configs/HYGCN_SMOKE.ini";
    }
    throw std::runtime_error("invalid profile: " + options.profile);
}

void ValidateInputs(const Options& options) {
    const auto info_path = std::filesystem::path(options.graph_dir) /
                           (options.dataset + ".txt");
    const auto edge_path = std::filesystem::path(options.graph_dir) /
                           (options.dataset + "_edge.csv");
    if (!std::filesystem::is_regular_file(info_path)) {
        throw std::runtime_error("missing graph metadata: " + info_path.string());
    }
    if (!std::filesystem::is_regular_file(edge_path)) {
        throw std::runtime_error("missing graph edges: " + edge_path.string());
    }
    if (options.model != "gcn" && options.model != "gin" && options.model != "gs") {
        throw std::runtime_error("unsupported model: " + options.model);
    }
    if (options.model == "gs") {
        const auto sample_path = std::filesystem::path("sample") /
                                 (options.dataset + "_sample.csv");
        if (!std::filesystem::is_regular_file(sample_path)) {
            throw std::runtime_error("missing GraphSAGE sample file: " + sample_path.string());
        }
    }
    if (options.engine == "legacy" && options.flags.aggregation_only) {
        throw std::runtime_error("aggregation scope is supported only by the paper engine");
    }
}

std::string ResultStem(const Options& options) {
    return options.engine + "_" + options.profile + "_" + options.model + "_" +
           options.dataset + "_" + ToString(options.flags.pipeline) + "_" +
           ToString(options.flags.combination) + "_sparse-" +
           (options.flags.sparsity_elimination ? "on" : "off") + "_priority-" +
           ToString(options.flags.memory_priority) + "_mapping-" +
           ToString(options.flags.address_mapping) + "_seed-" +
           std::to_string(options.seed) +
           (options.flags.aggregation_only ? "_scope-aggregation" : "") +
           (options.layer < 0 ? "" : "_layer-" + std::to_string(options.layer));
}

Graph LoadGraph(const Options& options) {
    GraphLoader loader;
    loader.LoadGraph(options.graph_dir, options.dataset);
    if (options.model == "gcn" || options.model == "gin") {
        return loader.GetGcnNormGraph();
    }
    return loader.LoadSampleGraph(25);
}

int RunPaper(const Options& options) {
    const auto architecture = ArchitectureConfig::Load(ProfilePath(options));
    auto graph = LoadGraph(options);
    PaperSimulator simulator(architecture);
    auto result = simulator.Run(
        graph, options.model, options.dataset, options.seed, options.flags, options.layer);
    result.binary_digest = DigestFile(std::filesystem::canonical("/proc/self/exe").string());
    result.config_digest = DigestFile(ProfilePath(options));
    result.graph_digest = DigestFile((std::filesystem::path(options.graph_dir) /
                                      (options.dataset + ".txt")).string()) + "-" +
                          DigestFile((std::filesystem::path(options.graph_dir) /
                                      (options.dataset + "_edge.csv")).string());
    if (options.model == "gs") {
        result.graph_digest += "-" + DigestFile(
            (std::filesystem::path("sample") / (options.dataset + "_sample.csv")).string());
    }

    const auto stem = ResultStem(options);
    const auto json_path = (std::filesystem::path(options.output_dir) / (stem + ".json")).string();
    const auto csv_path = (std::filesystem::path(options.output_dir) / (stem + ".csv")).string();
    WriteExperimentJson(result, json_path);
    WriteExperimentCsv(result, csv_path);
    if (!options.quiet) {
        std::cout << "result_json=" << json_path << '\n'
                  << "result_csv=" << csv_path << '\n'
                  << "total_cycles=" << result.TotalCycles() << '\n'
                  << "total_dram_bytes=" << result.TotalDramBytes() << '\n'
                  << "bandwidth_utilization=" << result.BandwidthUtilization() << '\n';
    }
    return 0;
}

int RunLegacy(const Options& options) {
    const std::string model_path = "gcn/" + options.model + ".ini";
    if (!std::filesystem::is_regular_file(model_path)) {
        throw std::runtime_error("missing model config: " + model_path);
    }
    std::filesystem::create_directories(options.output_dir);
    auto hy_config = std::make_shared<HyConfig>();
    hy_config->Init(options.graph_dir, options.dataset, "configs/HYGCN.ini", model_path);
    std::vector<HyRec> records;
    std::vector<int> record_layers;
    uint64_t total_cycles = 0;
    int current_layer = 0;
    while (true) {
        HyGCN hygcn(hy_config);
        while (!hygcn.IsDone()) {
            hygcn.ClockTick();
        }
        hygcn.Record();
        if (options.layer < 0 || options.layer == current_layer) {
            total_cycles += hygcn.clk;
            records.push_back(hygcn.hyrec);
            record_layers.push_back(current_layer);
        }
        if (options.layer == current_layer || !hy_config->PrepareNextLayer()) {
            break;
        }
        ++current_layer;
    }
    if (records.empty()) {
        throw std::runtime_error("selected layer was not produced by legacy model");
    }

    const auto stem = "legacy_" + options.model + "_" + options.dataset + "_seed-" +
        std::to_string(options.seed) +
        (options.layer < 0 ? "" : "_layer-" + std::to_string(options.layer));
    const auto csv_path = (std::filesystem::path(options.output_dir) / (stem + ".csv")).string();
    std::ofstream csv(csv_path);
    if (!csv) {
        throw std::runtime_error("cannot write legacy CSV: " + csv_path);
    }
    csv << "name,layer," << HyRec::GetHeaderString();
    for (std::size_t layer = 0; layer < records.size(); ++layer) {
        csv << stem << ',' << record_layers[layer] << ',' << records[layer].GetString();
    }

    const auto json_path = (std::filesystem::path(options.output_dir) / (stem + ".json")).string();
    json output = {
        {"schema_version", 1},
        {"manifest", {
            {"engine", "legacy"},
            {"git_commit", HYGCN_GIT_COMMIT},
            {"binary_digest", DigestFile(std::filesystem::canonical("/proc/self/exe").string())},
            {"model", options.model},
            {"dataset", options.dataset},
            {"seed", options.seed},
            {"selected_layer", options.layer < 0 ? "all" : std::to_string(options.layer)},
            {"graph_digest", DigestFile((std::filesystem::path(options.graph_dir) /
                                         (options.dataset + ".txt")).string()) + "-" +
                             DigestFile((std::filesystem::path(options.graph_dir) /
                                         (options.dataset + "_edge.csv")).string())},
        }},
        {"summary", {{"total_cycles", total_cycles}}},
        {"layers", json::array()},
    };
    for (std::size_t layer = 0; layer < records.size(); ++layer) {
        output["layers"].push_back({
            {"layer", record_layers[layer]},
            {"cycles", records[layer].finish_time},
            {"dram_edge_read", records[layer].dram_edge_read},
            {"dram_input_read", records[layer].dram_input_read},
            {"dram_weight_read", records[layer].dram_weight_read},
            {"dram_output_write", records[layer].dram_output_write},
        });
    }
    std::ofstream json_stream(json_path);
    if (!json_stream) {
        throw std::runtime_error("cannot write legacy JSON: " + json_path);
    }
    json_stream << std::setw(2) << output << '\n';
    if (!options.quiet) {
        std::cout << "result_json=" << json_path << '\n'
                  << "result_csv=" << csv_path << '\n'
                  << "total_cycles=" << total_cycles << '\n';
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = ParseOptions(argc, argv);
        ValidateInputs(options);
        return options.engine == "paper" ? RunPaper(options) : RunLegacy(options);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
