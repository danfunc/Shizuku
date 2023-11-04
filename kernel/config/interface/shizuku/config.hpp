#ifndef SHIZUKU_CONFIG_HPP
#define SHIZUKU_CONFIG_HPP
#include "shizuku/processors.hpp"
#include "shizuku/processors/processor.hpp"
namespace shizuku {
namespace processor = shizuku::types::processors::SHIZUKU_PROCESSOR;
using cpu_driver = processor::cpu_driver;
using context = processor::context;
} // namespace shizuku
#endif