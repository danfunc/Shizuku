#ifndef SHIZUKU_TEMPLATE_RESULT_HPP
#define SHIZUKU_TEMPLATE_RESULT_HPP
#include "concepts"
#include "cstdint"
#include "shizuku/concepts/register_sized.hpp"
namespace shizuku {
enum result_code : uintptr_t {
  success = 0,
  range_overflow,
  allocation_failed,
};
namespace templates {

template <typename VALUE_TYPE> struct [[nodiscard]] result {
  const shizuku::result_code header;
  struct Success_Payload {
    VALUE_TYPE value;
    constexpr Success_Payload(const VALUE_TYPE Value) : value(Value){};
  };
  struct Failed_Payload {
    const char *reason;
    constexpr Failed_Payload(const char *reason_Text) : reason(reason_Text){};
  };
  union {
    Success_Payload success_payload;
    Failed_Payload failed_payload;
  };
  result(const VALUE_TYPE value)
      : header(success), success_payload(value){};
  constexpr result(const shizuku::result_code code, const char *reason_Text)
      : header(code), failed_payload(reason_Text){};

  explicit operator bool() const { return header == success; }

  VALUE_TYPE &value() { return success_payload.value; }
  const VALUE_TYPE &value() const { return success_payload.value; }

  const char *reason() const {
    return failed_payload.reason;
  }
};
template <> struct [[nodiscard]] result<void> {
  const result_code header;
  const char *reason;

  result() : header(success), reason("operation successful"){};
  constexpr result(const result_code code, const char *reason_text)
      : header(code), reason(reason_text){};

  explicit operator bool() const {
    return header == success;
  }
};
} // namespace templates
} // namespace shizuku
#endif // SHIZUKU_TEMPLATE_RESULT_HPP