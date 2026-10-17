#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != Linux ]]; then
    echo 'Buildray currently supports Linux runners.' >&2
    exit 1
fi
. /etc/os-release
if [[ "$ID" != ubuntu || "$VERSION_CODENAME" != noble ]]; then
    echo 'Automatic setup supports Ubuntu 24.04. Set install-dependencies: false for a prepared Linux runner with Python 3.12+.' >&2
    exit 1
fi

sudo apt-get update
sudo apt-get install -y ca-certificates curl python3-venv
curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc > /dev/null
echo "deb https://apt.llvm.org/$VERSION_CODENAME/ llvm-toolchain-$VERSION_CODENAME-21 main" | sudo tee /etc/apt/sources.list.d/buildray-llvm21.list
sudo apt-get update
sudo apt-get install -y clang-21 clang-tools-21 clang-tidy-21 llvm-21-dev libclang-21-dev libclang-rt-21-dev ninja-build libzstd-dev

buildray_venv=$(mktemp -d "$RUNNER_TEMP/buildray-python-XXXXXX")
python3 -m venv "$buildray_venv"
"$buildray_venv/bin/python" -m pip install --disable-pip-version-check cmake==4.2.3 ninja==1.13.0
echo "$buildray_venv/bin" >> "$GITHUB_PATH"
