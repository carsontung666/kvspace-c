# 关于 KVSpace SHM 方案的几个问题

我把 `deepx-design` 里几份 SHM 草案和现在的 `kvspace-c` 实现放在一起看了
一遍。整体方向没有问题：KVSpace 确实适合放进共享内存，本地进程不应该再绕
一圈网络；远端读取也可以考虑 RDMA。

现在比较难判断的是，几份草案其实在讨论不同阶段的问题：

- `kvspace-shm.md` 更像 RDMA-first 的总设计，选了固定 region、cuckoo、
  seqlock/CRC、one-sided GET 和 RPC 写；
- `kvspace-c.md` 重点在本地 ART，实现上又选择了大 VMA 加动态
  `ftruncate`；
- ArtBox、Hash、memkv 几份文档是在比较本地索引和 allocator。

它们不完全矛盾，但还没有合成一份可以直接照着实现的设计。下面不是重写一份
spec，而是我认为下一轮需要先讲清楚的几个点，以及如果现在让我实现，我会怎么
选。

当前持久格式和恢复细节仍以
[DESIGN_RESOLUTIONS.md](../DESIGN_RESOLUTIONS.md) 为准。

## 1. Region 到底要不要在线扩容

### Owner 现在的想法

`kvspace-shm.md` 的 v0 是固定 region，创建时给定容量，不自动扩容。后来的
`kvspace-c.md` 又推荐了另一种做法：先映射一个很大的虚拟地址范围，比如
64 GB，底层文件先只扩到 128 MB；空间不够时再 `ftruncate`，原 mapping
继续使用。

这个方案的吸引力很直接：所有引用仍然是 `base + offset`，不用 remap，也不
需要 chunk table。

### 我想先问的问题

1. 在线扩容是实际需求，还是为了避免让调用方填写 `max_size`？
2. 所有 attach 进程能否保证一开始就使用相同的最大 mapping 长度？旧版本
   client 怎么处理？
3. `ftruncate` 失败、tmpfs 配额耗尽或者进程在更新 size 的中间死亡时，谁来
   决定哪些 offset 已经可以访问？
4. 如果同一块内存以后要注册为 RDMA MR，扩容后是重新注册、增加新 MR，还是
   让所有客户端继续使用旧 rkey？

最大的问题是，mmap 成功不等于 EOF 后面的页已经可以安全访问。访问完整 EOF
之后的页面可能得到 `SIGBUS`；底层文件改变大小后，不能把所有既有 mapping
会立刻安全扩展当成可移植协议。LMDB 也没有把 resize 做成无协调的透明操作，
其他进程需要发现 map size 变化并重新采用新配置。

### 我的建议

本地 v1 不做在线扩容：

1. Create 先完整计算 header、queue、allocator 和 engine section；
2. 所有 checked arithmetic 和容量检查通过后，执行
   `ftruncate(fd, max_size)`；
3. 映射完整 `[0, max_size)`；
4. 最后才初始化 header 和 ready 状态。

Attach 先用 `fstat` 和有界读取验证 magic、版本、layout hash、engine ID 和
完整文件长度，再映射整个 region。这样 allocator 的逻辑容量耗尽可以作为普通
的 `ErrCapacity` 返回，也不会因为进程看到不同的文件长度而访问完整 EOF 之后
的页面。这里仍有一个绕不过去的边界：`ftruncate` 只建立逻辑长度，如果 tmpfs
或文件系统在后续 page fault 时真正耗尽空间，进程仍可能收到 `SIGBUS`。

如果以后证明确实需要增长，我倾向于二选一：创建新 region 后切换
generation，或者增加固定大小的 chunk/MR。两种方案都显式通知客户端，虽然
代码多一点，但不会把文件大小变化藏在 allocator 里面。

## 2. 能不能用 ART 节点代替目录 Index

`kvspace-c.md` 前半部分提出，目录就是 ART 中的公共前缀节点，所以普通目录
不必再保存 `Index` TLV；`List` 可以直接返回节点的 children。这个思路能省掉
目录值的更新和 TLV 编解码。

但同一份文档后面的完整 API 设计又恢复了 `/a/ -> Index TLV`。Hash 草案也
明确依赖显式 Index。这里需要先选定语义，不能让不同 engine 各自解释目录。

这里我卡在一个很具体的反例：ART 的 child edge 只有一个 byte，它什么时候等于
一个直接子项名？例如：

```text
/a/bb/x
/a/bc/y
```

在 `/a/` 下面，ART 完全可能只有一个 edge `b`，到更深的位置才分成 `bb` 和
`bc`。读取 `/a/` 对应节点的 children，得不到应该返回的两个目录项。路径压缩
还可能让 `/a/` 根本不是一个独立物理节点。

另外还有三个不能靠树形推导的情况：

- 空目录没有 child，但仍然应该存在；
- `ExtIndex` 同时保存本地 children 和 fallback；
- `LinkIndex` 会改变路径解析。

即使能直接找到 children，返回 k 个名字也至少要写出 k 个结果，不能说
`List` 是 O(1)。

所以我不会删 Index。我会让四个 engine 共用同一套目录语义：

- 普通目录存 `Index`；
- 扩展目录存 `ExtIndex`；
- link 目录存 `LinkIndex`；
- `/a` 和 `/a/` 继续作为两个不同 key；
- `List(prefix, false)` 只读本地 children；
- `List(prefix, true)` 在 link 解析后合并 ExtIndex fallback。

ART、Hash、Trie 只负责“完整 key 到 value”的物理索引，不负责重新定义目录。
`DelTree` 的最终删除也应该扫描实际 key 前缀，不能只相信可能被用户写坏或已经
不一致的 Index。

## 3. 四个 engine 里应该默认用哪个

几份草案对 engine 的分析其实不差，而且比较的不只是复杂度：

- ArtBump 的节点简单，value/prefix 追加写，但长期运行需要 Compact；
- ArtBox 用可回收 allocator 换掉 bump zone；
- Hash 的点查路径短，目录继续走显式 Index；
- 256 叉 Trie 实现直接，但稀疏路径浪费空间。

这些判断大体合理。缺少的是同一组 workload 下的实测，所以现在还不能从这些
分析直接得到“默认 engine”。

我还缺几个最基本的 workload 数字：实际 `max_entries` 和平均 live entries、
读写比、key 的长度和 fanout，以及服务能不能接受 Compact。一次 VM step 里
Get、Set、List、DelTree 各有多少，也会直接改变选择。

这些问题会直接改变结果。比如当前 HashBox 的 Get 很短，但 mutation 不是普通
的原地 linear-probing update。它会清空 inactive table、扫描 active table、
把 live entry 重插到 inactive table、发布 selector，然后清空旧表。这里至少
包含按配置容量增长的清零和扫描，即使当前只有很少的 live key。

另外，公共 `Set` 对每个 pair 分别 Begin/Commit。kvlang 更新 PC 和 status 时
一次传两个 pair，因此 HashBox 会做两次 table image。这个成本可能比 hash
函数或 ART Node16 的差异大得多。

TrieBox 也有类似的“理论结构和当前实现不是一回事”的问题：Trie 理论上可以
先走到 prefix 再枚举，但当前 `EntriesWithPrefix` 会先遍历全树再过滤。

我的做法是先不把四个 engine 当成四个长期产品选项，而是用它们做一次同条件
选择：

- engine 层测 Get、Put、Erase、EntriesWithPrefix；
- `ShmClient` 层再测 TLV、Index、Link/ExtIndex、Notify 和 DelTree；
- 容量至少覆盖 1K、10K、100K、1M，并区分配置容量和 live entries；
- 单独测持续热更新、ArtBump 到第一次 Compact，以及 1/4/16 个进程争用；
- 除了吞吐，记录 p99、触碰字节、allocator 高水位、物理空间和恢复时间。

如果现在必须先给一个默认值，我会先用 ArtBox。原因不是它一定最快，而是它
有路径压缩，也能正常回收 value，长期运行的风险比 ArtBump 小；同时不会像
当前 HashBox 那样让每次写都包含完整 table image。

这个选择应该是临时的。真实 trace 如果证明短 key Trie 更快且空间可以接受，
就选 Trie；如果 HashBox 的 point-read 优势决定了总时间，就先重做 Hash 的
mutation protocol，再讨论默认值。没有数据前不值得再实现 HOT、Masstree 或
另一套 cuckoo 持久化 engine。

## 4. 进程死在 mutation 中间时，谁是可信状态

高层 SHM 草案已经说明：进程崩溃后 region 继续存在，机器重启后可以丢；
并发方案里也出现了 process-shared mutex、自旋锁以及 CAS/seqlock。这个保证
边界是合理的。

没有展开的是 owner death 之后如何判断 allocator、counter 和 root 哪一个
可信。普通 mutex 只解决并发，不能把死亡进程写了一半的数据自动回滚。

我主要关心三类 crash window：

- root 发布前后，counter、新对象和旧对象分别处于什么状态；
- allocator 或 recovery 自己写到一半又被杀；
- 最后一个 client unmap，或者 Destroy 后旧 mapping 与同名新对象并存。

如果这些状态没有唯一的 authority，就只能把“看起来合理”的 allocator 元数据
当真，恢复很容易提前复用仍被 root 引用的对象。

本地 v1 我会继续用一个 region-wide robust mutex。它不利于多核扩展，但先把提交
边界做清楚更重要：

1. 在未发布区域写完整的新节点、value 或 table image；
2. 完整校验新结构；
3. 写入可推导的 counter，并递增仅用于诊断的 generation；
4. 以 release store 发布 root 或 active-table selector；
5. 发布之后才回收旧对象。

我倾向于仍把 root/table selector 当作语义 authority。counter、free list、
bitmap 等从已提交对象图重新生成，generation 只做诊断，不参与内容正确性判断。

拿到 `EOWNERDEAD` 后，恢复分成两段：Prepare 只读并生成完整修复计划；全部
检查通过后 Apply 才开始写。成功后再调用 `pthread_mutex_consistent`。确定性
损坏应该 poison mutex；临时分配失败不能把 region 标成“已经修好”。Apply
中途再次死亡时，下一个 owner 必须可以重新 Prepare 并收敛。

这里我只会承诺“目标 Linux/glibc 上的进程死亡恢复”，不会写成断电持久化。
当前没有 `msync`/`fsync` 顺序、双 meta page 和 torn-write 模型。

还有一个容易漏掉的生命周期问题：POSIX 不保证最后一个 mapping 消失后，原
process-shared pthread 对象仍可移植地复用。当前代码可以把零 client 后重新
Attach 当作目标 Linux/glibc 上经过测试的行为；如果要把它写成更强的平台无关
契约，我会让一个 region host 在整个生命周期保持 mapping。`Destroy` 也只当
离线操作；否则旧 mapping 和同名新对象可以同时存在。

## 5. SHM 和 RDMA 是否一定要共用“同一个 region”

这是 owner 方案里我最赞同的方向：本地 mmap 和远端 RDMA 不应该维护两份
语义存储。`kvspace-shm.md` 也已经选了一个比较稳妥的 v0 边界：GET 用
seqlock/CRC 做 one-sided 校验，远端 mutation 交给 server RPC。

问题不在“one-sided GET + RPC 写”这个选择，而在当前整个本地 region 能不能
原样注册成远端协议。

当前 common header 里直接放了 `pthread_mutex_t` 和 `pthread_cond_t`。远端
reader 不会拿这个 mutex，也不能参与 futex、robust list 或 condition wake。

更麻烦的是回收。本地 reader 在全局锁内访问；writer 发布新 root 后可以立即
回收旧节点。远端 reader 可能已经读到旧 offset，但 host 根本不知道它下一次
RDMA READ 什么时候完成。如果这时复用旧空间，CRC 可以发现部分错误，却不能
代替“这个对象现在是否仍允许被引用”的生命周期协议。

还需要明确两件事：

1. ArtBox/ART 的多层依赖读取是否真的适合 one-sided RDMA？每层指针都可能是
   一次新的网络往返。
2. region 扩容或 Compact 后，旧 MR、rkey 和客户端缓存的 offset 怎么失效？

我的 v1 会保留“一份语义权威”，但不强求“一份物理索引”。同一个 backing object
可以分成两块：

```text
local-control
  pthread / condition / allocator journal / recovery state

rdma-data
  stable offset / immutable key-value / version / incarnation / checksum
```

本地进程映射全部区域。按这个边界，我不会给远端暴露控制面；客户端只拿
`rdma-data` 的 read-only rkey。

远端 Get 的最低协议是：先读 metadata，再读 immutable value，最后重读
metadata；前后 version/incarnation 一致且 checksum、key、length 都正确才返回，
否则重试。旧对象不能马上复用，要进入 retire list，等旧 epoch/lease 全部结束
后再回收。host 重启、Compact、替换 region 或重新注册 MR 时都提升 epoch、
轮换 rkey，让客户端丢弃缓存 offset。

除只读 point Get 外，其余写入和管理操作先走 host RPC。以后如果真实测量
证明 RPC 写不够，再单独设计 remote lock、lease、journal 和 allocator；
不能直接在 pthread 协议上打开 `REMOTE_WRITE`。

短期如果本地 ArtBox 不适合远端读取，可以由 host 维护一个 read-only 的
fixed-bucket/cuckoo projection。它是派生索引，不是第二份语义权威。这样比为了
“物理上只有一份 index”而让本地和远端都使用不合适的数据结构更实际。

## 6. 全局锁要不要现在就拆

本地方案倾向于一个全局 process-shared lock。实现简单，KVSpace 当前也没有
多 key transaction，因此先串行化所有操作很容易讲清楚。

设计里给出的 `shm://` 延迟目标大约是 0.1--1 us，但这个数字指的是裸
load/store、单次 Get，还是完整的 KVSpace 调用？完整调用还包含路径解析、
TLV、Index 更新、allocator、恢复检查和 pthread lock。

另外，实际部署里会同时有多少 VM、CLI 和执行器进程？如果常见情况只有一两个
writer，提前实现 node-level lock 的收益可能很小；如果十几个进程同时 Get，
全局 mutex 又可能很快成为主要瓶颈。

这里我会先忍住，不拆锁。先把单进程、2/4/8/16 进程的吞吐和 p99 测出来。
测试时把锁等待时间和 engine 内部时间分开。只有全局锁在真实 workload 上成为
稳定瓶颈后，再选择：

- read-mostly 时用 optimistic read + version；
- Hash 按 bucket/shard 分锁；
- ART 做 node-level lock 或 epoch read；
- queue 和 KV 数据拆成独立锁域。

这些方案都会扩大 owner-death 和回收状态空间，不应该只因为“全局锁看起来不
高级”就提前加入。

## 7. 我希望 owner 先确认的几个问题

在继续定默认 engine 或 RDMA layout 之前，我希望先有下面这些答案：

1. 真实 workload 到底是什么：容量、live entries、读写比和热 key 生命周期？
2. 生命周期保证到哪里：零 client 后重连、机器重启和断电分别要不要支持？
3. RDMA 只优化 point Get，还是要求全部 KVSpace 语义；“同一 region”指一份
   语义权威，还是严格的一份物理 index？

这些问题确定后，方案其实会简单很多。现在如果让我排实现顺序，我会先交付
固定容量的本地 ArtBox + 显式目录 + robust recovery；然后用真实 kvlang trace
决定 engine；最后再做 read-only RDMA view。不会同时推进动态扩容、细粒度锁和
one-sided write。

## 参考

- [deepx-design: kvspace-shm](https://github.com/array2d/deepx-design/blob/master/doc/kvspace-design-and-implementation/draft/kvspace-shm.md)
- [deepx-design: kvspace-c](https://github.com/array2d/deepx-design/blob/master/doc/kvspace-design-and-implementation/draft/kvspace-c.md)
- [deepx-design: ART + boxmalloc](https://github.com/array2d/deepx-design/blob/master/doc/kvspace-design-and-implementation/draft/kvspace-c-art-boxmalloc.md)
- [deepx-design: Hash](https://github.com/array2d/deepx-design/blob/master/doc/kvspace-design-and-implementation/draft/kvspace-c-hash.md)
- [POSIX mmap](https://pubs.opengroup.org/onlinepubs/9799919799/functions/mmap.html)
- [POSIX robust mutex](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_mutex_lock.html)
- [ART](https://db.in.tum.de/~leis/papers/ART.pdf)
- [Pilaf](https://www.usenix.org/system/files/conference/atc13/atc13-mitchell.pdf)
- [FaRM](https://www.usenix.org/system/files/conference/nsdi14/nsdi14-paper-dragojevic.pdf)
- [HERD](https://www.cs.cmu.edu/~dga/papers/herd-sigcomm2014-readable.pdf)
