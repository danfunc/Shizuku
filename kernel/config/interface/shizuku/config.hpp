#ifndef SHIZUKU_CONFIG
#define SHIZUKU_CONFIG PROVIDED_BY_CMAKE
#include "shizuku/processors.hpp"
#include "shizuku/processors/rp2040.hpp"
#define SHIZUKU_PROCESSOR rp2040
#define SHIZUKU_PLATFORM pico_sdk
namespace shizuku {
namespace processor = shizuku::types::processors::SHIZUKU_PROCESSOR;
using context = processor::context;
using cpu_driver = processor::cpu_driver;
} // namespace shizuku
#endif
