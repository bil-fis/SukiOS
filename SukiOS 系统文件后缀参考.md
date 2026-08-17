===============================================================================
SUKIOS 文件扩展名体系 v1.0（官方完整定义）
===============================================================================

一、系统核心

.ski    SukiOS Kernel Image         内核镜像。GRUB (Multiboot2) 加载的 ELF 文件，
                                    包含内核核心与内置驱动。启动后常驻内存。
                                    （位置：/boot/kernel.ski）

.ssvc   SukiOS System Service        系统服务。运行于 Ring3 用户态的后台服务，
                                    由 init 进程启动和管理。有独立进程、可崩溃重启。
                                    （位置：/system/services/*.ssvc）

.kdr    SukiOS Kernel Driver         内核驱动。Ring0 内核模块，可动态加载（.kdr 格式）。
                                    用于扩展内核功能（USB 主机控制器、NVMe、GPU 等）。
                                    （位置：/system/drivers/*.kdr）


二、可执行文件

.ska    SukiOS Application Bundle    用户态可执行应用。这是一个文件夹（Bundle），
                                    包含可执行文件、私有库、资源和配置。
                                    整体以文件夹形式存在，扩展名为 .ska。
                                    由 SukiCreateProcess 系统调用加载执行。
                                    （位置：/Apps/*.ska 或 /home/user/Apps/*.ska）

.skm    SukiOS Mach-O                 保留扩展名，用于将来支持 Mach-O 格式的
                                    兼容层（如 macOS 应用兼容性）


三、库文件

.sl     SukiOS Shared Library        动态共享库。ELF 共享对象格式，由动态链接器
                                    ld.suki 在运行时加载。用于代码复用和模块化。
                                    （位置：/system/lib/*.sl）

.sa     SukiOS Static Archive        静态库归档文件。.a 格式的静态链接库，
                                    用于编译时链接到 .ska 或 .ssvc。
                                    （位置：/system/lib/static/*.sa）


四、配置与数据文件

.cfg    SukiOS Config                配置文件。JSON、TOML 或纯文本格式。
                                    系统级和服务级配置。
                                    （位置：/system/config/*.cfg）
                                    用户级配置：（/home/user/.config/*.cfg）

.sres   SukiOS Resource              资源包。包含图标、图片、翻译等资源的容器文件，
                                    类似 Windows .res 或 macOS .car。
                                    （位置：/system/resources/*.sres）


五、字体文件（沿用行业标准，不设新扩展名）

.ttf    TrueType Font                标准 TrueType 字体文件。
                                    （位置：/system/fonts/*.ttf）

.otf    OpenType Font                标准 OpenType 字体文件。
                                    （位置：/system/fonts/*.otf）

.woff   Web Open Font Format         标准 Web 字体文件。
.woff2  Web Open Font Format 2       标准 Web 字体文件（压缩）。
.pcf    Portable Compiled Format     标准 X11 位图字体。
.psf    PC Screen Font               标准 PC 屏幕字体。


六、调试与日志

.log    SukiOS Log                   系统或应用日志文件。纯文本格式。
                                    由系统日志守护进程统一管理。
                                    （位置：/var/log/*.log）

.scd    SukiOS Crash Dump            崩溃转储文件。进程崩溃时的内存快照和寄存器状态。
                                    用于事后调试分析。
                                    （位置：/var/crash/*.scd）


七、磁盘与虚拟设备

.swap   SukiOS Swap                  交换分区/文件。用作虚拟内存的后备存储。
                                    （位置：/system/swap.swap）


八、文档（沿用通用扩展名）

.md     Markdown 文档                Markdown 格式的系统文档。
                                    （位置：/usr/doc/*.md）

.html   HTML 文档                   HTML 格式的文档（含 CSS 样式）。
                                    （位置：/usr/doc/*.html）


九、临时与运行时文件

.tmp    SukiOS Temporary             临时文件。系统或应用运行时生成的临时数据，
                                    关机时应被清理。
                                    （位置：/tmp/*.tmp 或 /var/run/*.tmp）

.pid    PID File                     进程 PID 锁文件。用于防止服务重复启动。
                                    （位置：/var/run/*.pid）


十、压缩与归档

.sar    SukiOS Archive               归档文件。SukiOS 原生打包格式，
                                    包含文件元数据和校验和。用于软件分发。
                                    （位置：可任意存放 .sar 包）

.spkg   SukiOS Package               软件包。包含 .ska 应用包、.sl 库、
                                    配置文件和元数据的安装包。
                                    （位置：用于软件包管理器分发）


十一、.ska 应用包内部结构（核心定义）

.ska 是一个文件夹，而非单文件。其内部结构如下：

MyApp.ska/                         ← 应用包目录，扩展名为 .ska
├── Contents/                      ← 标准内容目录
│   ├── Info.meta                 ← 应用元数据（JSON 或 TOML）
│   │   {
│   │       "name": "MyApp",
│   │       "version": "1.0.0",
│   │       "executable": "myapp",  ← 可执行文件名
│   │       "icon": "icon.png",
│   │       "author": "SukiOS Developer",
│   │       "category": "Utilities"
│   │   }
│   ├── Executable/                ← 可执行二进制文件
│   │   └── myapp                  ← 实际的 ELF 可执行文件（elf文件无后缀名）
│   ├── Libraries/                 ← 应用私有库 (.sl)
│   │   ├── libapp.sl
│   │   └── libextras.sl
│   ├── Resources/                 ← 应用资源文件
│   │   ├── icon.png               ← 应用图标
│   │   ├── desktop.html           ← 桌面界面文件
│   │   ├── images/                ← 图片资源
│   │   └── fonts/                 ← 应用私有字体（.ttf/.otf 等）
│   └── Config/                    ← 应用默认配置 (.cfg)
│       └── default.cfg
└── .metadata/                     ← 系统生成的元数据（缓存、状态等）


十二、路径规范

/                  根目录
/boot/            内核镜像 .ski、引导配置文件
/system/          系统核心
  bin/            系统命令 .ska
  lib/            动态库 .sl
  drivers/        内核驱动 .kdr
  services/       系统服务 .ssvc
  config/         系统配置 .cfg
  fonts/          系统字体（.ttf、.otf 等）
  resources/      系统资源 .sres
/var/
  log/            日志文件 .log
  crash/          崩溃转储 .scd
  run/            PID 锁文件 .pid
/home/用户名/
  .config/        用户配置 .cfg
  .local/         用户本地数据
/usr/
  share/          共享数据
  doc/            文档（.md、.html 等）
/Apps/            系统级应用安装目录
  <应用名>.ska/   应用包
/tmp/             临时文件 .tmp
