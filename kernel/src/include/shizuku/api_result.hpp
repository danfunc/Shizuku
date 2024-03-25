#ifndef SHIZUKU_API_RESULT_HPP
#define SHIZUKU_API_RESULT_HPP
#include "shizuku/config.hpp"
#include <concepts>

namespace shizuku {
namespace types {
enum struct error_code : intptr_t {
  success = 0,
  wait_for_async_call,
  no_such_object,               // for super object
  no_such_method,               // for ether object
  no_such_object_in_your_child, // for normal object
  you_are_not_super_object, // for normal object using method for super object
  your_thread_are_killed,
  your_object_are_killed, /* must no return because not only caller object but
                           also caller thread and context are killed and
                           disabled.*/
  kernel_not_have_enough_memory,
};
template <typename T> struct api_result {
  error_code operation_result;
  T *return_value;
  operator bool() {
    if (this->operation_result == error_code::success) {
      return false;
    } else {
      return true;
    }
  };
  api_result(T return_value) : operation_result{error_code::success} {
    return_value = return_value;
  };
  api_result(error_code error_code, T return_value = T{})
      : operation_result{error_code} {
    return_value = return_value;
  };
  ~api_result() {};
};

template <std::integral T> struct api_result<T> {

  error_code operation_result;
  T return_value;
  operator bool() {
    if (this->operation_result == error_code::success) {
      return false;
    } else {
      return true;
    }
  };
  api_result(T return_value) : operation_result{error_code::success} {
    return_value = return_value;
  };
  api_result(error_code error_code, T return_value = T{})
      : operation_result{error_code} {
    return_value = return_value;
  };
  ~api_result() {};
};

template <typename T> struct api_result<T *> {

  error_code operation_result;
  T *return_value;
  operator bool() {
    if (this->operation_result == error_code::success) {
      return false;
    } else {
      return true;
    }
  };
  api_result(T *return_value) : operation_result{error_code::success} {
    return_value = return_value;
  };
  api_result(error_code error_code, T *return_value = nullptr)
      : operation_result{error_code} {
    return_value = return_value;
  };
  ~api_result() {};
};

template <> struct api_result<void> {
  error_code operation_result;
  operator bool() {
    if (this->operation_result == error_code::success) {
      return true;
    } else {
      return false;
    }
  };
  api_result(error_code error_code) : operation_result{error_code} {};
  api_result() : operation_result{error_code::success} {};
};
using intptr_t_api_result = api_result<intptr_t>;
} // namespace types

} // namespace shizuku

#endif