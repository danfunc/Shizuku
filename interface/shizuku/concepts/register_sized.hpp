#ifndef SHIZUKU_REGISTER_SIZED_HPP
#define SHIZUKU_REGISTER_SIZED_HPP
#include "type_traits"
namespace shizuku {
namespace concepts {
template <typename VALUE_TYPE>
concept register_sized =
    (std::is_integral_v<VALUE_TYPE> || std::is_pointer_v<VALUE_TYPE> || std::is_reference_v<VALUE_TYPE>) &&
    sizeof(VALUE_TYPE) <= sizeof(uintptr_t);
}
} // namespace shizuku
#endif // SHIZUKU_REGISTER_SIZED_HPP