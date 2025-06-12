#ifndef SHIZUKU_TEMPLATE_RESULT_HPP
#define SHIZUKU_TEMPLATE_RESULT_HPP
#include "concepts"
#include "cstdint"
#include "shizuku/concepts/register_sized.hpp"
namespace shizuku {
enum result_code : uintptr_t {
  success = 0,
  range_overflow,
};
namespace templates {

template <typename VALUE_TYPE> struct [[nodiscard]] result {
  struct Success_Payload {
    const shizuku::result_code header;
    const VALUE_TYPE value;
    constexpr Success_Payload(const VALUE_TYPE Value):header(success),value(Value){};
  };
  struct Failed_Payload {
    const shizuku::result_code header;
    const char *reason;
    constexpr Failed_Payload(const shizuku::result_code code,const char* reason_Text):header(code),reason(reason_Text){};
  };
  union {
    Success_Payload success_payload;
    Failed_Payload failed_payload;
  };
  result(const VALUE_TYPE value):success_payload(value){};
  constexpr result(const shizuku::result_code code,const char* reason_Text):failed_payload(code,reason_Text){};
  operator bool(){
    return success_payload.header == success;
  }
};
template <> struct result<void> {
  struct Payload {
    const result_code op_result;
    const char *payload;
  } payload;
};
} // namespace templates
} // namespace shizuku
#endif // SHIZUKU_TEMPLATE_RESULT_HPP