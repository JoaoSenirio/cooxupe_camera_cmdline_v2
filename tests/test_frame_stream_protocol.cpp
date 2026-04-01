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

int TestSerializeLightRgbBlock() {
    FrameStreamEvent event;
    event.type = FrameStreamEventType::LightRgbBlock;
    event.job_id = 77;
    event.light_rgb_block.line_count = 2;
    event.light_rgb_block.line_index_start = 5;
    event.light_rgb_block.first_frame_number = 1234;
    event.light_rgb_block.last_frame_number = 1235;
    event.light_rgb_block.image_width = 2;
    event.light_rgb_block.rgb_pixels = {
        10, 20, 30,
        11, 21, 31,
        12, 22, 32,
        13, 23, 33,
    };

    std::vector<std::uint8_t> bytes;
    std::string error;
    TEST_ASSERT(FrameStreamProtocol::SerializeEvent(event, 9, &bytes, &error),
                "LightRgbBlock serialization should succeed");

    FrameStreamProtocol::Header header;
    TEST_ASSERT(FrameStreamProtocol::ParseHeader(bytes, &header, &error),
                "serialized bytes should contain a valid header");
    TEST_ASSERT(header.magic[0] == 'S' && header.magic[1] == 'S' &&
                    header.magic[2] == 'F' && header.magic[3] == 'R',
                "header magic must be SSFR");
    TEST_ASSERT(header.version == FrameStreamProtocol::kVersion, "header version must match protocol");
    TEST_ASSERT(header.header_bytes == FrameStreamProtocol::kHeaderBytes, "header size must be fixed");
    TEST_ASSERT(header.message_type ==
                    static_cast<std::uint32_t>(FrameStreamProtocol::MessageType::LightRgbBlock),
                "message type must match LightRgbBlock");
    TEST_ASSERT(header.job_id == 77, "job_id must be preserved");
    TEST_ASSERT(header.sequence == 9, "sequence must be preserved");
    TEST_ASSERT(header.payload_length == event.light_rgb_block.rgb_pixels.size() * sizeof(std::uint16_t),
                "payload length must encode every RGB uint16 sample");

    const std::string metadata(bytes.begin() + static_cast<long>(FrameStreamProtocol::kHeaderBytes),
                               bytes.begin() + static_cast<long>(FrameStreamProtocol::kHeaderBytes +
                                                                header.metadata_length));
    TEST_ASSERT(StringContains(metadata, "\"line_count\":2"),
                "metadata must include line_count");
    TEST_ASSERT(StringContains(metadata, "\"line_index_start\":5"),
                "metadata must include line_index_start");
    TEST_ASSERT(StringContains(metadata, "\"sample_format\":\"uint16_le\""),
                "metadata must declare the sample format");

    const std::size_t payload_offset =
        FrameStreamProtocol::kHeaderBytes + static_cast<std::size_t>(header.metadata_length);
    TEST_ASSERT(ReadU16Le(bytes, payload_offset) == 10, "payload must preserve the first R sample");
    TEST_ASSERT(ReadU16Le(bytes, payload_offset + 2) == 20, "payload must preserve the first G sample");
    TEST_ASSERT(ReadU16Le(bytes, payload_offset + 4) == 30, "payload must preserve the first B sample");
    TEST_ASSERT(ReadU16Le(bytes, payload_offset + 6) == 11, "payload must stay interleaved by pixel");
    return 0;
}

int TestSerializeRejectsPayloadSizeMismatch() {
    FrameStreamEvent event;
    event.type = FrameStreamEventType::LightRgbBlock;
    event.job_id = 1;
    event.light_rgb_block.line_count = 1;
    event.light_rgb_block.image_width = 2;
    event.light_rgb_block.rgb_pixels = {1, 2, 3};

    std::vector<std::uint8_t> bytes;
    std::string error;
    TEST_ASSERT(!FrameStreamProtocol::SerializeEvent(event, 1, &bytes, &error),
                "serialization must fail when payload size does not match geometry");
    TEST_ASSERT(StringContains(error, "size mismatch"),
                "serialization error must explain the mismatch");
    return 0;
}

}  // namespace

int main() {
    return RunSuite("test_frame_stream_protocol", {
        {"SerializeLightRgbBlock", TestSerializeLightRgbBlock},
        {"SerializeRejectsPayloadSizeMismatch", TestSerializeRejectsPayloadSizeMismatch},
    });
}
