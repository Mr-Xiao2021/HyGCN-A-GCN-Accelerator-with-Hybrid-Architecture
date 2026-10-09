#ifndef MEGA_QUANTIZATION_H
#define MEGA_QUANTIZATION_H

#include <cstdint>
#include <string>
#include <vector>

namespace mega {

enum class QuantizationProvenance {
    PAPER_DERIVED,
    LOCALLY_TRAINED,
    DIAGNOSTIC_HEURISTIC,
};

struct DegreeQuantizationRule {
    int min_degree = 0;
    int max_degree = 0;
    int bitwidth = 0;
    double scale = 0.0;
};

struct LayerQuantization {
    int layer = 0;
    int feature_count = 0;
    int output_features = 0;
    double feature_density = 0.0;
    int weight_bits = 0;
    std::vector<double> weight_scales;
    std::vector<DegreeQuantizationRule> degree_rules;

    const DegreeQuantizationRule& RuleForDegree(int degree) const;
    void Validate() const;
};

class QuantizationManifest {
public:
    static QuantizationManifest Load(const std::string& path);

    void Validate() const;
    void ValidateFor(const std::string& expected_dataset,
                     const std::string& expected_model,
                     const std::string& expected_graph_digest) const;
    const LayerQuantization& Layer(int layer) const;
    bool EligibleForRequiredBenchmark() const;

    int schema_version = 0;
    std::string manifest_version;
    std::string dataset;
    std::string model;
    std::string graph_digest;
    QuantizationProvenance provenance = QuantizationProvenance::DIAGNOSTIC_HEURISTIC;
    std::vector<LayerQuantization> layers;
};

class ReferenceQuantizer {
public:
    explicit ReferenceQuantizer(const QuantizationManifest& manifest);

    int Bitwidth(int layer, int degree) const;
    double Scale(int layer, int degree) const;
    int32_t Quantize(double value, int layer, int degree) const;
    double Dequantize(int32_t value, int layer, int degree) const;

private:
    const QuantizationManifest& manifest_;
};

std::string ToString(QuantizationProvenance provenance);
QuantizationProvenance ParseQuantizationProvenance(const std::string& value);

}  // namespace mega

#endif
