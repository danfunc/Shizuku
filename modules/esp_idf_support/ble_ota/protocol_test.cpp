#include "protocol.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

using namespace xno::ota::protocol;

struct Backend {
  std::vector<uint8_t> image;
  uint32_t begun = 0;
  uint32_t writes = 0;
  uint32_t aborts = 0;
  uint32_t validates = 0;
  uint32_t activations = 0;
  uint32_t missing_total = 0;
  uint32_t max_missing_total = 0;
  uint32_t missing_seq[8]{};
  uint32_t done = 0;
  uint32_t failed = 0;
  uint32_t commit_ready = 0;
  bool fail_write = false;
  bool fail_validate = false;
  bool fail_activate = false;
};

static bool begin(void *p, uint32_t size, uint32_t, Mode) {
  auto &b = *static_cast<Backend *>(p);
  if (b.fail_write)
    return false;
  b.image.assign(size, 0);
  ++b.begun;
  return true;
}
static bool write(void *p, uint32_t offset, const uint8_t *data, uint32_t size) {
  auto &b = *static_cast<Backend *>(p);
  if (offset > b.image.size() || size > b.image.size() - offset)
    return false;
  for (uint32_t i = 0; i < size; ++i)
    b.image[offset + i] = data[i];
  ++b.writes;
  return true;
}
static bool read(void *p, uint32_t offset, uint8_t *out, uint32_t size) {
  auto &b = *static_cast<Backend *>(p);
  if (offset > b.image.size() || size > b.image.size() - offset)
    return false;
  for (uint32_t i = 0; i < size; ++i)
    out[i] = b.image[offset + i];
  return true;
}
static bool validate(void *p, uint32_t size) {
  auto &b = *static_cast<Backend *>(p);
  ++b.validates;
  return !b.fail_validate && size == b.image.size();
}
static bool activate(void *p) {
  auto &b = *static_cast<Backend *>(p);
  ++b.activations;
  return !b.fail_activate;
}
static void abort_backend(void *p) { ++static_cast<Backend *>(p)->aborts; }
static void event(void *p, const Event &e) {
  auto &b = *static_cast<Backend *>(p);
  if (e.type == EventType::MissingSequence && e.sequence < 8 && e.missing_count != 0)
    b.missing_seq[e.sequence]++;
  if (e.type == EventType::MissingEnd) {
    b.missing_total = e.missing_count;
    b.max_missing_total = e.missing_count > b.max_missing_total
                              ? e.missing_count : b.max_missing_total;
  }
  if (e.type == EventType::Done)
    ++b.done;
  if (e.type == EventType::Failed)
    ++b.failed;
  if (e.type == EventType::CommitReady)
    ++b.commit_ready;
}

static std::vector<uint8_t> load(const char *path) {
  std::ifstream f(path, std::ios::binary);
  assert(f.good());
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}
static void feed_fragmented(Protocol &p, const std::vector<uint8_t> &v) {
  uint32_t at = 0;
  const uint32_t sizes[] = {1, 2, 7, 13, 3, 71, 244, 5};
  uint32_t i = 0;
  while (at < v.size()) {
    const uint32_t n = v.size() - at < sizes[i % 8] ? v.size() - at : sizes[i % 8];
    if (!p.feed(v.data() + at, n)) {
      std::fprintf(stderr, "feed failed at %u/%zu state=%u\n", at, v.size(),
                   static_cast<unsigned>(p.state()));
      assert(false);
    }
    at += n;
    ++i;
  }
}

int main(int argc, char **argv) {
  assert(argc == 10);
  const auto xnor = load(argv[1]);
  const auto expected = load(argv[2]);
  const auto xnou = load(argv[3]);
  const auto xnoz = load(argv[4]);
  const auto commit = load(argv[5]);
  const auto bad_header = load(argv[6]);
  const auto overflow = load(argv[7]);
  const auto before_query = load(argv[8]);
  const auto reset_with_tail = load(argv[9]);
  assert(expected.size() == 8501);

  Backend b;
  uint8_t encoded[4352], decoded[4096];
  uint32_t bitmap[2];
  tiny_inflate::state inflate;
  Workspace w{encoded, sizeof(encoded), decoded, sizeof(decoded), bitmap, 2,
              &inflate};
  Callbacks cb{&b, begin, write, read, validate, activate, abort_backend, event};
  Protocol p(w, cb, 16384);
  assert(p.valid());

  // Sender-generated XNOR: out of order, a payload CRC failure, QUERY missing
  // report, selective retry, final readback CRC, then sender's XNOC commit.
  feed_fragmented(p, xnor);
  assert(p.state() == State::Complete);
  assert(b.max_missing_total == 1);
  assert(b.missing_seq[1] == 1);
  assert(b.writes == 4); // seq 2 is written twice; duplicate is idempotent in content.
  assert(b.image == expected);
  assert(b.done == 1);
  assert(p.feed(nullptr, 0));
  feed_fragmented(p, commit);
  assert(b.validates == 1 && b.activations == 1 && b.commit_ready == 1);

  // Sender-generated XNOU is split at arbitrary byte boundaries and has the
  // same output, length, and CRC as the image.
  Backend raw;
  Callbacks raw_cb{&raw, begin, write, read, validate, activate, abort_backend, event};
  Protocol q(w, raw_cb, 16384);
  assert(q.valid());
  feed_fragmented(q, xnou);
  assert(q.state() == State::Complete);
  assert(raw.image == expected && raw.done == 1);

  Backend legacy;
  Callbacks legacy_cb{&legacy, begin, write, read, validate, activate,
                      abort_backend, event};
  Protocol z(w, legacy_cb, 16384);
  assert(z.valid());
  feed_fragmented(z, xnoz);
  assert(z.state() == State::Complete && legacy.image == expected && legacy.done == 1);

  // Public abort clears state and invokes backend cleanup.
  q.abort();
  assert(q.state() == State::Idle && raw.aborts == 1);

  // Alter the image CRC in a valid sender header: XNOU must fail closed.
  auto bad = xnou;
  bad[8] ^= 1;
  Backend rejected;
  Callbacks rejected_cb{&rejected, begin, write, read, validate, activate,
                        abort_backend, event};
  Protocol r(w, rejected_cb, 16384);
  assert(r.valid());
  assert(!r.feed(bad.data(), bad.size()));
  assert(r.state() == State::Failed && rejected.failed == 1 && rejected.aborts == 1);
  assert(rejected.validates == 0 && rejected.activations == 0);

  auto oversized = xnou;
  oversized[4] = 0x01;
  oversized[5] = 0x40; // 16,385 bytes, above this instance's 16,384-byte bound.
  oversized[6] = oversized[7] = 0;
  Backend bounded;
  Callbacks bounded_cb{&bounded, begin, write, read, validate, activate,
                       abort_backend, event};
  Protocol s(w, bounded_cb, 16384);
  assert(s.valid());
  assert(!s.feed(oversized.data(), 12));
  assert(s.state() == State::Failed && bounded.begun == 0 && bounded.failed == 1);

  // Header CRC failure must discard the frame, leaving every sequence missing.
  Backend bad_header_backend;
  Callbacks bad_header_cb{&bad_header_backend, begin, write, read, validate,
                          activate, abort_backend, event};
  Protocol h(w, bad_header_cb, 16384);
  assert(h.valid());
  feed_fragmented(h, bad_header);
  assert(bad_header_backend.writes == 0 && h.missing_count() == 3);

  // Inflater output is bounded by the caller's 4096-byte decoded buffer.
  Backend overflow_backend;
  Callbacks overflow_cb{&overflow_backend, begin, write, read, validate,
                        activate, abort_backend, event};
  Protocol o(w, overflow_cb, 16384);
  assert(o.valid());
  feed_fragmented(o, overflow);
  assert(overflow_backend.writes == 0 && o.missing_count() == 1);

  Backend no_query;
  Callbacks no_query_cb{&no_query, begin, write, read, validate, activate,
                        abort_backend, event};
  Protocol nq(w, no_query_cb, 16384);
  assert(nq.valid());
  feed_fragmented(nq, before_query);
  assert(nq.state() == State::ChunkSeek && nq.missing_count() == 0);
  nq.finish();
  assert(nq.state() == State::Failed && no_query.done == 0);

  Backend truncated_header;
  Callbacks truncated_header_cb{&truncated_header, begin, write, read, validate,
                                activate, abort_backend, event};
  Protocol th(w, truncated_header_cb, 16384);
  assert(th.valid());
  assert(th.feed(xnou.data(), 5));
  th.finish();
  assert(th.state() == State::Failed && truncated_header.begun == 0);

  Backend reset_backend;
  Callbacks reset_cb{&reset_backend, begin, write, read, validate, activate,
                     abort_backend, event};
  Protocol reset(w, reset_cb, 16384);
  assert(reset.valid());
  assert(reset.feed(reset_with_tail.data(), reset_with_tail.size()));
  assert(reset.state() == State::Idle && reset_backend.begun == 0);
  feed_fragmented(reset, xnou);
  assert(reset.state() == State::Complete && reset_backend.image == expected);

  // Abort midway clears protocol state; the next sender INIT starts cleanly.
  Backend midway;
  Callbacks midway_cb{&midway, begin, write, read, validate, activate,
                      abort_backend, event};
  Protocol m(w, midway_cb, 16384);
  assert(m.valid());
  assert(m.feed(xnou.data(), 20));
  m.abort();
  assert(m.state() == State::Idle && midway.aborts == 1);
  feed_fragmented(m, xnou);
  assert(m.state() == State::Complete && midway.image == expected);

  // Corrupt staged readback before XNOC: no validate/activate may occur.
  Backend readback_bad;
  Callbacks readback_cb{&readback_bad, begin, write, read, validate, activate,
                        abort_backend, event};
  Protocol rb(w, readback_cb, 16384);
  assert(rb.valid());
  feed_fragmented(rb, xnou);
  readback_bad.image[0] ^= 1;
  assert(!rb.feed(commit.data(), commit.size()));
  assert(rb.state() == State::Failed && readback_bad.validates == 0 &&
         readback_bad.activations == 0 && readback_bad.aborts == 1);

  // Backend failures are terminal and never activate a bad image.
  Backend write_failure;
  write_failure.fail_write = true;
  Callbacks write_failure_cb{&write_failure, begin, write, read, validate,
                             activate, abort_backend, event};
  Protocol wf(w, write_failure_cb, 16384);
  assert(wf.valid());
  assert(!wf.feed(xnou.data(), xnou.size()));
  assert(wf.state() == State::Failed && write_failure.activations == 0);

  Backend validate_failure;
  validate_failure.fail_validate = true;
  Callbacks validate_failure_cb{&validate_failure, begin, write, read, validate,
                                activate, abort_backend, event};
  Protocol vf(w, validate_failure_cb, 16384);
  assert(vf.valid());
  feed_fragmented(vf, xnou);
  assert(!vf.feed(commit.data(), commit.size()));
  assert(vf.state() == State::Failed && validate_failure.activations == 0);

  Backend activate_failure;
  activate_failure.fail_activate = true;
  Callbacks activate_failure_cb{&activate_failure, begin, write, read, validate,
                               activate, abort_backend, event};
  Protocol af(w, activate_failure_cb, 16384);
  assert(af.valid());
  feed_fragmented(af, xnou);
  assert(!af.feed(commit.data(), commit.size()));
  assert(af.state() == State::Failed && activate_failure.activations == 1 &&
         activate_failure.aborts == 1);

  std::puts("protocol_test: sender-generated XNOR retry/CRC/readback, XNOU, XNOZ, XNOC, abort and fail-closed CRC passed");
}
