#ifndef SHIZUKU_OBJECTLAND_HELLO_HPP
#define SHIZUKU_OBJECTLAND_HELLO_HPP

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Hello object のエントリポイント (CREATE_OBJECT に渡す entry_pc)
uintptr_t hello_main(uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4);

// Hello object のメソッド番号
enum hello_method : uintptr_t {
  HELLO_METHOD_MAIN = 0,
  HELLO_METHOD_PING = 1,
  HELLO_METHOD_PUSH = 2,
};

// 単体 ELF 実行用のエントリシンボル
void hello_entry(void);

#ifdef __cplusplus
}
#endif

#endif // SHIZUKU_OBJECTLAND_HELLO_HPP
