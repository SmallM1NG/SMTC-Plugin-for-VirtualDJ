# 开发相关

**中文** · [English](DEVELOPMENT_en.md)

## 构建基础

工程需要 Windows x64, Visual Studio C++ 桌面开发组件, MSVC v145 和 Windows SDK, 使用 C++20. 第三方依赖为 TagLib 与 zlib, 可通过 vcpkg 安装:

```powershell
.\vcpkg.exe install taglib:x64-windows-static
```

设置环境变量 `VCPKG_ROOT` 为 vcpkg 根目录并重新打开 Visual Studio. 工程会读取 `installed/x64-windows-static`; 自行编译依赖时, 可用 `TagLibRoot` 指定安装目录. Release 使用 /MT, Debug 使用 /MTd, 依赖的架构和运行库需要一致.

打开 [SMTC.vcxproj](../../Plugin/SMTC/SMTC.vcxproj), 选择 **Release / x64** 并生成项目. DLL 输出到 `artifacts/x64/Release/SMTC.dll`. SDK 头文件和依赖配置已包含在工程中.

## 源码结构

插件代码位于 `Plugin/SMTC`. VirtualDJ 官方 SDK 头文件已包含在该目录中.

| 文件 | 职责 |
| --- | --- |
| [SMTC.cpp](../../Plugin/SMTC/SMTC.cpp) | 插件入口, 内置设置, 生命周期, Windows SMTC 会话与按钮事件 |
| [BridgeCore.h](../../Plugin/SMTC/BridgeCore.h) | Deck 来源映射, 轮询间隔, 曲目身份与控制命令校验 |
| [Cover.h](../../Plugin/SMTC/Cover.h) / [Cover.cpp](../../Plugin/SMTC/Cover.cpp) | 异步封面任务, 本地标签解析, 主数据库查询与网络下载 |
| [Logger.h](../../Plugin/SMTC/Logger.h) | 日志格式, 写入同步与文件大小管理 |
| [SMTC.rc](../../Plugin/SMTC/SMTC.rc) / [SMTC.def](../../Plugin/SMTC/SMTC.def) | DLL 版本资源与导出定义 |

## 插件接口与 SMTC 会话

插件使用 VirtualDJ 的 Basic 接口, 通过宿主提供的查询和命令接口取得状态并执行控制. 设置由 VirtualDJ 内置参数界面展示, 不创建独立设置面板, 也不注册 DSP 或 StartStop 接口. 内部的 Enable SMTC 参数负责启动和停止桥接功能.

桥接线程初始化多线程 COM apartment, 创建隐藏顶层窗口, 再通过 `ISystemMediaTransportControlsInterop::GetForWindow` 获取该窗口对应的 SMTC 对象. 窗口与消息泵由同一线程管理. `DisplayUpdater` 发布标题, 艺术家, 专辑和封面, `PlaybackStatus` 发布播放或暂停状态. 当前实现不发布播放时间轴.

停止时先关闭命令邮箱并通知线程退出, 再取消封面任务, 解绑按钮事件, 关闭 SMTC 会话并销毁窗口. Windows 回调捕获共享邮箱而非插件对象, 避免回调直接访问已释放的插件实例.

## 状态查询与元数据更新

VirtualDJ 状态通过轮询获取, 间隔为 100-1000 ms 的十个档位, 默认 100 ms. 桥接线程使用 `MsgWaitForMultipleObjects` 同时等待停止事件, 唤醒事件, 轮询超时和窗口消息. 媒体按钮与封面完成事件会主动唤醒线程, 无需等待下一次轮询超时.

每次查询先解析来源 Deck, 再读取 `loaded`, `play` 和 `get_filepath`. 曲目身份包含实际 Deck 编号, 加载状态, 空 Deck 状态, 路径和设置修订号. 身份不变时复用标题, 艺术家和专辑, 减少文字查询与重复发布.

身份变化时优先读取 `get_title_remix`, 保留 Extended Mix 等版本信息; 无结果时依次回退到 `get_title`, `get_filename` 和 Deck 占位标题. 元数据读取结束后再次检查加载状态与路径, 丢弃查询期间已切歌的快照. 宿主查询并非事务, 这层检查用于降低混用两首曲目信息的可能性.

查询失败与明确空 Deck 分开处理. 已发布的曲目暂时不可用时保留旧信息最多两秒, 同时暂停接收控制并取消封面任务. 超时后显示 `No track loaded`, 保留 SMTC 对象. 明确为空的 Deck 仍可接收播放和切歌命令.

## Deck 来源与播放命令

固定来源直接使用 Deck 1-4. Left 和 Right 分别通过 `deck left get_deck` 与 `deck right get_deck` 解析实际编号, Master 使用 `get_activedeck`. 所有后续查询和命令都绑定解析后的编号, 避免处理过程中左右映射变化导致命令改用另一 Deck.

SMTC 的 `ButtonPressed` 回调只把动作, 已显示曲目的快照和当前 Windows 会话来源放入邮箱. 队列最多容纳 32 条请求. 桥接线程取出请求后重新检查启用状态, 设置修订与曲目身份, 过期请求不执行.

| 媒体动作 | VirtualDJ 命令 |
| --- | --- |
| 播放 | `deck N play` |
| 暂停 | `deck N pause` |
| 下一首 | `deck N load_next keepplay` |
| 上一首 | `deck N load_previous keepplay` |

这里的 N 为实际 Deck 编号. 上一首与下一首按 VirtualDJ 浏览器当前歌曲列表切歌, `keepplay` 用于保留播放状态. 明确空 Deck 使用同一组命令.

切歌请求还会检查 Windows 当前媒体会话: 比较回调时的应用来源与执行时的来源, 并匹配已发布的标题, 艺术家和专辑. 播放与暂停不使用这层会话过滤. 会话管理器不可用时退回插件自身的身份校验; 校验机制不负责改变 Windows 对媒体会话的选择.

## 封面解析与异步发布

封面使用独立工作线程, 不在状态查询线程中读取标签或下载图片. 每次曲目身份变化会清除上一首的封面并提交一个任务. 本地文件通过 TagLib 读取内嵌图片, 优先选择 Front Cover, 不查询数据库或同目录图片.

路径以 `netsearch://` 开头时, 插件通过 `get_vdj_folder` 找到主数据库 `database.xml`. XML 扫描匹配 Song 的 FilePath, 或 Link 的 NetSearch 属性与去掉前缀后的曲目标识, 从匹配的 Link 读取 Cover 属性. 下载由 WinHTTP 完成, 最多允许 1 次重定向. 名称解析, 连接, 发送与接收阶段的超时均设为 1 秒, 异步等待共用 5 秒期限. 无封面链接或下载失败时保留无封面状态.

XML 使用流式读取并禁止 DTD, 限制文件大小, 嵌套深度和扫描时间. 图片经过 WIC 校验, 当前限制为 10 MB (10,000,000 字节), 宽高各不超过 5000 像素, 超限拒绝发布, 不自动缩放.

每个任务持有递增票号. 换歌或停用使旧任务失效, 工作线程检查取消状态, 发布端再次比对票号. 有效图片写入 `InMemoryRandomAccessStream`, 通过 `RandomAccessStreamReference` 设置 SMTC 缩略图. 图片字节仅保留在当前处理与展示所需的对象中, 不建立历史封面或链接缓存.

每次曲目访问只尝试一次封面获取. 失败或取消后不自动恢复同一任务, 下一次曲目访问或重新启用插件时再尝试.

桥接线程中的 WinRT 异步操作使用有界等待, 最长等待 1 秒, 每次等待不超过 25 ms 并监听停止事件. 只有成功完成后才调用 GetResults. 超时或停止时请求取消, 不继续等待操作完成. 封面写入超时放弃本次发布, 切歌会话校验超时丢弃本次请求, 会话管理器初始化超时使用已有回退逻辑. 封面写入的完成回调仅保留内存流和写入器, 不访问插件对象, 避免迟到完成使用已释放资源.

## 异常处理与日志

一次状态更新失败会记录错误, 清除已发布状态并暂停接受命令, 后续轮询继续尝试更新. 桥接线程整体退出后, 设置界面会提示重新启用; 重新启用会回收已退出线程并创建新的线程, 不进行自动重启循环. 封面线程异常退出后, 下一次封面请求负责重新启动它.

日志由 `Logger.h` 串行写入 DLL 同目录的 `SMTC.log`, 记录 UTC 时间, 类型, 分类, 进程与线程编号及英文内容. 下一条记录使文件超过 10 MiB 时, 清空原文件再写入. 无法打开或写入时停止文件日志, 不改写到其他目录, 日志故障不阻断 SMTC 功能.
