#include "adaptive_package.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace mega {
namespace {

constexpr uint64_t kHeaderBits = 5;
constexpr uint64_t kBoundaryBitsPerEntry = 32;

uint64_t CeilDiv(uint64_t value, uint64_t divisor) {
    if (divisor == 0) {
        throw std::runtime_error("division by zero");
    }
    return value / divisor + (value % divisor != 0 ? 1 : 0);
}

uint64_t Align(uint64_t value, uint64_t alignment) {
    return CeilDiv(value, alignment) * alignment;
}

std::size_t PackageBits(uint8_t mode) {
    switch (mode) {
        case 0:
            return 64;
        case 1:
            return 128;
        case 2:
            return 192;
        default:
            throw std::runtime_error("Adaptive-Package mode 3 is reserved");
    }
}

uint8_t SelectMode(std::size_t remaining_values, int bitwidth) {
    for (uint8_t mode = 0; mode < 3; ++mode) {
        const std::size_t capacity = (PackageBits(mode) - kHeaderBits) / bitwidth;
        if (remaining_values <= capacity) {
            return mode;
        }
    }
    return 2;
}

void SetBit(std::vector<uint8_t>& bytes, std::size_t position, bool value) {
    if (!value) {
        return;
    }
    if (position >= bytes.size() * 8) {
        throw std::runtime_error("bit write exceeds package capacity");
    }
    bytes[position / 8] |= static_cast<uint8_t>(1U << (7 - position % 8));
}

bool GetBit(const std::vector<uint8_t>& bytes, std::size_t position) {
    if (position >= bytes.size() * 8) {
        throw std::runtime_error("bit read exceeds package capacity");
    }
    return (bytes[position / 8] & static_cast<uint8_t>(1U << (7 - position % 8))) != 0;
}

void WriteBits(std::vector<uint8_t>& bytes, std::size_t position,
               uint32_t value, int bits) {
    for (int bit = 0; bit < bits; ++bit) {
        const bool set = (value >> (bits - bit - 1)) & 1U;
        SetBit(bytes, position + bit, set);
    }
}

uint32_t ReadBits(const std::vector<uint8_t>& bytes, std::size_t position, int bits) {
    uint32_t value = 0;
    for (int bit = 0; bit < bits; ++bit) {
        value = (value << 1U) | (GetBit(bytes, position + bit) ? 1U : 0U);
    }
    return value;
}

int32_t SignExtend(uint32_t value, int bits) {
    if (bits == 32) {
        return static_cast<int32_t>(value);
    }
    const uint32_t sign = 1U << (bits - 1);
    const uint32_t mask = (1U << bits) - 1U;
    value &= mask;
    return (value & sign) != 0
        ? static_cast<int32_t>(value | ~mask)
        : static_cast<int32_t>(value);
}

uint32_t EncodeSigned(int32_t value, int bits) {
    const int32_t maximum = (int32_t{1} << (bits - 1)) - 1;
    if (value < -maximum || value > maximum) {
        throw std::runtime_error("quantized value exceeds symmetric signed range");
    }
    const uint32_t mask = (1U << bits) - 1U;
    return static_cast<uint32_t>(value) & mask;
}

std::size_t CountBitmapBits(const std::vector<uint8_t>& bitmap,
                            std::size_t begin, std::size_t count) {
    std::size_t set = 0;
    for (std::size_t bit = 0; bit < count; ++bit) {
        if (GetBit(bitmap, begin + bit)) {
            ++set;
        }
    }
    return set;
}

}  // namespace

EncodedFeatureStream AdaptivePackageCodec::Encode(
        const std::vector<QuantizedNode>& nodes,
        std::size_t transaction_bytes,
        std::size_t scale_entries) {
    if (nodes.empty() || transaction_bytes == 0) {
        throw std::runtime_error("Adaptive-Package input and transaction size must be non-zero");
    }
    EncodedFeatureStream encoded;
    encoded.feature_count = static_cast<int>(nodes.front().values.size());
    if (encoded.feature_count <= 0) {
        throw std::runtime_error("Adaptive-Package nodes require at least one feature");
    }
    encoded.node_value_offsets.push_back(0);
    encoded.bitmap.assign(CeilDiv(
        static_cast<uint64_t>(nodes.size()) * encoded.feature_count, 8), 0);

    std::vector<int32_t> nonzero_values;
    std::vector<std::size_t> run_starts;
    for (std::size_t node_index = 0; node_index < nodes.size(); ++node_index) {
        const auto& node = nodes[node_index];
        if (node.node_id < 0 || node.bitwidth < 1 || node.bitwidth > 8 ||
            static_cast<int>(node.values.size()) != encoded.feature_count) {
            throw std::runtime_error("invalid node supplied to Adaptive-Package encoder");
        }
        if (node_index > 0 && node.node_id <= nodes[node_index - 1].node_id) {
            throw std::runtime_error("Adaptive-Package nodes must use increasing IDs");
        }
        encoded.node_ids.push_back(node.node_id);
        encoded.node_bitwidths.push_back(static_cast<uint8_t>(node.bitwidth));
        if (run_starts.empty() ||
            encoded.node_bitwidths[node_index] != encoded.node_bitwidths[node_index - 1]) {
            run_starts.push_back(node_index);
        }
        for (int feature = 0; feature < encoded.feature_count; ++feature) {
            const int32_t value = node.values[feature];
            if (value == 0) {
                continue;
            }
            EncodeSigned(value, node.bitwidth);
            SetBit(encoded.bitmap,
                   node_index * static_cast<std::size_t>(encoded.feature_count) + feature,
                   true);
            nonzero_values.push_back(value);
        }
        encoded.node_value_offsets.push_back(nonzero_values.size());
    }
    run_starts.push_back(nodes.size());

    std::size_t value_cursor = 0;
    for (std::size_t run = 0; run + 1 < run_starts.size(); ++run) {
        const std::size_t first_node = run_starts[run];
        const std::size_t end_node = run_starts[run + 1];
        const int bitwidth = nodes[first_node].bitwidth;
        const std::size_t run_values =
            encoded.node_value_offsets[end_node] - encoded.node_value_offsets[first_node];
        std::size_t remaining = run_values;
        while (remaining > 0) {
            const uint8_t mode = SelectMode(remaining, bitwidth);
            const std::size_t package_bits = PackageBits(mode);
            const std::size_t capacity = (package_bits - kHeaderBits) / bitwidth;
            const std::size_t count = std::min(remaining, capacity);
            AdaptivePackage package;
            package.mode = mode;
            package.bitwidth = static_cast<uint8_t>(bitwidth);
            package.value_count = static_cast<uint32_t>(count);
            package.bytes.assign(package_bits / 8, 0);
            WriteBits(package.bytes, 0, mode, 2);
            WriteBits(package.bytes, 2, static_cast<uint32_t>(bitwidth - 1), 3);
            std::size_t bit_cursor = kHeaderBits;
            for (std::size_t index = 0; index < count; ++index) {
                WriteBits(package.bytes, bit_cursor,
                          EncodeSigned(nonzero_values[value_cursor++], bitwidth), bitwidth);
                bit_cursor += bitwidth;
            }
            encoded.traffic.payload_bits += count * bitwidth;
            encoded.traffic.header_bits += kHeaderBits;
            encoded.traffic.padding_bits += package_bits - bit_cursor;
            encoded.packages.push_back(std::move(package));
            remaining -= count;
        }
    }
    if (value_cursor != nonzero_values.size()) {
        throw std::runtime_error("Adaptive-Package encoder did not consume all values");
    }

    encoded.traffic.bitmap_bits =
        static_cast<uint64_t>(nodes.size()) * encoded.feature_count;
    encoded.traffic.boundary_bits =
        static_cast<uint64_t>(nodes.size() + 1) * kBoundaryBitsPerEntry;
    encoded.traffic.scale_bits = static_cast<uint64_t>(scale_entries) * 32;
    const uint64_t package_bytes = (encoded.traffic.payload_bits +
        encoded.traffic.header_bits + encoded.traffic.padding_bits) / 8;
    const uint64_t bitmap_bytes = CeilDiv(encoded.traffic.bitmap_bits, 8);
    const uint64_t boundary_bytes = CeilDiv(encoded.traffic.boundary_bits, 8);
    const uint64_t scale_bytes = CeilDiv(encoded.traffic.scale_bits, 8);
    encoded.traffic.logical_bytes =
        package_bytes + bitmap_bytes + boundary_bytes + scale_bytes;
    encoded.traffic.dram_bytes = Align(package_bytes, transaction_bytes) +
        Align(bitmap_bytes, transaction_bytes) +
        Align(boundary_bytes, transaction_bytes) +
        (scale_bytes == 0 ? 0 : Align(scale_bytes, transaction_bytes));
    encoded.traffic.dram_transactions =
        encoded.traffic.dram_bytes / transaction_bytes;
    Validate(encoded);
    return encoded;
}

std::vector<QuantizedNode> AdaptivePackageCodec::Decode(
        const EncodedFeatureStream& encoded) {
    Validate(encoded);
    std::vector<int32_t> flat_values;
    flat_values.reserve(encoded.node_value_offsets.back());
    std::size_t package_index = 0;
    std::size_t node_index = 0;
    while (node_index < encoded.node_ids.size()) {
        const int bitwidth = encoded.node_bitwidths[node_index];
        std::size_t run_end = node_index + 1;
        while (run_end < encoded.node_ids.size() &&
               encoded.node_bitwidths[run_end] == bitwidth) {
            ++run_end;
        }
        std::size_t remaining = static_cast<std::size_t>(
            encoded.node_value_offsets[run_end] - encoded.node_value_offsets[node_index]);
        while (remaining > 0) {
            if (package_index >= encoded.packages.size()) {
                throw std::runtime_error("Adaptive-Package stream ended before all values decoded");
            }
            const auto& package = encoded.packages[package_index++];
            const uint8_t mode = static_cast<uint8_t>(ReadBits(package.bytes, 0, 2));
            const uint8_t package_bitwidth =
                static_cast<uint8_t>(ReadBits(package.bytes, 2, 3) + 1);
            if (mode != package.mode || package_bitwidth != package.bitwidth ||
                package_bitwidth != bitwidth) {
                throw std::runtime_error("Adaptive-Package header does not match stream metadata");
            }
            const std::size_t capacity = (PackageBits(mode) - kHeaderBits) / bitwidth;
            const std::size_t count = std::min(remaining, capacity);
            if (package.value_count != count) {
                throw std::runtime_error("Adaptive-Package value count does not match run boundary");
            }
            std::size_t bit_cursor = kHeaderBits;
            for (std::size_t index = 0; index < count; ++index) {
                flat_values.push_back(SignExtend(
                    ReadBits(package.bytes, bit_cursor, bitwidth), bitwidth));
                bit_cursor += bitwidth;
            }
            remaining -= count;
        }
        node_index = run_end;
    }
    if (package_index != encoded.packages.size() ||
        flat_values.size() != encoded.node_value_offsets.back()) {
        throw std::runtime_error("Adaptive-Package stream contains unconsumed data");
    }

    std::vector<QuantizedNode> nodes;
    nodes.reserve(encoded.node_ids.size());
    for (std::size_t index = 0; index < encoded.node_ids.size(); ++index) {
        QuantizedNode node;
        node.node_id = encoded.node_ids[index];
        node.bitwidth = encoded.node_bitwidths[index];
        node.values.assign(encoded.feature_count, 0);
        std::size_t cursor = encoded.node_value_offsets[index];
        for (int feature = 0; feature < encoded.feature_count; ++feature) {
            const std::size_t bit = index * static_cast<std::size_t>(encoded.feature_count) +
                                    feature;
            if (!GetBit(encoded.bitmap, bit)) {
                continue;
            }
            if (cursor >= encoded.node_value_offsets[index + 1]) {
                throw std::runtime_error("bitmap contains more values than the boundary stream");
            }
            node.values[feature] = flat_values[cursor++];
        }
        if (cursor != encoded.node_value_offsets[index + 1]) {
            throw std::runtime_error("boundary stream contains values absent from the bitmap");
        }
        nodes.push_back(std::move(node));
    }
    return nodes;
}

void AdaptivePackageCodec::Validate(const EncodedFeatureStream& encoded) {
    if (encoded.feature_count <= 0 || encoded.node_ids.empty() ||
        encoded.node_ids.size() != encoded.node_bitwidths.size() ||
        encoded.node_value_offsets.size() != encoded.node_ids.size() + 1 ||
        encoded.node_value_offsets.front() != 0) {
        throw std::runtime_error("invalid Adaptive-Package stream metadata");
    }
    const uint64_t bitmap_bits =
        static_cast<uint64_t>(encoded.node_ids.size()) * encoded.feature_count;
    if (encoded.bitmap.size() != CeilDiv(bitmap_bits, 8)) {
        throw std::runtime_error("Adaptive-Package bitmap length mismatch");
    }
    for (std::size_t node = 0; node < encoded.node_ids.size(); ++node) {
        if (encoded.node_ids[node] < 0 || encoded.node_bitwidths[node] < 1 ||
            encoded.node_bitwidths[node] > 8 ||
            (node > 0 && encoded.node_ids[node] <= encoded.node_ids[node - 1]) ||
            encoded.node_value_offsets[node + 1] < encoded.node_value_offsets[node]) {
            throw std::runtime_error("invalid Adaptive-Package node metadata");
        }
        const std::size_t bitmap_count = CountBitmapBits(
            encoded.bitmap, node * static_cast<std::size_t>(encoded.feature_count),
            encoded.feature_count);
        if (encoded.node_value_offsets[node + 1] - encoded.node_value_offsets[node] !=
            bitmap_count) {
            throw std::runtime_error("Adaptive-Package bitmap and boundary counts disagree");
        }
    }
    uint64_t package_bits = 0;
    uint64_t payload_bits = 0;
    for (const auto& package : encoded.packages) {
        if (package.mode > 2 || package.bitwidth < 1 || package.bitwidth > 8 ||
            package.bytes.size() * 8 != PackageBits(package.mode)) {
            throw std::runtime_error("invalid Adaptive-Package payload");
        }
        const auto mode = ReadBits(package.bytes, 0, 2);
        const auto bitwidth = ReadBits(package.bytes, 2, 3) + 1;
        if (mode != package.mode || bitwidth != package.bitwidth) {
            throw std::runtime_error("Adaptive-Package header mutation detected");
        }
        const uint64_t capacity = (PackageBits(package.mode) - kHeaderBits) /
                                  package.bitwidth;
        if (package.value_count == 0 || package.value_count > capacity) {
            throw std::runtime_error("Adaptive-Package value count exceeds capacity");
        }
        package_bits += PackageBits(package.mode);
        payload_bits += static_cast<uint64_t>(package.value_count) * package.bitwidth;
    }
    if (payload_bits != encoded.traffic.payload_bits ||
        package_bits != encoded.traffic.payload_bits + encoded.traffic.header_bits +
                        encoded.traffic.padding_bits ||
        encoded.traffic.bitmap_bits != bitmap_bits ||
        encoded.traffic.boundary_bits !=
            static_cast<uint64_t>(encoded.node_ids.size() + 1) * kBoundaryBitsPerEntry) {
        throw std::runtime_error("Adaptive-Package traffic summary is inconsistent");
    }
}

}  // namespace mega
