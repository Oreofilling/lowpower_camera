# NeoLink — NE101 BLE 工作流程 Demo

| 项目 | 内容 |
| --- | --- |
| 所属项目 | NeoLink |
| 演示范围 | NE101 BLE Peripheral/GATT 最小闭环 |
| 更新日期 | 2026-10-27 |

这个 demo 只用于理解 BLE 的最小闭环，不会接收或保存任何 Wi-Fi、CAT1、Wi-Fi HaLow 或 Ethernet 配置。请勿用它执行真实通讯配置。

NeoLink 产品流程见 [`NEOLINK_PRODUCT_FLOW.md`](./NEOLINK_PRODUCT_FLOW.md)；正式 BLE 协议见
[`CT_PROV_PROTOCOL.md`](./CT_PROV_PROTOCOL.md)；设备开启指引二维码见
[`CT_DEVICE_QR_SPEC.md`](./CT_DEVICE_QR_SPEC.md)。

## 工作流程

```text
设备进入 CONFIG mode
        |
        +--> 原 softAP / 192.168.1.1 继续运行
        |
        +--> BLE 广播 NE101-BLE-XXXX
                  |
手机扫描并连接 ---+
        |
发现 NUS Service、RX 和 TX 特征
        |
先订阅 TX Notify，再向 RX 写入 ping 或 info
        |
设备解析命令并通过 TX Notify 返回结果
        |
手机断开后，设备自动恢复广播
```

## GATT 定义

| 对象 | UUID | 属性 | 方向 |
| --- | --- | --- | --- |
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | Primary | - |
| RX | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Write / Write Without Response | 手机 -> 设备 |
| TX | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Read / Notify | 设备 -> 手机 |

支持的 UTF-8 文本命令：

| 写入 RX | TX 返回 |
| --- | --- |
| `ping` | `pong` |
| `info` | `model=NE101`（实际取设备 model） |
| 其他内容 | `ERR:use ping/info` |

## 编译与烧录

仓库配置由 ESP-IDF 5.1.6 生成，请先进入对应的 ESP-IDF 环境：

```sh
. /path/to/esp-idf-v5.1.6/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

请按实际串口修改 `/dev/ttyUSB0`。

## 用手机验证

1. 按键让 NE101 进入 CONFIG mode。
2. 打开 nRF Connect 或 LightBlue，扫描并连接 `NE101-BLE-XXXX`。
3. 展开上述 Service，在 TX 特征上启用 Notify。
4. 在 RX 特征选择文本/UTF-8，写入 `ping`。
5. TX 应收到 `pong`；串口同时打印连接、订阅、RX、TX 和断开日志。
6. 断开连接，确认设备重新出现在扫描列表中。

若没有先启用 Notify，也可以直接读取 TX；初始值为 `ready`，每次 RX 写入后会更新。

## 和正式 CT-Prov 的关系

这个 demo 已覆盖 Peripheral/GATT Server 的骨架：广播、连接、服务发现、双向消息和断线后恢复广播。正式实现应替换文本命令层，增加 CT-Prov 分片、JSON 请求/响应、设备信息、通讯能力、`txid`、异步状态和模式专属错误码。

Demo 不解析 Guide QR，也不判断 Wi-Fi、CAT1、Wi-Fi HaLow 或 Ethernet 能力。BLE/AP/CT-Disc 扫描聚合、空结果后的扫码指引和动态配置页面属于 NeoLink 工具侧能力。
