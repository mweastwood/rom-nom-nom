#!/usr/bin/env bash
# Idempotent, non-interactive cloud environment bootstrap for rom-nom-nom.
# Installs Bazel (via Bazelisk), GNU MIPS cross-toolchain, KMC GCC 2.7.2 /
# Binutils 2.6 binaries, and in-process GCC 2.7.2 / GAS 2.6 harness archives.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

# 1. Ensure ~/.bashrc exports ~/.local/bin even in non-interactive shells.
mkdir -p "${HOME}/.local/bin"
if [[ -f "${HOME}/.bashrc" ]]; then
  sed -i '/case \$- in/,/esac/d' "${HOME}/.bashrc" 2>/dev/null || true
fi
grep -qxF 'export PATH="$HOME/.local/bin:$PATH"' "${HOME}/.bashrc" 2>/dev/null || \
  echo 'export PATH="$HOME/.local/bin:$PATH"' >> "${HOME}/.bashrc"
export PATH="${HOME}/.local/bin:${PATH}"

# 2. Install required system packages if any are missing.
REQUIRED_PACKAGES=(
  gcc-mips-linux-gnu
  g++-mips-linux-gnu
  binutils-mips-linux-gnu
  bison
  flex
  unzip
  zip
  clang-format
  clang-tidy
)
MISSING_PACKAGES=()
for pkg in "${REQUIRED_PACKAGES[@]}"; do
  if ! dpkg -s "${pkg}" >/dev/null 2>&1; then
    MISSING_PACKAGES+=("${pkg}")
  fi
done
if [[ ${#MISSING_PACKAGES[@]} -gt 0 ]]; then
  echo "[env-setup] Installing apt packages: ${MISSING_PACKAGES[*]}"
  sudo DEBIAN_FRONTEND=noninteractive apt-get update -qq
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${MISSING_PACKAGES[@]}"
fi

# 3. Install Bazelisk into ~/.local/bin/bazel if missing.
if [[ ! -x "${HOME}/.local/bin/bazel" ]]; then
  echo "[env-setup] Installing Bazelisk to ${HOME}/.local/bin/bazel..."
  TMP_BAZELISK="$(mktemp /tmp/bazelisk.XXXXXX)"
  curl -fSL "https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64" \
    -o "${TMP_BAZELISK}"
  chmod +x "${TMP_BAZELISK}"
  mv "${TMP_BAZELISK}" "${HOME}/.local/bin/bazel"
fi

# 4. Point Bazel's embedded JVM at the system Java CA truststore when present
#    so cloud proxy CAs are trusted for BCR downloads.
if [[ -f /etc/ssl/certs/java/cacerts ]]; then
  touch "${HOME}/.bazelrc"
  TRUSTSTORE_ARG="startup --host_jvm_args=-Djavax.net.ssl.trustStore=/etc/ssl/certs/java/cacerts"
  TRUSTPASS_ARG="startup --host_jvm_args=-Djavax.net.ssl.trustStorePassword=changeit"
  grep -qxF "${TRUSTSTORE_ARG}" "${HOME}/.bazelrc" || echo "${TRUSTSTORE_ARG}" >> "${HOME}/.bazelrc"
  grep -qxF "${TRUSTPASS_ARG}" "${HOME}/.bazelrc" || echo "${TRUSTPASS_ARG}" >> "${HOME}/.bazelrc"
fi

# 5. Pre-warm Bazel binary download via Bazelisk.
bazel --version

# 6. Install prebuilt KMC GCC 2.7.2 & Binutils 2.6 binaries if missing.
if [[ ! -x "${REPO_ROOT}/toolchain/gcc-2.7.2/gcc" || ! -x "${REPO_ROOT}/toolchain/binutils-2.6/as" ]]; then
  python3 "${REPO_ROOT}/tools/install_toolchain.py"
fi

# 7. Build in-process GCC 2.7.2 & GAS 2.6 harness static libraries if missing.
if [[ ! -f "${REPO_ROOT}/toolchain/libgcc272_harness.a" || ! -f "${REPO_ROOT}/toolchain/libgas26_harness.a" || ! -f "${REPO_ROOT}/toolchain/BUILD.bazel" ]]; then
  bash "${REPO_ROOT}/tools/setup_gcc_harness.sh"
fi

# 8. Enable repository pre-commit hooks when inside a git checkout.
if [[ -d "${REPO_ROOT}/.git" && -d "${REPO_ROOT}/.githooks" ]]; then
  git config core.hooksPath .githooks
fi

echo "[env-setup] Environment ready."
