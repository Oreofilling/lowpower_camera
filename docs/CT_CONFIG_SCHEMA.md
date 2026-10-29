# CT-Prov/1 配置数据结构

| 项目 | 值 |
| --- | --- |
| 文档状态 | Draft v2.0，按 NE101/NE301 实际实现重构为配置节模型 |
| 更新日期 | 2026-10-28 |
| 适用范围 | `config.set` 与 `config.get` 的 `data` 载荷 |
| 依赖协议 | [CT-Prov/1](./CT_PROV_PROTOCOL.md) |

本文只定义 `config.set` 载荷中各配置节的字段、类型、约束、示例，以及到 NE101/NE301 实际存储的映射。请求信封、能力声明、扫描、错误码和超时见协议文档。

## 1. 通用约定

- JSON 使用 UTF-8，顶层必须为 object，禁止 BOM；字段名区分大小写；整数不得用浮点数表达。
- 字符串默认最大 256 字节；密码类字段最大 128 字节。密码类字段只能在加密链路传输（协议 4.4）。
- `config.set` 是无事务的覆盖写：`ok=true` 只表示已校验并写入持久化存储，不表示已生效。协议不反馈联网、地址或服务验证结果。
- 载荷是「节名 → 节对象」映射；仅出现的节被写入，节内字段整体替换，不做字段级合并。
- 能力响应的 `fields` 决定可提交字段；未列出的字段不得提交，否则返回 `FIELD_NOT_SUPPORTED`。本文示例展示能力声明全部字段时的完整形态。
- 字段表中标注「可选」的字段省略时采用所述默认行为；未标注的字段在能力列出时必须提供。
- NE101 与 NE301 当前均为 DHCP，协议不定义静态 IP 字段。Wi-Fi 安全方式由设备根据密码是否提供自行推断：提供 `password` 按 WPA2/WPA3 处理，省略或空串表示开放网络。

## 2. 配置节总览

| 节名 | kind | 通用字段 | NE101 | NE301 |
| --- | --- | --- | --- | --- |
| `wifi` | network | ssid、password、bssid | ssid、password | ssid、password、bssid |
| `cellular` | network | apn、username、password、pin、auth | apn、username、password、pin、auth | apn、username、password、pin、auth、roaming |
| `halow` | network | ssid、password、country | ssid、password、country | 以 SKU 为准 |
| `ethernet` | network | 无字段 | 不适用 | 无字段（PoE 供电） |
| `comm` | selector | type | 不声明（固件自动探测） | type |
| `mqtt` | service | enable、host、port、client_id、username、password、tls、topic、qos | 除 bssid 类差异外全支持 | hostname/client_id/证书路径映射 |
| `webhook` | service | enable、url、secret、header | enable（push:mode）、url、header | enable、url、secret |

`comm` 节只出现在支持显式选择网络类型的设备能力中；NE101 的网络类型由 `netModule_check()` 按硬件探测写回，不提供该节。

## 3. wifi 节

```json
{
  "ssid": "Office-2.4G",
  "password": "example-password"
}
```

| 字段 | 类型 | 约束 |
| --- | --- | --- |
| `ssid` | string | 1～31 字节目标网络名称；隐藏网络直接输入 SSID |
| `password` | string | 密码类字段，8～63 字节；省略或空串表示开放网络 |
| `bssid` | string | 可选；6 组冒号分隔十六进制，锁定特定 AP；能力未声明时不得提交 |

安全类型不作为独立字段：设备依据 `password` 是否提供推断开放或 WPA 网络。

## 4. cellular 节

```json
{
  "apn": "cmiot.iot",
  "username": "device",
  "password": "example-password",
  "pin": "1234",
  "auth": "chap",
  "roaming": false
}
```

| 字段 | 类型 | 约束 |
| --- | --- | --- |
| `apn` | string | 1～64 字节 APN；能力声明免配置卡（`fields=[]`）时省略 |
| `username` | string | 可选；≤64 字节 |
| `password` | string | 密码类字段，可选；≤128 字节 |
| `pin` | string | 密码类字段，可选；4～8 位数字 |
| `auth` | string | 可选；`none`、`pap`、`chap`、`pap_or_chap`，缺省 `none` |
| `roaming` | bool | 可选；是否允许漫游；能力未声明时不得提交 |

运营商选择、串口波特率等模组私有参数不属于通用节，由设备本地管理界面维护。

## 5. halow 节

```json
{
  "ssid": "CT-HaLow",
  "password": "example-password",
  "country": "CN"
}
```

| 字段 | 类型 | 约束 |
| --- | --- | --- |
| `ssid` | string | 1～32 字节目标网络标识 |
| `password` | string | 密码类字段，8～63 字节；省略或空串表示开放网络 |
| `country` | string | 可选；2 位大写国家/区域代码，如 `CN`、`US` |

## 6. ethernet 节

```json
{}
```

Ethernet/PoE 设备插线即用（DHCP），本节无可写字段。设备通常在能力中以 `configurable=false` 呈现本节，仅用于展示存在性；PoE 是供电属性，见 `device.info` 的 `power` 字段。

## 7. comm 节（可选）

```json
{
  "type": "wifi"
}
```

| 字段 | 类型 | 约束 |
| --- | --- | --- |
| `type` | string | `wifi`、`cellular`、`halow`、`ethernet` 之一，且必须是能力中声明的网络节 |

仅显式选择网络类型的设备声明本节。写入本节即切换活动网络；与网络节同请求提交时，先应用网络节再切换。

## 8. mqtt 节

```json
{
  "enable": true,
  "host": "mqtt.example.com",
  "port": 8883,
  "client_id": "NE101202610270001",
  "username": "device",
  "password": "example-secret",
  "tls": true,
  "topic": "ct/NE101202610270001/data",
  "qos": 1
}
```

| 字段 | 类型 | 约束 |
| --- | --- | --- |
| `enable` | bool | 是否启用 MQTT 出口；设备同时支持双出口时可与 `webhook.enable` 并存，以设备策略为准 |
| `host` | string | 域名或 IP 字面量，≤128 字节 |
| `port` | uint16 | `1..65535`；启用 TLS 时推荐 8883 |
| `client_id` | string | 可选；≤64 字节；省略时设备用 `serial_number` 生成 |
| `username` | string | 可选；≤64 字节 |
| `password` | string | 密码类字段，可选；≤128 字节，省略表示匿名接入 |
| `tls` | bool | 可选；是否启用 TLS，缺省 `false` |
| `topic` | string | 可选；≤128 字节数据上报主题，不得包含 `+`/`#` 通配符 |
| `qos` | uint8 | 可选；`0..2`，缺省 `0` |

证书类字段（CA 名称、客户端证书路径）不属于通用节，由设备本地管理界面维护。

## 9. webhook 节

```json
{
  "enable": true,
  "url": "https://ops.example.com/hooks/camera",
  "secret": "whsec_example",
  "header": "X-API-Key: abc123"
}
```

| 字段 | 类型 | 约束 |
| --- | --- | --- |
| `enable` | bool | 是否启用 Webhook 出口 |
| `url` | string | http/https 绝对 URI，≤256 字节，推荐 https |
| `secret` | string | 密码类字段，可选；≤128 字节签名密钥；能力未声明时不得提交 |
| `header` | string | 可选；≤256 字节，`Key: Value` 格式的附加请求头；能力未声明时不得提交 |

`secret` 的签名算法（如 HMAC-SHA256 与请求头名称）由产品文档定义。事件筛选、超时等扩展字段按能力声明加入。

## 10. 平台实现映射

### 10.1 NE101（ESP32-S3，NVS 键值存储）

| 通用字段 | NE101 NVS 键 |
| --- | --- |
| wifi.ssid / wifi.password | `wifi:ssid` / `wifi:password` |
| cellular.apn | `cat1:apn` |
| cellular.username | `cat1:user` |
| cellular.password | `cat1:password` |
| cellular.pin | `cat1:pin` |
| cellular.auth | `cat1:authType` |
| halow.* | 复用 `wifi:ssid`、`wifi:password`，区域码存 `dev:country` |
| comm.type | 不适用；`dev:netmod` 由 `netModule_check()` 硬件探测写回 |
| mqtt.enable / host / port | `mqtt:enable` / `mqtt:host` / `mqtt:port` |
| mqtt.client_id / qos | `mqtt:clientId` / `mqtt:qos` |
| mqtt.username / password | `mqtt:user` / `mqtt:password` |
| mqtt.tls / topic | `mqtt:tlsEnable` / `mqtt:topic` |
| webhook.enable | `push:mode`（0=MQTT，1=Webhook） |
| webhook.url / header | `whk:url` / `whk:header` |
| webhook.secret | 不支持；能力不声明 |

应用时机：写入 NVS 后由 `netModule_open()` 在下次联网或重启时应用，`config.set.result` 的 `apply` 报告 `reconnect`。

### 10.2 NE301（STM32N6570 + Si91x + W5500，Services 架构）

| 通用字段 | NE301 实现 |
| --- | --- |
| wifi.ssid / password / bssid | Wi-Fi 服务的 Wi-Fi STA 配置（`/api/v1/system/network/wifi`） |
| cellular.apn / username / password | Cellular 服务设置 `apn/username/password` |
| cellular.pin / auth / roaming | `pin_code` / `authentication`（0=None,1=PAP,2=CHAP）/ `enable_roaming` |
| comm.type | `/api/v1/system/network/comm` 切换（`wifi`/`cellular`/`poe`→`ethernet`） |
| ethernet | comm type `poe`，无独立字段 |
| mqtt.host / port / client_id | MQTT 服务 `connection.hostname/port/client_id` |
| mqtt.username / password | MQTT 服务 `authentication.username/password` |
| mqtt.tls | 由 CA 证书存在性推导，写入时映射为证书配置状态 |
| mqtt.topic / qos | MQTT 服务 `topics` 上报主题与 QoS 配置 |
| webhook.enable / url / secret | Webhook 服务 `enable/url/secret` |
| webhook.header | 以设备 Web API 支持为准；未支持时能力不声明 |

应用时机：保存后由对应 Service 按其策略应用，`apply` 报告 `immediate` 或 `reconnect`。

两平台的字段上限（如 `ssid[32]`、`url[256]`）即本文字段约束的来源；通用约束取两平台交集或较严格值，单平台私有能力一律通过能力声明门控。

## 11. 扩展规则

- 新增字段必须先加入能力 `fields` 声明，Client 依能力构建表单；Server 对未声明字段返回 `FIELD_NOT_SUPPORTED`。
- 未识别的节名由 Client 隐藏或显示为不可配置，Server 返回 `SECTION_NOT_SUPPORTED`，不得按已知节猜测。
- 单平台私有字段（如 `bssid`、`roaming`、`secret`）留在通用 schema 中但必须能力门控；禁止无条件要求所有设备支持。
- 字段约束变更视为 schema 版本演进，随协议版本号发布。
