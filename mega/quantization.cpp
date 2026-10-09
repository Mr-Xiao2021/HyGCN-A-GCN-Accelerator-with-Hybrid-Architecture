#include "quantization.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

#include "json.hpp"

namespace mega {
namespace {

using json = nlohmann::json;

template <typename Type>
Type Require(const json& object, const char* name) {
    if (!object.contains(name)) {
        throw std::runtime_error(std::string("quantization manifest is missing ") + name);
    }
    return object.at(name).get<Type>();
}

}  // namespace

const DegreeQuantizationRule& LayerQuantization::RuleForDegree(int degree) const {
    if (degree < 0) {
        throw std::runtime_error("node degree cannot be negative");
    }
    const auto match = std::find_if(
        degree_rules.begin(), degree_rules.end(),
        [degree](const auto& rule) {
            return degree >= rule.min_degree && degree <= rule.max_degree;
        });
    if (match == degree_rules.end()) {
        throw std::runtime_error("node degree is not covered by the quantization manifest");
    }
    return *match;
}

void LayerQuantization::Validate() const {
    if (layer < 0 || feature_count <= 0 || output_features <= 0) {
        throw std::runtime_error("quantization layer dimensions must be positive");
    }
    if (!std::isfinite(feature_density) || feature_density < 0.0 || feature_density > 1.0) {
        throw std::runtime_error("feature_density must be in [0, 1]");
    }
    if (weight_bits != 4) {
        throw std::runtime_error("MEGA paper profile requires 4-bit weights");
    }
    if (weight_scales.size() != static_cast<std::size_t>(output_features)) {
        throw std::runtime_error("weight_scales must contain one scale per output column");
    }
    for (double scale : weight_scales) {
        if (!std::isfinite(scale) || scale <= 0.0) {
            throw std::runtime_error("weight scales must be finite and positive");
        }
    }
    if (degree_rules.empty()) {
        throw std::runtime_error("degree_rules cannot be empty");
    }
    int next_degree = 0;
    for (const auto& rule : degree_rules) {
        if (rule.min_degree != next_degree || rule.max_degree < rule.min_degree) {
            throw std::runtime_error("degree rules must be sorted, contiguous, and start at zero");
        }
        if (rule.bitwidth < 1 || rule.bitwidth > 8) {
            throw std::runtime_error("feature bitwidth must be in [1, 8]");
        }
        if (!std::isfinite(rule.scale) || rule.scale <= 0.0) {
            throw std::runtime_error("feature scales must be finite and positive");
        }
        if (rule.max_degree == std::numeric_limits<int>::max()) {
            next_degree = rule.max_degree;
        } else {
            next_degree = rule.max_degree + 1;
        }
    }
    if (degree_rules.back().max_degree != std::numeric_limits<int>::max()) {
        throw std::runtime_error("degree rules must cover all non-negative degrees");
    }
}

QuantizationManifest QuantizationManifest::Load(const std::string& path) {
    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error("cannot open quantization manifest: " + path);
    }
    json input;
    stream >> input;

    QuantizationManifest manifest;
    manifest.schema_version = Require<int>(input, "schema_version");
    manifest.manifest_version = Require<std::string>(input, "manifest_version");
    manifest.dataset = Require<std::string>(input, "dataset");
    manifest.model = Require<std::string>(input, "model");
    manifest.graph_digest = Require<std::string>(input, "graph_digest");
    manifest.provenance = ParseQuantizationProvenance(
        Require<std::string>(input, "provenance"));

    if (!input.contains("layers") || !input.at("layers").is_array()) {
        throw std::runtime_error("quantization manifest layers must be an array");
    }
    for (const auto& layer_json : input.at("layers")) {
        LayerQuantization layer;
        layer.layer = Require<int>(layer_json, "layer");
        layer.feature_count = Require<int>(layer_json, "feature_count");
        layer.output_features = Require<int>(layer_json, "output_features");
        layer.feature_density = Require<double>(layer_json, "feature_density");
        layer.weight_bits = Require<int>(layer_json, "weight_bits");
        layer.weight_scales = Require<std::vector<double>>(layer_json, "weight_scales");
        if (!layer_json.contains("degree_rules") ||
            !layer_json.at("degree_rules").is_array()) {
            throw std::runtime_error("degree_rules must be an array");
        }
        for (const auto& rule_json : layer_json.at("degree_rules")) {
            DegreeQuantizationRule rule;
            rule.min_degree = Require<int>(rule_json, "min_degree");
            if (rule_json.at("max_degree").is_string()) {
                if (rule_json.at("max_degree").get<std::string>() != "max") {
                    throw std::runtime_error("max_degree string must be 'max'");
                }
                rule.max_degree = std::numeric_limits<int>::max();
            } else {
                rule.max_degree = Require<int>(rule_json, "max_degree");
            }
            rule.bitwidth = Require<int>(rule_json, "bitwidth");
            rule.scale = Require<double>(rule_json, "scale");
            layer.degree_rules.push_back(rule);
        }
        manifest.layers.push_back(std::move(layer));
    }
    manifest.Validate();
    return manifest;
}

void QuantizationManifest::Validate() const {
    if (schema_version != 1 || manifest_version.empty() || dataset.empty() ||
        model.empty() || graph_digest.empty()) {
        throw std::runtime_error("invalid quantization manifest identity fields");
    }
    if (model != "gcn" && model != "gin" && model != "gs") {
        throw std::runtime_error("unsupported quantization model: " + model);
    }
    if (layers.empty()) {
        throw std::runtime_error("quantization manifest must contain layers");
    }
    std::set<int> layer_ids;
    for (const auto& layer : layers) {
        layer.Validate();
        if (!layer_ids.insert(layer.layer).second) {
            throw std::runtime_error("quantization manifest contains duplicate layers");
        }
    }
    for (int layer = 0; layer < static_cast<int>(layers.size()); ++layer) {
        if (layer_ids.count(layer) == 0) {
            throw std::runtime_error("quantization manifest layers must be contiguous from zero");
        }
    }
    if (provenance != QuantizationProvenance::DIAGNOSTIC_HEURISTIC &&
        graph_digest == "*") {
        throw std::runtime_error("required-eligible quantization manifests need an exact graph digest");
    }
}

void QuantizationManifest::ValidateFor(const std::string& expected_dataset,
                                       const std::string& expected_model,
                                       const std::string& expected_graph_digest) const {
    Validate();
    if (dataset != expected_dataset || model != expected_model) {
        throw std::runtime_error("quantization manifest dataset/model mismatch");
    }
    if (graph_digest != "*" && graph_digest != expected_graph_digest) {
        throw std::runtime_error("quantization manifest graph digest mismatch");
    }
}

const LayerQuantization& QuantizationManifest::Layer(int layer) const {
    const auto match = std::find_if(layers.begin(), layers.end(),
                                    [layer](const auto& item) {
                                        return item.layer == layer;
                                    });
    if (match == layers.end()) {
        throw std::runtime_error("quantization layer is missing");
    }
    return *match;
}

bool QuantizationManifest::EligibleForRequiredBenchmark() const {
    return provenance == QuantizationProvenance::PAPER_DERIVED ||
           provenance == QuantizationProvenance::LOCALLY_TRAINED;
}

ReferenceQuantizer::ReferenceQuantizer(const QuantizationManifest& manifest)
    : manifest_(manifest) {
    manifest_.Validate();
}

int ReferenceQuantizer::Bitwidth(int layer, int degree) const {
    return manifest_.Layer(layer).RuleForDegree(degree).bitwidth;
}

double ReferenceQuantizer::Scale(int layer, int degree) const {
    return manifest_.Layer(layer).RuleForDegree(degree).scale;
}

int32_t ReferenceQuantizer::Quantize(double value, int layer, int degree) const {
    if (!std::isfinite(value)) {
        throw std::runtime_error("cannot quantize a non-finite value");
    }
    const auto& rule = manifest_.Layer(layer).RuleForDegree(degree);
    const int32_t maximum = (int32_t{1} << (rule.bitwidth - 1)) - 1;
    if (maximum == 0) {
        return 0;
    }
    const double magnitude = std::floor(std::abs(value) / rule.scale + 0.5);
    const int32_t quantized = static_cast<int32_t>(
        std::min<double>(magnitude, static_cast<double>(maximum)));
    return value < 0.0 ? -quantized : quantized;
}

double ReferenceQuantizer::Dequantize(int32_t value, int layer, int degree) const {
    const auto& rule = manifest_.Layer(layer).RuleForDegree(degree);
    const int32_t maximum = (int32_t{1} << (rule.bitwidth - 1)) - 1;
    if (value < -maximum || value > maximum) {
        throw std::runtime_error("quantized value exceeds the configured signed range");
    }
    return static_cast<double>(value) * rule.scale;
}

std::string ToString(QuantizationProvenance provenance) {
    switch (provenance) {
        case QuantizationProvenance::PAPER_DERIVED:
            return "paper-derived";
        case QuantizationProvenance::LOCALLY_TRAINED:
            return "locally-trained";
        case QuantizationProvenance::DIAGNOSTIC_HEURISTIC:
            return "diagnostic-heuristic";
    }
    throw std::runtime_error("unknown quantization provenance");
}

QuantizationProvenance ParseQuantizationProvenance(const std::string& value) {
    if (value == "paper-derived") {
        return QuantizationProvenance::PAPER_DERIVED;
    }
    if (value == "locally-trained") {
        return QuantizationProvenance::LOCALLY_TRAINED;
    }
    if (value == "diagnostic-heuristic") {
        return QuantizationProvenance::DIAGNOSTIC_HEURISTIC;
    }
    throw std::runtime_error("invalid quantization provenance: " + value);
}

}  // namespace mega
