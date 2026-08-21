# ===========================================================================
#  ファーム = **合成 (composition)** だけを持つ層
# ===========================================================================
#  ★このディレクトリだけが実行ファイルを作る。カーネルもカーネルオブジェクトも
#    ボードもライブラリで、「どれを繋いで何を走らせるか」を決めているのはここ 1 つ
#    しかない。カーネルを配る側 (XNO 等) は、この main.cpp に相当するものを
#    **自分で書く** — それが「ライブラリとして提供する」ということ。
target_sources(Shizuku PRIVATE main.cpp)
target_link_libraries(Shizuku PRIVATE
                      shizuku_kernel shizuku_kernel_object shizuku_selftest shizuku_apps
                      pico_sdk_support pico_stdio pico_stdlib)
pico_enable_stdio_usb(Shizuku 1)
# ★起動直後の診断はホストがポートを開く前に流れてしまう (参照実装も同じ罠を踏んで
#   いる: PORT §7「焼く前にシリアルキャプチャを張る」)。ホストの接続を少しだけ
#   待つことで、自己テストの結果が**必ず**見えるようにする。
target_compile_definitions(Shizuku PRIVATE PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=2000)
