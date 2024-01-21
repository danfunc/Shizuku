#ifndef SHIZUKU_METHOD_HPP
#define SHIZUKU_METHOD_HPP
namespace shizuku {
namespace types {
using method = int (*)(size_t callee_object_num, size_t caller_object_num,
                       size_t arg1, size_t arg2);
}
} // namespace shizuku
#endif