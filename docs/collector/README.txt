Passport BLE 采集器文档索引
日期：2026-10-23（Asia/Shanghai）

本项目位于 passport/projects/ble-collector，使用独立功能分支 feature/ble-collector。
原 passport/firmware 保留为基线仓库；多种固件项目应分别放在 projects/ 下，
独立管理源码、build、logs、artifacts，复用工作区隔离 ESP-IDF 5.5.3。

阅读顺序：
0. acceptance-20261023.txt 与 resource-budget.txt：实际验证结果、资源上限与已知限制。
1. usb-configuration.txt：本机临时接入、界面操作、TLS 前置条件与配置清除。
2. laptop-handoff-20261023.txt：发布前归档快照和匹配镜像如何恢复到笔记本。
3. device-acceptance.txt：D01–D12 真实设备与网页验收表，默认全部 NOT RUN。
4. collector-config.example.json：空秘密模板，仅用于理解六字段格式。

构建与主机测试的最终结果以交付验收报告及 evidence 目录中的实际日志为准。
本目录的操作步骤不表示命令已经执行，也不授权提交、推送、刷机、擦除或 eFuse。
上游 docs/development/engineering/windows-native-handoff.txt 仍可用于 Windows
环境盘点，但其旧目录、Git 获取方式不能替代本次交接文档。

应用边界：
- 使用已有 BSP、无线基础初始化、ESP-IDF 和 NimBLE；应用入口改为独立采集器。
- 被动 BLE 观察，不连接/配对/GATT，不上传原始广播，不计算距离或合并跨节点身份。
- 最多 16 个聚合地址、32 个待确认事件、300 秒 TTL。
  每批最多 32 条且正文小于 8 KiB（包括结尾 NUL 的缓冲），满足协议 32 KiB 上限。
  聚合与队列有 64 KiB 编译约束；UI/TLS/线程栈等总资源须另行实测。
- 队列和凭据仅保存在 RAM，重启丢失；32 条是上限，不是断网三分钟零丢失保证。
- 服务端每节点仍有 5000 个设备标识、200000 条历史观测限制。
- 字库以项目收录字形为准；动态广播名不支持的字形显示 [?]，上传 UTF-8 不因此改变。
- 真实扫描、TLS、射频共存、屏幕/三键和长稳只有设备验收后才能作结论。
