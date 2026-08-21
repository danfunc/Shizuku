shizuku_declare_objects(${CMAKE_CURRENT_SOURCE_DIR}/objects.list)
# 自己テストは「カーネルの一部」ではなく、カーネルを使う**オブジェクトの集合**。
# ライブラリとして分けておくと、合成側が入れるか入れないかを選べる。
add_library(shizuku_selftest STATIC call_ladder.cpp thread_ladder.cpp stress.cpp
                                    unprivileged.cpp memory_ladder.cpp)
target_link_libraries(shizuku_selftest PUBLIC shizuku_kernel_object)
