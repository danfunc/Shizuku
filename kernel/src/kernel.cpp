#include "shizuku/kernel.hpp"
#include "shizuku.hpp"
#include "shizuku/object.hpp"
namespace shizuku {
types::kernel::kernel() {}
types::kernel::~kernel() {}
types::kernel kernel;

void context_switch() { kernel.context_switch(); }

void types::kernel::context_switch() {}
} // namespace shizuku
