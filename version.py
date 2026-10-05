#!/usr/bin/python3

import subprocess

try:
    with open('VERSION', encoding='utf-8') as f:
        print(f.readline().strip())
except:
    try:
        kwargs = { 'capture_output': True, 'encoding': 'utf-8', 'check': True }
        rev = subprocess.run(['git', 'rev-list',  '--count', 'HEAD'], **kwargs).stdout.strip()
        sha = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], **kwargs).stdout.strip()
        # the extras release this is (zextras-v9), or follows (zextras-v9+3):
        # the nearest extras-vN tag, which only the extras branch reaches.
        # After the sha, so REVISION still reads the count
        rel = ''
        try:
            desc = subprocess.run(['git', 'describe', '--tags', '--long', '--match', 'extras-v*'],
                                  **kwargs).stdout.strip()
            tag, since, _ = desc.rsplit('-', 2)
            rel = f'~z{tag}' + (f'+{since}' if since != '0' else '')
        except Exception:
            pass
        print(f'r{rev}~{sha}{rel}')
    except:
        print('r666~unknown')
