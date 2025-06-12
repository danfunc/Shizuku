#ifndef SHIZUKU_CONFIG_TEMPLATE_HPP
#define SHIZUKU_CONFIG_TEMPLATE_HPP

${INCLUDE_HEADERS_INSTRUCTION}

#include "shizuku/templates/kernel.hpp"
#include "shizuku/templates/cpu_manager.hpp"
#include "shizuku/templates/table.hpp"
#include "shizuku/templates/object.hpp"
#include "shizuku/dummy.hpp"

namespace shizuku{
    using KERNEL = templates::kernel<shizuku::templates::cpu_manager<shizuku::cpu_drivers::${SHIZUKU_CPU_MANAGER},${SHIZUKU_CPU_COUNT}>,shizuku::templates::object<${SHIZUKU_OBJECT_USE_THREAD_TABLE},${SHIZUKU_OBJECT_USE_MEMORY_TABLE},${SHIZUKU_OBJECT_USE_METHOD_TABLE}>>;
};

#endif // SHIZUKU_CONFIG_TEMPLATE_HPP