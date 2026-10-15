#!/usr/bin/env python3
"""オブジェクト番号を振って shizuku/object_ids.hpp および shizuku_composition_ids.cpp を作る。

★ID は合成時採番。object は名前ベース extern const symbol 参照、合成ごとの .c/.cpp 定義を link で解決。
これにより、同 chip/toolchain なら composition を越えて object を一度だけ compile できる。
"""
import argparse
import os
import re
import sys
import tempfile

VALID_IDENTIFIER_PATTERN = re.compile(r'^[a-zA-Z_][a-zA-Z0-9_]*$')

# テンプレート定義 (root, count) と衝突する予約シンボル
RESERVED_SYMBOLS = {"root", "count"}

# C99/C11/C23 および C++11/14/17/20/23 のキーワード
C_CPP_KEYWORDS = {
    # C keywords
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "inline", "int", "long", "register", "restrict", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
    "unsigned", "void", "volatile", "while", "_Alignas", "_Alignof",
    "_Atomic", "_Bool", "_Complex", "_Generic", "_Imaginary", "_Noreturn",
    "_Static_assert", "_Thread_local",
    # C++ keywords
    "alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel",
    "atomic_commit", "atomic_noexcept", "bitand", "bitor", "bool", "catch",
    "char8_t", "char16_t", "char32_t", "class", "compl", "concept",
    "consteval", "constexpr", "constinit", "const_cast", "co_await",
    "co_return", "co_yield", "decltype", "delete", "dynamic_cast",
    "explicit", "export", "false", "friend", "mutable", "namespace",
    "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or",
    "or_eq", "private", "protected", "public", "reflexpr",
    "reinterpret_cast", "requires", "static_assert", "static_cast",
    "template", "this", "thread_local", "throw", "true", "try",
    "typeid", "typename", "using", "virtual", "wchar_t", "xor", "xor_eq",
}


def read_names(path):
    names = []
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            name = line.split("#", 1)[0].strip()
            if name:
                names.append(name)
    return names


def atomic_write(target_path, content):
    target_dir = os.path.dirname(os.path.abspath(target_path))
    os.makedirs(target_dir, exist_ok=True)
    temp_fd, temp_file = tempfile.mkstemp(dir=target_dir, prefix=".tmp_gen_obj_")
    try:
        with open(temp_fd, "w", encoding="utf-8") as handle:
            handle.write(content)
        os.replace(temp_file, target_path)
    except Exception:
        if os.path.exists(temp_file):
            os.remove(temp_file)
        raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--template", required=False, help="Header template")
    parser.add_argument("--out", required=False, help="Header output path")
    parser.add_argument("--source-template", required=False, help="Source definition template")
    parser.add_argument("--source-out", required=False, help="Source definition output path")
    parser.add_argument("--mode", choices=["extern", "constexpr"], default="extern",
                        help="ID宣言/定義方式の明示契約 ('extern' または 'constexpr')")
    parser.add_argument("--limit", type=int, default=0,
                        help="SHIZUKU_OBJECT_COUNT。0 = 検査しない")
    parser.add_argument("--names", nargs="*", default=[],
                        help="直接指定するオブジェクト名のリスト")
    parser.add_argument("lists", nargs="*")
    args = parser.parse_args()

    # 引数ペア検証
    if bool(args.template) != bool(args.out):
        sys.exit("エラー: --template と --out は両方を指定するか、両方省略する必要があります。")
    if bool(args.source_template) != bool(args.source_out):
        sys.exit("エラー: --source-template と --source-out は両方を指定するか、両方省略する必要があります。")
    if not args.out and not args.source_out:
        sys.exit("エラー: 出力先 (--out または --source-out) が指定されていません。")

    # 負の limit 検証
    if args.limit < 0:
        sys.exit("エラー: --limit に負の値は指定できません: %d" % args.limit)

    seen = {}
    header_lines = []
    source_lines = []
    c_symbol_lines = []
    index = 2  # 0 は KERNEL_OBJECT, 1 はブートスレッド (ROOT_OBJECT) で予約

    all_names = []
    for path in sorted(args.lists):
        for name in read_names(path):
            all_names.append((name, path))

    for name in args.names:
        all_names.append((name, "command-line"))

    for name, source in all_names:
        if not VALID_IDENTIFIER_PATTERN.match(name):
            sys.exit("エラー: オブジェクト名 '%s' (%s) は無効な C 識別子です。" % (name, source))
        if name in RESERVED_SYMBOLS:
            sys.exit("エラー: オブジェクト名 '%s' (%s) は予約シンボルです。" % (name, source))
        if name in C_CPP_KEYWORDS:
            sys.exit("エラー: オブジェクト名 '%s' (%s) は C/C++ キーワードのため使用できません。" % (name, source))
        if name in seen:
            sys.exit("オブジェクト名 '%s' が二重に宣言された "
                     "(%s と %s)" % (name, seen[name], source))
        seen[name] = source
        if args.mode == "constexpr":
            header_lines.append("constexpr uintptr_t %s = %d;" % (name, index))
        else:
            header_lines.append("extern const uintptr_t %s;" % name)
        source_lines.append("extern const uintptr_t %s = %d;" % (name, index))
        c_symbol_lines.append("extern const uintptr_t shizuku_object_id_%s = %d;" % (name, index))
        index += 1

    if args.limit and index > args.limit:
        sys.exit("オブジェクトが %d 個あるが上限は %d" % (index, args.limit))

    # Header output
    if args.template and args.out:
        with open(args.template, encoding="utf-8") as handle:
            text = handle.read()
        text = text.replace("@SHIZUKU_OBJECT_ID_BODY@", "\n".join(header_lines))
        text = text.replace("@SHIZUKU_OBJECT_ID_COUNT@", str(index))
        atomic_write(args.out, text)

    # Source output (definition)
    if args.source_template and args.source_out:
        with open(args.source_template, encoding="utf-8") as handle:
            src_text = handle.read()
        src_text = src_text.replace("@SHIZUKU_OBJECT_ID_BODY@", "\n".join(source_lines))
        src_text = src_text.replace("@SHIZUKU_OBJECT_ID_C_SYMBOLS@", "\n".join(c_symbol_lines))
        src_text = src_text.replace("@SHIZUKU_OBJECT_ID_COUNT@", str(index))
        atomic_write(args.source_out, src_text)


if __name__ == "__main__":
    main()

