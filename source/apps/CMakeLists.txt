shizuku_declare_objects(${CMAKE_CURRENT_SOURCE_DIR}/objects.list)
# アプリは「カーネルの一部」ではなく、カーネルを使うオブジェクト。★いずれ XNO 側へ
# 移す (D17: Shizuku 本体は小さく保つ) が、今は動かす場所がここしか無い。
add_library(shizuku_apps STATIC thermal.cpp)
target_link_libraries(shizuku_apps PUBLIC shizuku_kernel_object)
