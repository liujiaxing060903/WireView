# WireView 全量通读·审计·修复报告（2026-11-08 深夜自主轮）

> 本报告由总代理在用户睡眠期间独立完成，按"独立可读"标准撰写。
> 范围：WireView 仓库（`local/customizations` 分支）全部源码。基线 HEAD=0df38f3（R4 完成态）。
> 最终状态：**13 个修复提交（0df38f3..dc3ab6d），18/18 CTest + 三组 Java 自测全绿，APK 构建通过，真机回归 7/7 通过（含 1 项以等效模拟方式执行）**。工作区干净。
>
> **附录（同日晚间用户实测反馈轮，dccf9d6..5f65d1e + 86cb836/8c3c478/fe20d8a）**：用户实测反馈三类问题并全部定位处置，见文末"附录：用户实测反馈轮"。

---

## 一、方法与执行概览

| 阶段 | 做了什么 | 产出 |
|---|---|---|
| 1 全量通读 | 10 个并行子代理，按模块簇 100% 通读全部源文件（Windows host 44 个源文件、Android 17 个类、protocol 双端、脚本与 Web UI），零抽样 | 10 份通读报告（架构速写 + Top 风险清单），会话工作区 `phase1/` |
| 2 冗余审计 | 10 个审计代理，高风险模块双视角（并发/生命周期、闭环/遥测、安全/字节级），交叉比对；单源主张由总代理逐条亲自落证 | 10 份审计报告 `phase2/`，去重后确认问题 ~45 项（P0×3 / P1×9 / P2×~18 / P3×~15） |
| 3 修复 | 波次 A：6 个修复代理（control/encoder/transport/wifi/session/input-edge）+ 总代理亲自实施 P0 安全协议修复；波次 B：2 个 Android 修复代理；随后 2 个对抗性复查代理审全部修复 diff，抓出 8 个修复引入的次级问题并全部修复 | 12 个修复提交 |
| 4 真机回归 | MI PAD 4（Android 11，USB + PC 热点 192.168.137.x），控制中心 UI 用 computer-use 自动点击，平板侧用 adb | 全部清单项通过（见第五节） |
| 5 交付 | 本报告 + LOCAL_NOTES 追加 + git 提交整理 | 本文件 |

审计与通读的中间产物（phase1/phase2/phase3 共 25 份报告）保存在会话工作区 `C:\Users\杨俊冉\.zcode\workspace\a96480b5-9ce1-48ab-979a-742386d08738\`，未入库（可要求归档）。

---

## 二、P0（安全）——AUTH v2 完整性层

### 2.1 发现（三家审计交叉确认 + 总代理逐字节复核）

1. **配对凭据明文上线**（wifi_link.cpp:149-161 旧行号）：配对成功后 32 字节长期凭据在 AUTH_RESULT(type17) payload 中**明文**传输。PBKDF2(PIN) 派生密钥只用于验证平板的 AUTH_RESPONSE MAC，从未加密任何字节——全链路（明文 TCP 39873）无加密通道。被动抓包配对交换即可永久冒充该平板。
2. **平板对 AUTH_RESULT 零验证**（ConnectionClient.java:379-393 旧行号）：type17 无 MAC、无签名，平板只看结果码。局域网任意攻击者可以：变体 A——免鉴权直连（伪造 `accepted` 直接进 HELLO，无需 PIN/凭据，无任何提示）；变体 B——静默自配对（伪造 `pairing_complete` + 任意凭据永久进白名单）。**整个挑战-应答对主动攻击者是装饰品**。
3. PC 从不向平板自证 + pc_uuid 每次握手明文广播——配合上述两条，冒充 PC 可截获用户在平板上的全部输入。

### 2.2 修复设计（`0df6de7`，两侧同改）

经包头 flag bit3（`PacketFlag::auth_v2`）能力协商，v2 层在旧端字节完全不变的前提下叠加：

- **result tag**：`tag = HMAC-SHA256(key, "WV1-result" || outcome || message || credentialView || noncePc || nonceTablet)`，key 在配对模式取 PIN 派生密钥、认证模式取存储凭据。平板恒时比较（`MessageDigest.isEqual`），验不过即断链。`accepted` 在"本方无任何密钥材料"时**无条件拒绝**（与对端版本无关，直接封死变体 A）。
- **凭据加密信封**：配对凭据以 `mask = HMAC(kdfKey, "WV1-wrap" || nonces)` 异或掩码上线——被动抓包不再白得凭据。
- **降级矩阵**（实测对齐）：
  - 新 PC + 旧平板：PC 收 schema1 响应 → 发与今天字节完全相同的旧格式（行为=现状，认证继续工作）；
  - 旧 PC + 新平板：`accepted` 且本方有凭据 → 放行（兼容）；`pairing_complete` → 拒绝并提示"电脑端版本过旧"（旧 PC 无法证明自己，明文凭据路径从此不可达）；
  - 新 + 新：tag + 掩码全量生效。
- **PBKDF2 10k → 600k 轮**：一次性交互成本（双端亚秒级），离线爆破 PIN 成本抬升两个数量级。存量配对不受影响（认证不走 PBKDF2）。
- 已知答案向量由独立工具（.NET HMACSHA256）计算，**同时锁进 C++ wifi_crypto_tests 与 Java ProtocolSelfTest**——任何一端漂移即测试失败。
- 平板侧凭据落盘从明文 SharedPreferences+Base64 改为 **Android Keystore AES-GCM 包裹**（`259e270`），首读自动迁移旧明文并重写（真机验证：存量配对迁移后认证通过）。

### 2.3 残余风险（如实声明）

- 主动降级（MITM 剥掉 bit3）可让认证模式回到"无 tag"状态——但响应 MAC 仍需真凭据，攻击者依旧进不来；配对模式不可降级（无 tag 即拒）。
- 6 位 PIN 的信息量上限仍在：录制配对交换后离线爆破 600k 轮 PBKDF2 约为 CPU 小时~天级。根除需 challenge/response 内嵌 X25519（列为后续项，不阻塞）。
- 真机证据：WiFi 会话三次建立/重建均出现 `WiFi authentication verified with MI PAD 4`（tag 验证链路真机闭环）。

---

## 三、功能性修复（按提交分组）

### `dbaa5f2` 悬空修复
上一轮会话引用了 `wifi_control::target_info()` 但漏提交实现（HEAD 单独检出无法编译）。补齐。

### `c841b9e` WiFi 基建
- **白名单并发写竞争**：三处无锁 `save`/`entries()` 纳入 g_mutex（配对线程 `upsert` 可能 realloc `entries_` 的同时 UI 线程在写文件——UB + 文件交叉写坏=凭据无声丢失）。
- **白名单/身份文件原子写**（temp+rename）：identity 撕裂曾会静默再生成 pc_uuid → 全部配对作废。
- **whitelist() 静态捕获陷阱**：改为 load_identity 显式构造；过早调用命中 assert + 空路径兜底，不再写脏文件。
- **mdns stop() UAF**（已知遗留#3）：上下文删除权收敛到完成回调（cancelled 标志 + 锁内 Cancel），stop 不再释放可能在飞回调仍解引用的内存；顺带修了 `DnsServiceFreeInstance` 泄漏与 stop 后迟到回调复活设备列表的竞态（复查轮补强）。
- **配对取消**（已知遗留#5 后端）：`cancel_pairing()` 原子标志 + 握手返回后丢弃/保存认证的语义修正（见复查轮：取消过晚时平板已存凭据，PC 侧也必须入白名单，否则留下永远认证失败的配对）。
- begin_pairing 补 pc_uuid 长度校验（对照会话路径）。

### `3d3b8bf` 控制中心
- **定时器撞号（P1，双审计+总代理三重确认）**：`kEdgeRemapTimer=5` 与 `kWifiDiscoverTimerId=5` 同窗口同 ID，WM_TIMER 的 wifi 分支永远先命中——**R2 拓扑重挂（WM_DISPLAYCHANGE→边缘映射重应用）是无条件死代码**，且任何显示变化把 WiFi 发现周期永久改成 1.2s。改 ID=7 + static_assert。
- **全桌面重绘风暴（P1）**：从未创建的 SimpleWindow 使 6 处 `InvalidateRect(simple_window=nullptr)` 变成全桌面失效，流式期间每 500ms 一轮。死代码删除 + 失效区改指主窗口。
- PID 复用误判"存活"卡死 VDD 恢复（加映像名比对）；STILL_ACTIVE 二义性（改句柄终止态判定）；三个 ini 原子写；自定义模式编辑器 500ms 重弹循环（会话级一次标志）；**VDD 创建在途点复制屏幕留孤儿副屏**（入口守卫）；recreate/copy 意图互斥丢弃可见化；**旋转重启被抢先时改为停旧起新**（失败可见）；重入 start_session 可见化；**WiFi 会话启动即置传输标志**（首轮询窗口内换向不再被陈旧 USB 标志误导， ruling#6 边角）；TaskbarCreated 托盘重注册；Job 加 BREAKAWAY_OK（配合 session 侧 adb 逃逸）；停止会话补 KillTimer。
- Web UI：配对进行中显示可用的"取消配对"按钮（接 `cancel_pairing()`）。

### `cb47c57` 编码器
- **自适应码率断链（P0 级功能缺陷，三重确认）**：governor 把目标降进原子量后，编码主循环从不消费 `consume_bitrate_command`、`set_bitrate` 全仓库零调用——拥塞时码率永远停在初始 24Mbps。真机吞吐 3.05Mbps 的旧观察被确认为 LowDelayVBR 静止画面欠冲（低于 governor 6Mbps 下限是决定性反证），LOCAL_NOTES 原记载"帧率优先生效"系误记（已在 LOCAL_NOTES 更正）。修复：NeedInput 分支消费 + 安全桌面重建后重放 + governor 变更加日志。
- 编码主循环空闲超时（MFT 静默时不再永久自旋，P1）；NV12 槽簿记 deque 化（时间戳未命中不再泄漏槽位到 3 槽耗尽自杀，P2）；MENonFatalError 双层修复（next_event 不再提前抛出；非致命错误记日志+关键帧刷新，连续 10 次才升级，P2）；force_keyframe 三处策略统一为"记日志重试、整个 GOP 连拒才致命"，且**自然 IDR 输出即清零失败计数**（复查轮修正：永远拒绝 force hint 但能出 IDR 的 MFT 不能杀）；统计标签 `bitrate=`→`measured=`（旧名已造成一次误诊）。

### `f868747` 传输层
- **sender 线程未保护窗口（P1）**：线程启动 200 行后 try 才开始——VDD 在重连预检后瞬间消失（重连机制为之设计的场景）会 std::terminate 整个 host。前移解析/注入器构造。
- 队列溢出丢最旧 **video_chunk** 而非队首（`start_stream` 曾可在 67ms 内被挤出→整场黑屏）；input_error 重抛移到 best-effort stop_stream 之后；**input 线程死亡传播**（on_frame 检查，触控静默失灵不再盲跑到时长耗尽——真机断连恢复中实测生效）；Surface 样本 512 滑动窗口+1s 节流（长会话内存/CPU 退化）；WinUSB 空闲 10s 超时不再误判断线；删除死代码 close()；单发 WiFi 入口补开 governor。

### `50a3f5d` 会话/构建
- 主循环/探测/notify 的 16 处 adb 调用全部换 `run_capture_bounded`（此前只有 reverse 监督线程有界）；**bounded 变体并发排空管道**（复查轮：dumpsys 大输出写满管道会触发 15s 误杀）。
- **adb server 连坐击杀**：adb 子进程以 CREATE_BREAKAWAY_FROM_JOB 逃逸 job（host 本体仍在 job 内），停止会话不再杀掉系统 adb server 殃及 Android Studio。
- broker 重试判定从异常文案子串改为类型化异常；preview 时长钳 1800s 契约；quote_argument 完整转义；parse_unsigned 防回绕；IPv6 端点解析期明确报错；write_status 失败留痕；wireview_host 补 /utf-8；check-env 改 vswhere；打包版本锚定；启动脚本日志 GUID 不再按秒增生；构建目录候选探测。

### `a136aa4` 输入/边缘映射
- **触控状态机 commit-on-success（P1）**：原"先改状态后校验"使触点超限/设备失败/SendInput 失败三条路径都留下幽灵触点（常驻重发或永久 wedged）。重构为暂存校验→成功才提交；注入失败尽力 UP 全触点后清空。
- hover 对按下触点按 move 更新（原被跳过→触点冻结）。
- **spawn_hook 环境块（P1）**：原两变量环境块让 LBM .NET 守护丢失 SystemRoot/TEMP（字体/注册表/加密初始化高风险）→ 继承父环境（剔除 LBM_HOOK_ 旧值）。
- 邻接判定要求另一轴重叠（对角/分离摆放不再生成"穿越空隙"链接）；事件连接 drain 线程（DuplicateHandle 副本 + CancelSynchronousIo 唤醒——复查轮把唤醒方式从可能命中复用句柄值的 CancelIoEx 改为线程句柄）；窗口列表 count 截断 + 命令字节自查。

### `259e270` Android 连接层
- **USB 探活误判（P1）**：probe 连接只证明 adbd 在场（adb reverse 本地 accept 与 PC 死活无关）→ host 死时 WiFi 被永久饿死。probe 现在预读 HELLO（800ms）确认 PC 真在隧道后侧，暂存包进主循环；失败落 WiFi accept。
- `setReadTimeout(0)` 挪到 HELLO_ACK 之后（ hostile 对端曾可把连接子系统钉死在无界读）；数据期 25s 空闲超时走重连（半开链路不再挂 TCP keepalive 的两小时）。
- 关 WiFi 开关拆掉活动 WiFi 会话（开关语义不再半开）；配对类失败提示 3s 保持窗（不再被 waitingStatus 即刻覆盖）；WifiServerConnector bind 失败落诊断日志；close() 先关 socket 再有限等待（主线程不再 4s 阻塞）；readPacket 去三重复制；concat 去重（`c3e7504`）。
- **凭据静态加密**（见 2.2）。

### `81b3440` Android 解码/UI
- **会话中屏幕中央的隐形可点 WiFi 按钮（P1）**：流式时只藏了三行，wifiRow 残留在 Surface 之上参与 hit-test——中央触摸被截获并误切传输开关。整个 connectionPanel 隐藏。
- onStatusChanged 兑底折叠吞错（解码失败/WiFi 反馈/停止通知现在原文显示）；解码队列到达侧预检丢帧（拒绝策略不再可能挤掉 setSurface/startStream/stopStream 控制任务）；自愈重建指数退避 + 重建后等关键帧并请求（原 P 帧喂新 codec 花屏到下个 GOP）；窗口管理操作全部走 BadToken/IllegalArgumentException 安全封装；窗口列表局部解析原子提交；setSurface(null) 同步排空；stopStream 双调去重；connection volatile；PIN 对话框超时 dismiss。

### `cb8b66c` + `dc3ab6d` 复查轮 + 真机轮
8 个修复引入问题的修复（非 ASCII 计算机名 tag 必失配→双端真 UTF-8 + Java 按原始线上字节验签；bounded 读管道；首帧 IDR 升级误杀；probe 解析 RuntimeException 杀连接线程；取消 split-brain；mdns 迟到 upsert；drain 唤醒句柄复用；旋转误升级探测会话）+ 状态行传输标签（WiFi 会话曾显示"ADB/TCP"）。

---

## 四、已知遗留清单的处置（逐项影响分析）

| # | 遗留项 | 处置 | 理由 |
|---|---|---|---|
| 1 | USB↔WiFi 热切换仲裁完整版（管线不重建） | **确认遗留** | 涉及通道代次/整帧写/管线保活的系统性重设计；当前"断开重连"语义经用户实测认可（"能恢复，非常强"），本轮真机再次验证恢复路径。夜间重构传输核心的风险 > 收益（黑屏 1-2s vs 可能引入劈帧/死锁）。建议单独立项。 |
| 2 | mDNS 热点网络发现 | **确认遗留（已复测）** | 本轮真机复测：热点开启、平板 NSD 广播、控制中心"正在搜索平板（局域网）"持续，仍无发现。防火墙 UDP 5353 已放行；mdns_browser 本身的 UAF/泄漏已修但发现不通疑似 Windows 热点网段组播转发问题（环境层）。last_ip 兜底实测有效（三次拨号成功）。 |
| 3 | mdns stop() UAF | **已修**（c841b9e + cb8b66c 补强） | 见第三节。 |
| 4 | whitelist() 静态捕获陷阱 | **已修**（显式传路径） | 见第三节。 |
| 5 | wifi-select 无取消命令 | **已修**（后端+UI） | cancel_pairing + 设置面板取消按钮；语义含"取消过晚"修正。 |
| 6 | RECEIVER_EXPORTED 假 PIN 注入 | **维持原取舍** | 签名权限会同时挡掉 `adb shell am broadcast`（USB 免 PIN 投递依赖它，shell 不持有应用签名权限），破坏拍板过的免 PIN 流程。攻击面=同平板恶意应用 + 已授权 USB 调试前提下的 DoS（无凭据泄露；v2 层上线后伪造结果码已无法获得会话）。收益不抵破坏。 |
| 7a | >2 显示器不在布局内 | **确认遗留** | display_layout 恰好 2 屏假设；放宽需 N 路径定位改造（A10 评估 30-40 行）+ **无法在当前硬件验证**（本机仅主屏+VDD）。盲改 SetDisplayConfig 路径风险高。 |
| 7b | SendInput 对提权前台被 UIPI 静默丢 | **确认遗留（系统层）** | Windows 完整性级别机制；绕过需 UIAccess 签名（分发红线）。 |
| 7c | 鼠标注入忽略 buttons（右键当左键） | **确认遗留（分析在案）** | F8 产出完整设计（inject_mouse_record 记边沿 + RIGHT/MIDDLE 并入同批 SendInput）；但 tool==mouse 的记录在当前平板触控路径上实际罕见（finger/stylus 为主），且改注入帧格式需真机多轮手感验证，夜间不宜动。设计已备，下轮实施。 |
| 7d | 1×1 Surface 杂散触摸注入 (0,0) | **确认遗留（上游）** | 影响极小；修复需 Android 侧事件过滤，与触控状态机改动耦合，留待 7c 一并做。 |
| 7e | IME 全屏窗口吞点击 | **确认遗留（系统层）** | MIUI 系统行为。 |
| 7f | 采集节拍跳槽不追赶 | **确认不修**（A3 论证） | 补发突发被 MFT NeedInput 节流、时间戳均匀、实测 59.99fps/15ms 无用户可见异常；动节拍=动全链延迟地基，风险>收益。 |
| 7g | 启动期约 56 帧丢帧 | **确认遗留** | 全部在首帧前（稳态 0），与 OMX 冷启动特性相关。 |
| 7h | picture-order 低延迟档未验证 | **确认遗留** | 本机首选档（null）已达标（P50 25ms）。 |
| 7i | GOP 拉长与 PTS-vsync 锁相观察项 | **确认遗留（观察项）** | 无故障证据。 |

**新确认为遗留（本轮发现、未修）**：
- **旋转翻转被设备画像联动吞掉（P3，预置交互）**：平板物理横放时点侧边栏换向，会话正确重启且保持传输，但设备画像自动选择按平板当前姿态重新选了原尺寸，翻转未生效。用户此前验证成功的场景（手持翻转）不受影响。机制：flip 改方向选择器 vs 画像改分辨率选择器的时序竞争。建议：rotate 请求置一次性"抑制画像自动选择"标志。
- ADB 通道零认证（平板侧恶意应用可竞连 adb reverse 端口）：A5 评估本机冒充无权限增益，真实增量威胁在平板侧；修复需 USB 通道复用 WiFi HMAC（无配对设备的首次连接无凭据可用，与免配对首连矛盾）。**记录威胁模型，留待设计**。
- publish_web_state 高频全量序列化（P3 性能，app.js 已去抖渲染）；edge_mapping UI 线程最长 15s 阻塞（守护进程不应答时，建议移后台线程）；UAC 提权等待阻塞 UI 最长 30s——三件均为体验级，无正确性损害。

---

## 五、验证结果

### 构建与自动化测试（每轮修复后全量回归）
- Windows：`scripts/build-windows.ps1` → **18/18 CTest 全绿**（最后一轮 + 标签修复后复跑）。
- Android：三组 Java 自测（协议/传输/诊断日志）**全绿**；新增 v2 向量与往返用例在两侧套件内；`assembleDebug` APK 构建通过并真机安装。
- 编码红线：wireview_host 全部改动新增行纯 ASCII（grep 复核）；协议测试常量双端一致（.NET 独立向量锁定）。

### 真机回归（MI PAD 4 / Android 11 / USB + PC 热点）

| 清单项 | 结果 | 证据 |
|---|---|---|
| USB 复制：点复制→画面→触控回传 | **通过** | DISPLAY1 流 59.99fps、首帧 111-124ms、Surface P50 25ms、解码 0 错误；CLI 定长会话优雅退出统计：`Android input: packets=8 \| records=8 \| injected=8 \| injectionFailures=0` |
| USB 扩展：VDD 创建→扩展桌面 | **通过** | vdd_broker 启动、DISPLAY20 创建 1920x1200@240、流式首帧 143ms |
| WiFi 复制（无 USB 模拟*） | **通过** | transport=wifi、首帧 118ms、`WiFi authentication verified with MI PAD 4`（**AUTH v2 真机闭环**，存量明文凭据经 Keystore 迁移后认证通过） |
| WiFi 扩展 | **通过** | DISPLAY20 + transport=wifi、首帧 136ms |
| 侧边栏横竖屏切换 | **通过（含保留项）** | 侧边栏按钮→会话重启→**transport 保持 wifi**（ruling#6）→新会话首帧 116ms；方向翻转本体被画像联动吞掉（见第四节新遗留，预置问题） |
| 断连恢复（关热点→重开） | **通过** | 热点关闭→`input thread is gone; ending the stream session`（**F3 修复实战生效**）→recovering→重拨→热点恢复+平板重连（同 IP）→streaming+重新认证。"平板 WiFi 不会自动重连热点，需 svc 重启 WiFi"为设备/系统行为，PC 侧重试全程正确 |
| R4：关闭=彻底退出 | **通过** | 开关开启→点 ✕→wireview_control 及 job 内全部子进程退出，exit-request 日志 |
| R4：空闲退出 | **通过** | 1 分钟档（经持久化 ini 注入——UI 最小档为 5 分钟）→无会话 75s 内进程自行退出，"空闲超时"日志；测试后默认值已复位 |

\* 物理拔线不可行（无人值守）；按预案以等效模拟执行：隐藏两处 adb.exe（构建目录 + PATH 的 SDK 目录，均已恢复）+ 杀 adb server，使 PC 侧设备检测判定"USB 不在场"，控制中心按设计走 WiFi。测试后 adb 全部恢复，`adb devices` 正常。

### 真机轮顺带验证的夜间修复
- F3 input 线程死亡传播（断连瞬间日志实证）；F6 USB 探活预读（CLI 会话平板端 HELLO 预读接管）；transport 标签修复前实测确认了 bug 本体。

---

## 六、关键自主决策（假设与权衡）

1. **遗留#1/#7 系列多数维持**：统一标准=「无正确性损害 + 修复需不可验证的重构或与拍板语义冲突」。全部留了影响分析（第四节）。
2. **WiFi 真机测试用"隐藏 adb.exe"模拟无 USB**：完全可逆（两处 mv 回原位），不触碰平板系统设置；比 adb kill-server 更彻底（PATH 里的 SDK adb 会兜底重启 server，第一次尝试实证了这点）。
3. **PBKDF2 600k 无版本协商**：新 PC×旧平板的**配对**必然 MAC mismatch 失败（两侧常数不同）——接受，因为配对本来就是双端同仓库同步发版的交互事件，且旧平板对 v2 PC 的配对本就被新平板侧规则拒绝；**认证**（存量配对）完全不受影响（不走 PBKDF2）。真机验证了存量配对升级后继续可用。
4. **配对取消的语义修正**（复查轮）：取消过晚（平板已存凭据）时 PC 也入白名单并如实提示——比"PC 丢弃"更安全（否则留下永远认证失败的僵尸配对）。
5. **旋转重启遇到探测会话**：不升级、不重启探测，记录方向待下次真会话生效（探测不承载方向语义，静默升级成完整会话是行为突变）。
6. **修复提交策略**：13 个逻辑独立提交，每个提交对应一组同模块修复；中央构建在波次 A/B/复查/真机共回归 5 次全部通过后再提交（工作树在提交点均为已验证状态）。
7. **未做 mDNS 根因排查**：真机确认仍不通后，因需组播抓包/网卡级诊断（深夜不可为），按遗留记录。last_ip 兜底三次实测可靠。

---

## 七、提交清单（0df38f3 → dc3ab6d）

```
dc3ab6d fix(control): show the real transport in the status line
cb8b66c fix(review): follow-ups from the adversarial diff review
c3e7504 refactor(android): share one byte-concat helper across the auth classes
81b3440 fix(android): hidden panel touch trap, decoder queue and rebuild safety
259e270 fix(android): USB probe validation, read timeouts, WiFi teardown, keystore at rest
a136aa4 fix(input): touch commit-on-success, hook environment, adjacency
50a3f5d fix(session): bounded adb calls, adb-server survival, CLI and script hygiene
f868747 fix(transport): terminate window, control-packet loss, telemetry window
cb47c57 fix(encoder): wire the adaptive bitrate governor into the encoder + robustness
3d3b8bf fix(control): timer collision, repaint storm, VDD races and lifecycle hardening
c841b9e fix(wifi): whitelist locking + atomic writes + explicit init + mdns UAF + pairing cancel
0df6de7 fix(security): AUTH v2 integrity layer - result tag, masked credential, mutual proof
dbaa5f2 fix(wifi): add missing target_info() accessor used by control_main device card
```

统计：43 文件，+2336 / -586 行。用户拍板的 10 项决定语义零变更（旋转重启 WiFi 保持、UIA 不加回、会话并发 1 等均经真机复核）。

---

## 八、给用户的晨读建议

1. **最值得看的两个修复**：AUTH v2（第二节，安全性质变）与自适应码率断链（第三节 `cb47c57`，一个"功能存在但从没工作过"的实锤）。
2. **LOCAL_NOTES 需要你知道的一处更正**：R3 验收记录里"静止画面码率自适应降至 3.05Mbps（帧率优先生效）"是误记——那是 VBR 欠冲，governor 从未接线（现已接线）。
3. **体验差异提示**：WiFi 会话数据期现在有 25s 空闲超时（PC ping 保活不会误触）；平板凭据已迁移到 Keystore 加密存储；配对必须双端同版（旧 PC 配新平板会被平板拒绝并提示升级）。
4. **建议下轮**：USB↔WiFi 热切换完整版立项；旋转×设备画像联动修掉；鼠标右键注入（设计已备）；mDNS 热点组播根因排查。

---

## 附录：用户实测反馈轮（同日晚间，dccf9d6..5f65d1e）

用户真机实测反馈三类问题，全部定位处置（每项含真机验证）：

### 反馈 1：设置面板无法滚动，"空闲 N 分钟退出"无法设置 —— 已修复 ✓
`.settings-panel` 无 max-height/overflow，内容超高被 body 裁掉。修复为内层 `.settings-panel-body` 滚动容器（终审复查指出第一版 `overflow-y:auto` 直接放面板上会把绝对定位在面板外侧的赞赏码弹层一并裁掉，二版改为面板不滚动+内层 body 滚动，赞赏码弹层移出滚动容器）。**用户复测：可以滚动了 ✓**。

### 反馈 2：复制模式延迟高 + 颜色刺眼 —— 两个独立根因，分别处理 ✓/方案已备
- **颜色刺眼（已修复 ✓，用户确认"颜色正常"）**：HDR 开启时桌面复制给 FP16 scRGB，旧 tone-map `saturate(v × 200/80)` 把普通窗口白色（DWM 按 SDR 亮度滑条提升到 scRGB 2.0~3.0）全部截断成纯白。改为 exp 曝光映射 `1-exp(-max(rgb,0)×0.85)`（高光平滑滚降、永不截断、负分量防护）。
- **画面"卡顿"（根因实锤：HDR+MPO 系统层限制，修复路线已调研完毕）**：用户精确描述"整体画面落后、鼠标实时、拖动窗口松手前不加载"——PC 侧数据证明采集层每秒拿新帧（desktopUpdates≈encoded），**Windows HDR 开启时部分窗口走 Independent Flip/MPO 硬件叠加平面绕过 DWM 合成，Desktop Duplication 采不到它们**。用户关 HDR 实验确认："关完 HDR 后就好了"✓。行业现状：**Parsec 官方不支持 HDR（错误码 15106）、spacedesk 官方声明不支持 Windows HDR**——均为回避而非修复。
- **修复路线（两个子代理调研+设计，报告在会话工作区 phase4/H1、H2）**：
  - 立即可试（5 分钟，需管理员）：注册表禁用 MPO——管理员 CMD 执行 `reg add "HKLM\SOFTWARE\Microsoft\Windows\Dwm" /v OverlayTestMode /t REG_DWORD /d 5 /f` 后注销/重启；24H2 无效则追加 `reg add "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers" /v DisableMPO /t REG_DWORD /d 1 /f`。副作用：任务栏缩略图等叠加功能失效。恢复 = `reg delete` 同键。
  - 正式方案（下一轮实施，约 6-9 人日）：HDR 激活时采集层切 **Windows.Graphics.Capture**（Sunshine master 默认路径，社区实证 WGC 强制脱离 Independent Flip 采得全部内容；HDR 下输出 FP16 scRGB 与现有 tone-map 管线零改动衔接；设计文档已备：共享 D3D 设备零拷贝、保持 60fps 时刻表、光标继续走系统合成）。已知代价：Win11 非打包应用无法去除采集黄边框（系统能力限制）。
  - 辅助：微软官方键 `OverlayMinFPS=0`（稳 MPO 减路径切换冻结）；升级 NVIDIA 610+ 驱动。

### 反馈 3：拔线/瞬断后无法自动恢复 —— 三层根因全部修复 ✓（用户终测"能够自动恢复"）
回归对比定位（用户质疑"一开始好好的、WiFi 能恢复 USB 不能"是关键指引）——R3 的"host 进程内重连"绕过了 session 编排：
1. **静态桌面死链不检测**（dccf9d6）：ping 只随帧发，静止画面无流量，拔线后 host 不知情、平板读超时撞僵尸 backlog。修复：sender 独立 5s 心跳 + 编码循环 should_abort 轮询 + 平板 HELLO 12s 界。
2. **reverse 映射无人重建**（86cb836）：拔线清掉 adb reverse 后，host 内部重连永不退出 → session 编排（重设 reverse）轮不到。修复：连续 6 轮 accept 超时（约 30s）host 以退出码 2 结束 → session 识别后回主循环重设 reverse → am start → 重启 host——**恢复到与上游/R3 前一致的语义**。
3. **adb server 脱节自愈**（8c3c478）：实测 adb server 会与 USB 链路脱节（devices 列表空但 TCP 会话健在），设备检测信任假象永久卡在"等待设备"。修复：devices 空 5 轮 `adb reconnect offline`、10 轮 `kill-server` 强制重枚举。
辅助修复（fe20d8a）：bounded 捕获在拉起常驻 adb server 时 reader 线程永久挂死（server 继承管道句柄）——先 CancelIoEx/关读端再 join；监督线程加诊断日志（`%LOCALAPPDATA%\WireView\logs\reverse-supervisor.log`，状态翻转时记录）。
**用户终测："能够自动恢复" ✓**。

### 终审复查（按用户要求双代理：全量 + 重点专项）
全量 46 文件逐一过（V3）+ 恢复链/HDR/滚动/标签四专项（V4）。产出 1 P2 + 4 P3 + 1 回归，全部当轮修复（5f65d1e）：tone-map 负 scRGB NaN 防护、CLI WiFi 路径环境变量 ACP 读改 UTF-8（与主路径一致）、bounded 读管道 CancelIoEx 顺序、touch [down,hover] 覆盖丢按下、设置滚动裁剪赞赏码弹层的结构修正。**全量+专项双通过后部署**。

### 验证状态（反馈轮末）
18/18 CTest + 三组 Java 自测全绿（每轮修复后）；真机：颜色 ✓、滚动 ✓、拔线自动恢复 ✓、HDR 卡顿=系统限制（MPO 实验命令已交付用户，WGC 路线文档齐备待下一轮实施）。

### 新增已知项
- HDR 开启时画面更新缺失：系统层限制，Phase1 提示代码与 WGC 路径留待下一轮（本报告文档化完毕）。
- adb server 脱节：自愈已加，长期观察。
- 用户感知"有线不如 WiFi 流畅"：USB 恒 24Mbps vs WiFi governor 自适应的码率差异 + adbd 转发抖动，未量化处理，列为观察项。
