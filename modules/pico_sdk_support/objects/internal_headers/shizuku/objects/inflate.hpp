#ifndef SHIZUKU_OBJECTS_INFLATE_HPP
#define SHIZUKU_OBJECTS_INFLATE_HPP
#include <cstdint>

// ===========================================================================
//  tiny_inflate — raw deflate (RFC1951) の展開だけ。圧縮側は持たない。
// ===========================================================================
namespace tiny_inflate {

enum : int32_t {
  ERR_INPUT = -1,    // 入力が途中で尽きた
  ERR_FORMAT = -2,   // ブロック種別・符号が不正
  ERR_OUTPUT = -3,   // 出力が out_cap を超えた
  ERR_DISTANCE = -4, // 後方参照が出力の先頭より前を指した
};

namespace detail {

constexpr int32_t FAST_BITS = 9;

struct huffman {
  int16_t count[16];   // 符号長 len の符号の個数 (count[0] は未使用)
  int16_t symbol[288]; // 符号長順に並べた記号
  uint16_t fast[1 << FAST_BITS];
};

struct bits {
  const uint8_t *in;
  uint32_t len;
  uint32_t at; // 次に読むバイト
  uint32_t bitbuf;
  uint32_t bitcnt;
};

// need ビットを LSB 先頭で取り出す。足りなければ負を返す。
inline int32_t get(bits &b, int32_t need) {
  uint32_t value = b.bitbuf;
  while (b.bitcnt < (uint32_t)need) {
    if (b.at >= b.len)
      return ERR_INPUT;
    value |= (uint32_t)b.in[b.at++] << b.bitcnt;
    b.bitcnt += 8;
  }
  b.bitbuf = value >> need;
  b.bitcnt -= (uint32_t)need;
  return (int32_t)(value & ((1u << need) - 1));
}

// 歩く道。符号長 1 から順に「その長さの符号の範囲」に入るか見る。
inline int32_t decode_slow(bits &b, const huffman &h) {
  int32_t code = 0, first = 0, index = 0;
  for (int32_t len = 1; len <= 15; ++len) {
    const int32_t bit = get(b, 1);
    if (bit < 0)
      return ERR_INPUT;
    code |= bit;
    const int32_t count = h.count[len];
    if (code - count < first)
      return h.symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return ERR_FORMAT;
}

inline int32_t decode(bits &b, const huffman &h) {
  while (b.bitcnt < (uint32_t)FAST_BITS && b.at < b.len) {
    b.bitbuf |= (uint32_t)b.in[b.at++] << b.bitcnt;
    b.bitcnt += 8;
  }
  const uint16_t entry = h.fast[b.bitbuf & ((1u << FAST_BITS) - 1)];
  const uint32_t length = (uint32_t)entry >> 12;
  if (length != 0 && length <= b.bitcnt) {
    b.bitbuf >>= length;
    b.bitcnt -= length;
    return (int32_t)(entry & 0x0FFFu);
  }
  return decode_slow(b, h);
}

// 符号長の配列から正準ハフマン表を組む。
inline int32_t build(huffman &h, const uint8_t *length, int32_t n) {
  for (int32_t len = 0; len <= 15; ++len)
    h.count[len] = 0;
  for (int32_t symbol = 0; symbol < n; ++symbol)
    ++h.count[length[symbol]];
  if (h.count[0] == n)
    return 0;
  int32_t left = 1;
  for (int32_t len = 1; len <= 15; ++len) {
    left <<= 1;
    left -= h.count[len];
    if (left < 0)
      return ERR_FORMAT;
  }
  int16_t offs[16];
  offs[1] = 0;
  for (int32_t len = 1; len < 15; ++len)
    offs[len + 1] = (int16_t)(offs[len] + h.count[len]);
  for (int32_t symbol = 0; symbol < n; ++symbol)
    if (length[symbol] != 0)
      h.symbol[offs[length[symbol]]++] = (int16_t)symbol;

  for (int32_t i = 0; i < (1 << FAST_BITS); ++i)
    h.fast[i] = 0;
  uint32_t first_code = 0;
  int32_t index = 0;
  for (int32_t len = 1; len <= 15; ++len) {
    uint32_t code = first_code;
    for (int32_t i = 0; i < h.count[len]; ++i) {
      const int32_t symbol = h.symbol[index++];
      if (len <= FAST_BITS) {
        uint32_t reversed = 0;
        for (int32_t bit = 0; bit < len; ++bit)
          reversed |= ((code >> bit) & 1u) << (len - 1 - bit);
        const uint16_t value =
            (uint16_t)(((uint32_t)len << 12) | (uint32_t)symbol);
        for (uint32_t at = reversed; at < (1u << FAST_BITS); at += (1u << len))
          h.fast[at] = value;
      }
      ++code;
    }
    first_code = (first_code + (uint32_t)h.count[len]) << 1;
  }
  return left;
}

struct tables {
  huffman lencode;
  huffman distcode;
};

constexpr int16_t LENS[29] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                              15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                              67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr int16_t LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                              2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr int16_t DISTS[30] = {1,    2,    3,    4,     5,     7,    9,    13,
                               17,   25,   33,   49,    65,    97,   129,  193,
                               257,  385,  513,  769,   1025,  1537, 2049, 3073,
                               4097, 6145, 8193, 12289, 16385, 24577};
constexpr int16_t DEXT[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                              4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                              9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

inline int32_t codes(bits &b, const tables &t, uint8_t *out, uint32_t out_cap,
                     uint32_t &at) {
  while (true) {
    const int32_t symbol = decode(b, t.lencode);
    if (symbol < 0)
      return symbol;
    if (symbol < 256) {
      if (at >= out_cap)
        return ERR_OUTPUT;
      out[at++] = (uint8_t)symbol;
      continue;
    }
    if (symbol == 256)
      return 0; // ブロック終端
    const int32_t index = symbol - 257;
    if (index >= 29)
      return ERR_FORMAT;
    const int32_t extra = get(b, LEXT[index]);
    if (extra < 0)
      return ERR_INPUT;
    const uint32_t length = (uint32_t)LENS[index] + (uint32_t)extra;

    const int32_t dsym = decode(b, t.distcode);
    if (dsym < 0)
      return dsym;
    if (dsym >= 30)
      return ERR_FORMAT;
    const int32_t dextra = get(b, DEXT[dsym]);
    if (dextra < 0)
      return ERR_INPUT;
    const uint32_t distance = (uint32_t)DISTS[dsym] + (uint32_t)dextra;
    if (distance > at)
      return ERR_DISTANCE;
    if (at + length > out_cap)
      return ERR_OUTPUT;
    for (uint32_t i = 0; i < length; ++i, ++at)
      out[at] = out[at - distance];
  }
}

inline void fixed_tables(tables &t) {
  uint8_t length[288];
  int32_t symbol = 0;
  for (; symbol < 144; ++symbol)
    length[symbol] = 8;
  for (; symbol < 256; ++symbol)
    length[symbol] = 9;
  for (; symbol < 280; ++symbol)
    length[symbol] = 7;
  for (; symbol < 288; ++symbol)
    length[symbol] = 8;
  build(t.lencode, length, 288);
  for (symbol = 0; symbol < 30; ++symbol)
    length[symbol] = 5;
  build(t.distcode, length, 30);
}

inline int32_t dynamic_tables(bits &b, tables &t) {
  static const int16_t ORDER[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                    11, 4,  12, 3, 13, 2, 14, 1, 15};
  const int32_t nlen = get(b, 5), ndist = get(b, 5), ncode = get(b, 4);
  if (nlen < 0 || ndist < 0 || ncode < 0)
    return ERR_INPUT;
  const int32_t n_len = nlen + 257, n_dist = ndist + 1, n_code = ncode + 4;
  if (n_len > 286 || n_dist > 30)
    return ERR_FORMAT;

  uint8_t length[288 + 30];
  for (int32_t i = 0; i < 19; ++i)
    length[i] = 0;
  for (int32_t i = 0; i < n_code; ++i) {
    const int32_t v = get(b, 3);
    if (v < 0)
      return ERR_INPUT;
    length[ORDER[i]] = (uint8_t)v;
  }
  if (build(t.lencode, length, 19) != 0)
    return ERR_FORMAT;

  int32_t index = 0;
  while (index < n_len + n_dist) {
    const int32_t symbol = decode(b, t.lencode);
    if (symbol < 0)
      return symbol;
    if (symbol < 16) {
      length[index++] = (uint8_t)symbol;
      continue;
    }
    int32_t repeat = 0;
    uint8_t value = 0;
    if (symbol == 16) {
      if (index == 0)
        return ERR_FORMAT;
      value = length[index - 1];
      const int32_t e = get(b, 2);
      if (e < 0)
        return ERR_INPUT;
      repeat = 3 + e;
    } else if (symbol == 17) {
      const int32_t e = get(b, 3);
      if (e < 0)
        return ERR_INPUT;
      repeat = 3 + e;
    } else {
      const int32_t e = get(b, 7);
      if (e < 0)
        return ERR_INPUT;
      repeat = 11 + e;
    }
    if (index + repeat > n_len + n_dist)
      return ERR_FORMAT;
    while (repeat-- > 0)
      length[index++] = value;
  }
  if (length[256] == 0)
    return ERR_FORMAT;

  if (build(t.lencode, length, n_len) < 0)
    return ERR_FORMAT;
  if (build(t.distcode, length + n_len, n_dist) < 0)
    return ERR_FORMAT;
  return 0;
}

} // namespace detail

struct state {
  detail::tables tables;
};

// raw deflate ストリームを展開する。戻り値 = 出力バイト数、負ならエラー。
inline int32_t run(state &scratch, const uint8_t *in, uint32_t in_len,
                   uint8_t *out, uint32_t out_cap) {
  detail::bits b{in, in_len, 0, 0, 0};
  detail::tables &t = scratch.tables;
  uint32_t at = 0;
  int32_t last = 0;
  do {
    last = detail::get(b, 1);
    if (last < 0)
      return ERR_INPUT;
    const int32_t type = detail::get(b, 2);
    if (type < 0)
      return ERR_INPUT;
    int32_t rc = 0;
    if (type == 0) {
      b.bitbuf = 0;
      b.bitcnt = 0;
      if (b.at + 4 > b.len)
        return ERR_INPUT;
      const uint32_t length =
          (uint32_t)b.in[b.at] | ((uint32_t)b.in[b.at + 1] << 8);
      const uint32_t nlength =
          (uint32_t)b.in[b.at + 2] | ((uint32_t)b.in[b.at + 3] << 8);
      if ((length ^ 0xFFFFu) != nlength)
        return ERR_FORMAT;
      b.at += 4;
      if (b.at + length > b.len)
        return ERR_INPUT;
      if (at + length > out_cap)
        return ERR_OUTPUT;
      for (uint32_t i = 0; i < length; ++i)
        out[at++] = b.in[b.at++];
    } else if (type == 1) {
      detail::fixed_tables(t);
      rc = detail::codes(b, t, out, out_cap, at);
    } else if (type == 2) {
      rc = detail::dynamic_tables(b, t);
      if (rc == 0)
        rc = detail::codes(b, t, out, out_cap, at);
    } else {
      return ERR_FORMAT;
    }
    if (rc < 0)
      return rc;
  } while (last == 0);
  return (int32_t)at;
}

} // namespace tiny_inflate
#endif // SHIZUKU_OBJECTS_INFLATE_HPP
