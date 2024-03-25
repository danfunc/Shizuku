#include <pico/stdlib.h>
#include <shizuku/kernel.hpp>
#include <stdio.h>
void caller_object_main(size_t callee_object_id, size_t creator_object_id,
                        int argc, char **argv);
int caller_method(size_t callee_object_id, size_t caller_object_id, size_t arg1,
                  size_t arg2);

void callee_object_main(size_t callee_object_id, size_t caller_object_id,
                        size_t arg1, size_t arg2);
void callee_object_method(size_t callee_object_id, size_t caller_object_id,
                          size_t arg1, size_t arg2);

void object_system_test_main() {

  // デモコードのエントリーポイント
  if (auto create_object_result = shizuku::kernel.create_object(
          "caller_object", (shizuku::types::method)caller_object_main, 1, 1)) {

  } else {
    while (1) {
      printf("create_error_code:%d", create_object_result.operation_result);
    }
  }; // オブジェクトを作成

  shizuku::kernel.abort_current_task();
  while (1) {
    printf("abort_error\n");
    sleep_ms(100);
  }
}

void caller_object_main(size_t callee_object_id, size_t creator_object_id,
                        int argc, char **argv) {

  // "caller_object"のエントリーポイント
  if (auto export_result =
          shizuku::kernel.export_method(caller_method, "caller_method")) {
    if (auto call_result = shizuku::kernel.call_method(callee_object_id,
                                                       "caller_method", 1, 1)) {
    } else {
      while (1)
        printf("call_error_code:%d", export_result.operation_result);
    }
  } else {
    while (1) {
      printf("export_error_code:%d", export_result.operation_result);
    }
  }
}
int caller_method(size_t callee_object_id, size_t caller_object_id, size_t arg1,
                  size_t arg2) {
  // "caller_method"のエントリーポイント
  while (1) {
    printf("callee_object_id:%d\n", callee_object_id);
    printf("caller_object_id:%d\n", caller_object_id);
    printf("arg1:%d\n", arg1);
    printf("arg2:%d\n", arg2);
    sleep_ms(1000);
  }
}
