#ifndef SHIZUKU_KERNEL_HPP
#define SHIZUKU_KERNEL_HPP
#include "shizuku/templates/table.hpp"
namespace shizuku {
namespace templates {
template <typename CPU_MANAGER,typename OBJECT> class kernel {
    CPU_MANAGER cpu_manager;
    static_table<OBJECT*,128> object_table;
};
} // namespace templates
} // namespace shizuku
#endif // SHIZUKU_KERNEL_HPP