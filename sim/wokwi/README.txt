Wokwi 固件运行仿真

范围
  当前只验证固件在模拟器中启动、显示、按键、USB 配置、合成 BLE 聚合、
  夹具应答、暂停/恢复与持续调度，不连接后端。
  实际云端运行结果及截图审查以 docs/collector/wokwi-acceptance-20261023.txt
  的最终报告为准；本文不预先声明仿真通过。
  为在云端单次运行时限内完成场景，独立仿真配置使用 80 MHz CPU、100 Hz RTOS
  tick；真实固件仍为 160 MHz、1000 Hz。业务毫秒计时和 30 秒心跳周期不变。
  截图在状态变化后等待 1 秒，覆盖 LVGL 唤醒和模型中的分块刷新。

执行（项目根目录）
  python tools/wokwi.py build
  python tools/wokwi.py lint
  python tools/wokwi.py smoke --token-file <本地Token文件> --output logs/wokwi/smoke-new
  python tools/wokwi.py flow --token-file <本地Token文件> --output logs/wokwi/flow-new

  build 生成独立的 build/wokwi/ 镜像及 simulation-manifest.json，明确标记
  WOKWI_ONLY_DO_NOT_FLASH。它复用项目 passport 入口与 ESP-IDF 5.5.3，
  不覆盖普通固件的 sdkconfig。此入口当前使用 macOS 隔离工具链；跨机须
  核对目标构建环境，指定 CLI 本身不代表 Windows 原生构建已经可用。
  lint 校验 diagram；smoke 检查启动、空状态页面及配置窗口，共五张图（01 至 05）。
  flow 先经模拟串口发送固定合成配置，再检查三个合成设备、两次心跳/上传 ACK、
  暂停和恢复，共八张图（01、05 至 11）；02 至 04 由 smoke 覆盖。
  flow 等待第二次正常周期心跳，没有缩短 30 秒心跳间隔。
  默认场景超时 150000 ms，可用 --timeout 调整。每次选择新的 --output；
  已有非空目录会被拒绝，防止覆盖证据。日志、截图和 result.json 写入该目录。

工具与凭据
  当前独立 CLI 为 v0.29.0，位置是外层 Passport 工作区下的
  .toolchains/wokwi/v0.29.0/wokwi-cli。
  工具优先使用 --cli，其次 PATH，再使用上述位置；跨机可显式指定：
  python tools/wokwi.py lint --cli <本地wokwi-cli或wokwi-cli.exe>
  python tools/wokwi.py flow --cli <本地CLI> --token-file <本地Token文件> --output <新的日志目录>
  本工具不自动安装依赖，不修改全局 PATH、默认 Python、系统证书或网络设置。

  Token 文件可只含 Token，或 WOKWI_CLI_TOKEN=... / export WOKWI_CLI_TOKEN=...
  赋值。程序只解析文本，不 source、不执行文件命令；也可沿用当前进程的
  WOKWI_CLI_TOKEN 环境变量。Token 只传给 CLI 子进程，不写入场景、摘要或
  固件；不要放进项目源码、日志、交付包或聊天。跨机在目标电脑单独取得。
  CLI 将仿真固件/ELF 上传到云端；只能使用本仓库的合成配置，不能嵌入真实秘密。

构建与证据一致性
  build 保存源文件和固件 SHA-256。smoke/flow 运行前核对源码、配置及镜像，
  任一摘要改变都会要求重新 build，防止拿旧镜像验证新代码。
  运行结果包括截图完整性、串口标记、运行故障和心跳/上传 ACK 计数；
  图片存在仍不等于画面正确，最终报告还需记录实际截图审查结果。

从交接包恢复预构建仿真产物
  归档内实际路径为 evidence/logs/simulation-only/；将其内容恢复到
  source/build/wokwi/。仅把 wokwi-sdkconfig.txt 重命名为 sdkconfig，
  其余名称和子路径保持不变，包括 bootloader/ 和 partition_table/。
  该别名避免被归档工具的 sdkconfig 排除规则过滤；恢复后仍须通过完整摘要校验。
  从 source/ 执行上述 smoke/flow 命令，指定目标机 CLI、Token 文件和新日志目录。
  交接材料准备不代表已传输、目标笔记本已验证或已刷机；具体步骤见
  docs/collector/wokwi-handoff-20261023.txt。

真实代码与替代边界
  保留真实 collector_core、控制器、UI、USB 配置解析和 120 秒配置窗口；
  模拟串口分段投递配置，不证明物理 USB 链路已验收。RECEIVED_RAM_ONLY
  只表示接收到 RAM 配置，后续 AUTH_CONFIG_APPLIED 是夹具配置已应用。
  CONFIG_COLLECTOR_WOKWI 才会编译本目录替代模块；模拟按键 GPIO4/5/6、
  ILI9341 显示与真实 Passport 接线及显示器不同，仿真镜像禁止刷机。
  BLE 广播是三个合成来源；不运行 NimBLE、不发现周边设备。

  网络模块不初始化 Wi-Fi、DNS、SNTP、HTTP 或 TLS，日志明确 NETWORK=FIXTURE。
  col_wifi_ready 仅表示夹具已配置；192.0.2.1 是 TEST-NET 占位地址。
  UTC 固定从 2026-10-23T00:00:00Z 随 esp_timer 推进，不证明真实校时。
  UI 网络状态及通用 WIFI_CONNECTING 日志不能解释为已经联网。

  fixtures/config.json 只含固定六项合成值：Wokwi-GUEST、空密码、
  https://fixture.wokwi.invalid、Wokwi、43 个 W、SYNTHETIC-NOT-A-CERT
  占位 PEM。任一字段改变均拒绝；该 PEM 不是有效证书，不用于 TLS。
  夹具配置和 ACK 遵循固件实际协议，心跳/事件先检查字段及范围，事件复用
  col_event_valid。WOKWI_FIXTURE_ACK devices=3 表示三个合成来源经过采集链路。
  本次启动最多保留 256 个事件 ID；重试计 duplicates，满后明确返回
  OBSERVATION_LIMIT（409）。清除/重新配置凭据不删除夹具历史，重启模拟器
  创建新会话。持续满速采集约 170 秒会到达此上限，正常场景建议不超过 120 秒。

独立主机回归与未验事项
  python tools/test_wokwi_fixture.py --sanitize
  该命令编译实际夹具、核心和 cJSON，不启动 Wokwi，也不执行后端测试。
  真实屏幕、按键、电池、物理 USB、BLE 射频、Wi-Fi/TLS、无线共存及长时运行
  都仍需实机验收。普通固件构建、主机测试、云端仿真和实机结果须分别记录。
  可选 HTTPS 材料保留在 optional-https.txt，不是当前 flow 的依赖。
