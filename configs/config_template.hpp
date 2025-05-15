#ifndef SHIZUKU_CONFIG_TEMPLATE_HPP
#define SHIZUKU_CONFIG_TEMPLATE_HPP

${INCLUDE_HEADERS_INSTRUCTION}

#include "shizuku/templates/kernel.hpp"
#include "shizuku/templates/cpu_manager.hpp"
#include "shizuku/dummy.hpp"

namespace shizuku{
    using kernel = templates::kernel<shizuku::templates::cpu_manager<shizuku::cpu_drivers::${SHIZUKU_CPU_MANAGER},${SHIZUKU_CPU_COUNT}>,shizuku::dummy::object>;
}

#endif // SHIZUKU_CONFIG_TEMPLATE_HPP