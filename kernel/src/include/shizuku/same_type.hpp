#ifndef SAME_TYPE_HPP
#define SAME_TYPE_HPP
namespace shizuku {
template <typename T, typename U> struct IsSame {
  inline static constexpr bool value = false;
};

template <typename T> struct IsSame<T, T> {
  inline static constexpr bool value = true;
};
template <typename T, typename U>
inline constexpr bool same_type_value = IsSame<T, U>::value;
template <typename T, typename U>
concept same_type = same_type_value<T, U>;
template <typename T, typename U>
concept same_as = same_type_value<T, U>;
}; // namespace shizuku
#endif