#ifndef MEGA_ADAPTIVE_PACKAGE_H
#define MEGA_ADAPTIVE_PACKAGE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mega {

struct QuantizedNode {
    int node_id = 0;
    int bitwidth = 0;
    std::vector<int32_t> values;
};

struct AdaptivePackage {
    uint8_t mode = 0;
    uint8_t bitwidth = 0;
    uint32_t value_count = 0;
    std::vector<uint8_t> bytes;
};

struct PackageTraffic {
    uint64_t payload_bits = 0;
    uint64_t header_bits = 0;
    uint64_t padding_bits = 0;
    uint64_t bitmap_bits = 0;
    uint64_t boundary_bits = 0;
    uint64_t scale_bits = 0;
    uint64_t logical_bytes = 0;
    uint64_t dram_bytes = 0;
    uint64_t dram_transactions = 0;
};

struct EncodedFeatureStream {
    int feature_count = 0;
    std::vector<int> node_ids;
    std::vector<uint8_t> node_bitwidths;
    std::vector<uint64_t> node_value_offsets;
    std::vector<uint8_t> bitmap;
    std::vector<AdaptivePackage> packages;
    PackageTraffic traffic;
};

class AdaptivePackageCodec {
public:
    static EncodedFeatureStream Encode(const std::vector<QuantizedNode>& nodes,
                                       std::size_t transaction_bytes,
                                       std::size_t scale_entries = 0);
    static std::vector<QuantizedNode> Decode(const EncodedFeatureStream& encoded);
    static void Validate(const EncodedFeatureStream& encoded);
};

}  // namespace mega

#endif
