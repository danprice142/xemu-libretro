#!/usr/bin/env python3

import sys
import os
import subprocess
from datetime import datetime

def main():
    if len(sys.argv) < 2:
        sys.exit(1)
    
    source_dir = sys.argv[1]
    
    # Get current date
    xemu_date = datetime.utcnow().strftime('%Y-%m-%d')
    
    # Get commit hash
    xemu_commit = ""
    git_dir = os.path.join(source_dir, '.git')
    if os.path.exists(git_dir):
        try:
            result = subprocess.run(['git', 'rev-parse', 'HEAD'], 
                                   cwd=source_dir, capture_output=True, text=True)
            if result.returncode == 0:
                xemu_commit = result.stdout.strip()
        except:
            pass
    else:
        commit_file = os.path.join(source_dir, 'XEMU_COMMIT')
        if os.path.exists(commit_file):
            with open(commit_file, 'r') as f:
                xemu_commit = f.read().strip()
    
    # Get version
    xemu_version = ""
    if os.path.exists(git_dir):
        try:
            result = subprocess.run(['git', 'describe', '--tags', '--match', 'v*'], 
                                   cwd=source_dir, capture_output=True, text=True)
            if result.returncode == 0:
                xemu_version = result.stdout.strip()
                if xemu_version.startswith('v'):
                    xemu_version = xemu_version[1:]
        except:
            pass
    else:
        version_file = os.path.join(source_dir, 'XEMU_VERSION')
        if os.path.exists(version_file):
            with open(version_file, 'r') as f:
                xemu_version = f.read().strip()
    
    if not xemu_version:
        xemu_version = "0.0.0"
    
    # Parse version components
    version_parts = xemu_version.split('-')
    base_version = version_parts[0]
    commit_part = version_parts[1] if len(version_parts) > 1 else "0"
    
    version_numbers = base_version.split('.')
    major = version_numbers[0] if len(version_numbers) > 0 else "0"
    minor = version_numbers[1] if len(version_numbers) > 1 else "0"
    patch = version_numbers[2] if len(version_numbers) > 2 else "0"
    
    # Output header
    print(f'#define XEMU_VERSION       "{xemu_version}"')
    print(f'#define XEMU_VERSION_MAJOR {major}')
    print(f'#define XEMU_VERSION_MINOR {minor}')
    print(f'#define XEMU_VERSION_PATCH {patch}')
    print(f'#define XEMU_VERSION_COMMIT {commit_part}')
    print(f'#define XEMU_COMMIT        "{xemu_commit}"')
    print(f'#define XEMU_DATE          "{xemu_date}"')

if __name__ == '__main__':
    main()
