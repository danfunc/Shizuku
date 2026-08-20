# カーネルオブジェクト = 方針の層。★カーネルとは**別のライブラリ**にする。
#   分けること自体が D1 (カーネルはオブジェクトを知らない) の検算になる —
#   もし kernel が kobj のシンボルを必要とするなら、それは知っている証拠。
#   実際、依存は一方向 (kobj → kernel) しか無い。
add_library(shizuku_kernel_object STATIC handler.cpp memory.cpp)
target_link_libraries(shizuku_kernel_object PUBLIC shizuku_kernel)
