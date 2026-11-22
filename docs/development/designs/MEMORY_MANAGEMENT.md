# 统一内存管理 / Unified memory management

`main/services/memory_manager` 统一分池准入、部分分配记账、压力监测、清理请求和实时堆快照。生命周期层协调可重建缓存的回收，快捷栏和设置页只提交请求及显示结果。
`main/services/memory_manager` owns pool admission, accounting for migrated allocations, pressure monitoring, cleanup requests and heap snapshots. The lifecycle layer coordinates eviction; quick controls and Settings submit requests and display results.

阅读预绘帧使用分配账本，OTA 复用容量检查。管理页通过 `memory_inventory` 合并账本与现有模块仍持有的资源，覆盖常驻字体、显示缓冲、UI 字形、文件结果和网络传输缓冲。未覆盖的驱动、任务栈及临时分配保留在“未追踪/外部”。业务高峰准入另见下文；驱动、任务栈和临时分配不自动纳入账本。真实泄漏、碎片和并发峰值仍须结合设备数据判断。
Reader prefetch frames use the allocation ledger, and OTA shares capacity checks. The page merges that ledger with live module-owned resources through `memory_inventory`, covering resident fonts, display buffers, UI glyphs, file results and network transfer buffers. Uncovered drivers, task stacks and temporary allocations remain untracked/external. Startup peak admission is described below; it does not implicitly add driver, stack or scratch allocations to the ledger. Leaks, fragmentation and concurrent peaks still require device evidence.

锁屏书签只在写入可重建的 TF 图层缓存时从 PSRAM 申请 32 KiB 编码暂存；缓存命中或只读预览不申请。申请失败不退回片内 RAM，也不影响本次已解码图层的合成。波形裁剪用小型栈上工作区，不保留全表临时副本。
Lock bookmarks request a 32 KiB PSRAM encoding buffer only when writing the rebuildable SD layer cache; cache hits and read-only previews do not allocate it. On allocation failure they do not fall back to internal RAM, and compositing the decoded layer still proceeds. Waveform trimming uses small stack scratch buffers instead of keeping a complete scratch copy.

## 自动刷新 / Automatic refresh

- 天气任务栈和响应缓冲、自动 OTA 清单任务创建共用高峰准入；天气响应缓冲计入网络账本。取得已知分配后，先归还名额，再请求无线；不持有名额等待 DNS/TLS/响应。SDK 分配仍未预留。/ Weather stack/response and automatic OTA task creation share startup admission; the weather body is tracked as network memory. Release the slot before radio acquisition and DNS/TLS/I/O; SDK allocations remain unreserved.
- 自动 OTA 无需同步清理文件任务；准入忙或内存不足时一分钟后重试，不消耗十二小时周期。天气与对时、OTA 避让；普通离页只通知取消，休眠和手动升级仍等待安全交还。/ Automatic checks do not synchronously drain file jobs; busy or low-memory admission retries after one minute without spending the twelve-hour interval. Weather defers around clock sync/OTA; navigation signals cancellation, while sleep and manual OTA still require safe handoff.
- NT26 复用已准入的 4 KiB 初始化栈在后台采样已连接信号，不再逐次创建任务或在承载控制门内等待 AT。该栈在线期间保留，停止时随任务交还。/ NT26 reuses the admitted 4 KiB init stack for background signal sampling while connected. No per-sample task creation or AT wait under the bearer gate; the stack remains live until modem teardown.

## 业务高峰准入 / Business startup admission

- 阅读开书、Wi-Fi 驱动启动和 STA 关联、扫描/对时任务创建及临时联网、NT26 初始化、NimBLE 起栈及连接建立共用一个代次闸门。正在阅读或已就绪的无线不持有高峰名额；关书、停服和取消不需要再次申请。闸门不拦截 SDK 内部 `malloc`，也不预占物理堆。/ Book opening, Wi-Fi startup/association/scan/time-sync task creation and temporary bring-up, NT26 init, and NimBLE startup/link establishment share one generation gate. Ongoing reading and ready radios hold no startup slot; close, teardown and cancellation need no new admission. The gate neither intercepts SDK allocations nor reserves physical heap.
- 异步阶段只在初始化完成、确认断开或最终清理后归还。STA 超时按退避重发断开；多次未确认时仅在无 Wi-Fi 租约、传书或对时的情况下尝试停驱动。NT26 失败或取消由停止方汇合任务、释放队列和 UHCI 后归还；无 SIM 或注册拒绝若有意保留 AT 通道，不长期占用名额。/ Async peaks release on completed initialization, confirmed disconnect or teardown. STA cancellation retries with backoff; after repeated missing acknowledgments, force-stop only without Wi-Fi leases, transfers or time sync. Failed/cancelled NT26 releases after the stopper joins tasks and frees queues and UHCI resources. A deliberately retained AT channel after SIM/registration denial holds no startup slot.
- 已知必需开销才作硬检查，例如 TXT 目录块、HTTP 任务与传输缓冲、NT26 三个创建栈；保留既有蓝牙起栈安全检查。开书前按需回收可重建封面、预绘和字形缓存，不拆活动会话或已装载字体；关书直接归还书源、布局和预绘帧。/ Hard checks cover known requirements such as the TXT index block, HTTP stack/transfer buffer and three NT26 stacks; the existing BLE startup guard remains. Opening evicts rebuildable cover, prefetch and glyph caches as needed without unloading active sessions or faces; closing returns source, layout and prefetch frame.
- 开书仍先确认封面后台任务交还。无 PSRAM 压力时保留已收取的封面和待补槽；内容或排序变更仍正常失效。触发回收时依次尝试封面/挡板、预绘、字形，逐层复测，达到目标就停；预绘和字形拿不到绘制锁就跳过。PSRAM 软线（可用 768 KiB、最大块 256 KiB）只触发回收，不拒绝开书。/ Opening still waits for the cover worker to hand back borrowed resources. Without PSRAM pressure it retains collected covers and pending slots; content or sort changes still invalidate stale entries. Reclaim tries covers/guards, prefetch, then glyph caches, rechecks after each tier and stops as soon as the target is met. Skip prefetch and glyph reclaim if the draw lock cannot be obtained. The PSRAM soft targets (768 KiB free, 256 KiB largest block) trigger reclamation, not rejection.
- Wi-Fi 和 4G 分别与 BLE 互斥，不抢占已连接、连接中、配对或扫描中的翻页器。只有空闲蓝牙可以临时暂停，保留用户设置和重连目标；驱动停止且资源归还后恢复。NT26 探测、启用和恢复共用同一独占入口。用户切换承载时，已有业务借用或传书拒绝切换。启动调用失败时尝试恢复旧承载并返回原始错误；手动切换后的终止驱动/STA 错误也可触发一次恢复，SIM/注册状态保留用户选择。/ Wi-Fi and cellular are each exclusive with BLE and never preempt an active link, connection attempt, pairing or scan. Only idle BLE is suspended, retaining its setting and reconnect target until teardown. NT26 probe, enable and resume share the same exclusion gate. Existing business leases or transfers reject manual bearer changes; a failed start attempts rollback and retains the original error. A terminal driver/STA failure after a manual switch can also trigger one rollback; SIM/registration states retain the user selection.
- 快捷栏与蓝牙设置在从关到开时检查实际 Wi-Fi / 4G 独占租约和既有起栈内存保底。已被占用、容量不足或连续块不足的请求当场提示，不写开关或排队重连。关闭操作不受限；已开启但临时让位的蓝牙仍在占用承载释放后恢复。预检不预留堆，实际起栈仍在生命周期锁内复检。/ Quick controls and Bluetooth settings check the actual Wi-Fi/cellular exclusion lease and existing startup memory floors before turning Bluetooth on. A held lease, insufficient capacity or contiguity is reported immediately without saving the switch or queuing a reconnect. Turning off remains allowed; previously enabled, suspended BLE still resumes after the occupying bearer releases. Preflight reserves no heap; startup rechecks under the lifecycle lock.
- 准入忙时，错误码携带拒绝当刻占用名额的阶段，界面提示开书、无线启动或连接等具体原因；不从延后的实时状态推断。非高峰控制门忙且没有所有者信息时只提示资源正在切换。/ Peak contention errors capture the phase occupying admission at rejection for a specific UI hint; never infer it from a later snapshot. Other busy gates without an owner report that resources are switching.
- 自动续读遇高峰忙时以 500ms 间隔保留一次请求，最多 90 秒；主动导航、拔卡或真实开书错误不重试。未准入不写开书检查点。/ Auto-resume retains one request across peak contention, at 500ms intervals for at most 90 seconds. Navigation, media loss or a real open error cancels it. No open checkpoint is written before admission.

## 使用入口 / User entry points

- 快捷栏默认显示 CPU 图标的内存键，可在「快捷栏管理」调整显隐和位置。短按先收起快捷栏、恢复底页，再提交一次清理；长按 500ms 进入内存管理页，抬手不再触发短按。移动离开按键或超过容差会取消内存长按。/ The default CPU icon in quick controls (visibility/order managed in Settings) clears memory on a tap after restoring the underlay. A 500ms hold opens Memory Management and consumes the release; leaving the key or exceeding movement tolerance cancels the hold.
- 管理页属于设备设置子页。从其他页面直达后，返回键回到来源页；中键回首页。从设置内直达时，返回设备设置主页。/ Memory Management is a Settings subpage. Back returns to the originating page after a quick-controls entry from elsewhere, while the middle key opens Home. Entries from Settings return to its main page.
- 页面分别显示内部 RAM、PSRAM 的总量、已用比例、可用量、最大连续块及启动以来的最低可用量，并显示可归属资源、分配未通过次数和最近清理结果。资源归属包括常驻内存和可清理缓存，不等于可释放量。/ The page separates internal RAM and PSRAM totals, usage, free bytes, largest contiguous blocks and boot-session low-water marks. It also shows attributed resources, admission failures and the latest cleanup result. Attributed usage includes resident allocations and evictable caches, so it does not equal reclaimable bytes.
- 每 500ms 采样；普通数据变化最多约每秒刷新一次内容区，清理状态变化在下一次采样时立即呈现，题头和底部导航保持稳定。绘图本身不采样或清理。/ Sample every 500ms and refresh changed data at most about once per second. Cleanup-state transitions present at the next sample; the heading and navigation remain stable. Rendering neither samples nor cleans resources.

横竖屏快捷栏共用已保存的显隐与顺序进行绘制和命中，最多两排、每排四个；按硬件能力略过前光、震动或移动网络，旋转键不因当前页面而消失。横屏为前光滑杆保留独立触摸空间。
Portrait and landscape share saved visibility/order for painting and hit tests, capped at two rows of four. Hardware gates light, haptics and cellular; rotation stays present across pages. Landscape reserves separate slider space.

## 分配与所有权 / Allocation and ownership

| 区域 / Pool | 实际能力 / Capabilities | 回退 / Fallback |
| --- | --- | --- |
| `MM_POOL_INTERNAL` | `MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT` | 不回退到 PSRAM / Never to PSRAM |
| `MM_POOL_PSRAM` | `MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT` | 不占用内部堆 / Never to internal RAM |

当前没有独立 DMA 池或“小对象自动回退”策略。DMA 和驱动内部缓冲继续由原模块按原有能力管理。
There is no separate DMA pool or automatic small-object fallback policy. Drivers retain their existing DMA and buffer contracts.

```c
void *mm_alloc(mm_owner_t owner, mm_pool_t pool,
               size_t bytes, size_t alignment, size_t reserve_bytes);
bool mm_free(void *ptr);
bool mm_check_capacity(mm_pool_t pool, size_t minimum_free, size_t largest_block);
```

`mm_alloc` 检查参数、整数溢出、区域可用量、连续块和固定账本容量。`alignment == 0` 使用普通分配，否则须为不小于指针大小的二次幂。准入以请求字节数及调用方预留量为预算，连续块另留对齐余量；实际堆元数据和未迁移模块的并发分配仍由底层分配器决定。
`mm_alloc` checks arguments, overflow, pool capacity, contiguous blocks and ledger capacity. Zero alignment selects ordinary allocation; otherwise alignment must be a power of two at least as large as a pointer. Admission budgets requested bytes plus the caller's reserve, with an alignment margin for the contiguous-block check. Allocator metadata and concurrent unmigrated allocations remain outside this budget.

账本固定 64 项，记录指针、请求字节数、区域和 owner，不额外申请元数据堆。元数据由短临界区保护，底层堆操作在临界区外执行。进行中的请求计入 pending；分配代次变化会使旧堆样本失效，最多重采样 32 次，持续竞争时返回 `MM_FAIL_BUSY`。
A fixed 64-entry ledger records pointers, requested sizes, pools and owners without metadata heap allocations. Short critical sections protect metadata while allocator calls run outside them. Pending requests count against admission, and allocation-epoch changes invalidate stale samples. After 32 retries, contention returns `MM_FAIL_BUSY`.

`mm_free(NULL)` 成功；非账本指针及重复释放会被拒绝。调用方仍须保证唯一所有权，并同步所有借用者，不能依赖账本解决悬空指针或旧地址复用。`mm_init` 仅在启动、业务分配前调用。
`mm_free(NULL)` succeeds; foreign and duplicate frees are rejected. Callers still maintain unique ownership and synchronize borrowers; the ledger does not solve dangling pointers or stale-address reuse. Call `mm_init` only at boot before business allocations.

已迁移的阅读预绘使用 `MM_OWNER_READER`、PSRAM、16字节对齐，准入保留 1MiB 余量；失败保留原有不预绘回退。OTA 的 `memory_available` 复用区域空闲与连续块检查，保留原阈值；容量检查不预占后续下载或 TLS 的峰值。
Migrated reader prefetch uses `MM_OWNER_READER`, PSRAM, 16-byte alignment and a 1MiB admission reserve, retaining its no-prefetch fallback on failure. OTA shares free/contiguous-capacity checks at its existing thresholds; these checks do not reserve a later download or TLS peak.

`mm_get_snapshot` 保留线程安全的账本快照；`memory_inventory_snapshot` 只由 UI 任务在安全边界调用。资源模块以只读访问者交付仍持有的分配基址，统一按实际所在堆及分配容量汇总；排除静态资源、借用别名和已登记预绘帧。回调不得分配、阻塞、读文件或改变所有权。字体与布局使用无等待的阅读绘制锁，繁忙时标记“繁忙项稍后重试”；头像、封面和锁屏图结果在各自发布锁内盘点，已转交的结果不再归属原模块。采样不停止任务或清理缓存。
`mm_get_snapshot` retains its thread-safe ledger contract; only the UI task calls `memory_inventory_snapshot` at a safety boundary. Read-only visitors expose live allocation bases, summed by actual heap and allocated capacity, excluding static data, borrowed aliases and already-tracked prefetch frames. Callbacks must not allocate, block, read files or change ownership. Fonts/layout use a nonblocking reader draw lock and report busy resources as partial. Avatar, cover and lock-image results are visited under their publication locks; transferred results leave their former owners. Sampling never stops workers or evicts caches.

| 分项 / Owner | 已覆盖的资源 / Covered resources |
| --- | --- |
| 系统/显示 / System/display | 显示前帧、后帧、差分帧与脏行/列 / Display front, back and difference frames plus dirty lines/columns |
| 阅读/字体 / Reader/fonts | 常驻字体源、字体表、工作区、字形/块缓存、布局、背景及已登记预绘帧 / Resident font sources/tables/workspaces/glyphs/blocks, layout, background and tracked prefetch |
| UI 缓存 / UI cache | UI 字形数组及位图、头像缓存 / UI glyph array/bitmaps and avatar caches |
| 网络传输 / Network transfer | 已发布的传输缓冲实际容量、HTTP 任务申请的内部栈；驱动不在此列 / Published transfer-buffer capacity and requested HTTP internal stack; excludes drivers |
| 文件任务 / File jobs | 封面队列与未交付结果、锁屏图目录与像素 / Cover queue/undelivered results, lock-image catalogue/pixels |

离开阅读页会释放书籍临时资源，常驻字体仍可保留；进入管理页看到的是当前持有量。未启用的传输服务可以为零，其驱动或尚未覆盖的分配仍归入未追踪。各池未知量按已用减去可归属量重新计算并截断到零；堆、账本和所有资源不是全系统原子快照，并发变化时可能存在短暂差值，不能当作完整逐模块堆分析。
Leaving the reader releases book-specific resources while resident fonts may remain; the page shows current ownership. Inactive transfer resources can read zero, while driver or uncovered usage stays untracked. Each pool's unknown usage is recomputed as used minus attributed bytes, floored at zero. Heap, ledger and resource sampling are not globally atomic; concurrent changes can produce transient differences. This is not a complete per-module heap profiler.

## 清理调度 / Cleanup scheduling

状态为 `IDLE → REQUESTED → RUNNING → DONE/PARTIAL`。每个接受的请求有非零递增编号。已有请求等待或运行时，重复请求返回 `ESP_ERR_INVALID_STATE`；清理回调内部同样不能重入。回调返回 false 或未注册回调时记为 PARTIAL。
States follow `IDLE → REQUESTED → RUNNING → DONE/PARTIAL`. Accepted requests receive increasing nonzero IDs. A waiting or running request rejects duplicates with `ESP_ERR_INVALID_STATE`, including reentrant requests from its callback. A false or absent callback produces PARTIAL.

任意任务可以请求清理；只有 UI 主循环推进 `mm_tick`。快捷层、菜单、电源弹窗、按压反馈或待切页请求存在时暂停推进，待同步绘制和阅读借用交还后才执行回调。分配失败不会在失败任务中直接清理其他模块。
Any task may request cleanup; only the UI loop advances `mm_tick`. Quick controls, menus, power dialogs, press feedback and pending navigation defer it until synchronous drawing and reader borrows have returned. Allocation failure never evicts another module's resources on the failing task.

| 参与者 / Participant | 回收范围和门控 / Eviction and guards |
| --- | --- |
| `app_lifecycle_memory_clean` | USB 存储或 OTA 忙时返回 PARTIAL。/ Return PARTIAL during USB storage or a busy OTA operation. |
| `reader_prefetch_trim` | 尝试绘制锁，不等待；繁忙时返回 PARTIAL并保留共享字体。成功时丢弃目标与帧，保留任务和锁供重建。/ Try the draw lock without waiting; retain shared fonts when busy. Otherwise discard the target/frame and retain the worker/locks for reuse. |
| `ttf_font_cache_clear_all` | 清空全部字体槽的字形位图，并真正释放文件块/run缓冲；保留字体来源、装载信息和阅读状态。/ Clear all slots' glyphs and free file block/run buffers, retaining faces and reading state. |
| `ui_font_cache_clear` | 释放 UI 字形位图和槽数组；内置字体元数据保留，下次绘制按需重建。/ Free UI glyph bitmaps and their slot array, retaining built-in face metadata for lazy reconstruction. |

清理不停止 Wi-Fi、4G 或 BLE，不删除书籍、进度、设置或 TF 缓存，不强拆仍由后台文件任务及驱动持有的资源。`storage_jobs_quiesce` 继续只负责原有生命周期交还，不纳入手动清理，以免清理后让封面或选图服务长期暂停。
Cleanup leaves radios, books, progress, settings and card caches intact and does not dismantle resources still owned by file workers or drivers. `storage_jobs_quiesce` remains a lifecycle handoff facility rather than a manual-clean participant, avoiding suspended cover/picker services after cleanup.

报告记录各池清理前后的 free、largest 和可用净增，净增按池截断为非负。后台并发及绘图重建会影响差值；“完成”表示本轮参与者完成回收，不表示所有已用内存都可释放或真实泄漏已消除。
Reports retain each pool's before/after free and largest block, plus nonnegative net free-space gains. Concurrent activity and cache rebuilding affect these differences. DONE means participating evictions completed, not that all usage was reclaimable or a leak was eliminated.

每轮清理结束仅输出一条 `MEMORY_CLEAN v=1` 串口记录，包含请求编号、原因、完成状态、两池清理前后可用量与最大连续块，以及启动以来最低可用量；单位为字节。记录在临界区外输出，常规压力采样不产生日志，可结合页面操作核对实际回收量。
Each cleanup emits one `MEMORY_CLEAN v=1` serial record with request ID, reason, completion, before/after free bytes and largest blocks for both pools, plus boot-session low-water marks. Logging runs outside the critical section; ordinary pressure sampling stays silent. Correlate these byte values with page interactions to measure cleanup.

管理页打开时最多每五秒输出一条 `MEMORY_USAGE v=1`，记录两池已用/可用、各分项总字节数及 `partial`，便于实屏与串口核对。离开页面停止盘点和此类日志。
While the page is active, emit at most one `MEMORY_USAGE v=1` every five seconds with each pool's used/free bytes, owner totals and `partial` for panel/serial correlation. Leaving the page stops inventories and these logs.

## 自动压力监测 / Automatic pressure monitoring

UI 安全边界每秒检查一次，任一条件成立即认为内存紧张：
At UI safety boundaries, check once per second and flag pressure if any condition holds:

| 区域 / Pool | 空闲阈值 / Free threshold | 最大连续块阈值 / Largest-block threshold |
| --- | --- | --- |
| 内部 RAM / Internal RAM | `< 64 KiB` | `< 16 KiB` |
| 已存在的 PSRAM / Present PSRAM | `< 768 KiB` | `< 256 KiB` |

准入容量、碎片、账本满或底层分配失败也会提交压力信号。非法参数和持续竞争只计数，不触发压力清理。每次清理结束后，自动清理冷却 30 秒；用户请求可立即提交。持续繁忙显示 PARTIAL，不在 UI 线程无限等待或连续重试。
Capacity, fragmentation, ledger exhaustion and allocator failures also signal pressure. Invalid arguments and contention only increment failures. Each cleanup starts a 30-second automatic cooldown; users may request cleanup immediately. Busy resources produce PARTIAL without unbounded UI waits or immediate retry loops.

## 维护与验证 / Maintenance and verification

新增 owner 分配应成对迁移申请与释放，并覆盖失败退出、借用交还及重复清理；新增清理参与者须证明其可重建且没有活动借用，繁忙时应保留资源。不得为显示更漂亮的统计，把未迁移模块虚记为已追踪。
Migrate allocation/free pairs together and cover failure exits, borrower handoff and repeated cleanup. New participants must be rebuildable and free of active borrowers; retain busy resources. Never label unmigrated usage as tracked merely to improve the display.

```sh
python3 tools/run_reader_baseline.py open shelf_covers dashboard_home memory_peak memory_manager memory_inventory memory memory_ui prefetch font_cache quick quick_pixels loop lifecycle ota
python3 tools/memory_ui_host_test.py --output build/memory-preview
python3 tools/check_architecture.py
idf.py --preview build
```

宿主回归覆盖准入/所有权/并发旧样本、状态机与冷却、真实预绘线程、文件块释放重建、快捷栏能力组合/旋转/点按/长按及管理页采样/刷新/导航/字体边界。产品构建须使用根目录规定的 ESP-IDF 6.1 / ESP32-S31 配置。宿主与构建通过不能替代设备长时间运行、网络并发和实屏验收。
Host regressions cover admission/ownership/stale concurrent samples, cleanup states/cooldown, real prefetch threads, block-cache release/rebuild, quick-control capability/rotation/tap/hold combinations and memory-page sampling/refresh/navigation/font bounds. Product builds use the root ESP-IDF 6.1 / ESP32-S31 configuration. Host/build success does not replace device soak, concurrent-network or panel acceptance.
