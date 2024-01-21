#include <pico/stdlib.h>
#include <shizuku/kernel.hpp>
#include <stdio.h>
void test_object_main(size_t callee_object_id, size_t creator_object_id,
                      int argc, char **argv);
int test_method(size_t callee_object_id, size_t caller_object_id, size_t arg1,
                size_t arg2);
void object_system_test_main() {
  shizuku::kernel.create_object("test_object",
                                (shizuku::types::method)test_object_main, 1,
                                1); // オブジェクトを作成

  shizuku::kernel.abort_current_task();
}

void test_object_main(size_t callee_object_id, size_t creator_object_id,
                      int argc, char **argv) {
  shizuku::kernel.export_method(callee_object_id, test_method,
                                "test_method"); // メソッドを公開
  shizuku::kernel.call_method(callee_object_id, callee_object_id, "test_method",
                              1, 1); // メソッドを呼び出し
}
int test_method(size_t callee_object_id, size_t caller_object_id, size_t arg1,
                size_t arg2) {
  while (1) {
    printf("callee_object_id:%d\n", callee_object_id);
    printf("caller_object_id:%d\n", caller_object_id);
    printf("arg1:%d\n", arg1);
    printf("arg2:%d\n", arg2);
    sleep_ms(1000);
  }
}
