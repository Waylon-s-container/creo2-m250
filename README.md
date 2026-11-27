# Creo 2.0 M250 MCP

在 Windows 上通过 MCP 控制 Creo Parametric 2.0 M250（64 位）。Codex 等 MCP 客户端通过标准输入输出连接 Node.js 服务，服务再把建模指令交给 Creo 进程中的 C TOOLKIT 插件。生成的 `.prt` 保留可编辑的原生草图、拉伸和孔特征。

## 环境要求

- Creo Parametric 2.0 M250 x64，已安装 C TOOLKIT，且当前 Creo 会话有权加载本机编译的 TOOLKIT 插件。
- Visual C++ 2010 x64 编译工具和 Windows SDK 7.1，用于生成插件。
- PowerShell 7、Node.js 18 或更高版本，以及支持本地 `stdio` 的 MCP 客户端。

将仓库放在当前 Windows 用户可写的短路径下，路径使用 ASCII 字符，例如 `C:\Projects\creo2-m250`。Creo 2.0 的插件登记文件由启动脚本按 ASCII 写入。仓库提供项目源码，编译使用本机安装的 PTC 和 Microsoft 开发组件。

## 构建和连接

在仓库根目录打开 PowerShell 7，运行以下命令。启动专用会话前，先保存并退出已打开的普通 Creo 会话。

```powershell
.\build.ps1
.\start-creo.ps1
```

构建生成 `bin/creo2_agent.dll`。如果依赖安装在其他位置，可以给 `build.ps1` 指定 `-CreoRoot`、`-VisualCpp`、`-VisualCppHeaders` 和 `-WindowsSdk`，也可以用 `-ToolRoot` 指向脚本支持的 VC++ 2010 和 Windows SDK 7.1 解包目录。若改用其他 Creo 安装路径，启动时也要传入对应的 `-CreoRoot`。

首次接入 Codex 时，在仓库根目录执行以下命令。

```powershell
$nodeRuntime = (Get-Command node).Source
$serverFile = (Resolve-Path .\server.mjs).Path
$sessionDirectory = Join-Path (Get-Location).Path 'session'
codex mcp add creo2-m250 -- $nodeRuntime $serverFile --session $sessionDirectory
codex mcp get creo2-m250 --json
```

如果 Codex 已有一个同名连接，请先用 `codex mcp get creo2-m250 --json` 核对其路径，再决定是否替换。若为启动脚本显式传入 `-Session`，MCP 注册也要使用同一个目录。其他 MCP 客户端可以用同样的 Node.js 命令和参数注册本地 `stdio` 服务。

在新的 Codex 会话里让 Agent 调用 `creo_status`。返回 `connected: true` 后，可以发一条建模指令。

> 用 `creo2-m250` 新建 `plate_01`。在 XY 平面画一个 100 × 60 mm 的矩形，向 +Z 拉伸 12 mm。在顶面 X=12、Y=−8 mm 处创建 Ø8 mm 通孔。保存原生零件，并列出草图、拉伸和孔三个特征。

## 建模和文件

| 工具 | 用途 |
| --- | --- |
| `creo_status` | 确认插件和 Creo 会话在线 |
| `creo_new_part` | 新建毫米制零件 |
| `creo_rectangle_sketch` | 在 XY 平面创建初始居中的矩形草图 |
| `creo_extrude` | 引用草图，沿 +Z 创建实体盲拉伸 |
| `creo_through_hole` | 在矩形拉伸顶面按模型 X/Y 位置创建直通孔 |
| `creo_inspect` | 读取特征、尺寸、体积和质心 |
| `creo_set_dimension` | 按特征与尺寸 ID 修改驱动长度或孔径并重新生成 |
| `creo_save_part` | 保存零件并返回版本文件路径 |
| `creo_open_part` | 打开已保存的零件，或从磁盘重新加载 |

默认零件目录为 `session/models`。Creo 为每次保存生成 `.prt.N` 版本。若要修改手头的零件，可以把独立的毫米制 `.prt` 副本放入该目录，再按模型名打开。`reload: true` 会重新读取磁盘上的版本，并拒绝覆盖会话中尚未保存的修改。

矩形改尺寸时沿用 Creo 的草图约束。孔用两条边定位，板件尺寸变化后如果要保持孔心的绝对 X/Y，需要重新核对边距。这套创建工具目前覆盖矩形、实体盲拉伸和直通孔。建模会话应放在可信的本地目录，因为能写入 `session/queue` 的程序可以提交该会话的建模指令。

## 验证

没有 Creo 时，可以检查脚本和 MCP 握手、工具列表及参数校验。

```powershell
node --check .\server.mjs
node --check .\smoke.mjs
node .\smoke.mjs --session (Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString()))
```

启动专用 Creo 会话后，可以运行实机测试。它会创建一个单独的测试零件，修改草图宽度、拉伸深度、孔径和孔位置，保存后打开检查，再次编辑并保存。

```powershell
$env:CREO2_LIVE_TEST = '1'
try { node .\smoke.mjs } finally { Remove-Item Env:\CREO2_LIVE_TEST }
```
