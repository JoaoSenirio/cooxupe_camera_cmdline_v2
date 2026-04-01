#ifndef FRAME_STREAM_PROTOCOL_H
#define FRAME_STREAM_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "types.h"

namespace FrameStreamProtocol {

constexpr std::size_t kHeaderBytes = 40;
constexpr std::uint16_t kVersion = 1;
constexpr char kMagic[4] = {'S', 'S', 'F', 'R'};

enum class MessageType : std::uint32_t {
    JobBegin = 1,
    LightRgbBlock = 2,
    JobEnd = 3
};

struct Header {
    char magic[4] = {'\0', '\0', '\0', '\0'};
    std::uint16_t version = 0;
    std::uint16_t header_bytes = 0;
    std::uint32_t message_type = 0;
    std::uint32_t flags = 0;
    std::uint64_t job_id = 0;
    std::uint64_t sequence = 0;
    std::uint32_t metadata_length = 0;
    std::uint32_t payload_length = 0;
};

bool SerializeEvent(const FrameStreamEvent& event,
                    std::uint64_t sequence,
                    std::vector<std::uint8_t>* out,
                    std::string* error);

bool ParseHeader(const std::vector<std::uint8_t>& bytes, Header* header, std::string* error);

}  // namespace FrameStreamProtocol

#endif
