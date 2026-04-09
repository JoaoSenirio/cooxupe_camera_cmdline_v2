#ifndef FRAME_STREAM_PROTOCOL_H
#define FRAME_STREAM_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "types.h"

namespace FrameStreamProtocol {

constexpr std::size_t kHeaderBytes = 12;
constexpr std::size_t kAckBytes = 8;
constexpr std::uint16_t kVersion = 1;
constexpr char kMagic[4] = {'S', 'S', 'F', 'R'};
constexpr char kAckMagic[4] = {'S', 'S', 'F', 'A'};

enum class MessageType : std::uint16_t {
    Begin = 1,
    LightBlock = 2,
    DarkBlock = 3,
    End = 4
};

struct Header {
    char magic[4] = {'\0', '\0', '\0', '\0'};
    std::uint16_t version = 0;
    std::uint16_t message_type = 0;
    std::uint32_t payload_length = 0;
};

struct Ack {
    char magic[4] = {'\0', '\0', '\0', '\0'};
    std::uint16_t version = 0;
    std::uint16_t status = 0;
};

struct SerializedMessage {
    MessageType message_type = MessageType::Begin;
    std::vector<std::uint8_t> header;
    std::vector<std::uint8_t> inline_payload;
    const std::vector<std::uint8_t>* external_payload = nullptr;
};

bool SerializeWorkItem(const WorkItem& item,
                       SerializedMessage* out,
                       std::string* error);

bool ParseHeader(const std::vector<std::uint8_t>& bytes, Header* header, std::string* error);
void BuildAck(bool success, std::vector<std::uint8_t>* out);
bool ParseAck(const std::vector<std::uint8_t>& bytes, Ack* ack, std::string* error);

}  // namespace FrameStreamProtocol

#endif
