#!/usr/bin/env python3
"""运行离线 CloudZao 报文、下载、文件提交和确认回归。
Run offline CloudZao framing, download, publication and acknowledgement regressions.
"""
import os
from pathlib import Path
import platform
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get("IDF_PATH", Path.home() / "esp/esp-idf"))
PARSER = IDF / "components/http_parser"
JSON = ROOT / "managed_components/espressif__cjson/cJSON"

with tempfile.TemporaryDirectory(prefix="mogo-cloudzao-") as tmp:
    binary = Path(tmp) / "test"
    # 使用仓库 runner 的 ASan/UBSan 工具链。/ Use the repository runner's ASan/UBSan toolchain.
    subprocess.run([
        "clang", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-g", "-O1",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-Wno-deprecated-declarations", "-Imain/services", "-Itools/cloudzao_stubs",
        f"-I{PARSER}", f"-I{JSON}", str(PARSER / "http_parser.c"), str(JSON / "cJSON.c"),
        "tools/cloudzao_host_test.c", "main/services/cloudzao_wire.c", "main/services/cloudzao_receive.c",
        "main/services/cloudzao_history.c", "main/services/cloudzao_partner.c",
        *([] if platform.system() == "Darwin" else ["-lcrypto"]), "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary), tmp], check=True)
