"""Prepare a common SDR corpus from an AWJ summary and check FFmpeg decoding."""
import argparse
import csv
import hashlib
import json
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor

from pathlib import Path
from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--benchmark-root', required=True, type=Path)
    parser.add_argument('--awj-summary', required=True, type=Path)
    parser.add_argument('--ffmpeg', required=True, type=Path)
    args = parser.parse_args()
    root = args.benchmark_root.resolve()
    inputs = root / 'inputs'
    output = root / 'ffmpeg-common-inputs'
    if output.exists():
        raise ValueError('Common corpus already exists; inspect it before preparing again.')
    with args.awj_summary.open(encoding='utf-8-sig', newline='') as stream:
        rows = list(csv.DictReader(stream))
    if not rows or any(Path(row['input']).name != row['input'] for row in rows):
        raise ValueError('Summary requires nonempty corpus-relative filenames.')

    def probe(row):
        result = subprocess.run([str(args.ffmpeg), '-nostdin', '-hide_banner', '-loglevel', 'error',
            '-threads', '1', '-i', str(inputs / row['input']), '-frames:v', '1',
            '-filter_threads', '1', '-f', 'null', '-'], stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, timeout=60)
        try:
            with Image.open(inputs / row['input']) as image:
                static = getattr(image, 'n_frames', 1) == 1
        except (OSError, ValueError):
            static = False
        eligible = (static and result.returncode == 0 and row['status'] == 'ok'
            and row['has_non_opaque_alpha'] == 'false' and int(row['source_bit_depth']) <= 10
            and row['source_has_icc'] == 'false' and row['source_has_hdr_metadata'] == 'false'
            and row['source_transfer_characteristics'] not in ('8', '16', '18'))
        return dict(input=row['input'], ffmpeg_decode_exit=result.returncode,
            common_sdr_opaque=eligible, error=result.stderr.decode('utf-8', errors='replace').strip())

    with ThreadPoolExecutor(max_workers=12) as pool:
        records = list(pool.map(probe, rows))
    output.mkdir()
    selected = []
    for record in records:
        if not record['common_sdr_opaque']:
            continue
        source = inputs / record['input']
        target = output / source.name
        shutil.copyfile(source, target)
        with source.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        with target.open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != digest:
                raise OSError(f'Copy hash mismatch: {target.name}')
        selected.append(dict(input=target.name, bytes=target.stat().st_size, sha256=digest))
    (root / 'ffmpeg-capabilities.json').write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding='utf-8')
    (root / 'ffmpeg-common-manifest.json').write_text(json.dumps(selected, indent=2), encoding='utf-8')
    version = subprocess.check_output([str(args.ffmpeg), '-hide_banner', '-version'], text=True, errors='replace')
    (root / 'ffmpeg-version.txt').write_text(version, encoding='utf-8')
    print(f'FFmpeg decoded {sum(r["ffmpeg_decode_exit"] == 0 for r in records)}/{len(records)}; common corpus: {len(selected)}.')


if __name__ == '__main__':
    main()
