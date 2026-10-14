#ifndef XNO_OBJECTS_OTA_PROTOCOL_HPP
#define XNO_OBJECTS_OTA_PROTOCOL_HPP

#include "inflate.hpp"
#include <cstdint>

namespace xno::ota::protocol {

constexpr uint32_t kWireSectorBytes = 4096;
constexpr uint32_t kMaxEncodedBytes = kWireSectorBytes + 256;
constexpr uint16_t kQuerySeq = 0xFFFF;
constexpr uint16_t kResetSeq = 0xFFFE;

enum class Mode : uint8_t { Xnou, Xnor, Xnoz };
enum class State : uint8_t { Idle, Header, RawData, ChunkSeek, ChunkHeader,
                             ChunkData, Complete, Failed };
enum class EventType : uint8_t { Ready, MissingCount, MissingSequence,
                                 MissingEnd, Done, Failed, Reset,
                                 CommitReady };

struct Event {
  EventType type; // Map to sender lines: ready, NEED, NEEDSEQ, NEEDEND, done, commit.
  uint32_t total_bytes;
  uint32_t image_crc32;
  uint32_t missing_count;
  uint32_t sequence;
  uint32_t offset;
};

// Every buffer is borrowed for the Protocol lifetime. No allocation is performed.
struct Workspace {
  uint8_t *encoded;
  uint32_t encoded_capacity;
  uint8_t *decoded;
  uint32_t decoded_capacity;
  uint32_t *received_bitmap;
  uint32_t bitmap_words;
  tiny_inflate::state *inflate_state;
};

// Callbacks are synchronous and must not retain data pointers. Feed from a worker,
// not from a latency-sensitive BLE callback, because write/read may block.
struct Callbacks {
  void *context;
  bool (*begin)(void *, uint32_t total_bytes, uint32_t image_crc32, Mode);
  bool (*write)(void *, uint32_t offset, const uint8_t *, uint32_t length);
  bool (*read)(void *, uint32_t offset, uint8_t *, uint32_t length);
  bool (*validate)(void *, uint32_t total_bytes);
  bool (*activate)(void *);
  void (*abort)(void *);
  void (*event)(void *, const Event &);
};

class Protocol {
public:
  Protocol(Workspace workspace, Callbacks callbacks, uint32_t max_image_bytes,
           uint32_t sector_bytes = kWireSectorBytes);

  bool valid() const;
  State state() const { return state_; }
  uint32_t total_bytes() const { return total_bytes_; }
  uint32_t expected_crc32() const { return expected_crc32_; }
  uint32_t chunk_count() const { return chunk_count_; }
  uint32_t missing_count() const;
  bool missing(uint32_t sequence) const;

  // Input may be split at any byte boundary. Returns false once failed.
  bool feed(const uint8_t *bytes, uint32_t length);
  void finish();
  void abort();

private:
  enum class Parse : uint8_t { InitHeader, ChunkSeek, ChunkHeader,
                               ChunkPayload, Raw, ZLength, ZPayload,
                               Complete, Failed };
  void fail();
  void emit(EventType type, uint32_t missing_count = 0,
            uint32_t sequence = 0, uint32_t offset = 0);
  void parse_init();
  void parse_chunk_header();
  void accept_chunk();
  bool verify_readback(uint32_t total, uint32_t expected);
  bool write_raw(const uint8_t *bytes, uint32_t length);
  bool bitmap_get(uint32_t sequence) const;
  void bitmap_set(uint32_t sequence);

  Workspace workspace_;
  Callbacks callbacks_;
  uint32_t max_image_bytes_;
  uint32_t sector_bytes_;
  State state_ = State::Idle;
  Parse parse_ = Parse::InitHeader;
  uint8_t header_[16]{};
  uint32_t header_used_ = 0;
  uint8_t seek_[4]{};
  uint32_t seek_used_ = 0;
  uint32_t total_bytes_ = 0;
  uint32_t expected_crc32_ = 0;
  uint32_t received_bytes_ = 0;
  uint32_t crc32_state_ = 0xFFFFFFFFu;
  uint32_t chunk_count_ = 0;
  uint32_t chunk_sequence_ = 0;
  uint32_t chunk_payload_length_ = 0;
  uint32_t chunk_payload_crc_ = 0;
  uint32_t chunk_payload_used_ = 0;
  bool discard_remainder_ = false;
};

uint16_t crc16_ccitt_false(const uint8_t *data, uint32_t length);
uint32_t crc32_ieee(const uint8_t *data, uint32_t length);

} // namespace xno::ota::protocol

#endif
