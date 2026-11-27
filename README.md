# MyStickyNotes

MyStickyNotes 是一个原生 C++ Windows 桌面便笺。支持中英文字体、局部字号与颜色、粗体、斜体、删除线、列表、主题色和窗口置顶。使用 Ctrl + [ 或 Ctrl + ] 调整整张便笺的行距。

## 下载与运行

克隆仓库后运行 [bin/StickyNotes.exe](bin/StickyNotes.exe)（Windows x64）。更新时先退出程序，再运行 `git pull`，即可一起取得对应的源码、EXE 和同步脚本。首次编辑后，程序会在同一目录创建 notes.json。备份此文件即可保留正文、排版与主题设置。

## 本地保存

最后一次修改后，程序默认等待 5 秒将便笺写入 `bin/notes.json`。继续编辑会重新计时，拼音组合期间暂停计时。正常退出时会立即保存。

`richText` 以十六进制保存正文和富文本排版。`appVersion` 记录写入文件的应用版本，`cache-fmt-version` 记录文件格式版本。文件末尾的 `_说明` 列出了各字段的含义。

## Gitee 多设备同步

各设备通过 Git 共用 Gitee 仓库中的一份 `notes.json`。点击便笺右下方的同步按钮可立即同步，默认也会在最后一次修改 10 分钟后自动同步。

1. 在每台设备上安装 Git 和 SSH，并准备对目标 Gitee 仓库有读写权限的 SSH 密钥。
2. 在便笺中编辑一次并正常退出，让程序生成 `bin/notes.json`。
3. 在已有的 JSON 中调整下表字段，重新打开程序，然后点击同步按钮。

| 字段 | 默认值 | 用途 |
| --- | --- | --- |
| `giteeRepository` | `hippo-yf/my-sticky-notes-cache` | 目标 Gitee 仓库，填写 `所有者/仓库名`。 |
| `sshKeyPath` | `ssh/id_ed25519` | 本机 SSH 私钥的路径。 |
| `knownHostsPath` | `""` | 省略或填空字符串时读取当前用户的 `~/.ssh/known_hosts`，通常位于 `C:/Users/你的用户名/.ssh/known_hosts`。 |
| `saveDelaySeconds` | `5` | 本地自动保存的延迟，单位为秒。 |
| `syncDelayMinutes` | `10` | Gitee 自动同步的延迟，单位为分钟。 |

两个 SSH 路径均支持绝对路径，相对路径从程序所在的 `bin` 目录解析。例如，已有项目专用主机密钥文件时，可设置 `"knownHostsPath": "ssh/gitee_known_hosts"`。选用的文件必须存在，并包含已核实的 Gitee 主机密钥。多台设备用户名不同时，留空可让每台设备使用各自用户目录中的文件。JSON 只记录密钥路径，私钥文件保存在本机。

同步失败时可查看 `bin/sync.log`，其中只记录警告和错误。若两台设备分别修改了同一份便笺，程序保留本地 `notes.json`，并把远端副本写入 `bin/notes.remote.json`，供比较和合并。

## 从源码构建

安装 Visual Studio 2022 Community 的「使用 C++ 的桌面开发」工作负载和 Windows SDK，在项目根目录运行 build.cmd。脚本选择 x64 工具链并输出 bin/StickyNotes.exe。

## 许可证

[MIT](LICENSE)
