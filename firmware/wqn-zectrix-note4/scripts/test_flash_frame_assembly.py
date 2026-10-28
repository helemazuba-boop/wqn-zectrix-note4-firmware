#!/usr/bin/env python3
"""Compile the real bounded assembler and exercise transport/message boundaries."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include "flash_frame_assembly.h"
#include <cassert>
#include <string>
using A = wqn::FlashFrameAssembly;
using R = A::Result;
int main() {
    A a;
    assert(a.Feed(1, true, 0, 5, "he", 2) == R::kPending);
    assert(a.Feed(1, true, 2, 5, "llo", 3) == R::kComplete);
    assert(a.bytes() == "hello" && a.opcode() == 1);
    assert(a.Feed(2, false, 0, 2, "ab", 2) == R::kPending);
    assert(a.Feed(0, true, 0, 3, "cd", 2) == R::kPending);
    assert(a.Feed(0, true, 2, 3, "e", 1) == R::kComplete);
    assert(a.bytes() == "abcde" && a.opcode() == 2);
    assert(a.Feed(0, true, 0, 1, "x", 1) == R::kInvalid);
    assert(a.Feed(1, true, 0, 131073, "x", 1) == R::kInvalid);
    assert(a.Feed(1, true, 1, 131073, "x", 1) == R::kInvalid);
    assert(a.bytes().empty()); // later callbacks cannot bypass the cap
    assert(a.Feed(2, true, 0, 65537, "x", 1) == R::kInvalid);
    assert(a.Feed(1, true, 0, 3, "a", 1) == R::kPending);
    assert(a.Feed(1, true, 2, 3, "b", 1) == R::kInvalid); // gap
    assert(a.Feed(1, true, 0, 3, "a", 1) == R::kPending);
    assert(a.Feed(1, true, 1, 2, "b", 1) == R::kInvalid); // changed length
    assert(a.Feed(1, true, 0, 3, "a", 1) == R::kPending);
    assert(a.Feed(1, true, 0, 3, "abc", 3) == R::kInvalid); // overlap/restart
    assert(a.Feed(1, true, 0, 0, nullptr, 0) == R::kComplete);
    const std::string cap(65536, 'x');
    assert(a.Feed(2, false, 0, cap.size(), cap.data(), cap.size()) == R::kPending);
    assert(a.Feed(0, true, 0, 1, "y", 1) == R::kInvalid); // aggregate cap
    assert(a.Feed(1, true, 0, 1, nullptr, 1) == R::kInvalid);
    assert(a.Feed(1, false, 0, 1, "x", 1) == R::kPending);
    assert(a.Feed(2, true, 0, 1, "y", 1) == R::kInvalid); // interleaved messages
    assert(a.Feed(1, true, 0, 1, "z", 1) == R::kComplete); // recovery
}
'''

with tempfile.TemporaryDirectory(prefix="flash-assembly-") as directory:
    source = Path(directory) / "probe.cpp"
    binary = Path(directory) / "probe"
    source.write_text(SOURCE)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I", str(ROOT / "main"), str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Flash frame boundaries: PASS")
