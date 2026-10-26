#!/usr/bin/env python3
"""Check actual word-pack append accounting with controlled I/O costs, not HIL."""
from pathlib import Path
import subprocess
import tempfile
from test_audio_runtime import function


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / 'main/word_pack.cpp').read_text()
    types = source[source.index('enum class WordPackStreamOperation'):source.index('esp_err_t WordPackStreamTransaction(')]
    body = '\n'.join(function(source, name) for name in (
        'esp_err_t WordPackStreamTransaction(', 'esp_err_t AppendWordPackStream(',
        'void LogWordPackStreamProfile('))
    fixture = (root / 'scripts/testdata/pack_stream_profile.cpp').read_text()
    fixture = fixture.replace('@@TYPES@@', types).replace('@@PRODUCTION@@', body)
    with tempfile.TemporaryDirectory(prefix='wqn-pack-profile-') as directory:
        temporary = Path(directory)
        (temporary / 'test.cpp').write_text(fixture)
        binary = temporary / 'test'
        subprocess.run(['g++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                        '-I', str(root / 'main'), str(temporary / 'test.cpp'),
                        '-Wl,--wrap=fwrite', '-o', str(binary)], check=True, timeout=45)
        return subprocess.run([str(binary), str(temporary)], timeout=15).returncode


if __name__ == '__main__':
    raise SystemExit(main())
