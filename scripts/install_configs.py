#!/usr/bin/env python3
import os
import sys
import shutil

def install_if_missing(src, dest_dir):
    filename = os.path.basename(src)
    dest = os.path.join(dest_dir, filename)
    
    print(f"Checking {dest}...")
    if not os.path.exists(dest):
        print(f"Installing {src} to {dest}")
        shutil.copy2(src, dest)
    else:
        print(f"Skipping {dest} (already exists)")

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: install_if_missing.py <dest_dir> <src_file1> [src_file2 ...]")
        sys.exit(1)
        
    dest_dir = sys.argv[1]
    src_files = sys.argv[2:]
    
    # Ensure dest_dir exists
    if not os.path.exists(dest_dir):
        try:
            os.makedirs(dest_dir)
        except OSError:
            pass

    for src in src_files:
        install_if_missing(src, dest_dir)
