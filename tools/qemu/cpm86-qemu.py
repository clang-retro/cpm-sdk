#!/usr/bin/env python3
"""Run CP/M-86 programs on Digital Research's CP/M-86 1.1, in QEMU.

    tools/qemu/cpm86-qemu.py [options] FILE... -- COMMAND...

Makes a 160 KiB IBM PC CP/M-86 boot floppy with CPM.SYS and the FILEs on
it, boots it in QEMU (qemu-system-i386) without a display, types each
COMMAND at the A> prompt and waits for the next prompt, then prints the
screen. The screen is read from the PC's text-mode memory, so this needs
nothing but QEMU, Python and cpmtools (cpmcp).

CP/M-86 itself is not part of this SDK. The script downloads CPM.SYS from
CP/M-86 1.1 for the IBM PC on the Unofficial CP/M Web Site, which may
distribute it (cpm.retropc.se), and a formatted boot floppy with the PC
boot loader from tsupplis/cpm86-kernel, into a cache directory
(--cache, default ~/.cache/cpm86-qemu). --cpm-sys and --boot-image take
local copies instead.

SeaBIOS boots only floppies with the 55AAh signature, which CP/M-86's
boot sector lacks, so QEMU boots a hard disk whose boot sector
(chainboot.S) loads the floppy's.

Options:
    --png FILE      also save a screenshot (needs Pillow)
    --timeout SEC   longest wait for each prompt (default 60)
    --out DIR       copy every file on the floppy back to DIR afterwards
"""

import argparse
import hashlib
import io
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))

CPM86_ZIP = ("https://cpm.retropc.se/cpm/download/cpm86-11.zip",
             "683b1d06a7f2dae2934434740d8773b27ff5e2bcbe3ffe071013bcd847353622")
BOOT_IMG = ("https://raw.githubusercontent.com/tsupplis/cpm86-kernel/"
            "9670f56e213171d57295aafc03804f522cb49aad/base-160.img",
            "85e033d684e5ef11dc786c28346291b5d36474ed5b8db836ded01aeb2b6592b2")

# cpmtools' definition of the IBM PC CP/M-86 single-sided 160 KiB format.
DISKDEFS = """diskdef ibmpc-514ss
  seclen 512
  tracks 40
  sectrk 8
  blocksize 1024
  maxdir 64
  skew 1
  boottrk 1
  os 2.2
end
"""
FORMAT = "ibmpc-514ss"


def fetch(url, sha256, cache):
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, os.path.basename(url))
    if not os.path.exists(path):
        with urllib.request.urlopen(url) as r:
            data = r.read()
        if hashlib.sha256(data).hexdigest() != sha256:
            sys.exit(f"cpm86-qemu: {url}: unexpected contents")
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.replace(path + ".tmp", path)
    return path


def chainboot(work):
    """The hard disk with chainboot.S as its boot sector."""
    obj = os.path.join(work, "chainboot.o")
    binf = os.path.join(work, "chainboot.bin")
    clang = os.environ.get("HOST_CLANG", "clang")
    lld = os.environ.get("HOST_LLD", "ld.lld")
    subprocess.run([clang, "--target=i386-unknown-elf", "-m16", "-c",
                    os.path.join(HERE, "chainboot.S"), "-o", obj], check=True)
    subprocess.run([lld, "-Ttext=0x600", "--image-base=0", "--oformat=binary", obj, "-o", binf],
                   check=True)
    with open(binf, "rb") as f:
        mbr = f.read()
    if len(mbr) != 512:
        sys.exit("cpm86-qemu: chainboot.bin is not 512 bytes")
    hd = os.path.join(work, "hd.img")
    with open(hd, "wb") as f:
        f.write(mbr + bytes(512 * 2047))
    return hd


class Qemu:
    def __init__(self, work, floppy, hd):
        self.sock_path = os.path.join(work, "monitor.sock")
        self.screen_path = os.path.join(work, "screen.bin")
        self.proc = subprocess.Popen(
            ["qemu-system-i386", "-machine", "pc", "-accel", "tcg", "-m", "1",
             "-drive", f"if=floppy,index=0,format=raw,file={floppy}",
             "-drive", f"if=ide,index=0,format=raw,file={hd}",
             "-boot", "c", "-display", "none", "-serial", "null",
             "-monitor", f"unix:{self.sock_path},server,nowait"],
            stdout=subprocess.DEVNULL)
        for _ in range(100):
            if os.path.exists(self.sock_path):
                break
            time.sleep(0.1)
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(self.sock_path)
        self.sock.settimeout(0.05)

    def command(self, cmd):
        self.sock.sendall((cmd + "\n").encode())
        time.sleep(0.03)
        try:
            while self.sock.recv(65536):
                pass
        except (socket.timeout, BlockingIOError):
            pass

    def screen(self):
        """The 80x25 text screen, one string per line."""
        if os.path.exists(self.screen_path):
            os.remove(self.screen_path)
        self.command(f'pmemsave 0xb8000 4000 "{self.screen_path}"')
        for _ in range(50):
            if os.path.exists(self.screen_path) and \
                    os.path.getsize(self.screen_path) == 4000:
                break
            time.sleep(0.05)
        with open(self.screen_path, "rb") as f:
            data = f.read()
        text = bytes(c if 32 <= c < 127 else 32 for c in data[0::2])
        return [text[i:i + 80].decode().rstrip() for i in range(0, 2000, 80)]

    def cursor_line(self):
        """The last line with text: where the prompt is."""
        lines = self.screen()
        for i in range(24, -1, -1):
            if lines[i].strip():
                return i, lines[i]
        return 0, ""

    KEYS = {" ": "spc", ".": "dot", ":": "shift-semicolon", "-": "minus",
            "/": "slash", "\\": "backslash", "=": "equal", ",": "comma",
            ";": "semicolon", "'": "apostrophe", '"': "shift-apostrophe",
            "*": "shift-8", "[": "bracket_left", "]": "bracket_right",
            "<": "shift-comma", ">": "shift-dot", "_": "shift-minus",
            "+": "shift-equal", "!": "shift-1", "?": "shift-slash",
            "$": "shift-4", "\n": "ret"}

    def type(self, text):
        for ch in text:
            if ch.isalpha() and ch.isupper():
                key = "shift-" + ch.lower()
            else:
                key = self.KEYS.get(ch, ch)
            self.command("sendkey " + key)

    def wait_prompt(self, timeout, after=None):
        """Wait until the screen ends with an A> prompt and stops changing."""
        end = time.time() + timeout
        last = None
        while time.time() < end:
            # The status line at the bottom has a running clock.
            lines = self.screen()[:24]
            filled = [l for l in lines if l.strip()]
            tail = filled[-1] if filled else ""
            if tail.rstrip() == "A>" and lines != after and lines == last:
                return lines
            last = lines
            time.sleep(0.5)
        sys.exit("cpm86-qemu: timed out waiting for the A> prompt; the screen:\n"
                 + "\n".join(self.screen()))

    def screenshot(self, png):
        ppm = png + ".ppm"
        self.command(f"screendump {ppm}")
        for _ in range(50):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
                break
            time.sleep(0.1)
        time.sleep(0.2)
        from PIL import Image
        Image.open(ppm).save(png)
        os.remove(ppm)

    def quit(self):
        try:
            self.command("quit")
        except OSError:
            pass
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def main():
    ap = argparse.ArgumentParser(add_help=True, usage=__doc__)
    ap.add_argument("files", nargs="*")
    ap.add_argument("--cache", default=os.path.expanduser("~/.cache/cpm86-qemu"))
    ap.add_argument("--cpm-sys")
    ap.add_argument("--boot-image")
    ap.add_argument("--png")
    ap.add_argument("--out")
    ap.add_argument("--timeout", type=float, default=60)
    argv = sys.argv[1:]
    commands = []
    if "--" in argv:
        i = argv.index("--")
        argv, commands = argv[:i], argv[i + 1:]
    args = ap.parse_args(argv)

    work = tempfile.mkdtemp(prefix="cpm86-qemu-")
    try:
        cpm_sys = args.cpm_sys
        if not cpm_sys:
            z = zipfile.ZipFile(fetch(*CPM86_ZIP, args.cache))
            cpm_sys = os.path.join(work, "CPM.SYS")
            with open(cpm_sys, "wb") as f:
                f.write(z.read("CPM.SYS"))
        boot = args.boot_image or fetch(*BOOT_IMG, args.cache)
        floppy = os.path.join(work, "a.img")
        shutil.copy(boot, floppy)
        with open(os.path.join(work, "diskdefs"), "w") as f:
            f.write(DISKDEFS)

        def cpmcp(*a):
            # cpmtools reads diskdefs from the current directory first.
            subprocess.run(["cpmcp", "-f", FORMAT] + list(a), check=True,
                           cwd=work)

        cpmcp(floppy, os.path.abspath(cpm_sys), "0:CPM.SYS")
        for name in args.files:
            cpmcp(floppy, os.path.abspath(name),
                  "0:" + os.path.basename(name).upper())

        vm = Qemu(work, floppy, chainboot(work))
        try:
            lines = vm.wait_prompt(args.timeout)
            for cmd in commands:
                vm.type(cmd + "\n")
                lines = vm.wait_prompt(args.timeout, after=lines)
            print("\n".join(l for l in lines))
            if args.png:
                vm.screenshot(os.path.abspath(args.png))
        finally:
            vm.quit()
        if args.out:
            os.makedirs(args.out, exist_ok=True)
            out = subprocess.run(["cpmls", "-f", FORMAT, floppy], cwd=work,
                                 capture_output=True, text=True, check=True)
            for name in out.stdout.split():
                if name.endswith(":"):
                    continue
                cpmcp(floppy, "0:" + name, os.path.join(os.path.abspath(args.out), name))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
