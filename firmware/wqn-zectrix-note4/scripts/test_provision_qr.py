#!/usr/bin/env python3
"""Test the production QR encoder; optionally decode native UI framebuffer output.

The default test needs GCC/G++. --verify-frames needs Pillow and zxing-cpp and
the --render-dir output of test_ui_settings.py. No radio/panel/phone HIL.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def verify_frames(folder, decoder_path):
    if decoder_path:
        sys.path.insert(0, str(decoder_path))
    import zxingcpp
    from PIL import Image

    expected = {
        'provision': 'WIFI:T:nopass;S:WQN_N4_LIVE;;',
        'provision-escaped': 'WIFI:T:nopass;S:Note\\;4\\,Lab\\:\\\\\\\"\\\';;',
        'provision-unicode': 'WIFI:T:nopass;S:设备热点-Note4;;',
        'provision-max': 'WIFI:T:nopass;S:' + '\\;' * 32 + ';;',
    }
    count = 0
    for dev in (0, 1):
        for name, payload in expected.items():
            path = folder / f'dev-{dev}' / (name + '.pbm')
            frame = Image.open(path).convert('L')
            decoded = zxingcpp.read_barcodes(frame)
            assert len(decoded) == 2, (path, len(decoded))
            assert {code.text for code in decoded} == {payload, 'http://192.168.4.1/'}, (
                path, [code.text for code in decoded], payload)
            # Both symbols must stay inside their allocated native QR slots.
            for code in decoded:
                position = code.position
                xs = [position.top_left.x, position.top_right.x,
                      position.bottom_left.x, position.bottom_right.x]
                ys = [position.top_left.y, position.top_right.y,
                      position.bottom_left.y, position.bottom_right.y]
                assert min(xs) >= 24 and max(xs) <= 376 and min(ys) >= 127 and max(ys) <= 250
            count += len(decoded)
    print(f'Native framebuffer: {count} QR symbols independently decoded with zxing-cpp PASS')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify-frames', type=Path)
    parser.add_argument('--decoder-path', type=Path, help='Optional isolated zxing-cpp installation')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    include = root / 'components/qr_code/include'
    with tempfile.TemporaryDirectory(prefix='note4-provision-qr-') as temp:
        obj = Path(temp) / 'qr.o'
        binary = Path(temp) / 'qr-test'
        subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-I', str(include),
                        '-c', str(root / 'components/qr_code/qrcodegen.c'), '-o', str(obj)],
                       check=True, timeout=30)
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(include),
                        '-I', str(root / 'main'), str(root / 'main/ui/provision_qr.cpp'),
                        str(root / 'scripts/testdata/provision_qr.cpp'), str(obj), '-o', str(binary)],
                       check=True, timeout=30)
        subprocess.run([str(binary)], check=True, timeout=10)
    if args.verify_frames:
        verify_frames(args.verify_frames, args.decoder_path)


if __name__ == '__main__':
    main()
