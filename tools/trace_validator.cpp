#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "json.hpp"

namespace {

using json = nlohmann::json;

constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::array<const char*, 4> kCommandNames{
    "PRE", "ACT", "READ", "WRITE",
};

[[noreturn]] void Fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string Hex64(uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

void UpdateFnv(uint64_t& hash, uint64_t value) {
    hash ^= value;
    hash *= kFnvPrime;
}

std::vector<uint8_t> DecodeBase64(const std::string& input) {
    static const std::array<int8_t, 256> table = [] {
        std::array<int8_t, 256> result{};
        result.fill(-1);
        const std::string alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (std::size_t index = 0; index < alphabet.size(); ++index) {
            result[static_cast<uint8_t>(alphabet[index])] =
                static_cast<int8_t>(index);
        }
        return result;
    }();
    if (input.size() % 4 != 0) {
        Fail("base64 payload length is invalid");
    }
    std::vector<uint8_t> output;
    output.reserve(input.size() / 4 * 3);
    for (std::size_t offset = 0; offset < input.size(); offset += 4) {
        uint32_t value = 0;
        int padding = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const uint8_t byte = static_cast<uint8_t>(input[offset + index]);
            if (byte == '=') {
                if (index < 2) {
                    Fail("base64 padding is invalid");
                }
                ++padding;
                value <<= 6U;
            } else {
                if (padding != 0 || table[byte] < 0) {
                    Fail("base64 payload contains an invalid character");
                }
                value = (value << 6U) | static_cast<uint8_t>(table[byte]);
            }
        }
        output.push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
        if (padding < 2) {
            output.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
        }
        if (padding == 0) {
            output.push_back(static_cast<uint8_t>(value & 0xffU));
        }
    }
    return output;
}

uint64_t DecodeVarint(const std::vector<uint8_t>& payload, std::size_t& offset) {
    uint64_t value = 0;
    unsigned shift = 0;
    while (offset < payload.size()) {
        const uint8_t byte = payload[offset++];
        value |= static_cast<uint64_t>(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0) {
            return value;
        }
        shift += 7;
        if (shift >= 70) {
            Fail("trace contains an oversized varint");
        }
    }
    Fail("trace ends inside a varint");
}

void RequireFields(const json& summary, const std::vector<std::string>& expected) {
    if (!summary.contains("fields") ||
        summary.at("fields").get<std::vector<std::string>>() != expected) {
        Fail("trace field schema differs");
    }
}

struct MappedBlock {
    uint64_t channel = 0;
    uint64_t bank = 0;
    uint64_t row = 0;
};

bool IsWriteClass(const std::string& request_class) {
    if (request_class == "output" || request_class == "intermediate_write") {
        return true;
    }
    if (request_class == "edge" || request_class == "input" ||
        request_class == "weight" || request_class == "intermediate_read") {
        return false;
    }
    Fail("memory request has an unknown request class");
}

MappedBlock MapBlock(const json& run, uint64_t block) {
    const auto& architecture = run.at("architecture");
    const uint64_t channels = architecture.at("hbm_channels").get<uint64_t>();
    const uint64_t banks = architecture.at(
        "hbm_banks_per_channel").get<uint64_t>();
    const uint64_t block_size = architecture.at("block_size").get<uint64_t>();
    const uint64_t row_bytes = architecture.at("hbm_row_bytes").get<uint64_t>();
    if (channels == 0 || banks == 0 || block_size == 0 ||
        row_bytes < block_size || row_bytes % block_size != 0) {
        Fail("architecture has invalid address-mapping dimensions");
    }
    const uint64_t blocks_per_row = row_bytes / block_size;
    const std::string mapping = run.at("manifest").at("address_mapping");
    if (mapping == "low-bits") {
        return {
            block % channels,
            (block / channels) % banks,
            block / (channels * banks * blocks_per_row),
        };
    }
    if (mapping != "row-first") {
        Fail("manifest has an unknown address mapping");
    }
    const uint64_t interleave = architecture.at(
        "row_first_bank_interleave").get<uint64_t>();
    if (interleave == 0 || interleave > banks || banks % interleave != 0) {
        Fail("row-first interleave is invalid");
    }
    const uint64_t interleaved_block = block / interleave;
    const uint64_t bank_groups = banks / interleave;
    return {
        (interleaved_block / blocks_per_row) % channels,
        ((interleaved_block / (blocks_per_row * channels)) % bank_groups) *
            interleave + block % interleave,
        interleaved_block / (blocks_per_row * channels * bank_groups),
    };
}

struct RequestIdentity {
    uint64_t first_block = 0;
    uint64_t block_count = 0;
    std::string request_class;
    bool is_write = false;
    uint64_t base_producer_ready_cycle = 0;
    uint64_t base_enqueue_cycle = 0;
    uint64_t producer_delay_cycles = 0;
    uint64_t producer_ready_cycle = 0;
    uint64_t enqueue_cycle = 0;
    uint64_t first_admission_cycle = 0;
    uint64_t last_admission_cycle = 0;
    uint64_t first_issue_cycle = 0;
    uint64_t completion_cycle = 0;
    uint64_t precharge_commands = 0;
    uint64_t activate_commands = 0;
    uint64_t first_precharge_cycle = 0;
    uint64_t last_precharge_cycle = 0;
    uint64_t first_activate_cycle = 0;
    uint64_t last_activate_cycle = 0;
    std::optional<uint64_t> producer_sequence;
    std::vector<uint64_t> admission_cycles;
};

struct AdmissionValidation {
    json oracle;
    std::vector<std::map<uint64_t, RequestIdentity>> layer_requests;
};

json AdmissionEventJson(
        uint64_t cycle,
        bool terminal,
        uint64_t admitted,
        uint64_t admitted_reads,
        uint64_t admitted_writes,
        const json& identities,
        const std::vector<uint64_t>& read_before,
        const std::vector<uint64_t>& read_after,
        const std::vector<uint64_t>& write_before,
        const std::vector<uint64_t>& write_after) {
    return {
        {"cycle", cycle},
        {"terminal_snapshot", terminal},
        {"admitted_blocks", admitted},
        {"admitted_read_blocks", admitted_reads},
        {"admitted_write_blocks", admitted_writes},
        {"admitted_block_identities", identities},
        {"channel_read_occupancy_before", read_before},
        {"channel_read_occupancy_after", read_after},
        {"channel_write_occupancy_before", write_before},
        {"channel_write_occupancy_after", write_after},
    };
}

AdmissionValidation ValidateAdmission(const json& run) {
    const auto& architecture = run.at("architecture");
    const uint64_t issue_limit = architecture.at(
        "coordinator_issue_blocks_per_cycle").get<uint64_t>();
    const uint64_t read_capacity = architecture.at(
        "hbm_read_queue_entries_per_channel").get<uint64_t>();
    const uint64_t write_capacity = architecture.at(
        "hbm_write_buffer_entries_per_channel").get<uint64_t>();
    const std::size_t channels = architecture.at("hbm_channels").get<std::size_t>();
    const uint64_t block_size = architecture.at("block_size").get<uint64_t>();
    uint64_t admitted_total = 0;
    uint64_t admitted_read_total = 0;
    uint64_t admitted_write_total = 0;
    uint64_t expected_total = 0;
    uint64_t expected_read_total = 0;
    uint64_t expected_write_total = 0;
    uint64_t admission_cycles = 0;
    uint64_t max_total_read = 0;
    uint64_t max_total_write = 0;
    uint64_t max_blocks_per_cycle = 0;
    std::vector<std::string> checksums;
    const std::vector<std::string> expected_fields{
        "cycle_delta", "terminal_snapshot", "admitted_blocks",
        "admitted_read_blocks", "admitted_write_blocks",
        "identity_count", "admitted_block_identities[sequence,block_offset]",
        "channel_read_occupancy_before[]",
        "channel_read_occupancy_after[]",
        "channel_write_occupancy_before[]",
        "channel_write_occupancy_after[]",
    };

    AdmissionValidation validation;
    for (const auto& layer : run.at("layers")) {
        std::map<uint64_t, RequestIdentity> requests;
        for (const auto& request : layer.at("memory_requests")) {
            const uint64_t sequence = request.at("sequence").get<uint64_t>();
            const uint64_t address = request.at("address").get<uint64_t>();
            const uint64_t bytes = request.at("bytes").get<uint64_t>();
            if (bytes == 0 || block_size == 0 ||
                address > std::numeric_limits<uint64_t>::max() - bytes) {
                Fail("memory request identity is invalid");
            }
            const uint64_t blocks = (bytes + block_size - 1) / block_size;
            RequestIdentity identity;
            identity.first_block = address / block_size;
            identity.block_count = blocks;
            identity.request_class = request.at("request_class").get<std::string>();
            identity.is_write = IsWriteClass(identity.request_class);
            identity.base_producer_ready_cycle = request.at(
                "base_producer_ready_cycle").get<uint64_t>();
            identity.base_enqueue_cycle = request.at(
                "base_enqueue_cycle").get<uint64_t>();
            identity.producer_delay_cycles = request.at(
                "producer_delay_cycles").get<uint64_t>();
            identity.producer_ready_cycle = request.at(
                "producer_ready_cycle").get<uint64_t>();
            identity.enqueue_cycle = request.at("enqueue_cycle").get<uint64_t>();
            identity.first_admission_cycle = request.at(
                "first_admission_cycle").get<uint64_t>();
            identity.last_admission_cycle = request.at(
                "last_admission_cycle").get<uint64_t>();
            identity.first_issue_cycle = request.at(
                "first_issue_cycle").get<uint64_t>();
            identity.completion_cycle = request.at(
                "completion_cycle").get<uint64_t>();
            identity.precharge_commands = request.at(
                "precharge_commands").get<uint64_t>();
            identity.activate_commands = request.at(
                "activate_commands").get<uint64_t>();
            identity.first_precharge_cycle = request.at(
                "first_precharge_cycle").get<uint64_t>();
            identity.last_precharge_cycle = request.at(
                "last_precharge_cycle").get<uint64_t>();
            identity.first_activate_cycle = request.at(
                "first_activate_cycle").get<uint64_t>();
            identity.last_activate_cycle = request.at(
                "last_activate_cycle").get<uint64_t>();
            if (!request.at("producer_sequence").is_null()) {
                identity.producer_sequence = request.at(
                    "producer_sequence").get<uint64_t>();
            }
            identity.admission_cycles.assign(
                static_cast<std::size_t>(blocks),
                std::numeric_limits<uint64_t>::max());
            if (!requests.emplace(sequence, std::move(identity)).second) {
                Fail("memory request sequence is duplicated");
            }
            expected_total += blocks;
            if (identity.is_write) {
                expected_write_total += blocks;
            } else {
                expected_read_total += blocks;
            }
        }
        const auto& summary = layer.at("transaction_admission_trace");
        if (summary.at("representation") !=
            "directional_occupancy_identity_delta_varint_base64_v3") {
            Fail("admission trace representation differs");
        }
        RequireFields(summary, expected_fields);
        if (summary.at("channel_count").get<std::size_t>() != channels) {
            Fail("admission trace channel count differs");
        }
        std::vector<uint64_t> previous_read(channels, 0);
        std::vector<uint64_t> previous_write(channels, 0);
        std::vector<uint64_t> read_peaks(channels, 0);
        std::vector<uint64_t> write_peaks(channels, 0);
        uint64_t previous_cycle = 0;
        bool have_previous_cycle = false;
        bool saw_terminal = false;
        uint64_t event_count = 0;
        uint64_t admission_event_count = 0;
        uint64_t first_cycle = 0;
        uint64_t last_cycle = 0;
        uint64_t checksum = kFnvOffset;
        std::map<std::string, uint64_t> weighted;
        std::map<std::string, std::map<uint64_t, uint64_t>> histograms;
        std::map<std::string, uint64_t> actual_max;
        std::vector<json> first_samples;
        std::vector<json> small_samples;
        std::deque<json> last_samples;

        for (const auto& chunk : summary.at("trace_chunks")) {
            const uint64_t count = chunk.at("event_count").get<uint64_t>();
            if (count == 0) {
                Fail("admission trace chunk has no events");
            }
            const auto payload = DecodeBase64(
                chunk.at("payload_base64").get<std::string>());
            std::size_t offset = 0;
            uint64_t chunk_previous_cycle = 0;
            for (uint64_t index = 0; index < count; ++index) {
                const uint64_t cycle_delta = DecodeVarint(payload, offset);
                const uint64_t cycle = index == 0
                    ? cycle_delta : chunk_previous_cycle + cycle_delta;
                chunk_previous_cycle = cycle;
                const bool terminal = DecodeVarint(payload, offset) != 0;
                const uint64_t admitted = DecodeVarint(payload, offset);
                const uint64_t admitted_reads = DecodeVarint(payload, offset);
                const uint64_t admitted_writes = DecodeVarint(payload, offset);
                const uint64_t identity_count = DecodeVarint(payload, offset);
                if (identity_count != admitted) {
                    Fail("admission identity count differs from admitted blocks");
                }
                json identities = json::array();
                std::vector<uint64_t> identity_reads(channels, 0);
                std::vector<uint64_t> identity_writes(channels, 0);
                for (uint64_t identity_index = 0;
                     identity_index < identity_count; ++identity_index) {
                    const uint64_t sequence = DecodeVarint(payload, offset);
                    const uint64_t block_offset = DecodeVarint(payload, offset);
                    auto request = requests.find(sequence);
                    if (request == requests.end()) {
                        Fail("admission trace references an unknown request sequence");
                    }
                    if (block_offset >= request->second.block_count) {
                        Fail("admission trace block offset is outside its request");
                    }
                    auto& admission_cycle = request->second.admission_cycles.at(
                        static_cast<std::size_t>(block_offset));
                    if (admission_cycle != std::numeric_limits<uint64_t>::max()) {
                        Fail("request block has duplicate admission identities");
                    }
                    admission_cycle = cycle;
                    const MappedBlock mapped = MapBlock(
                        run, request->second.first_block + block_offset);
                    if (request->second.is_write) {
                        ++identity_writes.at(static_cast<std::size_t>(mapped.channel));
                    } else {
                        ++identity_reads.at(static_cast<std::size_t>(mapped.channel));
                    }
                    identities.push_back({
                        {"sequence", sequence},
                        {"block_offset", block_offset},
                    });
                }
                std::array<std::vector<uint64_t>, 4> arrays;
                for (auto& values : arrays) {
                    values.resize(channels);
                    for (std::size_t channel = 0; channel < channels; ++channel) {
                        values[channel] = DecodeVarint(payload, offset);
                    }
                }
                const auto& read_before = arrays[0];
                const auto& read_after = arrays[1];
                const auto& write_before = arrays[2];
                const auto& write_after = arrays[3];
                if (have_previous_cycle && cycle <= previous_cycle) {
                    Fail("admission cycles are not strictly increasing");
                }
                if (saw_terminal) {
                    Fail("admission event follows terminal snapshot");
                }
                if (terminal) {
                    saw_terminal = true;
                    if (admitted != 0) {
                        Fail("terminal snapshot admits blocks");
                    }
                } else if (admitted == 0 || admitted > issue_limit) {
                    Fail("admission issue width is invalid");
                }
                uint64_t inferred_read_dispatch = 0;
                uint64_t inferred_write_dispatch = 0;
                uint64_t derived_read_admission = 0;
                uint64_t derived_write_admission = 0;
                uint64_t total_read_before = 0;
                uint64_t total_read_after = 0;
                uint64_t total_write_before = 0;
                uint64_t total_write_after = 0;
                uint64_t max_channel_read_after = 0;
                uint64_t max_channel_write_after = 0;
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    if (read_before[channel] > previous_read[channel] ||
                        write_before[channel] > previous_write[channel] ||
                        read_after[channel] < read_before[channel] ||
                        write_after[channel] < write_before[channel]) {
                        Fail("admission occupancy transition is invalid");
                    }
                    if (read_after[channel] > read_capacity ||
                        write_after[channel] > write_capacity) {
                        Fail("directional transaction queue capacity exceeded");
                    }
                    inferred_read_dispatch += previous_read[channel] - read_before[channel];
                    inferred_write_dispatch += previous_write[channel] - write_before[channel];
                    derived_read_admission += read_after[channel] - read_before[channel];
                    derived_write_admission += write_after[channel] - write_before[channel];
                    total_read_before += read_before[channel];
                    total_read_after += read_after[channel];
                    total_write_before += write_before[channel];
                    total_write_after += write_after[channel];
                    max_channel_read_after = std::max(
                        max_channel_read_after, read_after[channel]);
                    max_channel_write_after = std::max(
                        max_channel_write_after, write_after[channel]);
                    read_peaks[channel] = std::max(read_peaks[channel], read_after[channel]);
                    write_peaks[channel] = std::max(write_peaks[channel], write_after[channel]);
                }
                if (derived_read_admission != admitted_reads ||
                    derived_write_admission != admitted_writes ||
                    admitted != admitted_reads + admitted_writes) {
                    Fail("admission directional delta differs");
                }
                uint64_t identity_read_total = 0;
                uint64_t identity_write_total = 0;
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    const uint64_t read_delta = read_after[channel] - read_before[channel];
                    const uint64_t write_delta = write_after[channel] - write_before[channel];
                    if (identity_reads[channel] != read_delta ||
                        identity_writes[channel] != write_delta) {
                        Fail("admission block identity differs from directional occupancy");
                    }
                    identity_read_total += identity_reads[channel];
                    identity_write_total += identity_writes[channel];
                }
                if (identity_read_total != admitted_reads ||
                    identity_write_total != admitted_writes) {
                    Fail("admission block identity direction totals differ");
                }
                if (terminal && (total_read_before != 0 || total_read_after != 0 ||
                                 total_write_before != 0 || total_write_after != 0)) {
                    Fail("terminal occupancy is not zero");
                }
                const std::map<std::string, uint64_t> scalar_values{
                    {"admitted_blocks", admitted},
                    {"admitted_read_blocks", admitted_reads},
                    {"admitted_write_blocks", admitted_writes},
                    {"total_read_occupancy_after", total_read_after},
                    {"total_write_occupancy_after", total_write_after},
                    {"max_channel_read_occupancy_after", max_channel_read_after},
                    {"max_channel_write_occupancy_after", max_channel_write_after},
                };
                for (const auto& [name, value] : scalar_values) {
                    ++histograms[name][value];
                }
                weighted["admitted_blocks"] += admitted;
                weighted["admitted_read_blocks"] += admitted_reads;
                weighted["admitted_write_blocks"] += admitted_writes;
                weighted["inferred_dispatched_read_blocks"] += inferred_read_dispatch;
                weighted["inferred_dispatched_write_blocks"] += inferred_write_dispatch;
                weighted["total_read_occupancy_before"] += total_read_before;
                weighted["total_read_occupancy_after"] += total_read_after;
                weighted["total_write_occupancy_before"] += total_write_before;
                weighted["total_write_occupancy_after"] += total_write_after;
                actual_max["blocks_admitted_per_cycle"] = std::max(
                    actual_max["blocks_admitted_per_cycle"], admitted);
                actual_max["total_read_occupancy"] = std::max(
                    actual_max["total_read_occupancy"], total_read_after);
                actual_max["total_write_occupancy"] = std::max(
                    actual_max["total_write_occupancy"], total_write_after);
                actual_max["channel_read_occupancy"] = std::max(
                    actual_max["channel_read_occupancy"], max_channel_read_after);
                actual_max["channel_write_occupancy"] = std::max(
                    actual_max["channel_write_occupancy"], max_channel_write_after);
                for (const uint64_t value : {
                         cycle, static_cast<uint64_t>(terminal), admitted,
                         admitted_reads, admitted_writes}) {
                    UpdateFnv(checksum, value);
                }
                for (const auto& identity : identities) {
                    UpdateFnv(checksum, identity.at("sequence").get<uint64_t>());
                    UpdateFnv(checksum, identity.at("block_offset").get<uint64_t>());
                }
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    for (const uint64_t value : {
                             read_before[channel], read_after[channel],
                             write_before[channel], write_after[channel]}) {
                        UpdateFnv(checksum, value);
                    }
                }
                const json sample = AdmissionEventJson(
                    cycle, terminal, admitted, admitted_reads, admitted_writes,
                    identities,
                    read_before, read_after, write_before, write_after);
                if (event_count < 16) {
                    first_samples.push_back(sample);
                }
                if (event_count < 33) {
                    small_samples.push_back(sample);
                }
                if (last_samples.size() == 16) {
                    last_samples.pop_front();
                }
                last_samples.push_back(sample);
                previous_read = read_after;
                previous_write = write_after;
                first_cycle = event_count == 0 ? cycle : first_cycle;
                last_cycle = cycle;
                previous_cycle = cycle;
                have_previous_cycle = true;
                ++event_count;
                admission_event_count += terminal ? 0 : 1;
            }
            if (offset != payload.size()) {
                Fail("admission trace chunk has trailing bytes");
            }
        }
        if (!saw_terminal || std::any_of(previous_read.begin(), previous_read.end(),
                                         [](uint64_t value) { return value != 0; }) ||
            std::any_of(previous_write.begin(), previous_write.end(),
                        [](uint64_t value) { return value != 0; })) {
            Fail("admission trace lacks a zero terminal snapshot");
        }
        if (weighted["inferred_dispatched_read_blocks"] !=
                weighted["admitted_read_blocks"] ||
            weighted["inferred_dispatched_write_blocks"] !=
                weighted["admitted_write_blocks"]) {
            Fail("inferred directional dispatch totals differ");
        }
        if (event_count != summary.at("event_count").get<uint64_t>() ||
            admission_event_count !=
                summary.at("admission_event_count").get<uint64_t>() ||
            first_cycle != summary.at("first_cycle").get<uint64_t>() ||
            last_cycle != summary.at("last_cycle").get<uint64_t>()) {
            Fail("admission trace bounds differ");
        }
        std::vector<json> expected_samples;
        if (event_count <= 32) {
            expected_samples = small_samples;
        } else {
            expected_samples = first_samples;
            expected_samples.insert(
                expected_samples.end(), last_samples.begin(), last_samples.end());
        }
        if (json(expected_samples) != summary.at("edge_samples")) {
            Fail("admission edge samples differ");
        }
        json weighted_json = json::object();
        for (const auto& [name, value] : weighted) {
            weighted_json[name] = value;
        }
        if (weighted_json != summary.at("weighted_totals")) {
            Fail("admission weighted totals differ");
        }
        json histograms_json = json::object();
        for (const auto& [name, values] : histograms) {
            for (const auto& [value, count] : values) {
                histograms_json[name][std::to_string(value)] = count;
            }
        }
        if (histograms_json != summary.at("histograms")) {
            Fail("admission histograms differ");
        }
        json actual_max_json = json::object();
        for (const auto& [name, value] : actual_max) {
            actual_max_json[name] = value;
        }
        if (actual_max_json != summary.at("actual_max")) {
            Fail("admission actual maxima differ");
        }
        if (Hex64(checksum) != summary.at("trace_checksum_fnv1a64")) {
            Fail("admission checksum differs");
        }
        if (read_peaks != summary.at(
                "max_channel_read_queue_occupancy").get<std::vector<uint64_t>>() ||
            write_peaks != summary.at(
                "max_channel_write_buffer_occupancy").get<std::vector<uint64_t>>()) {
            Fail("admission peak vectors differ");
        }
        admitted_total += weighted["admitted_blocks"];
        admitted_read_total += weighted["admitted_read_blocks"];
        admitted_write_total += weighted["admitted_write_blocks"];
        admission_cycles += admission_event_count;
        max_total_read = std::max(
            max_total_read, actual_max["total_read_occupancy"]);
        max_total_write = std::max(
            max_total_write, actual_max["total_write_occupancy"]);
        max_blocks_per_cycle = std::max(
            max_blocks_per_cycle, actual_max["blocks_admitted_per_cycle"]);
        checksums.push_back(Hex64(checksum));

        for (const auto& [sequence, request] : requests) {
            (void)sequence;
            if (std::any_of(request.admission_cycles.begin(),
                            request.admission_cycles.end(),
                            [](uint64_t cycle) {
                                return cycle == std::numeric_limits<uint64_t>::max();
                            })) {
                Fail("admission trace has missing request-block identities");
            }
            const auto bounds = std::minmax_element(
                request.admission_cycles.begin(), request.admission_cycles.end());
            if (*bounds.first != request.first_admission_cycle ||
                *bounds.second != request.last_admission_cycle) {
                Fail("request admission timeline differs from block identities");
            }
            if (request.first_admission_cycle < request.enqueue_cycle ||
                request.last_admission_cycle < request.first_admission_cycle ||
                request.first_issue_cycle < request.first_admission_cycle) {
                Fail("request admission violates causality");
            }
        }
        validation.layer_requests.push_back(std::move(requests));
    }
    if (admitted_total != expected_total ||
        admitted_read_total != expected_read_total ||
        admitted_write_total != expected_write_total) {
        Fail("admission trace does not cover all request blocks");
    }
    validation.oracle = {
        {"admitted_blocks", admitted_total},
        {"admitted_read_blocks", admitted_read_total},
        {"admitted_write_blocks", admitted_write_total},
        {"admission_cycles", admission_cycles},
        {"max_blocks_admitted_per_cycle", max_blocks_per_cycle},
        {"issue_limit_blocks_per_cycle", issue_limit},
        {"max_total_read_queue_occupancy", max_total_read},
        {"max_total_write_buffer_occupancy", max_total_write},
        {"total_read_queue_capacity", channels * read_capacity},
        {"total_write_buffer_capacity", channels * write_capacity},
        {"trace_checksums", checksums},
        {"occupancy_reconstruction",
         "per-channel before/after admission snapshots plus request-block identity"},
        {"block_identity_cross_check", "PASS"},
        {"validator", "independent-cpp-v1"},
    };
    return validation;
}

struct ChannelTiming {
    uint64_t last_command = 0;
    bool saw_command = false;
    uint64_t next_read = 0;
    uint64_t next_write = 0;
};

struct BankTiming {
    uint64_t open_row = 0;
    bool row_open = false;
    uint64_t next_precharge = 0;
    uint64_t next_activate = 0;
    uint64_t next_read = 0;
    uint64_t next_write = 0;
};

json CommandEventJson(uint64_t cycle, uint64_t sequence, uint64_t block_offset,
                      uint64_t channel, uint64_t bank, uint64_t row,
                      uint64_t command) {
    return {
        {"cycle", cycle},
        {"sequence", sequence},
        {"block_offset", block_offset},
        {"channel", channel},
        {"bank", bank},
        {"row", row},
        {"command", kCommandNames.at(command)},
    };
}

struct RequestCommandStats {
    uint64_t first_issue_cycle = std::numeric_limits<uint64_t>::max();
    uint64_t completion_cycle = 0;
    uint64_t precharge_commands = 0;
    uint64_t activate_commands = 0;
    uint64_t first_precharge_cycle = std::numeric_limits<uint64_t>::max();
    uint64_t last_precharge_cycle = 0;
    uint64_t first_activate_cycle = std::numeric_limits<uint64_t>::max();
    uint64_t last_activate_cycle = 0;
    std::vector<bool> data_commands;
};

struct ProducerValidation {
    uint64_t dependencies = 0;
    uint64_t input_dependencies = 0;
    uint64_t intermediate_raw_dependencies = 0;
    std::map<uint64_t, uint64_t> delay_histogram;
};

ProducerValidation ValidateProducerDependencies(
        const std::map<uint64_t, RequestIdentity>& requests,
        const std::map<uint64_t, RequestCommandStats>& command_stats,
        uint64_t neighbor_index_ready_cycles) {
    for (const auto& [sequence, request] : requests) {
        if (!request.producer_sequence.has_value()) {
            continue;
        }
        if (*request.producer_sequence == sequence) {
            Fail("request has a self producer dependency");
        }
        if (requests.find(*request.producer_sequence) == requests.end()) {
            Fail("producer sequence is unknown");
        }
    }

    std::map<uint64_t, int> visit_state;
    std::function<void(uint64_t)> visit = [&](uint64_t sequence) {
        const int state = visit_state[sequence];
        if (state == 1) {
            Fail("producer dependency graph contains a cycle");
        }
        if (state == 2) {
            return;
        }
        visit_state[sequence] = 1;
        const auto& request = requests.at(sequence);
        if (request.producer_sequence.has_value()) {
            visit(*request.producer_sequence);
        }
        visit_state[sequence] = 2;
    };
    for (const auto& [sequence, request] : requests) {
        (void)request;
        visit(sequence);
    }

    ProducerValidation validation;
    for (const auto& [sequence, request] : requests) {
        const bool requires_producer = request.request_class == "input" ||
            request.request_class == "intermediate_read";
        if (requires_producer != request.producer_sequence.has_value()) {
            Fail("request class producer dependency presence differs");
        }
        if (!request.producer_sequence.has_value()) {
            if (request.producer_delay_cycles != 0) {
                Fail("request without a producer has a nonzero producer delay");
            }
            const uint64_t expected_ready = request.base_producer_ready_cycle;
            const uint64_t expected_enqueue = std::max(
                request.base_enqueue_cycle, expected_ready);
            if (request.producer_ready_cycle != expected_ready ||
                request.enqueue_cycle != expected_enqueue) {
                Fail("independent request effective timeline differs from base fields");
            }
            continue;
        }

        const uint64_t producer_sequence = *request.producer_sequence;
        const auto& producer = requests.at(producer_sequence);
        if (request.request_class == "input") {
            if (producer.request_class != "edge" ||
                request.producer_delay_cycles != neighbor_index_ready_cycles) {
                Fail("input producer class or delay differs");
            }
            ++validation.input_dependencies;
        } else if (request.request_class == "intermediate_read") {
            if (producer.request_class != "intermediate_write" ||
                request.producer_delay_cycles != 0) {
                Fail("intermediate RAW producer class or delay differs");
            }
            ++validation.intermediate_raw_dependencies;
        } else {
            Fail("request class has an unsupported producer dependency");
        }

        const uint64_t producer_completion = command_stats.at(
            producer_sequence).completion_cycle;
        if (producer_completion > std::numeric_limits<uint64_t>::max() -
                request.producer_delay_cycles) {
            Fail("producer completion plus delay overflows");
        }
        const uint64_t dependency_ready =
            producer_completion + request.producer_delay_cycles;
        const uint64_t expected_ready = std::max(
            request.base_producer_ready_cycle, dependency_ready);
        const uint64_t expected_enqueue = std::max(
            request.base_enqueue_cycle, expected_ready);
        if (request.first_admission_cycle < dependency_ready ||
            request.first_issue_cycle < dependency_ready) {
            Fail("consumer request precedes reconstructed producer completion");
        }
        if (request.producer_ready_cycle != expected_ready) {
            Fail("request producer-ready cycle differs from reconstructed dependency graph");
        }
        if (request.enqueue_cycle != expected_enqueue) {
            Fail("request enqueue cycle differs from reconstructed dependency graph");
        }
        ++validation.dependencies;
        ++validation.delay_histogram[request.producer_delay_cycles];
    }
    return validation;
}

struct CommandValidation {
    json command_oracle;
    json producer_oracle;
};

CommandValidation ValidateCommands(
        const json& run, const AdmissionValidation& admission) {
    const auto& architecture = run.at("architecture");
    const uint64_t interval = architecture.at(
        "hbm_command_issue_interval_cycles").get<uint64_t>();
    const std::size_t channels = architecture.at("hbm_channels").get<std::size_t>();
    const std::size_t banks_per_channel = architecture.at(
        "hbm_banks_per_channel").get<std::size_t>();
    const std::vector<std::string> expected_fields{
        "cycle_delta", "sequence", "block_offset", "channel", "bank",
        "row", "command",
    };
    uint64_t total_events = 0;
    uint64_t expected_request_blocks = 0;
    uint64_t read_data_commands = 0;
    uint64_t write_data_commands = 0;
    uint64_t reconstructed_requests = 0;
    uint64_t producer_dependencies = 0;
    uint64_t input_dependencies = 0;
    uint64_t intermediate_raw_dependencies = 0;
    std::map<uint64_t, uint64_t> producer_delay_histogram;
    json producer_layers = json::array();
    std::vector<std::string> checksums;
    const uint64_t block_size = architecture.at("block_size").get<uint64_t>();
    const double bytes_per_channel_cycle = std::max(
        1.0,
        architecture.at("hbm_bandwidth_gbps").get<double>() /
            architecture.at("frequency_ghz").get<double>() /
            static_cast<double>(channels));
    const uint64_t transfer_cycles = std::max<uint64_t>(
        1, static_cast<uint64_t>(std::ceil(
            static_cast<double>(block_size) / bytes_per_channel_cycle)));
    const uint64_t read_latency = architecture.at(
        "hbm_read_row_hit_cycles").get<uint64_t>();
    const uint64_t write_latency = architecture.at(
        "hbm_write_row_hit_cycles").get<uint64_t>();
    std::size_t layer_index = 0;
    for (const auto& layer : run.at("layers")) {
        const auto& summary = layer.at("command_trace");
        if (summary.at("representation") != "command_delta_varint_base64_v2") {
            Fail("command trace representation differs");
        }
        RequireFields(summary, expected_fields);
        if (layer_index >= admission.layer_requests.size()) {
            Fail("admission timeline layer count differs");
        }
        const auto& requests = admission.layer_requests[layer_index];
        std::map<uint64_t, RequestCommandStats> request_stats;
        for (const auto& [sequence, request] : requests) {
            RequestCommandStats stats;
            stats.data_commands.assign(
                static_cast<std::size_t>(request.block_count), false);
            request_stats.emplace(sequence, std::move(stats));
            expected_request_blocks += request.block_count;
        }
        std::vector<ChannelTiming> channel_state(channels);
        std::vector<BankTiming> bank_state(channels * banks_per_channel);
        std::array<uint64_t, 4> counts{};
        uint64_t checksum = kFnvOffset;
        uint64_t decoded = 0;
        uint64_t previous_cycle = 0;
        bool have_previous_cycle = false;
        std::vector<json> first_samples;
        std::deque<json> last_samples;
        for (const auto& chunk : summary.at("trace_chunks")) {
            const uint64_t count = chunk.at("event_count").get<uint64_t>();
            if (count == 0) {
                Fail("command trace chunk has no events");
            }
            const auto payload = DecodeBase64(
                chunk.at("payload_base64").get<std::string>());
            std::size_t offset = 0;
            uint64_t chunk_previous_cycle = 0;
            for (uint64_t index = 0; index < count; ++index) {
                std::array<uint64_t, 7> values{};
                for (uint64_t& value : values) {
                    value = DecodeVarint(payload, offset);
                }
                const uint64_t cycle = index == 0
                    ? values[0] : chunk_previous_cycle + values[0];
                chunk_previous_cycle = cycle;
                const uint64_t sequence = values[1];
                const uint64_t block_offset = values[2];
                const uint64_t channel = values[3];
                const uint64_t bank = values[4];
                const uint64_t row = values[5];
                const uint64_t command = values[6];
                if (command >= kCommandNames.size() || channel >= channels ||
                    bank >= banks_per_channel) {
                    Fail("command trace event is out of range");
                }
                const auto request = requests.find(sequence);
                if (request == requests.end()) {
                    Fail("command trace references an unknown request sequence");
                }
                if (block_offset >= request->second.block_count ||
                    request->second.first_block >
                        std::numeric_limits<uint64_t>::max() - block_offset) {
                    Fail("command trace block offset is outside its request");
                }
                const MappedBlock mapped = MapBlock(
                    run, request->second.first_block + block_offset);
                if (channel != mapped.channel || bank != mapped.bank) {
                    Fail("command trace channel/bank differs from request address mapping");
                }
                if (command != 0 && row != mapped.row) {
                    Fail("command trace row differs from request address mapping");
                }
                if (cycle < request->second.producer_ready_cycle) {
                    Fail("command precedes request producer-ready cycle");
                }
                if (cycle < request->second.enqueue_cycle) {
                    Fail("command precedes request enqueue cycle");
                }
                const uint64_t admission_cycle = request->second.admission_cycles.at(
                    static_cast<std::size_t>(block_offset));
                if (cycle < admission_cycle) {
                    Fail("command precedes matching block admission");
                }
                auto& stats = request_stats.at(sequence);
                if (command == 2 || command == 3) {
                    const uint64_t expected_command = request->second.is_write ? 3 : 2;
                    if (command != expected_command) {
                        Fail("data command direction differs from request class");
                    }
                    const std::size_t data_index = static_cast<std::size_t>(
                        block_offset);
                    if (stats.data_commands.at(data_index)) {
                        Fail("request block has duplicate data commands");
                    }
                    stats.data_commands[data_index] = true;
                    stats.first_issue_cycle = std::min(
                        stats.first_issue_cycle, cycle);
                    stats.completion_cycle = std::max(
                        stats.completion_cycle,
                        cycle + (command == 3 ? write_latency : read_latency) +
                            transfer_cycles);
                    if (command == 2) {
                        ++read_data_commands;
                    } else {
                        ++write_data_commands;
                    }
                } else if (command == 0) {
                    ++stats.precharge_commands;
                    stats.first_precharge_cycle = std::min(
                        stats.first_precharge_cycle, cycle);
                    stats.last_precharge_cycle = std::max(
                        stats.last_precharge_cycle, cycle);
                } else {
                    ++stats.activate_commands;
                    stats.first_activate_cycle = std::min(
                        stats.first_activate_cycle, cycle);
                    stats.last_activate_cycle = std::max(
                        stats.last_activate_cycle, cycle);
                }
                if (have_previous_cycle && cycle < previous_cycle) {
                    Fail("command trace is not time ordered");
                }
                previous_cycle = cycle;
                have_previous_cycle = true;
                auto& channel_timing = channel_state[channel];
                if (channel_timing.saw_command &&
                    cycle < channel_timing.last_command + interval) {
                    Fail("commands overlap on a channel lane");
                }
                channel_timing.saw_command = true;
                channel_timing.last_command = cycle;
                auto& timing = bank_state[
                    channel * banks_per_channel + bank];
                switch (command) {
                    case 0:
                        if (!timing.row_open || timing.open_row != row ||
                            cycle < timing.next_precharge) {
                            Fail("PRE violates row recovery");
                        }
                        timing.row_open = false;
                        timing.next_activate = std::max(
                            timing.next_activate,
                            cycle + architecture.at(
                                "hbm_precharge_to_activate_cycles").get<uint64_t>());
                        break;
                    case 1:
                        if (timing.row_open || cycle < timing.next_activate) {
                            Fail("ACT violates bank recovery");
                        }
                        timing.row_open = true;
                        timing.open_row = row;
                        timing.next_read = std::max(
                            timing.next_read,
                            cycle + architecture.at(
                                "hbm_activate_to_read_cycles").get<uint64_t>());
                        timing.next_write = std::max(
                            timing.next_write,
                            cycle + architecture.at(
                                "hbm_activate_to_write_cycles").get<uint64_t>());
                        timing.next_precharge = std::max(
                            timing.next_precharge,
                            cycle + architecture.at(
                                "hbm_activate_to_precharge_cycles").get<uint64_t>());
                        timing.next_activate = std::max(
                            timing.next_activate,
                            cycle + architecture.at(
                                "hbm_activate_to_activate_cycles").get<uint64_t>());
                        break;
                    case 2:
                        if (!timing.row_open || timing.open_row != row ||
                            cycle < timing.next_read ||
                            cycle < channel_timing.next_read) {
                            Fail("READ violates tRCD/tCCD/turnaround");
                        }
                        channel_timing.next_read = std::max(
                            channel_timing.next_read,
                            cycle + architecture.at(
                                "hbm_read_to_read_cycles").get<uint64_t>());
                        channel_timing.next_write = std::max(
                            channel_timing.next_write,
                            cycle + architecture.at(
                                "hbm_read_to_write_cycles").get<uint64_t>());
                        timing.next_precharge = std::max(
                            timing.next_precharge,
                            cycle + architecture.at(
                                "hbm_read_to_precharge_cycles").get<uint64_t>());
                        break;
                    case 3:
                        if (!timing.row_open || timing.open_row != row ||
                            cycle < timing.next_write ||
                            cycle < channel_timing.next_write) {
                            Fail("WRITE violates tRCD/tCCD/turnaround");
                        }
                        channel_timing.next_write = std::max(
                            channel_timing.next_write,
                            cycle + architecture.at(
                                "hbm_write_to_write_cycles").get<uint64_t>());
                        channel_timing.next_read = std::max(
                            channel_timing.next_read,
                            cycle + architecture.at(
                                "hbm_write_to_read_cycles").get<uint64_t>());
                        timing.next_precharge = std::max(
                            timing.next_precharge,
                            cycle + architecture.at(
                                "hbm_write_to_precharge_cycles").get<uint64_t>());
                        break;
                    default:
                        Fail("unknown command");
                }
                ++counts[command];
                for (const uint64_t value : {
                         cycle, sequence, block_offset, channel, bank, row, command}) {
                    UpdateFnv(checksum, value);
                }
                const json sample = CommandEventJson(
                    cycle, sequence, block_offset, channel, bank, row, command);
                if (decoded < 32) {
                    first_samples.push_back(sample);
                } else {
                    if (last_samples.size() == 32) {
                        last_samples.pop_front();
                    }
                    last_samples.push_back(sample);
                }
                ++decoded;
            }
            if (offset != payload.size()) {
                Fail("command trace chunk has trailing bytes");
            }
        }
        const std::array<uint64_t, 4> expected_counts{
            layer.at("precharge_commands").get<uint64_t>(),
            layer.at("activate_commands").get<uint64_t>(),
            layer.at("read_commands").get<uint64_t>(),
            layer.at("write_commands").get<uint64_t>(),
        };
        if (counts != expected_counts ||
            decoded != summary.at("event_count").get<uint64_t>()) {
            Fail("command event or type totals differ");
        }
        for (const auto& [sequence, request] : requests) {
            const auto& stats = request_stats.at(sequence);
            if (std::any_of(stats.data_commands.begin(), stats.data_commands.end(),
                            [](bool seen) { return !seen; })) {
                Fail("command trace has missing request-block data commands");
            }
            const uint64_t first_precharge = stats.precharge_commands == 0
                ? 0 : stats.first_precharge_cycle;
            const uint64_t first_activate = stats.activate_commands == 0
                ? 0 : stats.first_activate_cycle;
            if (stats.first_issue_cycle != request.first_issue_cycle ||
                stats.completion_cycle != request.completion_cycle ||
                stats.precharge_commands != request.precharge_commands ||
                stats.activate_commands != request.activate_commands ||
                first_precharge != request.first_precharge_cycle ||
                stats.last_precharge_cycle != request.last_precharge_cycle ||
                first_activate != request.first_activate_cycle ||
                stats.last_activate_cycle != request.last_activate_cycle) {
                Fail("request command timeline differs from reconstructed events");
            }
            ++reconstructed_requests;
        }
        const ProducerValidation producer_validation = ValidateProducerDependencies(
            requests, request_stats,
            architecture.at("neighbor_index_ready_cycles").get<uint64_t>());
        producer_dependencies += producer_validation.dependencies;
        input_dependencies += producer_validation.input_dependencies;
        intermediate_raw_dependencies +=
            producer_validation.intermediate_raw_dependencies;
        for (const auto& [delay, count] : producer_validation.delay_histogram) {
            producer_delay_histogram[delay] += count;
        }
        producer_layers.push_back({
            {"layer_index", layer_index},
            {"dependencies", producer_validation.dependencies},
            {"input_dependencies", producer_validation.input_dependencies},
            {"intermediate_raw_dependencies",
             producer_validation.intermediate_raw_dependencies},
        });
        std::vector<json> samples = first_samples;
        samples.insert(samples.end(), last_samples.begin(), last_samples.end());
        if (json(samples) != summary.at("samples")) {
            Fail("command samples differ from the full trace");
        }
        if (Hex64(checksum) != summary.at("checksum_fnv1a64")) {
            Fail("command checksum differs");
        }
        if (summary.at("command_lane_violations").get<uint64_t>() != 0) {
            Fail("simulator reports a command-lane violation");
        }
        total_events += decoded;
        checksums.push_back(Hex64(checksum));
        ++layer_index;
    }
    if (layer_index != admission.layer_requests.size()) {
        Fail("admission timeline layer count differs");
    }
    if (read_data_commands !=
            admission.oracle.at("admitted_read_blocks").get<uint64_t>() ||
        write_data_commands !=
            admission.oracle.at("admitted_write_blocks").get<uint64_t>() ||
        expected_request_blocks !=
            admission.oracle.at("admitted_blocks").get<uint64_t>()) {
        Fail("command data blocks differ from directional admission totals");
    }
    json delay_histogram = json::object();
    for (const auto& [delay, count] : producer_delay_histogram) {
        delay_histogram[std::to_string(delay)] = count;
    }
    CommandValidation validation;
    validation.command_oracle = {
        {"event_count", total_events},
        {"checksums", checksums},
        {"command_issue_interval_cycles", interval},
        {"command_lane_violations", 0},
        {"trace_representation", "command_delta_varint_base64_v2"},
        {"independently_reconstructed", true},
        {"expected_request_blocks", expected_request_blocks},
        {"read_data_commands", read_data_commands},
        {"write_data_commands", write_data_commands},
        {"reconstructed_requests", reconstructed_requests},
        {"request_identity_violations", 0},
        {"address_mapping_violations", 0},
        {"direction_violations", 0},
        {"duplicate_or_missing_data_commands", 0},
        {"admission_data_cross_check", "request-block identity and cycle PASS"},
        {"request_timeline_cross_check", "PASS"},
        {"transfer_cycles", transfer_cycles},
        {"validator", "independent-cpp-v1"},
    };
    validation.producer_oracle = {
        {"dependencies", producer_dependencies},
        {"input_dependencies", input_dependencies},
        {"intermediate_raw_dependencies", intermediate_raw_dependencies},
        {"producer_delay_histogram", delay_histogram},
        {"layers", producer_layers},
        {"producer_graph_cross_check", "PASS"},
        {"producer_timing_cross_check", "PASS"},
        {"unknown_producer_violations", 0},
        {"self_dependency_violations", 0},
        {"cycle_violations", 0},
        {"future_or_late_producer_violations", 0},
        {"class_or_delay_violations", 0},
        {"validator", "independent-cpp-v1"},
    };
    return validation;
}

json ValidateFile(const std::string& path) {
    std::ifstream stream(path);
    if (!stream) {
        Fail("cannot open input: " + path);
    }
    json run;
    stream >> run;
    const AdmissionValidation admission = ValidateAdmission(run);
    const CommandValidation commands = ValidateCommands(run, admission);
    return {
        {"transaction_admission_oracle", admission.oracle},
        {"command_trace_oracle", commands.command_oracle},
        {"producer_dependency_oracle", commands.producer_oracle},
    };
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: hygcn_trace_validator RESULT.json\n";
        return 2;
    }
    try {
        std::cout << ValidateFile(argv[1]).dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
