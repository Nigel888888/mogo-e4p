#!/usr/bin/env python3
"""中文：以 ASan/UBSan 回放真实电量学习模块。
English: Replay the actual battery learner with ASan/UBSan.
"""
from pathlib import Path
import re
import subprocess
import tempfile

from run_reader_baseline import toolchain

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="battery-calibration-") as directory:
    folder = Path(directory)
    env, _ = toolchain(folder)
    source = (ROOT / "components/device_power/device_power_metalio.c").read_text()
    curve = re.search(r"^static const int32_t s_soc_curve\[.*?\};", source, re.M | re.S).group(0)
    (folder / "baseline.h").write_text("#include <stdint.h>\n" + curve + "\n")
    binary = folder / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-Icomponents/device_power", "-I" + str(folder),
                    "tools/battery_calibration_host_test.c", "components/device_power/battery_calibration.c",
                    "-o", str(binary)], cwd=ROOT, env=env, check=True)
    subprocess.run([str(binary)], cwd=ROOT, env=env, check=True)
