"""A read copied into a recording's stream is reused only while no replay write of guest memory since overlapped it, and more
than the 64 writes the ring keeps count as one that did (gpu/vk/vk_memory.cpp: guest_cpu_write, replay_wrote_since).

python -I -B tests/test_copy_memo_ring.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cxx", "{cxx}"]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    mem = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
    ring = mem[mem.index("std::mutex replay_writes_mu;"):mem.index("uint64_t replay_write_now()")]
    fixture = f'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
struct Host {{ int gpu_write(uint64_t, const void*, uint64_t) {{ return 1; }} }};
Host h, *host = &h;
void guest_cpu_wrote(uint64_t, uint64_t) {{}}
{ring}
{block(mem, "uint64_t replay_write_now(")}
{block(mem, "bool replay_wrote_since(")}
{block(mem, "int guest_cpu_write(")}
int main() {{
    const uint32_t word = 1;
    const uint64_t gen = replay_write_now();
    CHECK(!replay_wrote_since(gen, 0x1000, 0x1100));
    guest_cpu_write(0x2000, &word, 4);                 // (elsewhere)
    CHECK(!replay_wrote_since(gen, 0x1000, 0x1100));
    guest_cpu_write(0x10fc, &word, 4);                 // (its last word)
    CHECK(replay_wrote_since(gen, 0x1000, 0x1100));
    CHECK(!replay_wrote_since(replay_write_now(), 0x1000, 0x1100));
    const uint64_t later = replay_write_now();
    for (int i = 0; i < 64; i++) guest_cpu_write(0x9000, &word, 4);
    CHECK(!replay_wrote_since(later, 0x1000, 0x1100)); // (64 writes elsewhere: all still in the ring)
    guest_cpu_write(0x9000, &word, 4);
    CHECK(replay_wrote_since(later, 0x1000, 0x1100));  // (65: the ring forgot some, so the copy is made again)
    std::printf("PASS: a copied read is reused only while no replay write since overlapped it\\n");
}}
'''
    with tempfile.TemporaryDirectory(prefix="copy-memo-ring-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
