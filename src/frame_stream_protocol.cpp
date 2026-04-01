#include "frame_stream_protocol.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>

namespace FrameStreamProtocol {
namespace {

std::string EscapeJson(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

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

void AppendU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
    AppendU32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFFULL));
    AppendU32(out, static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFFULL));
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

std::uint64_t ReadU64(const std::uint8_t* data) {
    return static_cast<std::uint64_t>(ReadU32(data)) |
           (static_cast<std::uint64_t>(ReadU32(data + 4)) << 32);
}

bool FitsU32(std::size_t size) {
    return size <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
}

std::uint32_t MessageTypeForEvent(FrameStreamEventType type) {
    switch (type) {
        case FrameStreamEventType::JobBegin:
            return static_cast<std::uint32_t>(MessageType::JobBegin);
        case FrameStreamEventType::LightRgbBlock:
            return static_cast<std::uint32_t>(MessageType::LightRgbBlock);
        case FrameStreamEventType::JobEnd:
            return static_cast<std::uint32_t>(MessageType::JobEnd);
    }
    return 0;
}

std::string BuildBeginMetadata(const FrameStreamEventBegin& begin) {
    std::ostringstream oss;
    oss << "{"
        << "\"sample_name\":\"" << EscapeJson(begin.sample_name) << "\","
        << "\"camera_name\":\"" << EscapeJson(begin.camera_name) << "\","
        << "\"acquisition_name\":\"" << EscapeJson(begin.acquisition_name) << "\","
        << "\"final_png_path\":\"" << EscapeJson(begin.final_png_path) << "\","
        << "\"image_width\":" << begin.image_width << ","
        << "\"expected_light_frames\":" << begin.expected_light_frames << ","
        << "\"expected_dark_frames\":" << begin.expected_dark_frames << ","
        << "\"source_byte_depth\":" << begin.source_byte_depth << ","
        << "\"rgb_wavelength_nm\":[" << begin.rgb_wavelength_nm[0] << ","
        << begin.rgb_wavelength_nm[1] << ","
        << begin.rgb_wavelength_nm[2] << "],"
        << "\"resolved_rgb_band_indices\":[" << begin.resolved_rgb_band_indices[0] << ","
        << begin.resolved_rgb_band_indices[1] << ","
        << begin.resolved_rgb_band_indices[2] << "]"
        << "}";
    return oss.str();
}

std::string BuildLightBlockMetadata(const FrameStreamEventLightRgbBlock& block) {
    std::ostringstream oss;
    oss << "{"
        << "\"line_count\":" << block.line_count << ","
        << "\"line_index_start\":" << block.line_index_start << ","
        << "\"first_frame_number\":" << block.first_frame_number << ","
        << "\"last_frame_number\":" << block.last_frame_number << ","
        << "\"image_width\":" << block.image_width << ","
        << "\"channel_count\":3,"
        << "\"sample_format\":\"uint16_le\""
        << "}";
    return oss.str();
}

std::string BuildEndMetadata(const FrameStreamEventEnd& end) {
    std::ostringstream oss;
    oss << "{"
        << "\"success\":" << (end.success ? "true" : "false") << ","
        << "\"sdk_error\":" << end.sdk_error << ","
        << "\"message\":\"" << EscapeJson(end.message) << "\","
        << "\"light_frames\":" << end.light_frames << ","
        << "\"dark_frames\":" << end.dark_frames << ","
        << "\"final_png_path\":\"" << EscapeJson(end.final_png_path) << "\""
        << "}";
    return oss.str();
}

bool BuildPayload(const FrameStreamEvent& event,
                  std::vector<std::uint8_t>* payload,
                  std::string* error) {
    payload->clear();
    if (event.type != FrameStreamEventType::LightRgbBlock) {
        return true;
    }

    const auto& block = event.light_rgb_block;
    const std::size_t expected_pixels =
        static_cast<std::size_t>(std::max<std::int64_t>(0, block.line_count)) *
        static_cast<std::size_t>(std::max<std::int64_t>(0, block.image_width)) * 3U;
    if (block.rgb_pixels.size() != expected_pixels) {
        if (error != nullptr) {
            *error = "LightRgbBlock rgb_pixels size mismatch";
        }
        return false;
    }

    payload->reserve(block.rgb_pixels.size() * sizeof(std::uint16_t));
    for (std::uint16_t sample : block.rgb_pixels) {
        AppendU16(payload, sample);
    }
    return true;
}

std::string BuildMetadata(const FrameStreamEvent& event) {
    switch (event.type) {
        case FrameStreamEventType::JobBegin:
            return BuildBeginMetadata(event.begin);
        case FrameStreamEventType::LightRgbBlock:
            return BuildLightBlockMetadata(event.light_rgb_block);
        case FrameStreamEventType::JobEnd:
            return BuildEndMetadata(event.end);
    }
    return "{}";
}

}  // namespace

bool SerializeEvent(const FrameStreamEvent& event,
                    std::uint64_t sequence,
                    std::vector<std::uint8_t>* out,
                    std::string* error) {
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "SerializeEvent requires a non-null output buffer";
        }
        return false;
    }

    std::vector<std::uint8_t> payload;
    if (!BuildPayload(event, &payload, error)) {
        return false;
    }

    const std::string metadata = BuildMetadata(event);
    if (!FitsU32(metadata.size()) || !FitsU32(payload.size())) {
        if (error != nullptr) {
            *error = "Frame stream message exceeds 32-bit length limits";
        }
        return false;
    }

    out->clear();
    out->reserve(kHeaderBytes + metadata.size() + payload.size());
    out->insert(out->end(), kMagic, kMagic + 4);
    AppendU16(out, kVersion);
    AppendU16(out, static_cast<std::uint16_t>(kHeaderBytes));
    AppendU32(out, MessageTypeForEvent(event.type));
    AppendU32(out, 0U);
    AppendU64(out, event.job_id);
    AppendU64(out, sequence);
    AppendU32(out, static_cast<std::uint32_t>(metadata.size()));
    AppendU32(out, static_cast<std::uint32_t>(payload.size()));
    out->insert(out->end(), metadata.begin(), metadata.end());
    out->insert(out->end(), payload.begin(), payload.end());
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
    header->header_bytes = ReadU16(bytes.data() + 6);
    header->message_type = ReadU32(bytes.data() + 8);
    header->flags = ReadU32(bytes.data() + 12);
    header->job_id = ReadU64(bytes.data() + 16);
    header->sequence = ReadU64(bytes.data() + 24);
    header->metadata_length = ReadU32(bytes.data() + 32);
    header->payload_length = ReadU32(bytes.data() + 36);
    return true;
}

}  // namespace FrameStreamProtocol
