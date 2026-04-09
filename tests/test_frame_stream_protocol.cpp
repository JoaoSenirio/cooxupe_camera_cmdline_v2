#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "frame_stream_protocol.h"
#include "test_support.h"

namespace {

std::uint16_t ReadU16Le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           (static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t ReadU32Le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

double ReadF64Le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    double value = 0.0;
    std::memcpy(&value, bytes.data() + static_cast<long>(offset), sizeof(double));
    return value;
}

WorkItem MakeBeginItem() {
    WorkItem item;
    item.type = WorkItemType::BeginJob;
    item.job_id = 77;
    item.begin.expected_light_frames = 21000;
    item.begin.expected_dark_frames = 50;
    item.begin.rgb_wavelength_nm[0] = 610;
    item.begin.rgb_wavelength_nm[1] = 534;
    item.begin.rgb_wavelength_nm[2] = 470;
    item.begin.sensor.image_width = 4;
    item.begin.sensor.image_height = 3;
    item.begin.sensor.byte_depth = 2;
    item.begin.sensor.frame_size_bytes = 24;
    item.begin.sensor.wavelengths_nm = {610.0, 534.0, 470.0};
    return item;
}

int TestSerializeBeginPayload() {
    WorkItem item = MakeBeginItem();

    FrameStreamProtocol::SerializedMessage message;
    std::string error;
    TEST_ASSERT(FrameStreamProtocol::SerializeWorkItem(item, &message, &error),
                "Begin serialization should succeed");
    TEST_ASSERT(message.message_type == FrameStreamProtocol::MessageType::Begin,
                "message type must be Begin");
    TEST_ASSERT(message.external_payload == nullptr,
                "Begin should not use an external payload");

    FrameStreamProtocol::Header header;
    TEST_ASSERT(FrameStreamProtocol::ParseHeader(message.header, &header, &error),
                "serialized header should parse");
    TEST_ASSERT(header.magic[0] == 'S' && header.magic[1] == 'S' &&
                    header.magic[2] == 'F' && header.magic[3] == 'R',
                "header magic must be SSFR");
    TEST_ASSERT(header.version == FrameStreamProtocol::kVersion,
                "header version must match protocol");
    TEST_ASSERT(header.message_type ==
                    static_cast<std::uint16_t>(FrameStreamProtocol::MessageType::Begin),
                "header must encode Begin");
    TEST_ASSERT(header.payload_length == message.inline_payload.size(),
                "header payload length must match inline payload");

    TEST_ASSERT(ReadU32Le(message.inline_payload, 0) == 4, "width must be serialized");
    TEST_ASSERT(ReadU32Le(message.inline_payload, 4) == 3, "band_count must be serialized");
    TEST_ASSERT(ReadU32Le(message.inline_payload, 8) == 2, "byte_depth must be serialized");
    TEST_ASSERT(ReadU32Le(message.inline_payload, 12) == 24, "frame size must be serialized");
    TEST_ASSERT(ReadU32Le(message.inline_payload, 16) == 21000,
                "expected light frames must be serialized");
    TEST_ASSERT(ReadU32Le(message.inline_payload, 20) == 50,
                "expected dark frames must be serialized");
    TEST_ASSERT(ReadU32Le(message.inline_payload, 24) == 610 &&
                    ReadU32Le(message.inline_payload, 28) == 534 &&
                    ReadU32Le(message.inline_payload, 32) == 470,
                "RGB target wavelengths must be serialized");
    TEST_ASSERT(std::fabs(ReadF64Le(message.inline_payload, 36) - 610.0) < 1e-9,
                "first wavelength must be serialized");
    TEST_ASSERT(std::fabs(ReadF64Le(message.inline_payload, 44) - 534.0) < 1e-9,
                "second wavelength must be serialized");
    TEST_ASSERT(std::fabs(ReadF64Le(message.inline_payload, 52) - 470.0) < 1e-9,
                "third wavelength must be serialized");
    return 0;
}

int TestSerializeLightChunkUsesExternalPayload() {
    WorkItem item;
    item.type = WorkItemType::LightChunk;
    item.job_id = 12;
    item.chunk.frame_count = 2;
    auto bytes = std::make_shared<std::vector<std::uint8_t>>();
    bytes->push_back(10);
    bytes->push_back(20);
    bytes->push_back(30);
    bytes->push_back(40);
    item.chunk.bytes = bytes;

    FrameStreamProtocol::SerializedMessage message;
    std::string error;
    TEST_ASSERT(FrameStreamProtocol::SerializeWorkItem(item, &message, &error),
                "Light chunk serialization should succeed");
    TEST_ASSERT(message.message_type == FrameStreamProtocol::MessageType::LightBlock,
                "message type must be LightBlock");
    TEST_ASSERT(message.inline_payload.empty(),
                "chunk messages should not duplicate payload inline");
    TEST_ASSERT(message.external_payload == bytes.get(),
                "chunk payload must be reused by pointer");

    FrameStreamProtocol::Header header;
    TEST_ASSERT(FrameStreamProtocol::ParseHeader(message.header, &header, &error),
                "chunk header should parse");
    TEST_ASSERT(header.message_type ==
                    static_cast<std::uint16_t>(FrameStreamProtocol::MessageType::LightBlock),
                "header must encode LightBlock");
    TEST_ASSERT(header.payload_length == bytes->size(),
                "header payload length must match external payload size");
    return 0;
}

int TestAckRoundTrip() {
    std::vector<std::uint8_t> bytes;
    FrameStreamProtocol::BuildAck(true, &bytes);
    TEST_ASSERT(bytes.size() == FrameStreamProtocol::kAckBytes,
                "ack must have fixed size");

    FrameStreamProtocol::Ack ack;
    std::string error;
    TEST_ASSERT(FrameStreamProtocol::ParseAck(bytes, &ack, &error),
                "ack should parse");
    TEST_ASSERT(ack.magic[0] == 'S' && ack.magic[1] == 'S' &&
                    ack.magic[2] == 'F' && ack.magic[3] == 'A',
                "ack magic must be SSFA");
    TEST_ASSERT(ack.status == 0, "success ack must have status zero");

    FrameStreamProtocol::BuildAck(false, &bytes);
    TEST_ASSERT(FrameStreamProtocol::ParseAck(bytes, &ack, &error),
                "failure ack should also parse");
    TEST_ASSERT(ack.status == 1, "failure ack must have status one");
    return 0;
}

}  // namespace

int main() {
    return RunSuite("test_frame_stream_protocol", {
        {"SerializeBeginPayload", TestSerializeBeginPayload},
        {"SerializeLightChunkUsesExternalPayload", TestSerializeLightChunkUsesExternalPayload},
        {"AckRoundTrip", TestAckRoundTrip},
    });
}
