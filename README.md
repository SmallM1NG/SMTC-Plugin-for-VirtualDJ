<!-- 模板来源: https://github.com/SmallM1NG/NCM-Auto-Dump-Tool/blob/HEAD/README.md -->

<p align="center">
  <img width="72%" alt="SMTC Plugin for VirtualDJ" src="Docs/imgs/title.png">
</p>

<p align="center"><b>SMTC-Plugin-for-VirtualDJ</b></p>
<p align="center">
	<a href="https://github.com/SmallM1NG/SMTC-Plugin-for-VirtualDJ/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/SmallM1NG/SMTC-Plugin-for-VirtualDJ?color=brightgreen&label=Latest&style=for-the-badge"></a>
	<img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-00599C.svg?logo=cplusplus&logoColor=white&style=for-the-badge">
	<img alt="Windows x64" src="https://img.shields.io/badge/Windows-x64-0078D6.svg?logo=windows&logoColor=white&style=for-the-badge">
	<img alt="VirtualDJ 8" src="https://img.shields.io/badge/VirtualDJ-8-E60000.svg?style=for-the-badge">
	<a href="LICENSE"><img alt="License: GPLv3" src="https://img.shields.io/badge/License-GPLv3-red.svg?style=for-the-badge"></a>
	<a href="https://github.com/SmallM1NG/SMTC-Plugin-for-VirtualDJ/stargazers"><img alt="Stars" src="https://img.shields.io/github/stars/SmallM1NG/SMTC-Plugin-for-VirtualDJ?style=for-the-badge"></a>
</p>

<p align="center"><b>中文</b> · <a href="README_en.md">English</a></p>

<p align="center"><a href="#项目介绍">项目介绍</a> · <a href="#功能展示">功能展示</a> · <a href="#如何安装">如何安装</a> · <a href="#如何使用">如何使用</a> · <a href="#其他内容">其他内容</a> · <a href="Docs/md/DEVELOPMENT.md">开发相关</a> · <a href="Docs/md/CHANGELOG.md">更改日志</a></p>

---

<a id="项目介绍"></a>
## 项目介绍 ℹ️

这是一款可以将你的 VirtualDJ 接入 Windows SMTC (系统媒体传输控制) 的插件.

插件可以向 Windows 共享当前曲目的标题, 艺术家, 专辑, 封面, 播放状态, 当前位置和总时长, 也支持通过系统媒体按钮控制播放, 暂停, 上一首和下一首, 以及通过支持进度控制的软件跳转播放位置. 支持 SMTC 的软件都可以获取信息或调用控制, 例如 Wallpaper Engine.

使用 VirtualDJ 内置设置界面, 支持选择 Deck 1-4, Left, Right 或 Master 作为信息与控制来源.

本地曲目封面从文件标签中读取, 网络曲目封面通过 VirtualDJ 主数据库中的封面链接获取.

> 本项目以 [GPLv3](LICENSE) 开源, 使用, 修改或分发时请遵守该协议.

<a id="功能展示"></a>
## 功能展示 ✨

<div align="center"><img width="1024" src="Docs/imgs/demo.webp" alt="VirtualDJ SMTC 功能展示"></div>

---

<a id="如何安装"></a>
## 如何安装 📥

本插件仅支持 **Windows x64** 的 **VirtualDJ 2021 及以上版本**, 需拥有 **VirtualDJ Pro** 许可证才可使用.

<ol>
<li>

从 [Releases](https://github.com/SmallM1NG/SMTC-Plugin-for-VirtualDJ/releases/latest) 下载最新发行版 **dll** 文件 (当前为 [v0.1.2](https://github.com/SmallM1NG/SMTC-Plugin-for-VirtualDJ/releases/tag/v0.1.2)).

</li>
<li>

将 **SMTC.dll** 放入 VirtualDJ 数据目录的 **Plugins64\AutoStart** 文件夹中.

2023之前的版本, 通常在 `C:\Users\用户名\Documents\VirtualDJ\Plugins64\AutoStart`  
2023之后的版本, 通常在 `C:\Users\用户名\AppData\Local\VirtualDJ\Plugins64\AutoStart`  
如果你更改了数据目录, 请放入你自定义的数据目录内.

</li>
<li>

启动 VirtualDJ, 在 Master 效果列表中看到 **SMTC** 字样, 则为安装成功.

</li>
</ol>

---

<a id="如何使用"></a>
## 如何使用 ▶️

### 1. 自动启动

插件默认会随 VirtualDJ 启动而自行开启, 无需手动开启.

### 2. 调整设置

如果你想调整来源等设置, 在 Master 效果列表内找到 **SMTC** 字样, 点击旁边的小齿轮打开内置设置页面.

<p align="center">
  <img src="Docs/imgs/settings.png" alt="SMTC 设置入口示意">
</p>

以下五项开关默认均启用, 已保存的设置会由 VirtualDJ 恢复.

- **Enable SMTC**: 控制插件功能的启用和停用.
- **Share Cover**: 控制是否共享封面, 默认启用. 关闭时清除封面并取消获取任务, 重新启用后获取当前曲目封面.
- **Share Track Info**: 控制是否共享标题, 艺术家和专辑, 默认启用. 关闭时清除这些信息并停止文字标签查询, 重新启用后读取当前曲目.
- **Share Progress**: 控制是否共享播放进度, 默认启用. 开启后按 Timeline Mode 共享当前位置和总时长; 可配合 Enable Seeking 开启跳转.
- **Enable Seeking**: 控制是否允许通过 SMTC 跳转播放位置, 默认启用, 紧凑界面短名称为 Seeking. 需要同时开启 Share Progress; 播放或暂停状态保持不变.
- **Timeline Mode**: 选择进度时间轴, 默认 Original + Pitch. Original + Pitch 共享原始位置, 原始总时长和实际播放速率, 更适合歌词时间轴; Adjusted 共享随速度折算的位置和总时长, 播放速率保持 1.
- **Source Deck**: 选择 Deck 1-4, Left, Right 或 Master 作为曲目信息和播放控制的来源.
- **Polling Interval**: 设置曲目信息和播放状态的查询间隔, 可选 100-1000 ms (每 100 ms 一档), 共十档, 默认 100 ms. 数值越小, 更新越及时, 查询次数也越多.

设置会在 VirtualDJ 关闭时自动保存.

> 如果想关闭插件功能, 请点击插件设置内的关闭按钮, 不要点击 VirtualDJ 提供的插件关闭功能 (小绿点). 这会导致插件在列表中不可见, 需要重启 VirtualDJ 才能恢复.

---

<a id="其他内容"></a>
## 其他内容 📚

<p align="center"><a href="Docs/md/DEVELOPMENT.md">开发相关</a> · <a href="Docs/md/CHANGELOG.md">更改日志</a></p>

### 补充说明 💡

#### 1. 为什么有些歌曲没有封面?

本地曲目只读取文件标签中的内嵌封面. 网络曲目需要 VirtualDJ 主数据库中存在可用的封面链接, 插件会自动下载封面.

如果没有封面, 下载失败或图片超过限制, 插件会保持无封面, 不定时重试. 切走再切回来, 重新启用插件或重新启用 Share Cover 时会再次尝试.

#### 2. 修改歌曲标签后为什么没有立即更新?

标题, 艺术家和专辑只在切歌, 切换来源, 重新启用插件或重新启用 Share Track Info 时读取.

#### 3. 上一首和下一首按哪个列表切歌?

按照 VirtualDJ 浏览器当前歌曲列表切歌. 列表边界和播放中的歌曲能否被替换由 VirtualDJ 决定.

#### 4. 播放进度如何共享和控制?

开启 Share Progress 后, 插件持续查询实际进度, 正常播放由接收端按播放速率推进; 状态或时间轴变化时立即发布, 实际位置与预测位置偏差超过 100 ms 时校正. 查询间隔由 Polling Interval 控制, 默认 100 ms. Timeline Mode 默认 Original + Pitch, 共享原始歌曲时间轴和实际播放速率. Adjusted 模式共享随 VirtualDJ 速度折算的位置和总时长, 速率保持 1.

例如原始时长 300 秒的歌曲在 1.25 倍速下, Original + Pitch 仍共享 300 秒并发布速率 1.25; Adjusted 共享 240 秒并发布速率 1. 歌词软件是否按速率推算进度取决于该软件的实现.

同时开启 Enable Seeking 后, 支持该功能的软件可以通过 SMTC 请求跳转. 两种模式下插件均按请求位置与已发布总时长的比例定位, 不再额外乘除播放速率, 保持原来的播放或暂停状态. 关闭位置控制后仍可共享进度; 关闭进度共享后不再允许跳转.

进度条是否显示, 以及点击或拖动如何操作, 由读取 SMTC 的软件决定. 空 Deck, 无有效时长或反向播放时不共享进度, 也不开放跳转.

#### 5. 如何查看运行日志?

日志保存在 DLL 同目录的 **SMTC.log** 中.

日志使用英语, 包含 UTC 时间, 条目类型和具体内容. 进度共享有效时, 每约 5 秒汇总有效查询次数, 发布尝试次数, 更新原因, 当前模式, 位置, 时长与播放速率. 更新原因计数可能重叠. 超过 10 MB 后自动清空并重新写入.

---

### BUG 汇报 😨

请详细描述遇到的问题: 具体行为, 是否可以复现, 并提供 SMTC 插件版本, Windows 版本, 复现步骤, 相关截图以及运行日志文件.

---

### 鸣谢 🙌

为此项目测试的朋友们

---

### LINK 🔗

<p align="center">
  <a href="https://space.bilibili.com/475951038">BILIBILI</a>
  ·
  <a href="https://cn.virtualdj.com/wiki/Developers.html">VirtualDJ 开发者文档</a>
</p>

---

### 捐赠 🧋

<p align="center">
  🥰请我喝奶茶喵 谢谢你喵🥰
</p>

<p align="center">
  <img width="420" src="Docs/imgs/qrcode.jpg" alt="请我喝奶茶">
</p>
