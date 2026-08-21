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
#  ★番号を振る規則そのものは tools/gen_object_ids.py に 1 つだけ置き、
#    **CMake も Bazel もそれを呼ぶ**。規則を 2 つ持つと片方だけ直して番号がずれる。
#
#  【使い方】各コンポーネントの CMakeLists で:
#      shizuku_declare_objects(${CMAKE_CURRENT_SOURCE_DIR}/objects.list)

function(shizuku_declare_objects list_file)
  if(NOT EXISTS "${list_file}")
    message(FATAL_ERROR "オブジェクト一覧が無い: ${list_file}")
  endif()
  get_property(files GLOBAL PROPERTY SHIZUKU_OBJECT_LISTS)
  list(APPEND files "${list_file}")
  set_property(GLOBAL PROPERTY SHIZUKU_OBJECT_LISTS "${files}")
  # 一覧を書き換えたら configure からやり直す (書き換えても効かない、が無いように)。
  set_property(DIRECTORY "${CMAKE_SOURCE_DIR}"
               APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${list_file}")
endfunction()

# 宣言を集め終えてから 1 回だけ呼ぶ。
function(shizuku_generate_object_ids output_dir)
  get_property(files GLOBAL PROPERTY SHIZUKU_OBJECT_LISTS)
  file(MAKE_DIRECTORY "${output_dir}/shizuku")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env python3
            ${CMAKE_SOURCE_DIR}/tools/gen_object_ids.py
            --template ${CMAKE_SOURCE_DIR}/cmake/object_ids.hpp.in
            --out ${output_dir}/shizuku/object_ids.hpp
            --limit ${SHIZUKU_OBJECT_COUNT}
            ${files}
    RESULT_VARIABLE status
    ERROR_VARIABLE  message)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "オブジェクト番号の生成に失敗: ${message}")
  endif()
  list(LENGTH files how_many)
  message(STATUS "object ids: generated from ${how_many} list(s)")
endfunction()
