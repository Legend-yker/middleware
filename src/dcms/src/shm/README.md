# SHM 共享内存分配器

---

## 一、整体三层架构

这套系统从上到下分三层，每层只和相邻层打交道：

```
用户代码
    ↓ 调用 slab_alloc / slab_free
Slab 层 —— 把大块内存切成碎大对象，管理对象级空闲链表
    ↓ 调用 buddy_alloc / buddy_free
Buddy 层 —— 按 2 的幂次管理片内存，负责大块的分配和合并
    ↓ 直接读写
pool[] —— 一片连续字节数组（模拟共享内存）
```

设计来源是 Linux 内核 mm/ 子系统，dcms 做了针对共享内存的改造，本代码是对 dcms 的精简复现，去掉了锁、无锁缓存层、日志，只留核心算法。

---

## 二、基本单位：PAGE 和 PFN

整片 pool 被等分成很多 4096 字节（4KB）的块，每块叫一个 PAGE，这是整个系统的最小分配粒度，所有操作以 PAGE 为单位对齐。

PFN（Page Frame Number，页帧号）就是"这是第几个 PAGE"，从 0 开始：

```
pool:
[ 4KB | 4KB | 4KB | 4KB | 4KB | 4KB | 4KB | 4KB ]
[pfn=0|pfn=1|pfn=2|pfn=3|pfn=4|pfn=5|pfn=6|pfn=7]

pfn = (某地址 - pool起始地址) >> 12
```

pfn 是整套系统的核心索引：`page_table[pfn]` 是这个 PAGE 的管理元数据，`ref_array[pfn]` 是这个 PAGE 上对象的空闲链表节点，两者都以 pfn 为下标，通过同一个索引关联到同一块物理内存。

---

## 三、Buddy 层

### 常量

| 常量 | 值 | 说明 |
|------|----|------|
| `BUDDY_PAGE_SHIFT` | 12 | PAGE 大小用位移表示，12 代表 2^12 = 4096 字节，所有地址到 pfn 的转换都用位移运算（>> 12）而不是除法，速度更快 |
| `BUDDY_PAGE_SIZE` | 4096 | PAGE 的字节数，等价于 1 << 12 |
| `BUDDY_MAX_ORDER` | 11 | buddy 最多管理 11 个阶（下标 0 到 10），最大单次分配 = 2^10 个 PAGE = 4MB。这是数组上界，实际用到几阶由 pool 大小决定 |
| `PG_BUDDY` | 0x01 | 标志位，置位表示这块在 buddy 的空闲链表里，可以被分配。buddy_alloc 取出块时清除，buddy_free 归还时置位 |
| `PG_SLAB` | 0x04 | 标志位，置位表示这个 PAGE 已被 slab 层拿走，buddy 不能再动它。slab_page_setup 时置位，slab page 完全归还后、调用 buddy_free 前清除。两个标志互斥，不会同时置位 |

### struct bpage —— 每个 4KB PAGE 的"管理卡片"

这是整个系统最核心的结构体，有 11 个字段，每个字段在 buddy 模式和 slab 模式下含义不同，这种复用是节省内存的关键设计。

**flags (uint32_t)**

存放这个 PAGE 当前的状态标志位组合：

- `0`：正在被使用，已分配出去，不在任何链表中
- `PG_BUDDY(0x01)`：在 buddy 空闲链表中，可以被分配
- `PG_SLAB(0x04)`：被 slab 层持有，是某个 slab_cache 的头页
- `PG_SLAB|PG_TAIL(0x0C)`：是 slab 复合块的尾页

用标志位而不是枚举，原因是 PG_SLAB 和 PG_TAIL 可以同时存在（尾页既需要标记"属于 slab"，又需要标记"我是尾页"），枚举无法表达这种组合。

**order (uint32_t)**

在 buddy 模式下：这个块包含 2^order 个连续 PAGE。order=0 表示 1 个 PAGE(4KB)，order=3 表示 8 个 PAGE(32KB)，只有块的头页的 order 字段有意义。

在 slab 模式下：这个字段不被 slab 层改写，但也不再代表 buddy 阶的含义——slab 通过 `slab_cache->page_order` 知道这个页有多大，不依赖 bpage.order。

**refcount (int32_t)**

引用计数。buddy_alloc 返回时设为 1，buddy_free 时先 refcount--，只有降到 0 才真正合并并放回空闲链表。设计目的是支持消息零拷贝：多个订阅者同时引用同一块内存，每个 subscriber 持有一个引用，全部释放后内存才真正回收。本简化版中始终为 1，未演示多引用场景。

**link_next / link_prev (struct bpage \*)**

这两个字段的含义完全取决于当前的 flags 状态，是整个系统内存复用设计的精髓：

当 `flags == PG_BUDDY` 时，这两个字段把 bpage 串联进 `free_area[order]` 的双向循环链表。link_next 指向链表中下一个空闲块，link_prev 指向上一个，两端都连接到 sentinel 哨兵节点。

当 `flags == PG_SLAB` 时，link_next 把 bpage 串联进 `slab_cache.partial` 或 `slab_cache.full` 的单向链表。link_prev 在 slab 模式下不使用，保持 NULL。

一个 PAGE 要么在 buddy 管要么在 slab 管，这两种状态严格互斥，所以复用同一对指针字段不会产生歧义。

**slab_cache (void \*)**

这个字段根据 bpage 是头页还是尾页，指向完全不同的东西：

- 头页（flags 中没有 PG_TAIL）：指向拥有此 slab page 的 `struct slab_cache*`。slab_free 时从对象地址找到 bpage，再从 bpage.slab_cache 找到该归还到哪个缓存，实现了"只知道地址就能 free"的接口设计。
- 尾页（flags 中有 PG_TAIL）：指向这个复合块的头页 `struct bpage*`。这是 O(1) 头页查找的关键。多页对象（如 8KB 对象跨 2 个 PAGE）在 slab_free 时，输入的 ptr 可能对应任意一页，通过 `if(page->flags & PG_TAIL) head = page->slab_cache` 一步就能取到头页，无需遍历任何链表。

**freelist (void \*，实际是 struct sobj_ref \*\*)**

只在头页中有意义。指向本 slab page 内第一个可用空闲对象的 sobj_ref 节点。这是一条单向链表的头指针，链表中的每个节点对应一个已归还但尚未再分配的对象。alloc 从这里弹出链表头节点。

注意 freelist 保存的不是对象地址，而是 `sobj_ref*`，需要通过 ref_to_addr() 换算成真实地址。

**nr_free (int16_t)**

本 slab page 内，已经被分配出去、又被 free 回来的对象数量，这些对象都在 freelist 链表中等待被再次分配。alloc 从 freelist 取走时 nr_free--，free 归还时 nr_free++。

**nr_total (int16_t)**

本 slab page 能容纳的对象总数，计算公式：`(2^page_order × PAGE_SIZE) / obj_size`。初始化时赋值，之后整个生命周期不变。

**nr_uninit (int16_t)**

本 slab page 内，从来没有被分配出去过的对象数量，初始值等于 nr_total。每次从"未初始化区"按序切出一个对象时 nr_uninit--。被 free 回来的对象进 freelist，不会增加 nr_uninit。

nr_free 和 nr_uninit 的区别很重要：nr_free 是"用过又还回来的"，nr_uninit 是"从没用过的"。两者之和加上"当前正在使用的对象数"等于 nr_total：

```
正在使用 = nr_total - nr_free - nr_uninit
```

当这个值等于 0 时，slab page 完全空闲，可以还给 buddy。

### struct free_area —— 单阶空闲链表

```c
struct free_area {
    struct bpage  sentinel;
    int32_t       count;
};
```

**sentinel (struct bpage，注意是值不是指针)**

哨兵节点（dummy head），是链表的虚拟起始节点。空链时 `sentinel.link_next = &sentinel`，`sentinel.link_prev = &sentinel`，自环。非空时 `sentinel.link_next` 指向第一个真实空闲块。

用哨兵模式的好处：insert 和 delete 操作不需要判断链表是否为空的边界情况，因为哨兵永远存在，操作前后链表的"两端"始终有节点，代码简洁很多。

哨兵节点的 flags、order 等字段没有意义，只当作链表锚点用。

**count (int32_t)**

本阶当前有多少个空闲块。list_add 时++，list_del/list_pop 时--。主要用于 buddy_dump 打印时快速知道每阶有多少块，以及快速判断某阶是否有空闲块（count==0 就跳过）。

### struct buddy_zone —— 整个 buddy 分配器的根

**\*pool (uint8_t)**

指向调用者传入的字节数组，这是真正被管理的、会被分配给用户的内存，所有返回给用户的地址都在 `[pool, pool+pool_size)` 范围内。必须按 PAGE_SIZE(4096) 字节对齐，否则 pfn 计算会出错。

**pool_size (size_t)**

pool 数组的总字节数，必须是 PAGE_SIZE 的整数倍。决定了 nr_pages 的值。

**nr_pages (uint32_t)**

= pool_size / PAGE_SIZE。page_table 数组和 ref_array 数组的长度都等于这个值。所有合法 pfn 的范围是 `[0, nr_pages)`，越界就是 bug。

**page_shift (uint32_t)**

固定等于 BUDDY_PAGE_SHIFT=12，存在结构体里是为了计算时不依赖全局宏，理论上可以支持不同 PAGE 大小的配置（虽然本实现固定 4KB）。用途：`pfn = offset >> page_shift`，`addr = pool + (pfn << page_shift)`。

**max_order (uint32_t)**

free_area 数组的有效上界（不含），合法阶范围是 `[0, max_order)`。由 pool 大小在 init 时动态计算，不超过 BUDDY_MAX_ORDER。注意 max_order 比真正最大可用阶大 1，最大可用阶是 max_order-1。例如 512KB=128PAGE，max_order=8，最大可用阶=7，最大单次分配=2^7=128PAGE=512KB。

**\*page_table (struct bpage)**

指向调用者提供的 bpage 数组缓冲区，长度=nr_pages。page_table[i] 对应 pool 中第 i 个 4KB PAGE 的元数据。地址关系：page_table 与 pool 是 1:1 对应，`page_table[pfn] ↔ pool + pfn*4096`。

这个缓冲区存在 pool 之外的普通内存里（由调用者提供），与 dcms 不同（dcms 把 page_table 嵌入共享内存头部）。本简化版分开存是为代码更清晰。

**free_area[BUDDY_MAX_ORDER] (struct free_area[11])**

核心数据结构，11 个槽位，free_area[i] 管理所有大小为 2^i 个 PAGE 的空闲块。初始化完成后（512KB pool）的状态：只有 free_area[7] 有 1 个块（整个 pool），其余都是空链表。

### Buddy 核心算法详解

**分配 (buddy_alloc)**

从 free_area[order] 开始向上找第一个非空的阶 cur，取出块之后调用 expand 把多余部分折回低阶。expand 的过程：设取到的块大小为 2^cur，目标是 2^order，从 high=cur low=order 逐步对折，每次把后半块加入 free_area[high-1]：

```
例: cur=3, order=1, 得到 8PAGE 的块 [0,1,2,3,4,5,6,7]
high=3→2: 后 4PAGE [4,5,6,7] → free_area[2]，剩 [0,1,2,3]
high=2→1: 后 2PAGE [2,3]    → free_area[1]，剩 [0,1]
返回 [0,1] (2PAGE, order=1 的块) ✓
```

最终清除 PG_BUDDY 标志，设 refcount=1，返回头页指针。

**释放 (buddy_free)**

引用计数减一，降到 0 后执行 coalesce_and_add。合并算法的核心是两个位运算公式：

```
buddy_pfn  = pfn XOR (1 << order)    // 翻转第 order 位 = 找到伴侣
merged_pfn = pfn AND buddy_pfn       // 取公共前缀 = 合并后的头位置
```

为什么 XOR 能找到伴侣？因为 buddy 系统把内存按 2 的幂次对齐分组，同一对伴侣的 pfn 只有第 order 位不同，XOR 翻转那一位就能得到对方。

为什么 AND 能得到合并头？两个 pfn 的 AND 就是取它们的公共二进制前缀，也就是两者中较小的那个（同阶伙伴一定是一大一小），合并后的块头永远在地址较小的那个。

```
二进制示例（order=1）：
pfn=0110(6): buddy=0110^0010=0100(4), merged=0110&0100=0100(4)
pfn=0100(4): buddy=0100^0010=0110(6), merged=0100&0110=0100(4)
两者 merged 结果相同，算法天然对称 ✓
```

合并时检查伙伴是否满足三个条件：pfn 在合法范围内、flags==PG_BUDDY、order 相同（不能把不同大小的块合并）。合并成功后升阶继续，直到找不到合法伙伴或达到 max_order 上限。

**初始化 (buddy_zone_init)**

不手动计算 free_area，而是先把所有 page 的 refcount 设为 1（标记为"已分配"），然后对每个页调用 coalesce_and_add（free 的内核逻辑）。由于合并算法会自动把相邻同阶块合并成更大阶，遍历完所有页后 free_area 自然形成正确的初态，等价于把整片 pool 注册进 buddy。

---

## 四、Slab 层

### 新增标志位 PG_TAIL = 0x08

这个标志与 PG_SLAB 同时使用（组合值 0x0C），标记"这是 slab 复合块的尾页"。

背景：一个 slab 对象可能跨多个 PAGE（如 8KB 对象占 2 个 PAGE）。slab_free 时输入的 ptr 是对象起始地址，对应的 bpage 可能是这个多页块的任意一页，但所有管理元数据（freelist/nr_free/nr_total/nr_uninit/slab_cache）都只存在头页里。如果没有 PG_TAIL 机制，找头页需要遍历 page_table，O(n)。有了 PG_TAIL，直接一步取到：

```c
static inline struct bpage *get_head_page(const struct slab_ctx *ctx, const void *ptr) {
    uint32_t pfn = ((const uint8_t*)ptr - ctx->mem_base) >> BUDDY_PAGE_SHIFT;
    struct bpage *page = &ctx->zone->page_table[pfn];
    if (page->flags & PG_TAIL)
        return (struct bpage *)page->slab_cache;  // 尾页：slab_cache 存的是头页指针
    return page;                                   // 头页：自身就是
}
```

### struct sobj_ref —— 外部空闲链表节点

这是 dcms 相对于标准 Linux slab 最核心的差异，整个设计的灵魂所在。

标准 slab 的做法是把"下一个空闲对象的地址"写进空闲对象本身的头几个字节，分配时弹出，释放时头插。这在普通堆内存里没问题，但 dcms 的对象在 mmap 共享内存里，这会导致两个致命问题：

第一，共享内存被多个进程 map 到不同的虚拟地址，写进去的指针（虚拟地址）在另一个进程看来是完全错误的地址，根本没法用。

第二，消息对象在传输过程中，发送方写入了数据，如果接收方能看到对象的"头几字节"被空闲链表指针污染，数据就被破坏了。

sobj_ref 的解决方案：在每个进程自己的普通内存里（不在共享内存 pool 里）维护一个 ref_array，`ref_array[pfn]` 就是 pfn 对应 PAGE 上那个对象的链表节点。空闲链表完全不接触 pool 的内容。

**refcount (int16_t)**：对象的引用计数，记录有多少地方在同时使用这个对象。dcms 零拷贝消息传递时，多个 subscriber 收到同一条消息，每人持有一个引用，全部读完才真正 free。

**flags (uint16_t)**：对象级别的状态，`SOBJ_BUSY=0x0001` 置位表示已分配出去，清零表示空闲。alloc 时置位，free 时清零。可以用来检测 double-free（释放一个 flags 已经是 0 的对象说明了有问题）。

**\*next (struct sobj_ref \*)**：空闲链表指针，指向同一个 slab page 内下一个空闲对象对应的 sobj_ref，链表末尾为 NULL。这个指针是 ref→ref，不是对象地址→对象地址。真正的对象地址由 ref_to_addr() 计算：`addr = mem_base + (ref - ref_array) * PAGE_SIZE`，即用 ref 在数组中的下标（等于 pfn）还原出物理地址。反过来 addr_to_ref()：`pfn = (addr - mem_base) >> 12`，`ref = &ref_array[pfn]`。

### struct slab_cache —— 固定大小对象池

**\*partial (struct bpage \*)**：有空位的 slab page 链表头，单向链表，通过 `bpage->link_next` 串联。链表中每个节点是一个 slab page，还有至少一个对象可以分配（nr_free>0 或 nr_uninit>0）。alloc 优先从这里找 slab page。名字来自 Linux：部分填满。

**\*full**：全满的 slab page 的链表头，nr_free=0 且 nr_uninit=0，没有可分配对象。维护 full 链表的目的是：当一个对象被 free 时，需要把它所在的 slab page 从 full 移到 partial，而找到这个 slab page 的 bpage 可以通过 get_head_page()，但判断它在 full 还是 partial 链表里需要检查 nr_free+nr_uninit 是否为 0，找到后从链表删除需要知道它在哪条链表。

**nr_partial / nr_full (int32_t)**：对应链表的节点数量。作用是 O(1) 判断是否有可用 slab page（`nr_partial > 0`），以及 alloc 决策：有 partial 就用，没有就向 buddy 申请新的。

**obj_size (int32_t)**：这个 slab_cache 分配的每个对象的字节数。必须是 PAGE_SIZE(4096) 的整数倍，原因是 ref_array 以 PAGE 粒度索引，若对象不从 PAGE 边界开始，就无法用单一 pfn 唯一标识一个对象的起始位置，addr_to_ref 和 ref_to_addr 的互换计算就会出错。

**objs_per_page (int16_t)**：每个 slab page 能容纳的对象数，计算公式：`(2^page_order × PAGE_SIZE) / obj_size`。例如 page_order=3，obj_size=8192：`(8 × 4096) / 8192 = 4`。这个值在 slab_cache_create 时算好，之后不变，所有 slab page 的 nr_total 都等于这个值。

**page_order (uint8_t)**：每次向 buddy 申请新 slab page 时的 order 参数，slab page 大小 = `2^page_order × PAGE_SIZE`。选择原则是让 objs_per_page 足够大（建议≥4），否则 slab page 管理开销占比太高，效率差。

**name (char[32])**：调试用名称，buddy_dump/slab_dump 打印时显示，对应实际业务中的消息类型名（如 "sensor_8k"、"cmd_4k"）。

### struct slab_ctx —— 全局上下文

**\*zone (struct buddy_zone \*)**：底层 buddy 分配器，slab 通过这个指针调用 `buddy_alloc(zone, page_order)` 申请新 slab page，通过 `buddy_free(zone, head)` 归还 slab page，通过 `zone->page_table[pfn]` 访问任意页的元数据。

**\*mem_base (uint8_t \*)**：= zone->pool，pool 的起始地址。单独存一份是为了计算方便，所有 pfn 计算都以 mem_base 为基准，不用每次写 zone->pool。

**\*ref_array (struct sobj_ref \*)**：由调用者分配，长度 >= zone->nr_pages。ref_array[i] 是 pool 中第 i 个 4KB PAGE 上的对象的链表节点。这个数组存在进程私有普通内存里，不在共享内存 pool 里，是实现共享内存安全的关键。

**ref_len (uint32_t)**：= zone->nr_pages，ref_array 的有效长度，用于越界检查。

**caches[SLAB_MAX_CACHES] (struct slab_cache[8])**：内嵌数组，最多同时管理 8 种不同大小的对象缓存。内嵌而不是指针+动态分配，是因为 slab_ctx 本身就是在调用者控制的内存里（可以是栈或者专门的元数据区），整个系统不依赖 malloc，完全可以用在共享内存场景里。

**ncaches (int32_t)**：已创建的 slab_cache 数量，同时也是下次 slab_cache_create 时要写入 caches 数组的下标。达到 SLAB_MAX_CACHES=8 时返回 NULL。

### Slab 核心算法详解

**slab_alloc 的两条取对象路径**

从 partial 链表找到头页后，有两种方式取对象，优先 freelist：

路径一（freelist，已归还的对象）：

```c
ref = (struct sobj_ref *)head->freelist
head->freelist = ref->next          // 弹出链表头
head->nr_free--
ref->flags = SOBJ_BUSY
obj = ref_to_addr(ctx, ref)         // pfn = ref - ref_array, obj = mem_base + pfn*4096
```

路径二（uninit，从未分配过的对象，按顺序切出）：

```c
obj_idx = nr_total - nr_uninit              // 当前是第几个对象（从0开始）
pages_per_obj = obj_size >> 12             // 每个对象占几个PAGE
obj_pfn = head_pfn + obj_idx * pages_per_obj  // 这个对象从第几个PAGE开始
obj = mem_base + obj_pfn * PAGE_SIZE
head->nr_uninit--
ref_array[obj_pfn].flags = SOBJ_BUSY
```

**slab_free 的全空判断**

```c
if ((int32_t)(head->nr_free + head->nr_uninit) == head->nr_total)
```

三个计数的不变式：正在使用的对象 = nr_total - nr_free - nr_uninit。当上式成立时，正在使用=0，即所有对象都归还了。

注意 nr_uninit 不会因为 free 增加，一旦 uninit 区域的对象被切出，它就进了 freelist（nr_free），不会回去 uninit，所以如果一个 slab page 只切出了 1 个对象就被 free 了：nr_free=1，nr_uninit=3，nr_total=4，1+3==4，正确判断为全空。

全空后清除所有 PAGE 的 PG_SLAB/PG_TAIL 标志，将 freelist 置 NULL，nr_free 置 0，然后调用 buddy_free 归还，buddy_free 内部的合并逻辑会自动把相邻空闲块合并成更大的块。

---

## 五、对外接口的封装设计

调用者不需要了解任何内部细节。

**slab_free 只需要 ptr，不需要指定 cache**：因为 `ptr → pfn → bpage.slab_cache → struct slab_cache*`，全链追溯自动完成。用户只需要知道"我要 free 这个地址"，不需要记住"这块内存是从哪个 cache 分配的"。

**buddy_alloc 返回 bpage 而不是地址**：因为内部操作（设置 PG_SLAB、初始化数据）需要操作 bpage，如果返回地址用完后再用 addr_to_bpage 转换回来会多一步。用户拿到 bpage 后调用 bpage_to_addr 才取得真实地址。

**调用者负责提供所有缓冲区**：pool、page_table、ref_array 全部由调用者分配和管理，分配器本身不调用 malloc/mmap。这是关键设计约束，使得这套分配器可以直接用于共享内存（调用者用 mmap 得到 pool，用普通内存放 page_table 和 ref_array）。

三个缓冲区的关系：

- **pool**：被管理的内存，用户最终读写的地方（通常是 mmap 共享内存）
- **page_table**：pool 的管理元数据，每个 PAGE 一个 bpage，存在普通内存里
- **ref_array**：对象级别的空闲链表节点，每个 4KB PAGE 一个 sobj_ref，存在普通内存里

pool 必须 PAGE_SIZE 对齐，page_table 长度 >= nr_pages，ref_array 长度 >= nr_pages，这些在 init 时都有隐含约束，调用者必须保证。

---

## 六、关键设计亮点

**亮点1：ref_array 外部空闲链表**。不写 pool，完全不影响共享内存里的消息内容，多个进程可以安全地把同一块 pool mmap 到各自的地址空间，ref_array 在各自进程的私有内存里，互不干扰。这是整个设计最重要的创新点，也是与标准 Linux slab 最大的差异。

**亮点2：buddy 初始化自举**。不手写 free_area 的初始状态，而是借用 coalesce_and_add（free 的核心逻辑）自动完成。遍历每个 PAGE 调用一次 free，合并算法自然形成最优的初态（128PAGE 会合并为 1 个 order=7 的块）。代码量减少了，也避免了初始化逻辑和 free 逻辑不一致的潜在 bug。

**亮点3：XOR/AND 公式的数学对称性**。`buddy_pfn=pfn XOR (1<<order)`，`merged_pfn=pfn AND buddy_pfn`，这两个公式无论从哪一侧的伙伴触发，最终 merged_pfn 的结果相同，算法完全对称，不需要 `if(pfn < buddy_pfn)` 这类判断。

**亮点4：bpage 字段的双重身份**。link_next/link_prev 在 buddy 空闲时是双向链表指针，在 slab 管理时 link_next 是单向链表指针，slab_cache 字段在 buddy 时无意义，在 slab 时有两种含义（头页指 slab_cache，尾页指头页 bpage）。用 flags 区分当前状态，一个结构体支撑了三种使用模式，内存占用最小化。

**亮点5：uninit 区域的延迟切分**。slab page 刚创建时不立刻切出所有对象，而是 nr_uninit=nr_total，按需从 uninit 区顺序切，用到几个切几个。好处是 slab page 刚创建时只需要初始化头页的几个计数字段，不需要预先把所有对象的 ref 都加入 freelist，减少了一次性的初始化开销。

**亮点6：slab_cache 内嵌在 slab_ctx 里**。最多 8 个 slab_cache 内嵌在 slab_ctx 结构体里，整个 slab_ctx 可以放在调用者栈上或者一块固定内存里，完全不依赖动态分配。配合调用者提供 pool/page_table/ref_array 的设计，整套系统可以用在任何不能调用 malloc 的场景（内核、裸机、共享内存初始化阶段）。
