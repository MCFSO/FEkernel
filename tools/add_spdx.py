# SPDX-License-Identifier: 0BSD
"""给项目内所有源文件加上 SPDX 许可证标识。

0BSD 不要求保留版权声明，因此加 SPDX 头纯粹是工程规范（让工具链一眼看出许可证），
而不是法律义务。

用法：
    python tools/add_spdx.py          # 幂等：已有 SPDX 行的文件会跳过
    python tools/add_spdx.py --check  # 只检查不修改，缺标识的文件列出来

跳过哪些目录：见下面的 SKIP_DIRS（构建产物等）与 VENDORED_DIRS（第三方代码，
它们有自己的许可证，整目录豁免）。两者都只按"目录名精确相等"匹配。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPDX = "SPDX-License-Identifier: 0BSD"

# 按扩展名选择注释风格
COMMENT_STYLE = {
    ".c": "/* {spdx} */",
    ".h": "/* {spdx} */",
    ".ld": "/* {spdx} */",
    ".asm": "; {spdx}",
    ".py": "# {spdx}",
    ".conf": "# {spdx}",
    ".md": "<!-- {spdx} -->",
}

SKIP_DIRS = {"build", "toolchain", ".git"}

# 第三方 vendor 目录的豁免名单：目录级、逐个显式列出（按路径分段做精确相等比较，
# 不做"路径里带 third_party 就放过"这类模糊匹配，也不顺带放过别的目录）。
#
# 为什么这些目录不检查、也不加头（许可证理由）：
#   这些目录里放的是从外部引进的代码，它们各自保留**自己的**许可证标识和许可证文件
#   ——例如 libc++ 是 Apache-2.0 WITH LLVM-exception，Qt 5.15 是 LGPLv3/GPLv3/商业
#   三选一。它们**不使用**本项目的 0BSD。给别人的代码补一个 0BSD 头不是"整理格式"，
#   而是改写了别人的许可证信息，所以这里既不检查、也绝不自动加头。
#
# 新增第三方代码时怎么办：
#   放到 third_party/<名字>/ 下，并在 third_party/README.md 的登记表里登记一行
#   （名字/版本/许可证/来源/为什么需要它/是否被我们改过）——这种情况**不需要**动本文件。
#   只有当第三方代码万不得已必须落在 third_party/ 之外的顶层目录时，才往这个集合里
#   加一行那个目录名，并在这一行上写明它的许可证。
VENDORED_DIRS = {"third_party"}


def collect() -> list[Path]:
    """列出要参与 SPDX 管理的文件。

    这是唯一的入口：--check 与自动补头都只处理这里返回的文件，
    所以第三方目录在这一点被挡掉之后，补头模式不可能写到那里去。
    """
    out: list[Path] = []
    for path in ROOT.rglob("*"):
        if not path.is_file():
            continue
        if any(part in SKIP_DIRS for part in path.relative_to(ROOT).parts):
            continue
        # 第三方 vendor 目录：整目录豁免（既不检查也不加头），理由见 VENDORED_DIRS
        if any(part in VENDORED_DIRS for part in path.relative_to(ROOT).parts):
            continue
        if path.name == "LICENSE":
            continue
        if path.suffix in COMMENT_STYLE:
            out.append(path)
    return sorted(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="添加 SPDX 许可证标识")
    ap.add_argument("--check", action="store_true", help="只检查，不修改")
    args = ap.parse_args()

    missing: list[Path] = []
    changed = 0

    for path in collect():
        text = path.read_text(encoding="utf-8", errors="replace")
        if SPDX in text[:400]:            # 头部已有标识
            continue
        missing.append(path)
        if args.check:
            continue
        header = COMMENT_STYLE[path.suffix].format(spdx=SPDX)
        # 放在文件最前面；若首行是 shebang 则插到它后面
        lines = text.split("\n")
        if lines and lines[0].startswith("#!"):
            new = "\n".join([lines[0], header] + lines[1:])
        else:
            new = header + "\n" + text
        path.write_text(new, encoding="utf-8")
        changed += 1

    if args.check:
        if missing:
            print(f"缺少 SPDX 标识的文件 {len(missing)} 个：")
            for p in missing:
                print("  " + str(p.relative_to(ROOT)))
            return 1
        print("所有源文件都有 SPDX 标识")
        return 0

    print(f"已为 {changed} 个文件添加 SPDX 标识（0BSD）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
