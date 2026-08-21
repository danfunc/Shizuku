#!/bin/sh
# ===========================================================================
#  ファーム書き込み (bazelisk run //firmware:flash [uf2])
# ===========================================================================
#  picotool は BOOTSEL で挿したデバイスにも、-f で「動いているファームを再起動させて」
#  も書ける (このファームは USB CDC を出しているのでリセット要求を受けられる)。
#  ★動いているファームが固まっていると、そのリセット要求を処理できず**書かずに
#    終わる**ことがある (参照実装 HANDOFF §7 の罠)。終了コードを必ず見る。
set -eu

uf2="${1:-}"
if [ -z "${uf2}" ]; then
  # bazel run から呼ばれたときは、ワークスペースの bazel-bin を見る。
  if [ -n "${BUILD_WORKSPACE_DIRECTORY:-}" ]; then
    uf2="${BUILD_WORKSPACE_DIRECTORY}/bazel-bin/firmware/shizuku.uf2"
  else
    uf2="bazel-bin/firmware/shizuku.uf2"
  fi
fi

if [ ! -f "${uf2}" ]; then
  echo "書き込む uf2 が無い: ${uf2}" >&2
  echo "  先に: bazelisk build //firmware:shizuku_uf2" >&2
  exit 1
fi

picotool="${PICOTOOL:-}"
if [ -z "${picotool}" ]; then
  for candidate in /opt/picotool/bin/picotool "${HOME}/.pico-sdk/picotool/bin/picotool"; do
    [ -x "${candidate}" ] && picotool="${candidate}" && break
  done
fi
if [ -z "${picotool}" ]; then
  picotool="$(command -v picotool || true)"
fi
if [ -z "${picotool}" ]; then
  echo "picotool が見つからない (PICOTOOL=/path/to/picotool で指定できる)" >&2
  exit 1
fi

echo "picotool load: ${uf2}"
# -x 書いたら実行 / -v 書き込みを検証 / -u 同一セクタは飛ばす / -f 動作中でも再起動
if ! "${picotool}" load -x -v -u -f "${uf2}"; then
  echo "" >&2
  echo "書き込みに失敗した。" >&2
  echo "  ・デバイスが見つからない/固まっている場合は BOOTSEL を押しながら挿し直す" >&2
  echo "  ・出力が出ないときも、まず本当に書けたかを疑うこと" >&2
  exit 1
fi
