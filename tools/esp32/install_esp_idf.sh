#!/usr/bin/env bash
set -euo pipefail

# Pin the latest stable ESP-IDF release verified on 2026-09-28. v6.1.1 is a
# signed bugfix release on the current v6.1 stable line (not a moving branch).
readonly IDF_VERSION="v6.1.1"
readonly IDF_DIR="${HOME}/esp/esp-idf"
readonly TARGETS="esp32s3,esp32c6"
readonly BREW_PACKAGES=(cmake ninja dfu-util ccache python libgcrypt glib pixman sdl2 libslirp)

printf '%s\n' \
  "ESP-IDF ${IDF_VERSION} を ${IDF_DIR} に用意します。" \
  "Homebrew の前提パッケージを確認し、不足分をインストールします。" \
  "続いて ESP32-S3/C6 用ツールチェーンと Python 環境を設定します。" \
  "ダウンロードはツールチェーンを含む数 GB 規模になる見込みです。"

if [[ "$(uname -s)" != "Darwin" || "$(uname -m)" != "arm64" ]]; then
  printf '%s\n' "このスクリプトの対象は Apple Silicon 搭載 macOS です。" >&2
  exit 1
fi

if ! command -v brew >/dev/null 2>&1; then
  printf '%s\n' "Homebrew がありません。https://brew.sh/ の手順で導入してから再実行してください。" >&2
  exit 1
fi
if ! command -v git >/dev/null 2>&1; then
  printf '%s\n' "git がありません。Xcode Command Line Tools (xcode-select --install) を導入してください。" >&2
  exit 1
fi

missing=()
for package in "${BREW_PACKAGES[@]}"; do
  if ! brew list --formula "$package" >/dev/null 2>&1; then
    missing+=("$package")
  fi
done
if ((${#missing[@]})); then
  printf '不足している Homebrew パッケージ: %s\n' "${missing[*]}"
  read -r -p "Homebrew で不足分をインストールしますか? [y/N] " answer
  case "$answer" in
    y|Y|yes|YES) brew install "${missing[@]}" ;;
    *) printf '%s\n' "前提パッケージを入れてから再実行してください。" >&2; exit 1 ;;
  esac
fi

python_cmd="$(brew --prefix python)/bin/python3"
if [[ ! -x "$python_cmd" ]]; then
  python_cmd="$(command -v python3 || true)"
fi
if [[ -z "$python_cmd" ]]; then
  printf '%s\n' "Python 3 が見つかりません。Homebrew の python を確認してください。" >&2
  exit 1
fi
python_version_ok="$("$python_cmd" -c 'import sys; print(int(sys.version_info >= (3, 10)))')"
python_version="$("$python_cmd" -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
if [[ "$python_version_ok" != "1" ]]; then
  printf 'ESP-IDF 6.x には Python 3.10 以上が必要です (検出: %s)。\n' "$python_version" >&2
  exit 1
fi

mkdir -p "${HOME}/esp"
if [[ -e "$IDF_DIR" ]]; then
  if [[ ! -d "$IDF_DIR/.git" ]]; then
    printf '%s\n' "$IDF_DIR は存在しますが Git checkout ではありません。内容を保持するため停止します。" >&2
    exit 1
  fi
  actual="$(git -C "$IDF_DIR" describe --tags --exact-match HEAD 2>/dev/null || true)"
  if [[ "$actual" != "$IDF_VERSION" ]]; then
    printf '既存 ESP-IDF は %s です。自動 checkout はせず停止します。必要なら退避後に %s を再実行してください。\n' \
      "${actual:-tag不明}" "$0" >&2
    exit 1
  fi
  printf '既存の ESP-IDF %s を再利用します。\n' "$IDF_VERSION"
else
  git clone --recursive --branch "$IDF_VERSION" --depth 1 \
    https://github.com/espressif/esp-idf.git "$IDF_DIR"
fi

cd "$IDF_DIR"
./install.sh "$TARGETS"

cat <<EOF

インストールが完了しました。
新しいシェルごとに次を実行してください:
  . "$IDF_DIR/export.sh"

確認:
  idf.py --version
  # 最小プロジェクトのディレクトリへ移動してから:
  idf.py set-target esp32c6
  idf.py build
  idf.py fullclean
  idf.py set-target esp32s3
  idf.py build

このスクリプトはツールを導入するだけです。書込み (flash) や monitor は行いません。
EOF
