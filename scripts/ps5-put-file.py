#!/usr/bin/env python3
"""Upload one file to the console over FTP and verify it by size and by the
SHA-256 of a raw readback. Only paths under /data/PS5LM/ are accepted.

    scripts/ps5-put-file.py LOCAL /data/PS5LM/models/NAME.gguf [EXPECTED_SHA256]

The file goes to REMOTE.part first and is renamed only after the staged copy
verifies, so an interrupted upload never leaves a complete-looking file.
Every connection sends SELF first, which turns off ftpsrv's on-the-fly SELF
decryption, so the readback is the stored bytes. Prints one JSON line.
"""
import ftplib
import hashlib
import json
import os
import posixpath
import sys
import time

BLOCK = 1 << 20


def connect(host, port):
    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=60)
    ftp.login()
    ftp.sendcmd('TYPE I')
    ftp.sendcmd('SELF')
    return ftp


def remote_size(ftp, path):
    try:
        return ftp.size(path)
    except ftplib.error_perm:
        return None


def readback(host, port, path):
    ftp = connect(host, port)
    digest = hashlib.sha256()
    count = 0

    def take(block):
        nonlocal count
        digest.update(block)
        count += len(block)

    ftp.retrbinary('RETR ' + path, take, blocksize=BLOCK)
    ftp.quit()
    return count, digest.hexdigest()


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    local, remote = sys.argv[1], sys.argv[2]
    expected = sys.argv[3] if len(sys.argv) == 4 else None
    remote = posixpath.normpath(remote)
    if not remote.startswith('/data/PS5LM/') or '..' in remote.split('/'):
        sys.exit('ps5-put-file: remote path must be under /data/PS5LM/')
    host = os.environ.get('PS5_HOST') or sys.exit('ps5-put-file: set PS5_HOST')
    port = int(os.environ.get('PS5_FTP_PORT', '2121'))
    size = os.path.getsize(local)
    digest = hashlib.sha256()
    with open(local, 'rb') as f:
        for block in iter(lambda: f.read(BLOCK), b''):
            digest.update(block)
    local_sha = digest.hexdigest()
    if expected and expected != local_sha:
        sys.exit(f'ps5-put-file: local SHA-256 {local_sha} is not the expected {expected}')
    staged = remote + '.part'
    ftp = connect(host, port)
    if remote_size(ftp, remote) is not None:
        sys.exit(f'ps5-put-file: {remote} already exists; not replacing it')
    start = time.time()
    with open(local, 'rb') as f:
        ftp.storbinary('STOR ' + staged, f, blocksize=BLOCK)
    upload_s = time.time() - start
    ftp.quit()
    start = time.time()
    count, remote_sha = readback(host, port, staged)
    readback_s = time.time() - start
    if count != size or remote_sha != local_sha:
        sys.exit(f'ps5-put-file: staged copy differs ({count} bytes, {remote_sha}); left at {staged}')
    ftp = connect(host, port)
    ftp.rename(staged, remote)
    final_size = remote_size(ftp, remote)
    ftp.quit()
    count, final_sha = readback(host, port, remote)
    if final_size != size or count != size or final_sha != local_sha:
        sys.exit(f'ps5-put-file: final copy differs ({count} bytes, {final_sha})')
    print(json.dumps({'local': local, 'remote': remote, 'bytes': size, 'sha256': local_sha,
                      'readback_sha256': final_sha, 'upload_s': round(upload_s, 1),
                      'readback_s': round(readback_s, 1), 'verified': True}))


if __name__ == '__main__':
    main()
