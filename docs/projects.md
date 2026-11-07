# プロジェクト形式 (pyproject.toml と src/)

`pyproject.toml` のあるディレクトリを、インストールされたプロジェクトとして実行する。

## 使い方

```bash
# [project.scripts] が 1 つなら、それを実行する
./build/bin/lyc jit path/to/proj [args...]

# 複数あるなら名前で選ぶ
./build/bin/lyc jit --script mylib-admin path/to/proj

# python -m と同じ。src/mylib/__main__.py を __main__ として実行する
./build/bin/lyc jit -m mylib path/to/proj
./build/bin/lyc jit -m mylib.tool path/to/proj

# AOT も同じ
./build/bin/lyc path/to/proj -o mylib
./build/bin/lyc -m mylib path/to/proj -o mylib
```

- `-m` と `--script` は入力の**前**に書く。`lyc jit` は入力より後ろをすべて
  プログラムの引数として渡す (`lyc jit file.py args...` と同じ)。
- `-m` と `--script` は同時に使えない。入力がディレクトリでなければ使えない。

## 何をプロジェクトとみなすか

- **入力に渡したディレクトリだけ。** そこに `pyproject.toml` が必要である。
  ない場合は `'<dir>' is a directory without a pyproject.toml` で止まる。
- ⛔ **メインファイルから親を辿って `pyproject.toml` を探すことはしない。**
  pytest の rootdir のような上方探索は検討したが採らなかった。
  - 例: このリポジトリ自体に `pyproject.toml` と `src/` がある。上方探索すると
    `examples/hello.py` まで `src/` を import の起点に持つことになる。
  - 置き場所だけで import の結果が変わるのは、プログラムを読んでも分からない。
- `--project <dir>` のような別フラグも検討したが、入力にディレクトリを渡す形にした。
  `python -m` 相当と console script 相当の両方を、入力 1 つで表せるからである。

## import の起点 (import root)

- `src/` があれば `src/` (src レイアウト)。なければ `pyproject.toml` のある
  ディレクトリ (フラットレイアウト)。
- インストールしたときに sys.path に入るディレクトリに当たる。
- ⛔ `[tool.setuptools] package-dir` や hatch の設定などは読まない。ビルドバックエンド
  ごとに書き方が違い、どれか 1 つに合わせると他で黙って違う結果になるからである。

## pyproject.toml から読むもの

- `[project] name` (文字列)。
- `[project.scripts]` と `[project.gui-scripts]`。値は entry point の
  object reference (`module:object.attr`)。
  - extras (`module:func [extra]`) は受け付けて無視する。仕様がそう認めている。
  - `module` だけで呼ぶ対象がないものは拒否する。console script は呼び出す。
  - 両方の表に同じ名前があれば拒否する。
- それ以外の表 (`[build-system]`、`[tool.*]` など) は読み飛ばす。
- ファイルは TOML 1.0.0 として全体を検証する。仕様に反する箇所があれば、その行と列
  (文字数で数える) を出して止まる。途中まで読んで使うことはしない。
- `dynamic = ["scripts"]` (ビルド時に決まる scripts) には対応しない。scripts が
  見えないので、`-m` で実行する。

## script の実行

インストーラが書く console script と同じものを `__main__` として実行する。

```python
import sys
from mylib.cli import main
sys.exit(main())
```

- `object.attr` は `from mylib.cli import App` のあと `App.main()` を呼ぶ。
- 終了コードは戻り値で決まる。`None` なら 0、int ならその値である。
- `sys.argv[0]` は `<project>/<script 名>` になる。インストールした script は
  環境の bin に名前で置かれるので、それに合わせた。
- traceback のファイル名は `<project>/<script 名>-script.py` になる
  (setuptools が Windows の launcher の隣に置くファイルの名前)。
  - ⛔ `<script 名>` のままにしない。traceback のフレームは `.py` で終わる
    ファイル名でランタイムのものと区別しているので、`<module>` のフレームが消えた。
  - ⛔ `<script 名>.py` にもしない。フラットレイアウトでは同じ名前の本物の
    モジュールがありえ、それが `__main__` と取り違えられる。`-` を含む名前は
    import できないので衝突しない。
  - このファイルは実在しないので、フレームにソースの行は表示されない。

## -m の実行

`python -m` と同じ規則で探す。

- `pkg` がパッケージ (`__init__.py` がある) なら `pkg/__main__.py` を実行する。
  `__main__.py` がなければ `'pkg' is a package and cannot be directly executed`。
- そうでなければ `pkg.py` を実行する。
- `pkg.mod` なら、途中の `pkg` はすべてパッケージである必要がある。
- `__main__` の属するパッケージ (`-m pkg` と `-m pkg.mod` では `pkg`) は、
  `__main__` より先に import される。相対 import (`from .cli import main`) は
  このパッケージから解決する。
- `sys.argv[0]` は実行するファイルのパスである (CPython と同じ)。
- パスはすべて絶対パスに正規化する。`__main__` とモジュールをパスで見分けるので、
  `.` や `./proj` と書いたときに結果が変わらないようにするためである。
- ⛔ パッケージの `__init__.py` が `-m` で実行するモジュール自身を import する形
  (`-m pkg.mod` で `pkg/__init__.py` が `from .mod import X`) は拒否される。
  CPython はそのモジュールを 2 回 (`pkg.mod` と `__main__` として) 実行する。
- モジュール名とオブジェクト名にキーワード (`class` など) は使えない。
  ソフトキーワード (`match`、`type` など) は使える。

## 絶対 import は常に import root から探す

これはプロジェクト形式に限らない変更である。

- 以前は、取り込まれたモジュールの絶対 import を、そのモジュール自身の
  ディレクトリから探していた。
  - `pkg/a.py` の `import pkg.b` は `pkg/pkg/b.py` を探して失敗した。
    パッケージが自分のモジュールを完全な名前で import できなかった。
  - `pkg/a.py` の `import b` は `pkg/b.py` を見つけていた。Python 2 の暗黙の
    相対 import で、CPython 3 では ModuleNotFoundError になる。
- いまは CPython の sys.path と同じく、起点はプログラム全体で 1 つである。
  - ファイルを実行したときは、そのファイルのディレクトリ。
  - プロジェクトを実行したときは、import root。
- 相対 import (`from . import x`) は、これまでどおり import するモジュールの
  ディレクトリから探す。

## CPython との違い

- **import root は標準ライブラリより先に探す。** インストールしたプロジェクトは
  site-packages にあり、標準ライブラリより後ろである。ここでは sys.path[0] の位置に
  ある。標準ライブラリと同じ名前のモジュールを持つプロジェクトでは結果が違う。
- **`-m` の sys.path[0] はカレントディレクトリではなく import root である。**
  コンパイルの結果がカレントディレクトリで変わらないようにするためである。
- 名前空間パッケージ (`__init__.py` のないパッケージ) には対応しない。
  これは import 全体の既存の制約である。
- シンボリックリンクで置いたパッケージは見つからない。大文字小文字の検査が
  リンク先の名前と比べるためで、これも import 全体の既存の制約である。
- 取り込まれたモジュールの本体は、定義と大域変数の初期化以外を実行しない
  (既存の制約)。`__init__.py` に `print(...)` があるとコンパイル時に拒否される。

## TOML の読み方

`src/lython/common/TOML.{h,cpp}` は pyproject.toml 専用ではない TOML 1.0.0 の
読み取り器である。

- 整数は 64 ビット符号付きで表せないものを拒否する (仕様の要求)。CPython の
  tomllib は Python の int に幅がないので受け付ける。
- 浮動小数点数は IEEE 754 binary64 として丸める。`1e400` は無限大になる
  (tomllib と同じ)。
- 配列とインライン表の入れ子は 256 段までにしている (`Table::parse` の引数で
  変えられる)。仕様に上限はないが、上限がないと悪意のあるファイルでスタックが
  尽きる。
- 日時は検証したうえで、書かれた綴りのまま持つ。計算に使う箇所がないからである。
  秒の 60 (うるう秒) は RFC 3339 に従って受け付ける。tomllib は datetime に
  入らないので拒否する。
- テストの期待値は仕様から取り、tomllib にも同じ文書を与えて突き合わせてある
  (`tests/unit/ProjectTests.cpp`)。
