# R-08 原始 NVS 的离线上下文（仍非维修授权）

此工具为已有 [原始 Flash 导入](RAW_FLASH_OFFLINE_IMPORT_20261107.md) 增加一个单独、
固定来源的 NVS 补充证据包。使用 ESP-IDF 实际 NVS/CRC 实现读取 namespace `wqn` 中
与设备逻辑证据采集器相同的 11 个键，不修改源 NVS，不调用迁移、设备或云端。

## 解释范围

- `cur_seq/cur_rnd/cur_dic/cur_rev/cur_int/cur_shf/cur_mis`：保留原始 blob，严格检查
  52 字节、WCUR、版本、SDK 约定的游标 CRC、完整 UUID、paused 与保留字节。
- `word_deck_gen`：保留完整 u64 十进制文本。超过 UINT32_MAX 的值为未知语义，
  不能截断成有效的 scope generation。
- `word_deck_chg/word_deck`：保留原始 NUL/长度信息，区分缺失、空字符串与异常字节。
  存在任何 marker 都阻止 scope 就绪判断，不在离线工具中重放合法 marker。
- `word_seq_idx`：用十进制文本保留全部 u64，避免 JSON/JavaScript 丢失高位。

每个值区分 present / absent / present_opaque / unknown。只有完整 SDK 读取与独立的
条目枚举一致，才能报告缺失。损坏/未知/FREEING 页、重复页序号、非法 bitmap、未发布
残片、namespace 或键冲突，以及任何 SDK 写/擦除尝试，都会阻止上下文确认。
不使用页序号或磁盘位置选择“看起来更新”的冲突值，也不复活已擦除的键。

实际 SDK 的 `Storage::findItem` 可把页级 TYPE_MISMATCH 折叠成 NOT_FOUND，因此
本工具还比较该键的原始条目清单。已存在的错误类型不能当作未设置的默认值。
对应的 [固件修正](NVS_TYPED_PRESENCE_20261107.md) 现已单独实现并提供实际 SDK 回归；离线工具包保持其各自固定的源码身份，不因此获得维修授权或实机验收结论。

`session_context()` 只把匹配当前 mode/SID 的 NVS 暂停值、相同的已提交 generation
与一个已解码的保留会话关联。它不证明当前 pack admission、RAM scope ready 或来源
身份。现有 review-plan/candidate CLI 尚未消费这个补充包，仍保留原来的 NVS/身份前提；
后续接入必须重新验证包、原始镜像与 forensic pin，不能只把 JSON 值当可信输入。

## 只读 SDK 适配

16 KiB NVS 源文件必须为普通 O_RDONLY 文件。子进程读入固定 RAM 数组后关闭源 fd，
SDK 分区适配器的 read/read_raw 仅复制该数组；write/write_raw/erase_range 始终拒绝
并计数。即使只读分区，SDK 初始化/读取仍可能尝试清除孤立 blob 或坏 CRC 条目，
这些尝试不是正常缺失值，不能让初始化成功掩盖它们。

Linux seccomp 拒绝文件打开、网络、读取与媒体写入，仅允许 stdout/stderr 写和必要
内存/运行时系统调用；实际检测 pwrite/open/socket 三种拒绝。SDK 日志关闭，原生输出
仅含这 11 个键，其他 namespace 和凭据值不被序列化。NVS 原文副本本身仍可能包含
凭据，所以所有导入/复验都要求显式敏感范围确认和私有目录。

SDK NVS/CRC 代码不修改；构建固定 SDK 源文件、host stubs、wrapper、helper 和 reader
二进制摘要。静态检查 Item=32、key offset=8、data offset=24 与主机小端布局。
CPU 20 秒、普通进程地址空间 512 MiB、输出文件 1 MiB、默认等待 30 秒；超时或取消
终止并回收同一个子进程，不能据此声称底层磁盘 I/O 有物理最坏延迟保证。

ASan/UBSan 保持启用虚类型检查。该检查首次可能读取进程映射，因此在安装 seccomp
前对固定 RAM 适配器执行一次真实只读虚调用，建立同一基类/派生类的类型检查缓存。
没有放开隔离系统调用或关闭 vptr 检查。LeakSanitizer 在隔离 child 中关闭，不宣称完成
泄漏检查；任何 sanitizer 诊断按执行故障处理，不作为语义负对照成功。

主机回归使用固定的 robustness Python 依赖环境，官方 NVS generator 则使用 SDK Python。
测试优先采用 `NVS_GENERATOR_PYTHON`，其次读取 `IDF_PYTHON_ENV_PATH`，最后在
`IDF_TOOLS_PATH` 或当前用户的标准 ESP-IDF 5.5 环境中定位对应 Python 版本。
CI 已显式 source SDK export.sh；不依赖某位开发者的绝对 home 路径。

## 使用

只支持 Linux/WSL。先按原始导入文档固定 WQR1、profile、SPIFFS reader 和 import pin。
构建 NVS reader 不需要、也不读取完整设备 sdkconfig：

```sh
python3 scripts/nvs_context.py build \
  --idf-path /home/unknow/esp/esp-idf-v5.5 --output /private/new-nvs-reader

python3 scripts/nvs_context.py capture \
  --source-bundle /private/raw-evidence --source-manifest-sha256 RAW_SHA256 \
  --profile /private/profile.json --profile-sha256 PROFILE_SHA256 \
  --spiffs-build /private/spiffs-reader --spiffs-build-manifest-sha256 SPIFFS_BUILD_SHA256 \
  --import-bundle /private/raw-import --import-manifest-sha256 IMPORT_SHA256 \
  --nvs-build /private/new-nvs-reader --nvs-build-manifest-sha256 NVS_BUILD_SHA256 \
  --output /private/new-nvs-context --sensitive-main-flash

# 其他参数相同，output 指向已经完成的包，并增加该包的 manifest pin。
python3 scripts/nvs_context.py verify \
  --source-bundle /private/raw-evidence --source-manifest-sha256 RAW_SHA256 \
  --profile /private/profile.json --profile-sha256 PROFILE_SHA256 \
  --spiffs-build /private/spiffs-reader --spiffs-build-manifest-sha256 SPIFFS_BUILD_SHA256 \
  --import-bundle /private/raw-import --import-manifest-sha256 IMPORT_SHA256 \
  --nvs-build /private/new-nvs-reader --nvs-build-manifest-sha256 NVS_BUILD_SHA256 \
  --output /private/new-nvs-context --expected-manifest-sha256 NVS_CONTEXT_SHA256 \
  --sensitive-main-flash
```

capture 前后均从原 WQR1 重新运行 SPIFFS 导入校验；上下文固定原镜像、NVS、forensic、
reader 和 Python 解释源码摘要。verify 再从原始证据重新采集并逐文件比较，拒绝只改
派生事实与摘要的自洽伪造。它使用同一个 SDK 实现，不是第二个独立 NVS 实现。

输出含 `nvs-raw.bin`、`native.json`、`context.json`、`manifest.json`。新目录/文件使用
0700/0600，完成前保留 `.incomplete`，失败不覆盖重试。退出码 0 表示保存/复验完成，
capture 的 2 表示已保存但仍有上下文阻塞，1 表示拒绝或未完成；verify 成功也可能是在
复现“未知”证据，不代表 scope、身份或维修就绪。

原始镜像导入仍保持其 `nvs_decoded=false`；本工具产生独立补充证据，不改写历史包。
NVS 加密不在支持范围，密文/未知格式保持原文和阻塞。真实介质、认证、候选计划接入、
受控安装/回滚与物理 HIL 仍待完成，R-08 没有关闭。
