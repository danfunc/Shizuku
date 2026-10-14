#include "protocol.hpp"

#include <cstring>

namespace xno::ota::protocol {
namespace {
constexpr uint8_t kChunkMagic[4] = {'X', 'N', 'C', 'K'};
uint16_t le16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
uint32_t le32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
bool eq4(const uint8_t *p, const char *s) {
  return p[0] == static_cast<uint8_t>(s[0]) &&
         p[1] == static_cast<uint8_t>(s[1]) &&
         p[2] == static_cast<uint8_t>(s[2]) &&
         p[3] == static_cast<uint8_t>(s[3]);
}
} // namespace

uint16_t crc16_ccitt_false(const uint8_t *data, uint32_t length) {
  uint16_t crc = 0xFFFF;
  for (uint32_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint32_t bit = 0; bit < 8; ++bit)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

uint32_t crc32_ieee(const uint8_t *data, uint32_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint32_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
  }
  return crc ^ 0xFFFFFFFFu;
}

Protocol::Protocol(Workspace workspace, Callbacks callbacks,
                   uint32_t max_image_bytes, uint32_t sector_bytes)
    : workspace_(workspace), callbacks_(callbacks),
      max_image_bytes_(max_image_bytes), sector_bytes_(sector_bytes) {
  if (workspace_.received_bitmap != nullptr)
    for (uint32_t i = 0; i < workspace_.bitmap_words; ++i)
      workspace_.received_bitmap[i] = 0;
}

bool Protocol::valid() const {
  return sector_bytes_ != 0 && workspace_.encoded != nullptr &&
         workspace_.encoded_capacity >= kMaxEncodedBytes &&
         workspace_.decoded != nullptr &&
         workspace_.decoded_capacity >= sector_bytes_ &&
         workspace_.received_bitmap != nullptr && workspace_.bitmap_words != 0 &&
         workspace_.inflate_state != nullptr &&
         callbacks_.begin != nullptr && callbacks_.write != nullptr &&
         callbacks_.read != nullptr && callbacks_.validate != nullptr &&
         callbacks_.activate != nullptr && callbacks_.abort != nullptr &&
         callbacks_.event != nullptr && max_image_bytes_ != 0;
}

void Protocol::emit(EventType type, uint32_t missing_count, uint32_t sequence,
                    uint32_t offset) {
  if (callbacks_.event == nullptr)
    return;
  callbacks_.event(callbacks_.context,
                   Event{type, total_bytes_, expected_crc32_, missing_count,
                         sequence, offset});
}

bool Protocol::bitmap_get(uint32_t sequence) const {
  return (workspace_.received_bitmap[sequence >> 5] &
          (1u << (sequence & 31))) != 0;
}

void Protocol::bitmap_set(uint32_t sequence) {
  workspace_.received_bitmap[sequence >> 5] |= 1u << (sequence & 31);
}

bool Protocol::missing(uint32_t sequence) const {
  return sequence < chunk_count_ && !bitmap_get(sequence);
}

uint32_t Protocol::missing_count() const {
  uint32_t count = 0;
  for (uint32_t i = 0; i < chunk_count_; ++i)
    count += missing(i) ? 1u : 0u;
  return count;
}

void Protocol::fail() {
  if (state_ == State::Failed)
    return;
  state_ = State::Failed;
  parse_ = Parse::Failed;
  if (callbacks_.abort != nullptr)
    callbacks_.abort(callbacks_.context);
  emit(EventType::Failed);
}

void Protocol::abort() {
  if (callbacks_.abort != nullptr)
    callbacks_.abort(callbacks_.context);
  state_ = State::Idle;
  parse_ = Parse::InitHeader;
  header_used_ = seek_used_ = 0;
  discard_remainder_ = false;
  total_bytes_ = expected_crc32_ = received_bytes_ = chunk_count_ = 0;
  crc32_state_ = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < workspace_.bitmap_words; ++i)
    workspace_.received_bitmap[i] = 0;
  emit(EventType::Reset);
}

void Protocol::finish() {
  if ((state_ != State::Idle && state_ != State::Complete &&
       state_ != State::Failed) || header_used_ != 0 || parse_ != Parse::InitHeader)
    fail();
}

bool Protocol::verify_readback(uint32_t total, uint32_t expected) {
  uint32_t crc = 0xFFFFFFFFu;
  uint32_t offset = 0;
  while (offset < total) {
    const uint32_t size = total - offset < workspace_.decoded_capacity
                              ? total - offset
                              : workspace_.decoded_capacity;
    if (!callbacks_.read(callbacks_.context, offset, workspace_.decoded, size))
      return false;
    for (uint32_t i = 0; i < size; ++i) {
      crc ^= workspace_.decoded[i];
      for (uint32_t bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
    }
    offset += size;
  }
  return (crc ^ 0xFFFFFFFFu) == expected;
}

bool Protocol::write_raw(const uint8_t *bytes, uint32_t length) {
  if (length > total_bytes_ - received_bytes_)
    return false;
  uint32_t at = 0;
  while (at < length) {
    const uint32_t n = length - at < sector_bytes_ ? length - at : sector_bytes_;
    if (!callbacks_.write(callbacks_.context, received_bytes_, bytes + at, n))
      return false;
    for (uint32_t i = 0; i < n; ++i) {
      crc32_state_ ^= bytes[at + i];
      for (uint32_t bit = 0; bit < 8; ++bit)
        crc32_state_ = (crc32_state_ >> 1) ^
                       ((crc32_state_ & 1) ? 0xEDB88320u : 0u);
    }
    received_bytes_ += n;
    at += n;
  }
  if (received_bytes_ == total_bytes_) {
    if ((crc32_state_ ^ 0xFFFFFFFFu) != expected_crc32_) {
      fail();
      return false;
    }
    state_ = State::Complete;
    parse_ = Parse::Complete;
    emit(EventType::Done);
  }
  return true;
}

void Protocol::parse_init() {
  total_bytes_ = le32(header_ + 4);
  expected_crc32_ = le32(header_ + 8);
  if (eq4(header_, "XNOC")) {
    if (total_bytes_ == 0 || total_bytes_ > max_image_bytes_ ||
        !verify_readback(total_bytes_, expected_crc32_) ||
        !callbacks_.validate(callbacks_.context, total_bytes_)) {
      fail();
      return;
    }
    // Let the adapter publish the sender's commit acknowledgement before
    // activation can restart or otherwise stop execution.
    emit(EventType::CommitReady);
    if (!callbacks_.activate(callbacks_.context)) {
      fail();
      return;
    }
    state_ = State::Complete;
    parse_ = Parse::Complete;
    return;
  }
  Mode mode;
  if (eq4(header_, "XNOU"))
    mode = Mode::Xnou;
  else if (eq4(header_, "XNOR"))
    mode = Mode::Xnor;
  else if (eq4(header_, "XNOZ"))
    mode = Mode::Xnoz;
  else {
    fail();
    return;
  }
  if (total_bytes_ == 0 || total_bytes_ > max_image_bytes_) {
    fail();
    return;
  }
  const uint32_t required_chunks = mode == Mode::Xnor
      ? (total_bytes_ - 1) / sector_bytes_ + 1 : 0;
  if (mode == Mode::Xnor &&
      (required_chunks + 31) / 32 > workspace_.bitmap_words) {
    fail();
    return;
  }
  if (!callbacks_.begin(callbacks_.context, total_bytes_, expected_crc32_, mode)) {
    fail();
    return;
  }
  received_bytes_ = 0;
  crc32_state_ = 0xFFFFFFFFu;
  if (mode == Mode::Xnou) {
    state_ = State::RawData;
    parse_ = Parse::Raw;
  } else if (mode == Mode::Xnor) {
    chunk_count_ = required_chunks;
    const uint32_t words = (chunk_count_ + 31) / 32;
    for (uint32_t i = 0; i < words; ++i)
      workspace_.received_bitmap[i] = 0;
    state_ = State::ChunkSeek;
    parse_ = Parse::ChunkSeek;
  } else {
    state_ = State::RawData;
    parse_ = Parse::ZLength;
    header_used_ = 0;
  }
  emit(EventType::Ready);
}

void Protocol::parse_chunk_header() {
  chunk_sequence_ = le16(header_ + 4);
  chunk_payload_length_ = le16(header_ + 6);
  chunk_payload_crc_ = le32(header_ + 8);
  const bool valid_header = crc16_ccitt_false(header_, 14) == le16(header_ + 14);
  header_used_ = 0;
  seek_used_ = 0;
  if (!valid_header) {
    state_ = State::ChunkSeek;
    parse_ = Parse::ChunkSeek;
    return;
  }
  if (chunk_sequence_ == kResetSeq && chunk_payload_length_ == 0) {
    abort();
    discard_remainder_ = true;
    return;
  }
  if (chunk_sequence_ == kQuerySeq && chunk_payload_length_ == 0) {
    if (chunk_count_ == 0) {
      fail();
      return;
    }
    const uint32_t missing_total = missing_count();
    emit(EventType::MissingCount, missing_total);
    for (uint32_t i = 0; i < chunk_count_; ++i)
      if (missing(i))
        emit(EventType::MissingSequence, missing_total, i);
    emit(EventType::MissingEnd, missing_total);
    if (missing_total == 0) {
      if (!verify_readback(total_bytes_, expected_crc32_)) {
        fail();
        return;
      }
      state_ = State::Complete;
      parse_ = Parse::Complete;
      emit(EventType::Done);
      return;
    }
    state_ = State::ChunkSeek;
    parse_ = Parse::ChunkSeek;
    return;
  }
  if (chunk_sequence_ >= chunk_count_ || chunk_payload_length_ == 0 ||
      chunk_payload_length_ > workspace_.encoded_capacity) {
    state_ = State::ChunkSeek;
    parse_ = Parse::ChunkSeek;
    return;
  }
  chunk_payload_used_ = 0;
  state_ = State::ChunkData;
  parse_ = Parse::ChunkPayload;
}

void Protocol::accept_chunk() {
  if (crc32_ieee(workspace_.encoded, chunk_payload_length_) !=
      chunk_payload_crc_)
    return; // bad chunk remains missing and can be resent
  const uint32_t offset = chunk_sequence_ * sector_bytes_;
  const uint32_t expected_raw = total_bytes_ - offset < sector_bytes_
                                    ? total_bytes_ - offset
                                    : sector_bytes_;
  const int32_t decoded = tiny_inflate::run(
      *workspace_.inflate_state, workspace_.encoded, chunk_payload_length_,
      workspace_.decoded, workspace_.decoded_capacity);
  if (decoded <= 0 || static_cast<uint32_t>(decoded) != expected_raw)
    return;
  if (!callbacks_.write(callbacks_.context, offset, workspace_.decoded,
                        static_cast<uint32_t>(decoded))) {
    fail();
    return;
  }
  bitmap_set(chunk_sequence_);
}

bool Protocol::feed(const uint8_t *bytes, uint32_t length) {
  if (!valid() || (bytes == nullptr && length != 0) ||
      state_ == State::Failed)
    return false;
  if (state_ == State::Complete && length != 0) {
    state_ = State::Idle;
    parse_ = Parse::InitHeader;
    header_used_ = 0;
  }
  uint32_t at = 0;
  while (at < length) {
    if (parse_ == Parse::InitHeader) {
      if (state_ == State::Idle)
        state_ = State::Header;
      const uint32_t n = 12 - header_used_ < length - at
                             ? 12 - header_used_
                             : length - at;
      std::memcpy(header_ + header_used_, bytes + at, n);
      header_used_ += n;
      at += n;
      if (header_used_ == 12) {
        if (eq4(header_, "XNCK")) {
          std::memcpy(header_ + 12, bytes + at, length - at < 4 ? length - at : 4);
          const uint32_t copied = length - at < 4 ? length - at : 4;
          header_used_ += copied;
          at += copied;
          if (header_used_ == 16)
            parse_chunk_header();
          else
            return true;
          if (discard_remainder_) {
            discard_remainder_ = false;
            return true;
          }
        } else {
          header_used_ = 0;
          parse_init();
          if (parse_ == Parse::Raw && at < length) {
            const uint32_t nraw = length - at < total_bytes_ - received_bytes_
                                      ? length - at
                                      : total_bytes_ - received_bytes_;
            if (!write_raw(bytes + at, nraw))
              return false;
            at += nraw;
          }
        }
      }
      continue;
    }
    if (parse_ == Parse::Raw) {
      const uint32_t n = length - at < total_bytes_ - received_bytes_
                             ? length - at
                             : total_bytes_ - received_bytes_;
      if (!write_raw(bytes + at, n))
        return false;
      at += n;
      continue;
    }
    if (parse_ == Parse::ZLength) {
      header_[header_used_++] = bytes[at++];
      if (header_used_ == 2) {
        chunk_payload_length_ = le16(header_);
        header_used_ = 0;
        chunk_payload_used_ = 0;
        if (chunk_payload_length_ == 0 ||
            chunk_payload_length_ > workspace_.encoded_capacity) {
          fail();
          return false;
        }
        parse_ = Parse::ZPayload;
      }
      continue;
    }
    if (parse_ == Parse::ZPayload) {
      const uint32_t n = chunk_payload_length_ - chunk_payload_used_ < length - at
                             ? chunk_payload_length_ - chunk_payload_used_
                             : length - at;
      std::memcpy(workspace_.encoded + chunk_payload_used_, bytes + at, n);
      chunk_payload_used_ += n;
      at += n;
      if (chunk_payload_used_ == chunk_payload_length_) {
        const int32_t decoded = tiny_inflate::run(
            *workspace_.inflate_state, workspace_.encoded, chunk_payload_length_,
            workspace_.decoded, workspace_.decoded_capacity);
        if (decoded <= 0 || !write_raw(workspace_.decoded,
                                       static_cast<uint32_t>(decoded))) {
          fail();
          return false;
        }
        if (state_ != State::Complete) {
          parse_ = Parse::ZLength;
          header_used_ = 0;
        }
      }
      continue;
    }
    if (parse_ == Parse::ChunkSeek) {
      seek_[0] = seek_[1]; seek_[1] = seek_[2]; seek_[2] = seek_[3];
      seek_[3] = bytes[at++];
      if (seek_used_ < 4) ++seek_used_;
      if (seek_used_ == 4 && std::memcmp(seek_, kChunkMagic, 4) == 0) {
        std::memcpy(header_, kChunkMagic, 4);
        header_used_ = 4;
        state_ = State::ChunkHeader;
        parse_ = Parse::ChunkHeader;
      }
      continue;
    }
    if (parse_ == Parse::ChunkHeader) {
      const uint32_t n = 16 - header_used_ < length - at
                             ? 16 - header_used_
                             : length - at;
      std::memcpy(header_ + header_used_, bytes + at, n);
      header_used_ += n;
      at += n;
      if (header_used_ == 16)
        parse_chunk_header();
      if (discard_remainder_) {
        discard_remainder_ = false;
        return true;
      }
      continue;
    }
    if (parse_ == Parse::ChunkPayload) {
      const uint32_t n = chunk_payload_length_ - chunk_payload_used_ < length - at
                             ? chunk_payload_length_ - chunk_payload_used_
                             : length - at;
      std::memcpy(workspace_.encoded + chunk_payload_used_, bytes + at, n);
      chunk_payload_used_ += n;
      at += n;
      if (chunk_payload_used_ == chunk_payload_length_) {
        accept_chunk();
        if (parse_ != Parse::Failed && parse_ != Parse::Complete) {
          state_ = State::ChunkSeek;
          parse_ = Parse::ChunkSeek;
          seek_used_ = 0;
        }
      }
      continue;
    }
    break;
  }
  return state_ != State::Failed;
}

} // namespace xno::ota::protocol
