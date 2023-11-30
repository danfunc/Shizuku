#ifndef PROCESSOR_HPP
#define PROCESSOR_HPP PROVIDED_BY_CMAKE
#include "shizuku/processors.hpp"
#include "shizuku/processors/rp2040.hpp"
namespace shizuku {
namespace processor = shizuku::types::processors::SHIZUKU_PROCESSOR;
using context = processor::context;
using cpu_driver = processor::cpu_driver;
constexpr unsigned int processor_count = SHIZUKU_PROCESSOR_COUNT;
} // namespace shizuku
#endif
