# R-08 原始镜像的离线只读导入

本工具把已有 WQR1 原始证据导入为 SDK 可见的 SPIFFS 文件原文和 Word/Note
forensic 包，供既有回执客户端逐记录核对。它不连接设备，不修改镜像，不读取真实
凭据，不调用云端，不初始化/修复文件系统，不安装候选或授权维修。

当前配置固定为 Note4 16 MiB、分区表 0x8000、storage 0x800000/8 MiB、NVS
0x9000/16 KiB、SPIFFS 256/4096 页/块、name32/meta4、magic 与 magic-length。
这是对本项目已有验证基线的支持，不是通用 ESP32 解密/数据恢复程序。

## 三个独立的 pin

1. WQR1 源 manifest SHA-256：重新验证完整 wire、主镜像与源端全范围重读结果；
   原始证据必须完整，拒绝未验证重读的镜像和加密标记。
2. 格式 profile SHA-256：由独立固定的 baseline.json、匹配其摘要的 resolved
   sdkconfig 和 SDK SPIFFS 源码生成。只复制格式字段与摘要，不发布完整 sdkconfig。
   ELF pin 必须与源证据一致；镜像内还需有相同 ELF 的 app descriptor。
3. reader build manifest SHA-256：固定 SDK 源码、主机适配、wrapper、C helper 与
   编译二进制。它是构建一致性记录，不是硬件或可信构建签名。

这些都是操作方 pin，始终标记 operator_pinned_unattested。MAC、ELF、摘要和离线
重放都不能证明物理设备归属。app descriptor 检查也不替代应用镜像签名验证。

## 使用

仅支持 Linux/WSL 本地文件系统与本机 GCC。先保留 USB 接收工具生成的原始 WQR1
证据及 manifest pin；不要让此工具对真实串口、设备块介质或活动目录运行。
原始 Flash 可能含凭据，每次导入/复验均需显式敏感范围选项。

已经获得匹配基线的 profile 时，可从第二步开始。所有输出路径必须不存在。

```sh
# 生成 profile：config SHA 必须等于 baseline 中的 resolved_sdkconfig_sha256。
python3 scripts/raw_flash_import.py profile \
  --baseline /path/to/baseline.json --baseline-sha256 BASELINE_SHA256 \
  --resolved-sdkconfig /path/to/build/config/sdkconfig.json \
  --sdkconfig-sha256 SDKCONFIG_SHA256 \
  --idf-path /home/unknow/esp/esp-idf-v5.5 --output /tmp/note4-profile.json

# 使用 profile 输出的 pin 编译只读 reader。共享 SDK 不会被修改。
python3 scripts/raw_flash_import.py build \
  --profile /tmp/note4-profile.json --profile-sha256 PROFILE_SHA256 \
  --idf-path /home/unknow/esp/esp-idf-v5.5 --output /tmp/note4-reader

python3 scripts/raw_flash_import.py import \
  --source-bundle /path/to/raw-evidence --source-manifest-sha256 RAW_MANIFEST_SHA256 \
  --profile /tmp/note4-profile.json --profile-sha256 PROFILE_SHA256 \
  --build-dir /tmp/note4-reader --build-manifest-sha256 READER_BUILD_SHA256 \
  --output /path/to/new-import --sensitive-main-flash

python3 scripts/raw_flash_import.py verify \
  --source-bundle /path/to/raw-evidence --source-manifest-sha256 RAW_MANIFEST_SHA256 \
  --profile /tmp/note4-profile.json --profile-sha256 PROFILE_SHA256 \
  --build-dir /tmp/note4-reader --build-manifest-sha256 READER_BUILD_SHA256 \
  --output /path/to/new-import --expected-manifest-sha256 IMPORT_MANIFEST_SHA256 \
  --sensitive-main-flash
```

独立工具包中去掉命令路径的 `scripts/`。二进制与 wrapper 的摘要发生变化时必须
重新构建，不复用旧 build pin。构建依赖 SDK 源码的确切内容，不自动下载或切换 SDK。

## 输出与下一步核对

- main-flash.bin：固定源镜像的完整私有副本；原 WQR1 包保持不变。
- nvs-raw.bin：NVS 分区原文，**没有解码为键、pause、scope generation 或 cursor**。
- spiffs-stream.bin、objects/：SDK 可见对象的字节、原始 name32/meta4、对象 ID 与页号。
  不把这些对象说成全部磁盘残留，未链接/已删除页仍仅保留在主镜像中。
- files/：只为可表示且唯一的平铺名称建立别名。路径样式、未终止/不可表示名称保留
  为编号对象；规范化碰撞不选择其中一个作为主日志。此时不生成 forensic 包。
- forensics-word/、forensics-note/：可用时生成既有格式，输出回执给出各自 pin。
  `.tmp` 仍是未发布证据，不成为运行候选；原有 CRC、容量、暂停和原文保留规则继续适用。

退出码 0 表示导入原文完成且没有声明的逻辑解析阻塞；2 表示原文保留成功但逻辑
解释需要复核；1 表示拒绝或未完成。任何退出码都不代表维修、可信来源、NVS 上下文
或文件系统一致性已经证实。失败和取消保留 `.incomplete`、已有私有原文/子进程输出，
不得覆盖重试。嵌套 forensic 子目录可能保留自己的不完整标记，根清单会记录解析阻塞。

已有认证回执工具可直接使用导入得到的 forensic 包，不使用原始镜像当上传请求：

```sh
# 使用 scripts/robustness-requirements.txt 对应的固定 Python 环境。
python3 scripts/outbox_reconcile.py collect \
  --source-bundle /path/to/new-import/forensics-word \
  --source-manifest-sha256 WORD_FORENSIC_SHA256 \
  --endpoint https://YOUR_SERVER/api/esp32/v3/study/observations/receipts \
  --device-token-file /private/device-token \
  --expected-user-id EXPECTED_USER_UUID --expected-device-id EXPECTED_DEVICE_UUID \
  --output /path/to/new-receipt-evidence
```

Note 对应 forensics-note 与其 pin。真实凭据、账号/设备归属、接口部署与 SQL 授权仍需
单独验证；本轮只在注入传输中接通此路径，没有调用真实后端或使用凭据。
匹配事实不授权 ACK/skip。NVS 尚未解码、来源未认证，既有 review-plan 阻塞继续保留。

## 实现边界与证据

主机子进程使用实际 SDK SPIFFS 代码的 READ_ONLY 版本；HAL 写/擦除回调只拒绝。
源句柄独立要求 O_RDONLY，Linux seccomp 拒绝路径打开、网络和源写入，只允许源
pread、stdout/stderr 写与必要内存操作。每次执行实际检测 pwrite/open/socket 被拒绝，
并要求 SDK 创建操作返回只读拒绝且未调用写/擦除回调。无兼容性降级绕过这些限制。
CPU 20 秒、普通进程地址空间 512 MiB、输出 40 MiB、默认墙钟等待 30 秒，超时/取消
终止并回收同一子进程；这些不替底层文件系统提供最坏 I/O 延迟或断电持久性保证。

SDK 的 49 字节 packed object header 后跟 16 位页索引。主机适配把 page-index
类型设为 byte-aligned/may-alias，仍为 2 字节，并静态检查 header=49/name-offset=13。
这修正主机 UBSan 检出的未对齐加载，磁盘字节布局未改变。适配和原 SDK 摘要均被固定，
不修改全局 SDK，也不据主机告警推断设备发生过对齐崩溃。

接收后重读对象、别名与 NVS 摘要，fsync 文件和目录，固定完整库存；verify 会重新从
原 WQR1 包运行同一只读 SDK 解码并逐文件比较，不能只改派生 SHA 来冒充正确。
这是从原始证据重放，不是第二个独立文件系统实现，也不证明物理 HIL。
子进程崩溃/sanitizer 诊断属于执行故障，保留私有日志并与普通受控拒绝分开，不计作
负对照成功。sanitizer 测试启用地址和未定义行为检查；受 seccomp 限制的 child 关闭 leak
检查，不能据此宣称完成泄漏检查。

主机测试以真实 raw collector、SDK spiffsgen、实际只读 SPIFFS 与生产 Word/Note
编码器生成样例，覆盖 pin/格式/源重读/加密/分区摘要、空文件、opaque 原文、名字
padding/路径/碰撞、I/O 和期限、派生篡改、对账桥接，以及去除关键检查的语义负对照。
真实损坏介质、加密数据的授权解读、NVS 上下文、可信来源、安装/回滚与 HIL 仍未完成。

已有固定原始导入包时，可另行生成 [离线 NVS 补充证据](OFFLINE_NVS_CONTEXT_20261107.md)。原始导入包及其 nvs_decoded=false 保持不变；补充工具重新从 WQR1 复验，并保留身份与维修计划前提。
