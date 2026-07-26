#!/bin/bash
# SukiOS WSL 开发环境一键安装脚本

# 1. 更新软件源
sudo apt update
sudo apt upgrade -y

# 2. 安装基础编译工具与依赖
sudo apt install -y \
    build-essential \
    bison \
    flex \
    libgmp3-dev \
    libmpc-dev \
    libmpfr-dev \
    texinfo \
    libisl-dev \
    wget \
    file \
    unzip \
    git

# 3. 安装模拟器 (QEMU)
sudo apt install -y \
    qemu-system-x86 \
    qemu-utils \
    ovmf

# 4. 安装调试器 (GDB)
sudo apt install -y gdb

# 5. 安装磁盘与镜像工具
sudo apt install -y \
    mtools \
    xorriso \
    grub-pc-bin \
    grub-efi-amd64-bin

# 6. 安装其他可能需要的工具
sudo apt install -y \
    nasm \
    cmake \
    python3 \
    python3-pip

# 7. 编译安装 x86_64-elf 交叉工具链 (GCC + Binutils)
#    这步时间较长（约15-30分钟），请耐心等待

# 设置安装路径（推荐放在用户目录下，避免 sudo）
export PREFIX="$HOME/opt/cross"
export TARGET=x86_64-elf
export PATH="$PREFIX/bin:$PATH"

# 创建工作目录
mkdir -p ~/src
cd ~/src

# 下载 Binutils 和 GCC 源码
wget -nc https://ftp.gnu.org/gnu/binutils/binutils-2.42.tar.gz
wget -nc https://ftp.gnu.org/gnu/gcc/gcc-13.2.0/gcc-13.2.0.tar.gz

# 解压
tar -xzf binutils-2.42.tar.gz
tar -xzf gcc-13.2.0.tar.gz

# ---- 编译 Binutils ----
mkdir -p build-binutils
cd build-binutils
../binutils-2.42/configure \
    --target=$TARGET \
    --prefix="$PREFIX" \
    --with-sysroot \
    --disable-nls \
    --disable-werror
make -j$(nproc)
make install
cd ..

# ---- 编译 GCC ----
mkdir -p build-gcc
cd build-gcc
../gcc-13.2.0/configure \
    --target=$TARGET \
    --prefix="$PREFIX" \
    --disable-nls \
    --enable-languages=c,c++ \
    --without-headers
make -j$(nproc) all-gcc
make -j$(nproc) all-target-libgcc
make install-gcc
make install-target-libgcc
cd ..

# 8. 将交叉编译器加入 PATH（永久生效）
echo 'export PATH="$HOME/opt/cross/bin:$PATH"' >> ~/.bashrc
export PATH="$HOME/opt/cross/bin:$PATH"

# 9. 验证安装
echo "=========================================="
echo "验证工具链安装："
x86_64-elf-gcc --version
qemu-system-x86_64 --version
echo "=========================================="
echo "SukiOS 开发环境安装完成！"
echo "请执行 'source ~/.bashrc' 或重启终端使 PATH 生效。"