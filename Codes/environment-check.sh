#!/bin/bash
# environment-check.sh
#
# GIVEN BY PROFESSOR
#
# What it does:
#   Checks if your Ubuntu VM is ready to build and run this project.
#   Run this FIRST before doing anything else.
#
# How to use:
#   chmod +x environment-check.sh
#   sudo ./environment-check.sh

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

ok()   { echo -e "${GREEN}[OK]${NC}    $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC}  $1"; }
fail() { echo -e "${RED}[FAIL]${NC}  $1"; FAILED=1; }

FAILED=0

echo "=== OS-Jackfruit Environment Check ==="
echo ""

# 1. Check running as root
if [ "$EUID" -eq 0 ]; then
    ok "Running as root"
else
    fail "Must run as root (sudo ./environment-check.sh)"
fi

# 2. Check OS
if grep -q "Ubuntu" /etc/os-release 2>/dev/null; then
    VER=$(grep VERSION_ID /etc/os-release | cut -d'"' -f2)
    ok "Ubuntu $VER detected"
    if [[ "$VER" != "22.04" && "$VER" != "24.04" ]]; then
        warn "Recommended: Ubuntu 22.04 or 24.04"
    fi
else
    fail "Ubuntu required (WSL will NOT work for kernel modules)"
fi

# 3. Check gcc
if command -v gcc &>/dev/null; then
    ok "gcc found: $(gcc --version | head -1)"
else
    fail "gcc not found — run: sudo apt install build-essential"
fi

# 4. Check kernel headers
KHEADERS="/lib/modules/$(uname -r)/build"
if [ -d "$KHEADERS" ]; then
    ok "Kernel headers found: $KHEADERS"
else
    fail "Kernel headers missing — run: sudo apt install linux-headers-$(uname -r)"
fi

# 5. Check make
if command -v make &>/dev/null; then
    ok "make found"
else
    fail "make not found — run: sudo apt install build-essential"
fi

# 6. Check Secure Boot
if command -v mokutil &>/dev/null; then
    if mokutil --sb-state 2>/dev/null | grep -q "disabled"; then
        ok "Secure Boot is OFF (required for kernel modules)"
    else
        warn "Secure Boot may be ON — kernel modules may fail to load"
        warn "Disable Secure Boot in VM BIOS settings"
    fi
else
    warn "Cannot check Secure Boot status (mokutil not found)"
fi

# 7. Check virtualization
if systemd-detect-virt -q 2>/dev/null; then
    ok "Running in a VM: $(systemd-detect-virt)"
else
    warn "Not detected as a VM — ensure you are using VirtualBox or similar"
fi

# 8. Check wget (for downloading rootfs)
if command -v wget &>/dev/null; then
    ok "wget found"
else
    warn "wget not found — run: sudo apt install wget"
fi

echo ""
echo "==================================="
if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}All checks passed! You are ready to build.${NC}"
    echo ""
    echo "Next steps:"
    echo "  1. make"
    echo "  2. mkdir rootfs-base"
    echo "  3. wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz"
    echo "  4. tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base"
else
    echo -e "${RED}Some checks failed. Fix the issues above before proceeding.${NC}"
fi
