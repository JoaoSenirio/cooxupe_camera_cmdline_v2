#include "frame_stream_protocol.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace FrameStreamProtocol {
namespace {

void AppendU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xFF));
    out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void AppendU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xFF));
    out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out->push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

void AppendI32(std::vector<std::uint8_t>* out, std::int32_t value) {
    AppendU32(out, static_cast<std::uint32_t>(value));
}

void AppendF64(std::vector<std::uint8_t>* out, double value) {
    const std::uint8_t* raw = reinterpret_cast<const std::uint8_t*>(&value);
    out->insert(out->end(), raw, raw + sizeof(double));
}

std::uint16_t ReadU16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0]) |
           (static_cast<std::uint16_t>(data[1]) << 8);
}

std::uint32_t ReadU32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

bool FitsU32(std::size_t size) {
    return size <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
}

MessageType MessageTypeForItemType(WorkItemType type) {
    switch (type) {
        case WorkItemType::BeginJob:
            return MessageType::Begin;
        case WorkItemType::LightChunk:
            return MessageType::LightBlock;
        case WorkItemType::DarkChunk:
            return MessageType::DarkBlock;
        case WorkItemType::EndJob:
            return MessageType::End;
    }
    return MessageType::Begin;
}

bool BuildBeginPayload(const WorkItem& item,
                       std::vector<std::uint8_t>* payload,
                       std::string* error) {
    const SaveEventBegin& begin = item.begin;
    const SensorSnapshot& sensor = begin.sensor;
    const std::size_t band_count = sensor.wavelengths_nm.size();
    if (!FitsU32(band_count)) {
        if (error != nullptr) {
            *error = "wavelength list exceeds 32-bit length limits";
        }
        return false;
    }

    payload->clear();
    payload->reserve(36U + (band_count * sizeof(double)));
    AppendU32(payload, static_cast<std::uint32_t>(std::max<std::int64_t>(0, sensor.image_width)));
    AppendU32(payload, static_cast<std::uint32_t>(band_count));
    AppendU32(payload, static_cast<std::uint32_t>(std::max<std::int64_t>(0, sensor.byte_depth)));
    AppendU32(payload, static_cast<std::uint32_t>(std::max<std::int64_t>(0, sensor.frame_size_bytes)));
    AppendU32(payload, static_cast<std::uint32_t>(std::max<std::int64_t>(0, begin.expected_light_frames)));
    AppendU32(payload, static_cast<std::uint32_t>(std::max<std::int64_t>(0, begin.expected_dark_frames)));
    AppendU32(payload, static_cast<std::uint32_t>(std::max(0, begin.rgb_wavelength_nm[0])));
    AppendU32(payload, static_cast<std::uint32_t>(std::max(0, begin.rgb_wavelength_nm[1])));
    AppendU32(payload, static_cast<std::uint32_t>(std::max(0, begin.rgb_wavelength_nm[2])));
    for (double wavelength : sensor.wavelengths_nm) {
        AppendF64(payload, wavelength);
    }
    return true;
}

bool BuildChunkPayload(const WorkItem& item,
                       const std::vector<std::uint8_t>** payload,
                       std::string* error) {
    if (item.chunk.frame_count <= 0 || item.chunk.bytes == nullptr) {
        if (error != nullptr) {
            *error = "chunk payload is missing";
        }
        return false;
    }

    *payload = item.chunk.bytes.get();
    return true;
}

void BuildEndPayload(const WorkItem& item,
                     std::vector<std::uint8_t>* payload) {
    payload->clear();
    payload->reserve(16);
    payload->push_back(item.end.success ? 1U : 0U);
    payload->push_back(0U);
    payload->push_back(0U);
    payload->push_back(0U);
    AppendI32(payload, static_cast<std::int32_t>(item.end.sdk_error));
    AppendU32(payload, static_cast<std::uint32_t>(std::max<std::int64_t>(0, item.end.light_frames)));
    AppendU32(payload, static_cast<std::uint32_t>(std::max<std::int64_t>(0, item.end.dark_frames)));
}

void BuildHeader(MessageType message_type,
                 std::uint32_t payload_length,
                 std::vector<std::uint8_t>* header) {
    header->clear();
    header->reserve(kHeaderBytes);
    header->insert(header->end(), kMagic, kMagic + 4);
    AppendU16(header, kVersion);
    AppendU16(header, static_cast<std::uint16_t>(message_type));
    AppendU32(header, payload_length);
}

}  // namespace

bool SerializeWorkItem(const WorkItem& item,
                       SerializedMessage* out,
                       std::string* error) {
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "SerializeWorkItem requires a non-null output";
        }
        return false;
    }

    out->message_type = MessageTypeForItemType(item.type);
    out->inline_payload.clear();
    out->external_payload = nullptr;

    if (item.type == WorkItemType::BeginJob) {
        if (!BuildBeginPayload(item, &out->inline_payload, error)) {
            return false;
        }
        BuildHeader(out->message_type,
                    static_cast<std::uint32_t>(out->inline_payload.size()),
                    &out->header);
        return true;
    }

    if (item.type == WorkItemType::LightChunk || item.type == WorkItemType::DarkChunk) {
        const std::vector<std::uint8_t>* payload = nullptr;
        if (!BuildChunkPayload(item, &payload, error)) {
            return false;
        }
        if (!FitsU32(payload->size())) {
            if (error != nullptr) {
                *error = "chunk payload exceeds 32-bit length limits";
            }
            return false;
        }
        out->external_payload = payload;
        BuildHeader(out->message_type,
                    static_cast<std::uint32_t>(payload->size()),
                    &out->header);
        return true;
    }

    BuildEndPayload(item, &out->inline_payload);
    BuildHeader(out->message_type,
                static_cast<std::uint32_t>(out->inline_payload.size()),
                &out->header);
    return true;
}

bool ParseHeader(const std::vector<std::uint8_t>& bytes, Header* header, std::string* error) {
    if (header == nullptr) {
        if (error != nullptr) {
            *error = "ParseHeader requires a non-null header";
        }
        return false;
    }
    if (bytes.size() < kHeaderBytes) {
        if (error != nullptr) {
            *error = "Frame stream buffer is too small for header";
        }
        return false;
    }

    std::memcpy(header->magic, bytes.data(), 4);
    header->version = ReadU16(bytes.data() + 4);
    header->message_type = ReadU16(bytes.data() + 6);
    header->payload_length = ReadU32(bytes.data() + 8);
    return true;
}

void BuildAck(bool success, std::vector<std::uint8_t>* out) {
    if (out == nullptr) {
        return;
    }

    out->clear();
    out->reserve(kAckBytes);
    out->insert(out->end(), kAckMagic, kAckMagic + 4);
    AppendU16(out, kVersion);
    AppendU16(out, success ? 0U : 1U);
}

bool ParseAck(const std::vector<std::uint8_t>& bytes, Ack* ack, std::string* error) {
    if (ack == nullptr) {
        if (error != nullptr) {
            *error = "ParseAck requires a non-null ack";
        }
        return false;
    }
    if (bytes.size() < kAckBytes) {
        if (error != nullptr) {
            *error = "Ack buffer is too small";
        }
        return false;
    }

    std::memcpy(ack->magic, bytes.data(), 4);
    ack->version = ReadU16(bytes.data() + 4);
    ack->status = ReadU16(bytes.data() + 6);
    if (std::memcmp(ack->magic, kAckMagic, 4) != 0) {
        if (error != nullptr) {
            *error = "Ack magic mismatch";
        }
        return false;
    }
    if (ack->version != kVersion) {
        if (error != nullptr) {
            *error = "Ack version mismatch";
        }
        return false;
    }
    return true;
}

}  // namespace FrameStreamProtocol
