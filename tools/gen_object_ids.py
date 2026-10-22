#!/usr/bin/env python3
"""オブジェクト番号を振って shizuku/object_ids.hpp を作る (D28)。

★CMake と Bazel の**両方がこれを呼ぶ**。番号を振る規則を 2 つ持つと、片方だけ
直して番号がずれる — それは黙って壊れる類の壊れ方なので、実装は 1 つにする。

★入力の .list は**パス順に並べ替えてから**読む。呼ぶ側が渡す順に依存させると、
2 つのビルドシステムで並びが食い違って番号がずれる (同じ理由)。
"""
import argparse
import sys


def read_names(path):
    names = []
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            name = line.split("#", 1)[0].strip()
            if name:
                names.append(name)
    return names


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--template", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--limit", type=int, default=0,
                        help="SHIZUKU_OBJECT_COUNT。0 = 検査しない")
    parser.add_argument("lists", nargs="*")
    args = parser.parse_args()

    seen = {}
    lines = []
    index = 2  # 0 は KERNEL_OBJECT, 1 はブートスレッド (ROOT_OBJECT) で予約
    for path in sorted(args.lists):
        for name in read_names(path):
            if name in seen:
                sys.exit("オブジェクト名 '%s' が二重に宣言された "
                         "(%s と %s)" % (name, seen[name], path))
            seen[name] = path
            lines.append("constexpr uintptr_t %s = %d;" % (name, index))
            index += 1
    if args.limit and index > args.limit:
        sys.exit("オブジェクトが %d 個あるが上限は %d" % (index, args.limit))

    with open(args.template, encoding="utf-8") as handle:
        text = handle.read()
    text = text.replace("@SHIZUKU_OBJECT_ID_BODY@", "\n".join(lines))
    text = text.replace("@SHIZUKU_OBJECT_ID_COUNT@", str(index))
    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write(text)


if __name__ == "__main__":
    main()
