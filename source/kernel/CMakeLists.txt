# カーネル = 機構だけの層。**資源を持たない** (スレッドの記憶も貸してもらう) ので、
# 実体としてはライブラリ以外になりようがない。
add_library(shizuku_kernel STATIC init.cpp dispatch.cpp thread.cpp
                                  ../cpu_manager/init.cpp)
# ★依存するのはボードの**型**だけ (テンプレート引数として食う)。実装との循環は
#   モジュール側が宣言している (例外の入口 ↔ カーネル)。
target_link_libraries(shizuku_kernel PUBLIC Shizuku_internal_HEADERS
                                            pico_sdk_support_headers)
