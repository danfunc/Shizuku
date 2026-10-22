#ifndef SHIZUKU_METHOD_ENTRY_HPP
#define SHIZUKU_METHOD_ENTRY_HPP
#include <cstdint>
#include <type_traits>

// ===========================================================================
//  ABI converter (callee 側) — オブジェクトのメソッド / ハンドラの入口を登録するときの形
// ===========================================================================
//  CREATE_OBJECT の main、EXPORT_METHOD の入口、objectland handler の入口は、関数の
//  アドレスを直に渡さず、この関数が返す入口 stub のアドレスを渡す (2026-10-22 決定、
//  .handoff/v3-design-notes.md §4/§5)。
//
//    ARCH::syscall(CREATE_OBJECT, id, shizuku::method_entry<ARCH, &main>(), flags);
//
//  理由: method call の入口でカーネルはレジスタを callee 用に入れ直さない。caller のレジスタが
//  そのまま入力として通る (引数は a0–a5、戻り口は ra、あとは sp / pc だけがカーネルの決めた値)。
//  gp はオブジェクトごと (リンク単位ごと) に違い、カーネルは callee の __global_pointer$ を
//  知らないので、callee 側が自分で入れる。入口 stub はオブジェクトと同じ翻訳単位で
//  template から生成されるため、__global_pointer$ はそのオブジェクトの値に解決される。
//
//  ISA ごとに別実装 (ARCH::method_stub<F>() を持つ ARCH だけが stub を作る)。持たない ARCH
//  (ARM) は変換が要らないので F をそのまま返す。
//  ★F は定数式の関数ポインタ。入口を実行時に選ぶ表では、表に stub を引いたものを入れる
//    (表の初期化は実行時でよい。この関数は constexpr ではない)。
namespace shizuku {

template <class ARCH, auto F>
  requires std::is_function_v<std::remove_pointer_t<decltype(F)>>
inline uintptr_t method_entry() {
  if constexpr (requires { ARCH::template method_stub<F>(); })
    return reinterpret_cast<uintptr_t>(ARCH::template method_stub<F>());
  else
    return reinterpret_cast<uintptr_t>(F);
}

} // namespace shizuku
#endif // SHIZUKU_METHOD_ENTRY_HPP
