#include "shizuku/kernel.hpp"
#include "shizuku.hpp"
#include "shizuku/object.hpp"
namespace shizuku {
types::kernel::kernel() {}
types::kernel::~kernel() {}
types::kernel kernel;
void types::kernel::context_switch() { this->cpu_driver[0].context_switch(); }

} // namespace shizuku
