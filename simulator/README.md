# littlefs Simulator

一个功能完整的 littlefs 文件系统模拟器，用于在 Windows 环境下调试 littlefs 文件系统。

## 功能特性

### 基础功能
- ✅ **镜像文件支持** - 挂载和操作 littlefs 镜像文件
- ✅ **文件操作** - 创建、读取、写入、删除文件
- ✅ **目录操作** - 创建目录、列出内容
- ✅ **重命名功能** - 文件和目录重命名

### 增强功能
- ✅ **cd 命令** - 目录导航 (`cd <path>`, `cd ..`)
- ✅ **pwd 命令** - 显示当前工作目录
- ✅ **跨目录重命名** - 支持文件在不同目录间移动
- ✅ **交互模式** - 命令行交互界面

## 项目结构

```
simulator/
├── littlefs_simulator.c     # C语言模拟器源码 (完整功能版)
└── README.md               # 本说明文档
```

## 使用方法

### C语言版本 (推荐)

**基本操作：**
```bash
# 格式化镜像
littlefs_simulator.exe -f -i myimage.img

# 创建文件
littlefs_simulator.exe -i myimage.img -c hello.txt

# 写入文件
littlefs_simulator.exe -i myimage.img -w "hello.txt=Hello World!"

# 读取文件
littlefs_simulator.exe -i myimage.img -r hello.txt

# 重命名文件/目录
littlefs_simulator.exe -i myimage.img -n "oldname.txt=newname.txt"

# 交互模式
littlefs_simulator.exe -i myimage.img
```

**交互模式命令：**
```
ls                    # 列出目录内容
create <path>         # 创建文件
mkdir <path>          # 创建目录
read <path>           # 读取文件
write <path=data>     # 写入文件
rename <old=new>      # 重命名文件/目录
cd <path>             # 切录导航
pwd                   # 显示当前路径
rm <path>             # 删除文件/目录
quit/exit             # 退出
```

## 编译指南 (C语言版本)

### 环境要求
- GCC 编译器 (推荐 TDM-GCC 或 MinGW-w64)
- CMake (可选)

### 编译步骤
```bash
# 直接编译
gcc -I. -I./bd -std=c99 -Wall -Wextra -Os -o littlefs_simulator.exe lfs.c lfs_util.c bd/lfs_filebd.c simulator/littlefs_simulator.c

# 或使用构建脚本
build_c_version.bat
```

## 已验证功能

### 核心功能
- [x] 镜像创建和格式化
- [x] 文件创建、读取、写入、删除
- [x] 目录创建和管理
- [x] 重命名（文件和目录）

### 扩展功能  
- [x] cd 命令 - 目录导航
- [x] pwd 命令 - 路径显示
- [x] 跨目录重命名
- [x] 绝对路径支持
- [x] 交互式命令行界面

## 应用场景

- 调试 littlefs 应用程序
- 预先创建文件系统镜像
- 测试文件系统操作
- 验证嵌入式代码的文件操作逻辑
- littlefs 功能演示和学习

## 技术说明

- 基于真实的 littlefs 库开发
- 使用 filebd 后端模拟闪存行为
- 与嵌入式系统完全兼容
- 支持 littlefs 的所有基本功能