# ===========================================================================
#  オブジェクト番号を**ビルドシステムが振る** (docs/03_porting_policy.md D28)
# ===========================================================================
#  ★閉じた系では、番号の衝突は実行時の登録簿ではなく**ビルド時**に潰す。
#    ラジコンカーのような系では「何が居るか」は作った時点で全部分かっているので、
#    実行時に名前で引く機構は費用だけ払って何も買わない。
#
#  【今日 (2026-08-21) 2 回踏んだ事故】どちらも別ファイルで手で振った番号が衝突し、
#    どちらも**無音**だった:
#      flash_fs の 11 × 非特権プローブの 11 → 「非特権のはずが特権」に化けた
#      sleeper の 6  × blink の 6          → 揺らぎの計測が黙って止まった
#    原因は「手で振ったこと」ではなく「**空いている番号を目で探したこと**」。
#
#  【なぜビルドシステムか】C++ の列挙でも同一ヘッダ内の衝突は潰せるが、モジュールを
#  またぐと結局「どの範囲を誰が使うか」を手で書く合意が要る。**ビルドシステムは
#  どのモジュールが入っているかを既に知っている** (USE_SHIZUKU_MODULES) ので、
#  そこが消える。各モジュールは自分の名前を宣言するだけでよく、他モジュールの
#  ヘッダを見る必要が無い (XNO を別リポジトリに分ける D17 とも噛み合う)。
#
#  【使い方】各コンポーネントの CMakeLists で:
#      shizuku_declare_objects(peripherals gpio spi led)
#  すると生成ヘッダに shizuku::object_id::gpio などが出る。番号は宣言順。

# 名前を 1 つ以上、大域リストへ積む。同じ名前が 2 度来たらその場で落とす。
function(shizuku_declare_objects component)
  get_property(known GLOBAL PROPERTY SHIZUKU_OBJECT_NAMES)
  foreach(name IN LISTS ARGN)
    if("${name}" IN_LIST known)
      message(FATAL_ERROR
              "オブジェクト名 '${name}' が二重に宣言された (${component})")
    endif()
    list(APPEND known "${name}")
  endforeach()
  set_property(GLOBAL PROPERTY SHIZUKU_OBJECT_NAMES "${known}")
endfunction()

# 宣言を集め終えてから 1 回だけ呼ぶ。番号は宣言順に 1 から振る
# (0 はブートスレッドが名乗る root で予約)。
function(shizuku_generate_object_ids output_dir)
  get_property(names GLOBAL PROPERTY SHIZUKU_OBJECT_NAMES)
  list(LENGTH names count)
  math(EXPR needed "${count} + 1")
  if(needed GREATER SHIZUKU_OBJECT_COUNT)
    message(FATAL_ERROR
            "オブジェクトが ${needed} 個あるが SHIZUKU_OBJECT_COUNT=${SHIZUKU_OBJECT_COUNT}")
  endif()
  set(body "")
  set(index 1)
  foreach(name IN LISTS names)
    string(APPEND body "constexpr uintptr_t ${name} = ${index};\n")
    math(EXPR index "${index} + 1")
  endforeach()
  set(SHIZUKU_OBJECT_ID_BODY "${body}")
  set(SHIZUKU_OBJECT_ID_COUNT "${needed}")
  configure_file(${CMAKE_SOURCE_DIR}/cmake/object_ids.hpp.in
                 ${output_dir}/shizuku/object_ids.hpp @ONLY)
  message(STATUS "object ids: ${count} declared (${names})")
endfunction()
