# KVSpace 共享内存方案评审

## 1. 范围

本文评审 `deepx-design` 中与 KVSpace 共享内存有关的几份草案，并给出
`kvspace-c` 的实现建议。评审关注四个问题：

1. 本地多进程共享内存应采用什么布局和生命周期协议；
2. ART、Hash、Trie 等索引在 KVSpace 工作负载下各自适合什么场景；
3. 进程崩溃恢复能够提供哪些保证，不能提供哪些保证；
4. 本地 mmap 数据和远端 RDMA 数据能否共用同一套布局。

本文不是持久格式规范。当前实现的格式、恢复状态机和兼容性约束仍以
[DESIGN_RESOLUTIONS.md](../DESIGN_RESOLUTIONS.md) 为准。

评审涉及的原始草案包括：

- [`kvspace-shm.md`](https://github.com/array2d/deepx-design/blob/master/doc/kvspace-design-and-implementation/draft/kvspace-shm.md)：固定 region、cuckoo hash、显式目录索引，以及 one-sided GET / RPC 写入；
- [`kvspace-c.md`](https://github.com/array2d/deepx-design/blob/master/doc/kvspace-design-and-implementation/draft/kvspace-c.md)：ART、固定节点 slab、bump value 区和虚拟地址预留扩容；
- [`kvspace-c-art-boxmalloc.md`](https://github.com/array2d/deepx-design/blob/master/doc/kvspace-design-and-implementation/draft/kvspace-c-art-boxmalloc.md)：ART 与可回收 Box allocator 的组合；
- [`kvspace-c-hash.md`](https://github.com/array2d/deepx-design/blob/master/doc/kvspace-design-and-implementation/draft/kvspace-c-hash.md)：固定容量开放寻址 Hash 与显式目录索引；
- [`kvspace-c-memkv-reuse.md`](https://github.com/array2d/deepx-design/blob/master/doc/kvspace-design-and-implementation/draft/kvspace-c-memkv-reuse.md)：复用 block/box allocator，并讨论 256 叉 byte trie。

这些文件记录的是同一阶段的候选方案，不应被当作一份已经收敛的规范。
例如，`kvspace-c.md` 前半部分主张取消目录 Index，后半部分的完整 API
设计又重新要求目录 key 保存 Index TLV；Hash 草案则从一开始就要求显式
Index。实现时需要逐条验证，而不是从某一份草案直接推导最终布局。

## 2. 结论

本地 SHM 建议采用以下基线：

- 创建时一次性确定固定容量，先把 backing object 扩展到完整
  `max_size`，再映射和初始化；
- 所有持久引用使用 offset 或 allocator-local ID，不保存进程虚拟地址；
- 通用标量和引擎记录使用明确的小端编码；原生 pthread 对象只属于受限的
  Linux/glibc 控制面；
- 保留显式 `Index`、`ExtIndex` 和 `LinkIndex` 语义，不用 ART 内部节点代替
  目录记录；
- v1 使用一个 process-shared robust mutex 串行化操作，通过 COW、最后发布
  root/selector 和 allocator 重建处理进程死亡；
- 只承诺进程死亡恢复，不承诺断电持久化、系统重启恢复或多 key 事务；
- 没有真实应用 trace 时，`ArtBox` 可作为保守的通用候选，但不能仅凭数据
  结构名称决定默认引擎。

RDMA 不应直接注册并暴露当前整个 region。可保留同一个 backing object 和
同一份语义权威数据，但必须区分本地控制面与远端只读数据面。远端 v1 只做
可校验的 one-sided point Get；写入、目录操作、通知和维护操作由 host RPC
执行。

## 3. 对关键设计问题的判断

| 问题或命题 | 判断 | 原因 |
| --- | --- | --- |
| 本地 mmap 与 RDMA 使用同一份物理数据 | 有条件成立 | 数据页可以共用，但 pthread、allocator journal 和回收协议不能直接成为远端 ABI |
| 先映射巨大 VMA，再通过 `ftruncate` 在线扩大文件即可透明扩容 | 不作为正确性基础 | 映射超过 EOF 的访问可能触发 `SIGBUS`；底层文件改变大小后既有 mapping 的行为不适合作为可移植协议保证 |
| ART 内部节点可以完全替代目录 Index | 不成立 | ART edge 是分支字节，不是直接子项名；压缩前缀、空目录、ExtIndex 和 LinkIndex 都需要显式语义记录 |
| `List` 读取 ART child 数组后可变为 O(1) | 不成立 | 返回 k 个名字至少需要 O(k) 和相应输出字节；内部 child 也不一定与目录项一一对应 |
| 现有草案足以确定普遍默认引擎 | 未证实 | 草案比较了复杂度、推算延迟、空间、代码量和假设 workload，但缺少统一 trace 与同条件实测；原始 ART 论文也不包含共享内存 COW、allocator、TLV 和全局锁成本 |
| Hash 查找与 key 长度无关 | 不成立 | 计算 hash 至少需要读取完整 key，碰撞时还要比较 key；低负载时探测次数通常较少，当前线性探测最坏仍可扫描完整 table |
| 草案中的锁和并发描述覆盖 owner-death 恢复 | 未覆盖 | 各草案分别描述了 process-shared mutex、自旋锁或 CAS/seqlock，但没有给出进程持锁死亡后的结构恢复协议 |
| one-sided GET、RPC mutation | 成立 | 该边界避免把远端写锁、分配、回收和客户端故障恢复一次性引入 v1 |
| 一个全局锁适合作为最终并发方案 | 不成立 | 它适合先建立正确性基线，但所有 Get、Set、List、Watch 状态变化都会在多核下争用同一个锁 |

## 4. 本地 SHM 设计

### 4.1 创建和映射

创建过程应在第一次持久写之前完成全部几何计算和边界检查：

1. 校验 `max_entries`、`max_queues`、页大小和引擎参数；
2. 用 checked arithmetic 计算 header、队列、allocator 和引擎区间；
3. 证明所有区间互不重叠且落在 `[0, max_size)`；
4. 执行 `ftruncate(fd, max_size)`；
5. 映射完整 `[0, max_size)`；
6. 初始化 header、引擎区、robust mutex 和 condition variable；
7. 最后发布 ready 状态。

Attach 不能先信任 backing object 中的长度和 offset。它应先通过 `fstat` 和
有界读取检查 magic、版本、endian、header size、engine ID、layout hash 和
`region_size == region_max`，确认文件至少覆盖完整 region 后才映射并遍历
内部记录。

该方案会预留较大的逻辑文件和虚拟地址范围，但避免了在线增长期间不同进程
看到不同 size、访问 EOF 后页面以及重新注册 RDMA MR 等问题。`ftruncate`
只建立逻辑长度，不等于预留全部物理空间；tmpfs 配额或文件系统空间耗尽仍需
作为运行时错误处理。

如果未来确实需要增长，应在以下方案中单独选择，而不是默认认为原 mapping
会安全扩展：

- 创建新 region，复制并切换 generation；
- 使用固定大小的多个 chunk，并显式发布 chunk table；
- 在无活跃客户端的维护窗口重新映射。

### 4.2 持久引用和 ABI

region 内不能保存进程指针。不同进程的 mmap 基址可以不同，远端客户端也不
共享主机虚拟地址。允许的引用形式包括：

- 相对 region 起点的 64 位 offset；
- allocator 内部的固定宽度 object ID；
- 带有明确 null 编码和范围校验的 biased offset。

所有可移植记录都应逐字段编码，不能把 C/C++ native struct 当作跨版本、跨
编译器协议。当前 common header 中的 `pthread_mutex_t` 和
`pthread_cond_t` 是例外，因此整个 ABI 明确限制在 little-endian x86_64
Linux 和指定的 glibc pthread ABI。它不是通用的跨平台 C ABI，更不是网络
协议。

### 4.3 目录语义

KVSpace 的目录不是索引实现的偶然内部节点，而是公开语义：

- 普通目录保存 `Index`，内容为本地直接子项；
- `ExtIndex` 保存本地子项和 fallback 目录；
- `LinkIndex` 改变路径解析；
- `/a` 与 `/a/` 是两个独立 key；
- 空目录在没有任何子 key 时仍需要存在。

压缩 ART 不能替代这些记录。例如同时存在 `/a/bb/x` 和 `/a/bc/y` 时，
`/a/` 下的 ART 可能只有一个边 `b`，之后才在更深位置分裂；读取该节点的
child 数组得不到目录项 `bb` 和 `bc`。同理，路径压缩可能使 `/a/` 根本不是
一个独立物理节点。

因此：

- `List(prefix, false)` 读取本地目录记录；
- `List(prefix, true)` 在完成 link 解析后按 ExtIndex 规则合并 fallback；
- 返回 k 个名字的复杂度至少为 O(k) 加输出字节；
- `DelTree` 的物理删除不能只相信可由用户写入或可能损坏的 Index，应以实际
  key 前缀枚举为最终依据。

### 4.4 并发和恢复

一个 region-wide robust mutex 是合理的 v1 起点。它提供清楚的顺序关系，
也让四个引擎可以先共享同一套语义层和恢复框架。但 robust mutex 只提供
owner-death 通知，不提供事务回滚。

每次 mutation 应遵循以下提交顺序：

1. 在未发布区域构造新节点、value 或 table image；
2. 完整校验即将发布的结构；
3. 写入 entry count、node count 等可推导计数，并递增非权威的诊断字段
   generation；owner-death recovery 会从已提交 root/table 重建计数，
   generation 不参与内容正确性判断；
4. 以 release store 最后发布 root 或 active-table selector；
5. 只在发布后回收旧对象。

发生 `EOWNERDEAD` 后，恢复分为两个阶段：

- Prepare 只读，检查 immutable geometry、权威 root/table、对象引用、journal
  和 allocator 边界，并在内存中生成完整修复计划；
- Apply 执行可重复的修复，重建 free list、bitmap、counter 和 queue 状态；
  成功后才调用 `pthread_mutex_consistent`。

确定性格式损坏应让 mutex 进入不可恢复状态。临时内存不足等 Prepare 失败
不能把部分修复伪装成成功；Apply 中途死亡后，下一 owner 必须能够重新
Prepare 并收敛。

该协议只针对仍在运行的内核中的进程死亡。它不包含 `msync`/`fsync` 顺序、
双 meta page、文件目录同步或 torn-write 模型，因此不是断电持久化协议。

POSIX 还规定，包含进程共享同步对象的最后一个 mapping 被所有进程解除后，
同步对象状态不再具有可移植保证。如果产品要求“零客户端一段时间后仍能按
严格契约重新 Attach”，应让一个 region host 在生命周期内始终保持 mapping，
或者把该行为明确限制为经过测试的 Linux/glibc 扩展。

`Destroy` 也应作为离线操作：unlink 只删除名字，已有 mapping 仍然引用旧
对象；随后用同名 Create 可以生成另一个对象，形成两个同时存活的 region。

## 5. 四种引擎的取舍

记 `K` 为 key 字节数，`C` 为 Hash table capacity，`D` 为目标前缀下的
条目数。

| 引擎 | 点查与前缀 | mutation 和回收 | 适用判断 |
| --- | --- | --- | --- |
| `ArtBump` | 压缩 ART，点查随 K 和树高变化；前缀定位后枚举子树 | 节点 slab 可回收，prefix/value 继续追加到 raw zone，依赖 `Compact` 回收 | 适合读多写少、生命周期明确或允许维护窗口的场景 |
| `ArtBox` | 压缩 ART，支持从匹配子树枚举 | COW 路径节点；旧 value 和节点在发布后由 FixedBlock/Box 回收 | 当前最均衡的通用候选，仍需真实 trace 验证 |
| `HashBox` | 读取完整 key 计算 hash，再线性探测；前缀操作需扫描物理 key | 当前每次 mutation 清空 inactive table、扫描 active table、重插 live entry、发布 selector，再清空旧表 | 小容量、强读多写少时可能合适；当前提交协议不适合作为大容量更新型默认值 |
| `TrieBox` | 严格逐 byte 下降；理论上可从 prefix 节点枚举 | 每次更新复制 byte path，每节点固定 1032 字节；当前 prefix 枚举实现还会先遍历全树 | 可作为简单基线；深路径和低 fanout 时空间放大明显 |

### 5.1 ART

ART 的 Node4/16/48/256 能根据 fanout 调整空间，适合路径 key 和公共前缀较多
的场景。原始 ART 论文中的 Node16 SIMD 是常数优化，不是从 O(16) 变成新的
渐进复杂度；16 本来就是固定上限。当前实现仍需用隔离 benchmark 判断
scalar、binary search、SSE2 或 NEON 哪种方案值得维护。

论文结果不能直接套到本实现。共享内存版本增加了 COW、allocator、TLV、
robust mutex 和恢复检查，这些成本可能超过单个节点查找的差异。

### 5.2 Hash

Hash 的优势是点查不需要沿树逐层下降。当前 HashBox 使用线性探测，probe 数
随装载率和聚簇变化，最坏可扫描完整容量；只有未来采用固定 bucket 或 cuckoo
的远端 projection 才能给出较小的 probe 上界。当前 HashBox 的主要问题不在
hash 函数，而在 crash-consistent 提交方式。每次 mutation 都包含完整容量的
清零和扫描，并重插 live entry；校验、区间排序和碰撞行为还会增加额外成本。
因此不能把它描述成“所有操作 O(1)”。

若要让 HashBox 成为通用默认值，应先重新设计写入协议，例如使用 per-slot
journal、分段 COW 或可恢复的增量 table 更新。引入 cuckoo hashing 只能限制
读取 probe，不能自动解决多 slot displacement、rehash 和 crash atomicity。

### 5.3 引擎选择方法

默认引擎应由两层 benchmark 决定：

1. engine 层直接测 Get、Put、Erase、EntriesWithPrefix，隔离 allocator 和
   index 成本；
2. semantic 层通过 `ShmClient` 测完整 TLV、目录 Index、Link/ExtIndex、
   Notify/Watch 和 DelTree。

测试至少覆盖：

- `max_entries` 为 1K、10K、100K、1M，并区分实际 live entries；
- key 长度 16、32、64、128 字节和不同路径深度、fanout；
- value 为 None、16B、128B、4KiB；
- Get hit/miss、Zipf hot set、持续覆盖写、批量 Set；
- 小目录和大目录的 List，以及删除 0.01%、1%、50% 数据的 DelTree；
- ArtBump 首次 Compact 前的 high-water/live 比和 Compact pause；
- 1、4、16 个进程下全局 mutex 的吞吐与 p99；
- Close/Attach、owner death 和恢复后的结果 digest。

指标应包括吞吐、p50/p99/p999、CPU cycles、cache/TLB miss、每次 mutation
触碰的字节、allocator 高水位、物理空间放大和恢复时间。没有这组数据时，
`ArtBox` 只是风险较低的工程起点，不是已经证明的性能冠军。

## 6. RDMA 方案

### 6.1 当前 region 不能直接暴露

当前 common header 含有原生 `pthread_mutex_t` 和 `pthread_cond_t`。RNIC
不能成为 robust mutex owner，不能进入 Linux robust list，也不能执行
futex wake 或 condition broadcast。远端 one-sided reader 同样不会获取本地
全局锁。

另一个关键问题是对象回收。本地 reader 在 mutex 保护下读取；writer 发布新
root 或 table 后可以立即回收旧对象。远端 reader 可能已经读到旧 offset，
但 host 无法知道它何时完成下一次 RDMA READ。若旧空间被立即复用，远端会把
不同 generation 的 metadata 和 value 拼在一起。当前 allocator journal 的
checksum 只服务本地恢复，不能替代逐 entry 的远端一致性校验。

### 6.2 建议布局

可以保留一个 backing object，但明确分为两个协议域：

```text
kvregion backing
├── local-control
│   ├── pthread mutex / condition
│   ├── allocator journal
│   └── recovery / lifecycle state
└── rdma-data
    ├── stable offset or object ID
    ├── immutable key/value payload
    ├── version and incarnation
    └── checksum
```

本地进程可以映射全部区域；远端只获得 `rdma-data` 的 read-only rkey。
`local-control` 不注册，或至少不授予 remote read/write/atomic 权限。

远端 point Get 至少执行：

1. 读取 bucket/root metadata，包括 offset、version 和 incarnation；
2. 读取 immutable key/value；
3. 再次读取 metadata；
4. 检查前后 version 相同且为已提交状态；
5. 校验 key、长度和 checksum，不满足则重试。

跨多个 cache line 的对象需要逐 cache-line version，或者对象级 checksum 加
前后 version。单纯把一个 generation counter 放在 region header 中不足以
保护多次 RDMA READ 之间的对象复用。

### 6.3 回收、扩容和失效

one-sided reader 对 host 不可见，因此发布新对象后不能立即复用旧对象。
需要 epoch、lease 或 RCU 类协议：

- 客户端连接时取得 epoch 和 rkey；
- 被替换对象进入 retire list；
- 旧 epoch 的 lease 全部结束后才回收；
- host 重启、Compact、region replacement 或 MR 变化时提升 epoch 并轮换
  rkey；
- 客户端发现 epoch 变化后丢弃缓存 offset 并重新连接。

RDMA region 不应依赖对已注册巨大 mapping 进行透明 `ftruncate`。扩容可以
增加固定 MR/chunk，或者创建新 region 后切换 generation。两种方式都需要
显式更新远端 capability。

### 6.4 操作边界

v1 只考虑 one-sided point Get。以下操作由 host RPC 执行：

- Set、Del、Clear 和 Compact；
- List、DelTree 和目录 Index 维护；
- Link/ExtIndex 路径解析；
- Notify、Watch 和 queue 生命周期；
- allocator、GC 和 recovery。

本地最优索引和远端最优索引不一定相同。ART 在 CPU cache 中的多层下降成本
较低，但远端每个依赖指针都可能增加一次网络往返；固定 bucket 的 Hash/Cuckoo
更适合限制远端 probe 数。产品应坚持“一份语义权威”，不必把“一份物理索引”
当成不可妥协目标。短期可以由 host 维护只读 RDMA projection，长期再决定是否
把权威格式收敛到可直接远端读取的布局。

## 7. 与 owner 草案的主要差异

| 方面 | 原始草案中的倾向 | 本文建议 |
| --- | --- | --- |
| 文档定位 | 既有高层 SHM 方案，也有多份按复杂度、推算延迟、空间、代码量和假设 workload 比较的本地引擎草案，但没有统一实验收敛 | 把草案视为假设集合，以语义、标准和统一 workload 逐项筛选 |
| region 增长 | 草案不一致：`kvspace-shm.md` 的 v0 使用固定 region，`kvspace-c.md` 另行推荐巨大 VMA 加在线 `ftruncate` | v1 固定完整 region；增长使用新 region、chunk 或维护窗口 |
| 目录 | 一度尝试用 ART 节点消除 Index | 保留 Index、ExtIndex、LinkIndex，树只负责 key 索引 |
| 默认引擎 | 已比较查找复杂度、推算延迟、回收、空间和实现量，并按假设 workload 给出条件性选择；尚无统一 trace 实测 | 再纳入提交协议、长期 churn 和真实 trace；无数据时以 ArtBox 为保守候选 |
| Hash | 强调期望 O(1) 点查 | 同时计入完整 key hash、全容量 table image 和 prefix scan |
| 崩溃恢复 | 草案描述了 process-shared 锁、自旋锁或 CAS/seqlock，但没有 owner-death 后的结构恢复协议 | robust lock 只提供通知；必须 COW、最后发布、只读 Prepare 和可重复 Apply |
| 生命周期 | 已说明进程退出后 region 存在、机器重启丢失、固定容量和显式 `shm_unlink` | 保留这些边界，并补充最后 unmap 后同步对象保证、同名重建 split-brain 和物理容量限制 |
| RDMA | 同一 region 注册为 MR；GET 用 seqlock/CRC 校验，远端 mutation 走 RPC；未隔离原生控制面，也未定义 one-sided reader 的延迟回收 | 保留可校验 GET 与 RPC 写边界；控制面不暴露，并增加 incarnation、epoch/lease 和 rkey 失效协议 |
| 一致性目标 | 同一物理 region 和同一布局 | 一份语义权威；允许远端 read projection，不强迫本地和远端共用同一索引 |

## 8. 落地顺序

### 第一阶段：固定本地正确性边界

- 保持当前固定 section、显式小端记录、offset 引用和 robust mutex；
- 保持 COW、root/selector 最后发布和从权威对象图重建 allocator；
- 明确文档保证仅覆盖目标 Linux/glibc 上的进程死亡；
- 把 Destroy 定义为离线操作，并为长期生命周期保留 host mapping。

### 第二阶段：用 trace 选择引擎

- 记录并回放 kvlang 的实际 KV 调用序列；
- 同时测 engine 层和完整语义层；
- 在改掉全 table image 提交前，不把大容量 HashBox 作为更新型默认值；
- 按持续 churn、空间放大和维护暂停决定是否启用 ArtBump；
- 在确认真实瓶颈前，不新增 HOT、Masstree 或 cuckoo 持久化实现。

### 第三阶段：只读 RDMA 数据面

- 先实现 read-only export/projection；
- 加入 version、incarnation、checksum 和 epoch/lease；
- mutation 和复杂语义保持 RPC；
- 用远端往返次数、重试率、回收延迟和 MR/rkey 更新成本验证方案。

### 第四阶段：按测量结果扩展

只有在全局 mutex 已成为可重复的瓶颈后，再考虑分段锁、node-level lock 或
optimistic read。只有在 RPC mutation 明确不能满足目标时，才讨论 remote
write/atomic；该步骤需要独立设计锁租约、日志、客户端故障解锁和远端 allocator，
不能在现有 pthread 协议上直接打开 `REMOTE_WRITE`。

## 9. 参考资料

- [POSIX `mmap`](https://pubs.opengroup.org/onlinepubs/9799919799/functions/mmap.html)
- [POSIX robust mutex](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_mutex_lock.html)
- [Linux `shm_open` / `shm_unlink`](https://man7.org/linux/man-pages/man3/shm_open.3.html)
- [The Adaptive Radix Tree](https://db.in.tum.de/~leis/papers/ART.pdf)
- [Pilaf: Scalable Multi-Core In-Memory Key-Value Storage Using RDMA](https://www.usenix.org/system/files/conference/atc13/atc13-mitchell.pdf)
- [FaRM: Fast Remote Memory](https://www.usenix.org/system/files/conference/nsdi14/nsdi14-paper-dragojevic.pdf)
- [Using RDMA Efficiently for Key-Value Services](https://www.cs.cmu.edu/~dga/papers/herd-sigcomm2014-readable.pdf)
- [LMDB: A Symmetric Key/Value Store](https://www.openldap.org/pub/hyc/mdb-paper.pdf)
