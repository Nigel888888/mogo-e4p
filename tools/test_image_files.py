#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real streaming JPEG/PNG tests. Requires Pillow and libpng (pkg-config or --png-prefix)."""
import argparse
from pathlib import Path
import random
import shlex
import shutil
import subprocess
import sys
import tempfile
from PIL import Image
from run_reader_baseline import toolchain

ROOT = Path(__file__).resolve().parents[1]

def main():
    args = argparse.ArgumentParser()
    args.add_argument("--png-prefix", type=Path)
    options = args.parse_args()
    if not options.png_prefix and not shutil.which("pkg-config"):
        for prefix in (Path("/opt/homebrew/opt/libpng"), Path("/usr/local/opt/libpng")):
            if (prefix / "include/libpng16/png.h").is_file():
                options.png_prefix = prefix
                break
    if options.png_prefix:
        png_cflags = ["-I" + str(options.png_prefix / "include/libpng16")]
        png_link = ["-L" + str(options.png_prefix / "lib"), "-lpng16", "-lz",
                    "-Wl,-rpath," + str(options.png_prefix / "lib")]
    else:
        if not shutil.which("pkg-config"):
            args.error("libpng requires pkg-config or --png-prefix <installation prefix>")
        png_cflags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "libpng"], text=True))
        png_link = shlex.split(subprocess.check_output(["pkg-config", "--libs", "libpng"], text=True))
    with tempfile.TemporaryDirectory(prefix="pico-image-stream-") as temp:
        work = Path(temp)
        env, cc = toolchain(work)
        cxx = shlex.split(env.get("CXX", str(Path(cc[0]).with_name("clang++")) if sys.platform == "darwin" else "c++"))
        (work / "sdkconfig.h").write_text("\n".join([
            "#define CONFIG_JD_SZBUF 512", "#define CONFIG_JD_FORMAT 0", "#define CONFIG_JD_USE_SCALE 1",
            "#define CONFIG_JD_TBLCLIP 1", "#define CONFIG_JD_FASTDECODE 0"]) + "\n")
        includes = ["tools/book_epub_stubs", "main/book", "main/image", "components/jpegdec/src",
                    "managed_components/espressif__esp_jpeg/tjpgd", "managed_components/espressif__esp_jpeg/include"]
        flags = ["-g", "-D__LINUX__", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-ffunction-sections", "-fdata-sections"]
        flags += ["-I" + str(ROOT / path) for path in includes] + ["-I" + str(work)] + png_cflags
        sources = ["tools/book_image_file_host_test.c", "main/image/image_decode.c", "main/image/book_image_header.c",
                   "managed_components/espressif__esp_jpeg/tjpgd/tjpgd.c",
                   "components/jpegdec/src/JPEGDEC.cpp", "components/jpegdec/src/jpegdec_shim.cpp"]
        objects = []
        for index, source in enumerate(sources):
            obj = work / f"part{index}.o"
            cpp = source.endswith(".cpp")
            # Upstream JPEGDEC intentionally uses unaligned loads and signed shifts;
            # retain ASan there, and full UB checks in the new file/stream wrappers.
            upstream_flags = ["-fno-sanitize=alignment,shift"] if source.endswith("JPEGDEC.cpp") else []
            subprocess.run([*(cxx if cpp else cc), "-std=c++17" if cpp else "-std=c11", *flags, *upstream_flags,
                            "-c", str(ROOT / source), "-o", str(obj)], check=True,env=env)
            objects.append(str(obj))
        gc = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
        exe = work / "test"
        subprocess.run([*cxx, *flags, gc, *objects, *png_link, "-o", str(exe)], check=True,env=env)
        image = Image.frombytes("RGB", (1536, 1536), random.Random(82).randbytes(1536 * 1536 * 3))
        image.save(work / "large.jpg", quality=100, subsampling=0)
        image.save(work / "large.png")
        image.save(work / "progressive.jpg", quality=95, progressive=True)
        Image.new("RGB", (829, 731), (160, 160, 160)).save(work / "odd.jpg")
        Image.new("RGB", (53, 41), (160, 160, 160)).save(work / "small.jpg", progressive=True)
        files = [work / name for name in ("large.jpg", "large.png", "progressive.jpg", "odd.jpg", "small.jpg")]
        assert all(path.stat().st_size > 2 * 1024 * 1024 for path in files[:3])
        subprocess.run([str(exe), "read", *map(str, files)], check=True,env=env)
        # 同尺寸快速路径逐字节核对灰度、预乘与alpha；缩放灰度与等价RGB对照。
        # Check native gray, premultiplication and alpha byte-for-byte; compare scaled gray against equivalent RGB.
        native=[]
        size=(176,240)
        rng=random.Random(184)
        gray=Image.frombytes("L",size,rng.randbytes(size[0]*size[1]))
        rgba=Image.frombytes("RGBA",size,rng.randbytes(size[0]*size[1]*4))
        palette=gray.convert("P")
        palette.putpalette(rng.randbytes(768))
        cases=[("gray",gray,{}),("gray-transparent",gray,{"transparency":127}),
               ("rgb",rgba.convert("RGB"),{}),("rgba",rgba,{}),("gray-alpha",rgba.convert("LA"),{}),
               ("palette",palette,{"transparency":bytes(range(256))}),("mono",gray.convert("1"),{}),
               ("crop",Image.frombytes("RGB",(176,250),rng.randbytes(176*250*3)),{})]
        raw16=rng.randbytes(size[0]*size[1]*2)
        cases.append(("gray16",Image.frombytes("I;16",size,raw16),{}))
        for name,picture,options in cases:
            path=work/(name+".png")
            picture.save(path,**options)
            if name=="gray16":
                decoded=Image.frombytes("L",size,raw16[1::2]).convert("RGBA")
            else:
                decoded=Image.open(path).convert("RGBA")
                if name=="crop":
                    decoded=decoded.crop((0,5,176,245))
            white,premult,alpha=bytearray(),bytearray(),bytearray()
            for r,g,b,a in decoded.getdata():
                luminance=(r*77+g*150+b*29)>>8
                white.append((luminance*a+255*(255-a)+127)//255)
                premult.append((luminance*a+127)//255)
                alpha.append(a)
            Path(str(path)+".raw").write_bytes(white+premult+alpha)
            native.append(path)
        subprocess.run([str(exe),"pixels",*map(str,native)],check=True,env=env)
        scaled=work/"scaled-gray.png"
        large_gray=Image.frombytes("L",(823,1171),rng.randbytes(823*1171))
        large_gray.save(scaled)
        large_gray.convert("RGB").save(str(scaled)+"-rgb.png")
        subprocess.run([str(exe),"compare",str(scaled)],check=True,env=env)
        (work / "bad.jpg").write_bytes(b"not an image")
        (work / "bad.png").write_bytes((work / "large.png").read_bytes()[:400])
        with (work / "overlimit.jpg").open("wb") as file:
            file.write((work / "odd.jpg").read_bytes())
            file.truncate(50 * 1024 * 1024 + 1)
        subprocess.run([str(exe), "reject", *map(str, [work / "bad.jpg", work / "bad.png", work / "overlimit.jpg"])], check=True,env=env)
        print("PASS: streaming baseline/progressive JPEG and PNG over 2 MiB; native gray/RGB/RGBA/gray-alpha/palette/mono/16-bit pixels and alpha; scaled grayscale parity; cancellation; corruption/limit bounds")

if __name__ == "__main__":
    main()
