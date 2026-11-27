# MyStickyNotes

MyStickyNotes 是一个原生 C++ Windows 桌面便笺。支持中英文字体、局部字号与颜色、粗体、斜体、删除线、列表、主题色和窗口置顶。使用 Ctrl + [ 或 Ctrl + ] 调整整张便笺的行距。

## 下载与运行

克隆仓库后运行 [bin/StickyNotes.exe](bin/StickyNotes.exe)（Windows x64）。更新时先退出程序，再运行 `git pull`，即可一起取得对应的源码、EXE 和同步脚本。首次编辑后，程序会在同一目录创建 notes.json。备份此文件即可保留正文、排版与主题设置。

## 保存与同步

notes.json 中的 appVersion 表示写入文件的应用版本，与 EXE 产品版本一致。cache-fmt-version 表示 JSON 格式版本，当前为 4。saveDelaySeconds 控制最后一次修改到本地保存的间隔，默认 5 秒。syncDelayMinutes 控制自动同步的间隔，默认 10 分钟。

使用 Gitee 同步时，安装 Git 和 SSH，将 [sync.ps1](bin/sync.ps1) 放在程序旁，并在同级 ssh 目录准备有仓库访问权限的 id_ed25519 和包含已核实服务器主机密钥的 gitee_known_hosts。脚本默认连接 hippo-yf/my-sticky-notes-cache；使用自己的仓库时修改脚本中的仓库地址。各设备共用远端的 notes.json。两端同时修改时，程序保留本地便笺，并将远端副本保存为 notes.remote.json。

## 从源码构建

安装 Visual Studio 2022 Community 的「使用 C++ 的桌面开发」工作负载和 Windows SDK，在项目根目录运行 build.cmd。脚本选择 x64 工具链并输出 bin/StickyNotes.exe。

## 许可证

[MIT](LICENSE)
