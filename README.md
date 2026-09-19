# Sempervirens

Sempervirens 是一款支持自定义规则迁移的 Minecraft 实例迁移器。它会先比较两个实例，再由你选择需要带走的内容以及同名文件的处理方式。

当前正式版本：**0.1.0.0**

支持系统：**Windows 10 / Windows 11（x64）**

## 下载

从 [GitHub Releases](https://github.com/XintingleiTeam/Sempervirens/releases/latest) 下载最新的 `Sempervirens-0.1.0.0-Setup.exe`。

Sempervirens 使用原生 C++ 与 Windows API 构建，不需要安装 .NET 运行时。安装向导和应用均支持简体中文与英文。

## 使用方法

1. 关闭 Minecraft 和正在使用相关实例的启动器。
2. 选择迁出实例和迁入实例。
3. 扫描可迁移内容，并查看每一项的差异说明。
4. 选择需要迁移的内容和同名文件处理方式。
5. 确认清单后开始迁移。

处理重要存档前，建议额外保留一份独立备份。迁出实例始终只读，程序只会改动你选定的迁入实例。

## 主要功能

- 自动发现 Minecraft 实例并显示完整路径。
- 内置“仅原版 Minecraft”和“新亭泪整合实例”规则。
- 支持从本地 JSON 文件载入自定义迁移规则。
- 在迁移前逐项展示差异、迁移内容和处理方式。
- 提供备份后覆盖、直接覆盖和保留目标三种同名内容策略。
- 支持迁移记录、备份定位、托盘运行、中英文界面和键盘操作。
- 支持应用内差分更新；更新失败时自动保留或恢复可用版本。
- 优先使用 GitHub Release 下载更新，无法访问时自动切换到国内加速源。

## 自定义规则

规则文件使用 UTF-8 JSON。可以复制 [`profiles/example-custom.json`](profiles/example-custom.json) 后修改，再从应用的“设置 → 迁移规则”中选择文件。

程序会检查规则结构、路径范围和路径安全性；无效规则不会参与扫描或迁移。

## 图标显示异常

Windows 有时会继续显示旧图标。先刷新资源管理器；仍未恢复时，可在项目目录运行：

```powershell
.\Refresh-Icons.cmd -Rebuild
```

该操作只会重建当前用户的图标缓存并短暂重启资源管理器，不会删除程序或个人文件。

## 开发与贡献

项目使用 Zig 0.16 构建 Windows x64 原生程序，并使用 Inno Setup 6 生成安装包。依赖已包含在仓库中。

```bat
Build.cmd
Build-Installer.cmd
```

提交改动前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。安全问题请按照 [SECURITY.md](SECURITY.md) 私下报告。

## 许可证

Sempervirens 采用 [MIT License](LICENSE) 开源。
