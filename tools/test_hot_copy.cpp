#include "shizuku/freestanding_mem.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>

int main() {
  uint32_t cases = 0;
  for (unsigned source_offset = 0; source_offset < 8; ++source_offset)
    for (unsigned dest_offset = 0; dest_offset < 8; ++dest_offset)
      for (unsigned bytes = 0; bytes <= 256; ++bytes) {
        alignas(8) uint8_t source[288], dest[288];
        for (unsigned i = 0; i < 288; ++i) {
          source[i] = static_cast<uint8_t>(i * 37 + 11);
          dest[i] = 0xa5;
        }
        const unsigned begin = 8 + dest_offset;
        shizuku::shizuku_memcpy(dest + begin, source + 8 + source_offset, bytes);
        for (unsigned i = 0; i < 288; ++i) {
          const uint8_t expected = i >= begin && i < begin + bytes
              ? source[8 + source_offset + i - begin] : 0xa5;
          assert(dest[i] == expected); // copied bytes plus untouched guards
        }
        ++cases;
      }
  std::printf("hot_copy cases=%u PASS\n", cases);
}
