# 事实核对基线：切歌 open 的阻塞上限与可取消性（Windows UNC / POSIX NFS+CIFS / 介质）

> 取证时点：**2026-11-23**，commit **1400cbcb8**（`master`）。
> 取证票：GitHub issue [#286](https://github.com/wt159/SoundBridge/issues/286)（本地文件 open 的阻塞上限与可取消性）。
> 取证方式：**只读公开一手文档与上游源码**（Microsoft Learn、man7.org、Linux 内核源码、厂商规格表），不跑实验、不读闭源实现。事实与推断分开标注；每条给出可点开的来源；查不到的写「未验证 + 已查范围」。
> 本机不实测：开发机只有 NVMe SSD、无网络盘，票面已裁「本机不实测」，本文**没有**任何本机实验数据。
> 本文只回答平台事实，**不做设计取舍**，也不把结论写成规格条款。

**场景锚点（只用于说明「open」具体指哪几步，不是本文的取证对象）**：`FileSource::openInternal()` 在 Windows 先 `_wfopen(wide, L"rb")`（`sdk/extractor/FileSource.cpp:84`），成功后立刻 `_fseeki64(SEEK_END)` + `_ftelli64` + `_fseeki64(SEEK_SET)` 取长度（`:92-94`）；POSIX 侧是 `std::ifstream::open` + `seekg(end)` + `tellg` + `seekg(beg)`（`:109-111`）。因此「一次 open」在实现上等于「打开 + 一次 seek-to-end」，两者都是可能阻塞的系统调用。

---

## 0. 事实表

| # | 平台 | 场景 | 阻塞上限（文档事实） | 可否取消 | 依据 |
|---|---|---|---|---|---|
| 1 | Windows | UNC，主机名无法解析 | 名称解析约 **10 s** 封顶（无任何 DNS 响应时；有 NXDOMAIN 则立即失败） | 可发取消请求（`CreateFile` 属取消范围；DNS 阶段是否响应未验证），通常已自行返回 | [S1][S2] |
| 2 | Windows | UNC，名称可解析、主机不可达（SYN 无响应） | 约 **21 s**（InitialRto 3 s、SYN 重传 2 次时的算术和；**默认重传次数官方页未给值**，见 §1.2） | 同上 | [S3][S4] |
| 3 | Windows | UNC，TCP 可连、SMB 无响应 | **60 s**（`SessTimeout` 默认）；若服务器回 `STATUS_PENDING` 走异步，则 **4×=240 s** | 可发取消请求，但**不保证在 60 s 内生效** | [S5][S6] |
| 4 | Windows | UNC，会话已建立但共享不存在 | 约 1 个 SMB 往返（毫秒–RTT 量级）；**无官方数值** | 无需取消 | [S7]（推断） |
| 5 | Windows | 已映射但离线的网络驱动器 | **无独立官方数值**；走 MUP→SMB 重定向器，与 UNC 同一条超时链（未验证） | 同 #1–#3 | [S8] |
| 6 | POSIX | NFS **hard**（默认）挂载 | **无上限**，请求无限重试；`timeo`/`retrans` 只决定多久报一次 "server not responding" | **不可**（普通信号、`EINTR`、`pthread_cancel` 均无效；只有 `SIGKILL` 能中断，代价是杀进程） | [S10][S11][S12] |
| 7 | POSIX | NFS **soft** 挂载 | `retrans` 次重传后返回 `EIO`，**约 3 分钟量级**（`timeo` 60 s × 线性退避） | 不需要取消，到点自己返回 | [S10]（量级为推算） |
| 8 | POSIX | CIFS/SMB **soft**（`mount.cifs` 默认） | 无响应服务器约 **3×`echo_interval` = 180 s** 判定断连并报错 | 不需要取消 | [S13] |
| 9 | POSIX | CIFS/SMB **hard** | 服务器失联时**挂起**（无上限） | 不可（`intr`/`nointr` 均 "currently unimplemented"） | [S13] |
| 10 | 介质 | 机械盘从停转/待机上电到可读写 | **2.8–8.0 s**（WD Blue 2.5" 官方规格，按型号） | 未验证（本地盘通常远快于网络路径） | [S14] |
| 11 | 介质 | Defender 云端判定阻塞文件运行 | 默认 **10 s**，可配到 **60 s** 上限 | 不可（在扫描/云查询路径内） | [S15] |

> 表里「阻塞上限」是**单阶段**的系统默认值。真实一次 `open` 可能跨阶段叠加（例如 DNS 10 s + TCP 21 s，或 TCP 21 s + SMB 60 s），文档没有给出「一次 `CreateFile` 总上界」这个数，本文也不编一个。

---

## 1. Windows

### 1.1 打开路径：CRT → Win32，且 Win32 层没有超时参数

**事实**
- CRT 文档明确：`fopen` 接受 UNC 路径与映射网络驱动器，**前提是执行时系统能访问该共享/映射盘**；`_wfopen` 与 `fopen` 行为完全一致，只是参数是宽字符（[S8]）。
- 三个可用来「打开文件」的 Win32 入口——`CreateFileW`（[S16]）、`CreateFile2`（[S17]）、`GetFileAttributesExW`（[S18]）——**签名里都没有超时参数**，Remarks 里也没有任何「调用自身可阻塞多久」的说明。`CreateFileW` 只有一句同步/异步语义：「同步句柄上使用该句柄的 I/O 调用会一直阻塞到完成」（[S16]）。
- `FILE_FLAG_OVERLAPPED` 只影响**拿到句柄之后**的读写，`CreateFileW` 文档把它定义为「句柄的同步/异步行为」，不改变打开动作本身是否阻塞（[S16]）。

**未验证**
- `_wfopen` 内部具体走 `CreateFileW`：票面把它作为前提；本次**没有取得**可引用的 UCRT / MinGW-w64 源码（本机未安装 Windows SDK Source，MinGW 默认链接的 `msvcrt.dll` 是闭源的），故这一层实现细节记为未验证。可确认的是：CRT 文档说明 `fopen` 最终作用于操作系统文件系统对象，阻塞发生在该层（[S8]）。

### 1.2 不可达 UNC 的四个阶段

**（a）名称解析（DNS）**
- 默认 5 次尝试的时间上限依次为 **1 / 2 / 2 / 4 / 8 秒**，全部无响应时约 **10 s** 后停止查询（`DNSQueryTimeouts` 的默认值 `1 2 2 4 8 0`，[S1]；同一行为的英文版实测表见 [S2]：0/1/2/4/8/10 s）。
- 收到否定应答（Name Error）会**立即终止**解析，不重试（[S2]）。
- 该值是 `HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\DNSQueryTimeouts`（REG_MULTI_SZ），可改（[S1]）。
- 未验证：DNS 失败后 LLMNR / NetBIOS-NS 回落的额外耗时。已查范围：Microsoft Learn DNS 查询文档只描述 DNS 客户端五轮重试，未给 LLMNR/NetBIOS 的默认超时（[S2]）。

**（b）TCP 连接（SYN）**
- 初始 RTO 默认 **3000 ms**（`InitialRto`，`MSFT_NetTCPSetting`，[S3]）。
- 连接失败前的 SYN 重传次数是 `MaxSynRetransmissions`，**官方只给范围 2–8，没给默认值**（[S3]）。因此「约 21 s」是「若默认取 2，则 3+6+12=21 s」的算术结果，属**推算**；默认值本身记为未验证（论坛与 `netsh int tcp show global` 的实测输出记为 2，但那是二手来源，不作依据）。
- 可调：`netsh int tcp set global` / `Set-NetTCPSetting -MaxSynRetransmissions`（[S3]）。

**（c）SMB 协商与会话建立（请求级超时）**
- `SessTimeout`（`HKLM\SYSTEM\CurrentControlSet\Services\LanmanWorkstation\Parameters`）就是 «Request Expiration Timer»：客户端等待服务器响应的时长，**默认 60 s**（Vista 起的 SMB 2.x/3.0 文档，[S5]）；更早的 Windows 2000 资源文档记默认 **45 s**（[S6]），两处指向同一注册表值。
- 超时后客户端**直接重置连接**，因为「该操作被认为已阻塞」（[S5]）。
- 若请求被服务器以 `STATUS_PENDING` 异步受理，Windows 7+ 会把超时延长到 **4 × `SessTimeout`**（默认 240 s）；只有配置了 `ExtendedSessTimeout` 才用该值（[S5]）。
- 连续可用（CA）集群协商时，Negotiate 的超时被压到 `max(10 s, SessTimeout/6)`（[S5]）——这是**更短**的特例。
- 可调：`SessTimeout` / `ExtendedSessTimeout` 注册表值（[S5][S6]）。

**（d）共享不存在（TREE_CONNECT 失败）**
- 会话建立后，访问不存在的共享由一次 TREE_CONNECT 返回失败；失败码为 `ERROR_BAD_NETPATH`（53，"The network path was not found"，[S7]）。
- 该阶段的耗时应为一个 SMB 往返（RTT 量级），**文档没有给数值**，此处为推断（[S7]）。

**总上界**：文档没有定义「一次 `CreateFileW` 对不可达 UNC 的总超时」。按阶段取最大值：名称不可解析 ≈10 s；主机不可达 ≈21 s；主机可达但 SMB 静默 ≈60 s（异步 240 s）。阶段可叠加，但叠加后的单一数字无一手来源。

### 1.3 已映射但离线的网络驱动器（`net use` 断连、休眠唤醒后）

**事实**
- CRT 层：`fopen` 接受映射盘路径，**只要求执行时该映射盘可访问**（[S8]）；不可访问时表现为打开失败，但没有给失败所需时间。
- 重定向器层：映射盘与 UNC 共用 `LanmanWorkstation` 的同一套 SMB 请求超时（`SessTimeout`），文档未为「映射盘」单列参数（[S5][S6]）。
- 本次**没有**找到「离线映射盘访问超时」的一手数值。已查范围：`SessTimeout`/`ExtendedSessTimeout` 条目、`fopen` 的 UNC/映射盘说明；均无映射盘专属超时。

**结论（边界）**：能确认「离线映射盘不是一条独立的快路径，它仍会走 SMB 超时链」；**不能**确认「离线映射盘有独立的 N 秒上限」。该格记为**未验证**。至于「盘符已删除/映射不存在时是否立刻失败」，本次同样没有取得官方原文，因此只列为待核项，不作结论。

### 1.4 `CancelSynchronousIo` 能否取消「正在进行且尚未返回句柄」的 `CreateFileW`

**事实（能）**
- `CancelSynchronousIo` 的定义是「把指定线程发出的挂起同步 I/O 操作标记为取消」，调用方需要线程的 `THREAD_TERMINATE` 权限；**它不等待取消完成**（[S19]）。
- Microsoft 的官方示例正是这个场景：工作线程里 `CreateFileA(...)`，UI 线程拿线程句柄调 `CancelSynchronousIo(syncThread)`；示例注释写明「CreateFile failed or was canceled. If it was canceled, GetLastError() returns ERROR_OPERATION_ABORTED」（[S20]）。
- 同一篇文档开头的动机句：「if a call to the OpenFile function is blocked because the call is to a very slow device, canceling it enables the call to be made again」（[S20]）。
- 更早的 Vista 取消支持文档说得更直白：「This service is most useful when a create request has been issued that needs to be canceled, since the user will not yet have a handle to the file.」（[S21]）

**事实（但不能当作「有界」）**
- 「Most types of operations can be canceled immediately; other operations can continue toward completion before they are actually canceled」（[S19]）。
- 操作最终以三种状态之一完成：正常完成 / 已取消（`ERROR_OPERATION_ABORTED`）/ 其他错误；**即使发了取消，也可能正常完成**，因为取消请求可能没赶上（[S19]）。
- 「There is no guarantee that underlying drivers correctly support cancellation.」（[S20]）
- 若线程当前没有可取消的请求，`CancelSynchronousIo` 返回 0、`GetLastError()` 为 `ERROR_NOT_FOUND`（[S19]）。

**结论**：机制上，**未返回句柄的 `CreateFileW` 属于可取消对象**，官方示例就是拿它举例；但取消是「请求 + 异步生效」，成功与否取决于重定向器/驱动，且不承诺时延。所以「能取消」不等于「能给出阻塞上限」。

### 1.5 「放弃等待、让线程自行返回」的代价

**事实**
- `CreateFileW` 在返回前**没有句柄**：返回值就是句柄，失败才是 `INVALID_HANDLE_VALUE`（[S16]）；Vista 文档也以「用户此时还没有文件句柄」解释为何需要取消创建（[S21]）。因此「放弃等待」期间**不占文件句柄**。
- 线程本身仍然存在：它阻塞在内核等待里，直到系统调用返回；`CancelSynchronousIo` 也不等待完成（[S19]）。
- 线程的栈在链接时确定：x86/x64 默认 **reserve 1 MB、commit 4 KB**（`/STACK`，[S22]）。即每个被放弃的线程至少保留 1 MB 虚拟地址空间。
- 未验证：被放弃线程在内核等待期间的具体内存/句柄占用（线程对象、内核栈）的精确数值。已查范围：`CreateThread`/`/STACK` 文档只给用户态栈参数，不给内核对象大小；本次未引 Windows Internals 的可引段落。

**结论（边界）**：放弃等待的代价是「一个不会回收的线程 + 1 MB 栈保留 + 直到系统调用返回才释放」，**不占文件句柄**，且几乎不耗 CPU（睡眠等待）。线程滞留时长 = 该 `CreateFile` 实际阻塞时长，即 §1.2 的某一个阶段或叠加。

### 1.6 带超时的替代 API：逐个核对（全部为否定）

| API | 是否有超时参数 | 结论 |
|---|---|---|
| `CreateFile2` | 无（参数只有路径、访问、共享、处置、`CREATEFILE2_EXTENDED_PARAMETERS`） | 不能给 open 加超时（[S17]） |
| `GetFileAttributesExW` | 无 | 签名无超时；它同样是重定向器请求，不能当「预热 + 超时」（[S18]，推断） |
| `CopyFileEx` | 无超时；取消靠进度回调返回 `PROGRESS_CANCEL` 或 `pbCancel` | 回调「每复制完一部分调用一次」，**打开源文件阶段还没有回调可返回**，因此不能给 open 加超时（[S23]） |
| `FILE_FLAG_OVERLAPPED` | 不是超时 | 只改变句柄后续 I/O 的同步性（[S16]） |

**结论**：Win32 文件 API 里**不存在**「带超时的 open」。唯一的机制是 §1.4 的线程级取消，或把调用放到独立线程后放弃等待（§1.5）。

### 1.7 `_fseeki64(SEEK_END)` / `_ftelli64` 在 UNC 上的额外阻塞

**事实**
- 场景里的 open 序列是 `_wfopen` 成功后立刻 `_fseeki64(SEEK_END)`（`sdk/extractor/FileSource.cpp:92`）。
- `_fseeki64` 的语义是移动文件指针（CRT 文档，[S9]）；`SEEK_END` 指向文件末尾，需要文件长度，因此这次调用本身是一次文件系统操作，不是纯内存计算。
- 对 SMB 句柄，这类操作受同一 `SessTimeout` 约束（[S5]）——文档没有为「seek」单列超时，属于同一请求超时机制。

**推断（明确标注）**：`_fseeki64(SEEK_END)` 会在 `open` 成功之后**再引入一次可阻塞的操作**；如果介质在 open 与 seek 之间掉线，阻塞会重新计时。`_ftelli64` 通常只读 CRT 缓存的当前位置，不再发 I/O——这一点未取得实现源码，记为未验证。

---

## 2. POSIX

### 2.1 `open(2)` 与 `seekg(end)` 的可阻塞性

**事实**
- `open(2)` 在「可能阻塞」的场合是可被信号中断的（`EINTR`）：`open(2)` 的 ERRORS 明写「While blocked waiting to complete an open of a slow device (e.g., a FIFO) ... interrupted by a signal handler」（[S24]）。
- `signal(7)` 把 `open(2)` 列入「可被信号处理函数中断」的接口，但限定语是「if it can block (e.g., when opening a FIFO)」；同页明确「a (local) disk is not a slow device ... I/O operations on disk devices are not interrupted by signals」（[S12]）。
- 网络文件系统不在「local disk」这句话的覆盖范围内，但 `open(2)`/`signal(7)` 都没有给 NFS 单列语义；NFS 的行为要看 §2.2–§2.4。

### 2.2 NFS：hard / soft / intr / timeo / retrans

**事实（全部来自 `nfs(5)`，[S10]）**
- 不指定或指定 `hard`：**请求无限重试**（"retried indefinitely"）。
- `soft`：`retrans` 次重传后返回 **`EIO`**；`softerr` 返回 **`ETIMEDOUT`**。文档同时警告 soft 可能导致静默数据损坏。
- `intr` / `nointr`：**仅为向后兼容保留，内核 2.6.25 之后被忽略**（"It is ignored after kernel 2.6.25."）。
- `timeo`：TCP 默认 **600 分秒 = 60 s**；每次重传线性退避、单次上限 600 s。
- `retrans`：TCP 默认 **2 次**，UDP 3 次。
- 重传耗尽后客户端认为发生网络分区，**在新 socket 上重连**——注意这不是「放弃」，hard 挂载下仍会继续重试。
- 挂载阶段（`mount(8)`）的 `retry` 是另一层：前台默认 2 分钟、后台默认 10000 分钟；文档还提到「TCP the default is 3 minutes」。

**结论**：NFS hard 挂载下，`open(2)` 作为一次 RPC 可以**无限期阻塞**；`timeo`/`retrans` 只影响「多久打印一次 server not responding」，不构成返回上界。soft 挂载是唯一有返回上界的配置，量级为「分钟级」（按 60 s × 退避推算，非文档直接给出的单一数字）。

### 2.3 `EINTR` 能否让 `open` 提前返回，`pthread_cancel` 是否有效

**事实（内核侧）**
- Linux 的 RPC 等待用 `rpc_wait_bit_killable`，其逻辑是 `schedule()` 后检查 `signal_pending_state(mode, current)`，挂起则返回 `-ERESTARTSYS`；调用点传入的 mode 是 **`TASK_KILLABLE`**（`net/sunrpc/sched.c`，[S11]）：
  ```c
  static int rpc_wait_bit_killable(struct wait_bit_key *key, int mode)
  {
      schedule();
      if (signal_pending_state(mode, current))
          return -ERESTARTSYS;
      return 0;
  }
  ```
  `TASK_KILLABLE` 的语义是「只有致命信号才唤醒」，因此**普通信号（包括 `SIGINT`、`SIGTERM`）不会打断这个等待**，只有 `SIGKILL` 会——而 `SIGKILL` 杀掉的是整个进程。
- 这与 `nfs(5)` 的「`intr` 自 2.6.25 起被忽略」互相印证（[S10]）。

**事实（POSIX 取消语义）**
- `pthread_cancel()` 只是「请求取消」，**何时生效由目标线程的 cancelability state/type 决定**，取消处理「相对于 `pthread_cancel()` 返回是异步的」（[S25]）。
- POSIX 上，取消只在**取消点**生效；阻塞中的系统调用若不是取消点、或线程根本没被唤醒，取消请求就一直挂着。

**推断（明确标注）**：`pthread_cancel` 在 glibc 上通过内部信号（`SIGCANCEL`，实时信号）通知目标线程；它不是致命信号，唤不醒 `TASK_KILLABLE` 的 NFS 等待。因此**对阻塞在 NFS hard 挂载 `open` 上的线程，`pthread_cancel` 不会让它提前返回**，线程会一直挂到 RPC 返回为止（hard 挂载下没有「自行放弃」这一步）。该推导未在真实 NFS 上实测（本机无网络盘）。

### 2.4 `O_NONBLOCK` 对普通文件与网络文件系统是否生效

**事实**
- `open(2)`：「this flag has **no effect for regular files and block devices**; that is, I/O operations will (briefly) block when device activity is required, regardless of whether `O_NONBLOCK` is set」，并提醒应用不要依赖普通文件上的阻塞行为（[S24]）。
- 同一页的 NOTES 里，NFS 只被提到「`O_SYNC` 和 `O_NDELAY` 受 NFS 协议缺陷影响」，没有说 `O_NONBLOCK` 能让 NFS `open` 变成非阻塞（[S24]）。

**结论**：NFS 上的文件是普通文件，`O_NONBLOCK` **不能**把 `open` 变成「立即返回」。没有任何一手文档承诺 NFS 的 `open` 支持非阻塞语义。

### 2.5 CIFS/SMB（`mount.cifs`）

**事实（`mount.cifs(8)`，[S13]）**
- `hard`：「程序访问该文件系统时会在服务器崩溃时挂起」（hang）。
- `soft`：**默认值**；服务器崩溃时向应用返回错误，不挂起。
- `echo_interval=n`：空闲连接上发送 echo 的间隔，默认 **60 s**；「对无响应服务器的超时判定时间是该值的 3 倍」→ 约 **180 s**。
- `intr` / `nointr`：两者都标注 **"currently unimplemented"**。
- 文档没有给出「单次 `open` 最多阻塞多久」的硬上限；180 s 是「连接判定」的时机，不是单操作上界（[S13]）。

**结论**：CIFS 默认是 soft，因此「服务器失联」通常表现为**分钟级后返回错误**；`hard` 则是挂起且无 `intr` 可用。

---

## 3. 介质侧量级

### 3.1 机械盘 spin-up / 从待机唤醒

**事实**
- WD Blue 2.5" 官方规格表给出「Average drive ready time」：按型号 **2.8 s / 3.0 s / 6.5 s / 8.0 s**（5400/5200 rpm 各容量），[S14]。
- 这是「上电到就绪」的厂商规格，可当作「停转后重新可用」的量级参考。

**未验证**
- 3.5" 桌面/企业盘的具体 spin-up 上界（不同型号差异大，且部分支持 staggered spin-up）；本次只核对了 WD Blue 2.5" 一份规格表，未穷举。
- 运行中磁头加载（head load）的耗时。已查范围：WD 规格表只给 drive ready time，不给 head load/unload 时间。

### 3.2 Windows Defender / 杀软实时扫描

**事实**
- Microsoft 文档：Defender 发现可疑文件时，**默认阻止该文件运行 10 秒**以等待云判定；通过 `CloudExtendedTimeout` 最多再加 **50 秒**，即**上限 60 秒**（[S15]）。
- 这只覆盖「可疑文件 + 云查询」这一档；普通文件的实时扫描耗时官方**没有公布**（已查范围：Defender 云阻止超时文档、Intune 防病毒策略文档）。

**结论**：能以一手文档支撑的「杀软导致的 open 延迟上界」只有 **60 s**（且需要云保护/BAFS 开启、文件可疑）。「几十到几百毫秒的常规扫描」没有官方数字，属经验值，本文不写成事实。

### 3.3 冷缓存

**事实**
- 本次没有取得任何官方文档给出「冷缓存 open 的耗时上界」。
**结论**：记为**未验证**。可说的只有机制层：本地 SSD 没有寻道动作，机械盘需要一次寻道才能取到元数据/数据（毫秒量级），但这些都不构成「open 上界」，本文不给数字。

---

## 4. 「不可取消、只能放弃等待」的边界

把上面的事实收成一条边界：

1. **能「请求取消」的只有 Windows。** `CancelSynchronousIo` 明确覆盖「已发出、尚未拿到句柄的 create」（官方示例与 Vista 文档，[S20][S21]）；但它是**标记取消、异步生效**，可能仍以正常完成结束，且依赖底层驱动（[S19][S20]）。因此「可取消」不等于「有上界」。
2. **POSIX 侧没有对应的「取消一次 open」机制。** NFS hard 等待是 `TASK_KILLABLE`，只有 `SIGKILL` 能唤醒（[S11]），而 `SIGKILL` 的粒度是进程；`pthread_cancel` 与 `EINTR` 都不改变这个等待（§2.3）。CIFS 的 `intr` 明确 "currently unimplemented"（[S13]）。
3. **有界的配置只有「挂载语义」这一层**：NFS `soft`（EIO）、CIFS `soft`（默认，分钟级错误）、Windows `SessTimeout`（60 s / 异步 240 s）。这些都是**全局/挂载级**参数，不是「这一次 open 的超时」，而且 soft 会改变数据语义（`nfs(5)` 明确警告静默数据损坏）。
4. **「放弃等待」的形态**：把 open 放在独立线程、不再 join。代价是
   - Windows：线程滞留到 `CreateFile` 返回；**期间没有文件句柄**（[S16][S21]），保留默认 1 MB 栈（[S22]），几乎不耗 CPU；取消请求可发但可能无效（[S19]）。
   - POSIX：线程滞留到 RPC 返回；除杀进程外没有外部手段回收，`pthread_cancel` 也挂起（§2.3）。
5. **不成立的推论**（本文明确排除）：
   - 「`CreateFile2` / `GetFileAttributesEx` / `CopyFileEx` 能带超时地预检」——三者都没有超时参数（§1.6）。
   - 「`O_NONBLOCK` 能让 NFS open 立刻返回」——对普通文件无效（§2.4）。
   - 「NFS `soft` 是安全的取消手段」——`nfs(5)` 警告可能静默损坏数据（[S10]）。

---

## 5. 未验证清单（含已查范围）

1. **`_wfopen` 内部是否直接调 `CreateFileW`**。已查：Microsoft Learn `fopen/_wfopen` 文档（只说 `fopen` 接受 UNC/映射盘，不提底层 API）；本机无 Windows SDK Source，MinGW 默认 `msvcrt.dll` 闭源。票面按 `CreateFileW` 讨论，本文沿用该前提但标注未核实。
2. **Windows `MaxSynRetransmissions` 的默认值**。已查：`MSFT_NetTCPSetting` 文档只给范围 2–8，未给默认值；`netsh int tcp show global` 的实测值出现在论坛/博客，属二手，不作依据。因此「TCP 阶段约 21 s」是**条件推算**（若默认 2）。
3. **DNS 失败后 LLMNR / NetBIOS-NS 回落的耗时**。已查：Microsoft Learn「DNS 查询和查找」只描述 DNS 客户端五轮重试（1/2/2/4/8 s）；未给 LLMNR/NetBIOS 默认超时。
4. **离线映射盘的独立超时**。已查：`SessTimeout`/`ExtendedSessTimeout` 条目、`fopen` 的 UNC/映射盘说明，均无「映射盘」专属数值；只能确认它复用 SMB 请求超时链。
5. **`CreateFileW` 对不可达 UNC 的「总上界」**。没有任何一手文档给出单一数值；本文只给分阶段默认值与叠加规则。
6. **`_ftelli64` 是否零 I/O**。未取得 CRT 实现源码；本文只说 `_fseeki64(SEEK_END)` 是可阻塞操作。
7. **`CancelSynchronousIo` 对「SMB create 卡在 TCP connect 阶段」是否生效**。文档只保证机制与「驱动应支持」，没有逐阶段说明；本文只写「可发请求、不保证时延」。
8. **放弃等待时线程的内核对象/内核栈占用**。未取得可引用的 Windows Internals 段落；只引到用户态栈默认值（`/STACK` 1 MB reserve）。
9. **3.5" 机械盘与磁头加载的具体秒数**。只核对 WD Blue 2.5" 一份规格表。
10. **Defender 对普通（非可疑）文件的扫描耗时**。官方未公布；本文只保留 10 s / 60 s 这一档。

---

## 6. 来源

- [S1] Microsoft Learn，《DNSQueryTimeouts》注册表条目（默认 `1 2 2 4 8 0`，五次尝试各 1/2/2/4/8 秒）。https://msdn.microsoft.com/zh-cn/cc977482
- [S2] Microsoft Learn / Windows Server，《DNS 查询和查找》（单适配器逐个查询，1 s→2 s→2 s→4 s→8 s，8 秒无响应即超时；否定响应立即终止；随后 30 s 内该适配器全部查询超时）。https://learn.microsoft.com/zh-cn/windows-server/networking/dns/queries-lookups
- [S3] Microsoft Learn，《MSFT_NetTCPSetting 类》（`InitialRto` 默认 3000 ms；`MaxSynRetransmissions` 范围 2–8）。https://learn.microsoft.com/en-us/windows/win32/fwp/wmi/nettcpipprov/msft-nettcpsetting
- [S4] Microsoft Support，《How to modify the TCP/IP maximum retransmission time-out》（`TCPInitialRtt` 默认 3000 ms）。https://learn.microsoft.com/en-us/troubleshoot/windows-server/networking/modify-tcp-ip-maximum-retransmission-time-out
- [S5] Microsoft Learn archive（Open Specifications），《SMB 2.x and SMB 3.0 Timeouts in Windows》（Request Expiration Timer = `SessTimeout`，默认 60 s；超时即重置连接；异步操作延长到 4×；CA Negotiate 用 max(10 s, SessTimeout/6)；`ExtendedSessTimeout`）。https://learn.microsoft.com/en-us/archive/blogs/openspecification/smb-2-x-and-smb-3-0-timeouts-in-windows
- [S6] Microsoft Learn，《SessTimeout》（Windows 2000 资源文档：REG_DWORD，范围 10–65535 s，默认 45 s；超时后「认为操作失败」）。https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-2000-server/cc938292(v=technet.10)
- [S7] Microsoft Learn，《System Error Codes (0-499)》（`ERROR_BAD_NETPATH` = 53，"The network path was not found"；`ERROR_SEM_TIMEOUT` = 121）。https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes--0-499-
- [S8] Microsoft Learn，《fopen, _wfopen》（`fopen` 接受 UNC 与映射盘，前提是执行时可访问；`_wfopen` 行为与 `fopen` 相同）。https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fopen-wfopen?view=msvc-170
- [S9] Microsoft Learn，《fseek, _fseeki64》（「Moves the file pointer to a specified location」；`SEEK_END` = End of file）。https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fseek-fseeki64?view=msvc-170
- [S10] man7.org，《nfs(5)》（hard 无限重试；soft→EIO / softerr→ETIMEDOUT；`intr` 自 2.6.25 起被忽略；`timeo` TCP 默认 600 分秒；`retrans` TCP 默认 2；重传耗尽后新 socket 重连；mount `retry` 默认前台 2 分钟）。https://man7.org/linux/man-pages/man5/nfs.5.html
- [S11] Linux 内核源码，`net/sunrpc/sched.c`：`rpc_wait_bit_killable()` 与两处 `TASK_KILLABLE` 调用点。https://raw.githubusercontent.com/torvalds/linux/master/net/sunrpc/sched.c
- [S12] man7.org，《signal(7)》（`open(2)` 仅在「可能阻塞，例如 FIFO」时可被中断；「a (local) disk is not a slow device ... I/O operations on disk devices are not interrupted by signals」）。https://man7.org/linux/man-pages/man7/signal.7.html
- [S13] man7.org，《mount.cifs(8)》（`hard` 挂起；`soft` 为默认并返回错误；`echo_interval` 默认 60 s，无响应服务器按 3× 判定；`intr`/`nointr` "currently unimplemented"）。https://man7.org/linux/man-pages/man8/mount.cifs.8.html
- [S14] WD Blue 2.5-inch 官方规格表（Average drive ready time 2.8 / 3.0 / 6.5 / 8.0 s，按型号）。https://ftp.hydata.com/Downloads/scsitools/scsitools.DRT/Resources/LOD_FactoryMadeLOD-Files/WD/2.5-inch/NULL/WD-NULL-SPE.pdf
- [S15] Microsoft Learn，《Configure Microsoft Defender Antivirus cloud block time-out》（默认阻止 10 s，可加 0–50 s，上限 60 s）。https://learn.microsoft.com/en-us/defender-endpoint/configure-cloud-block-timeout-period-microsoft-defender-antivirus
- [S16] Microsoft Learn，《CreateFileW》（无超时参数；同步句柄的 I/O 调用会阻塞到完成；`FILE_FLAG_OVERLAPPED` 只定义句柄的同步/异步行为）。https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew
- [S17] Microsoft Learn，《CreateFile2》（参数为路径/访问/共享/处置/扩展参数，无超时）。https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfile2
- [S18] Microsoft Learn，《GetFileAttributesExW》（无超时）。https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfileattributesexw
- [S19] Microsoft Learn，《CancelSynchronousIo》（标记指定线程的挂起同步 I/O；不等待完成；三种完成状态；`ERROR_NOT_FOUND`；SMB 3.0 支持）。https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelsynchronousio
- [S20] Microsoft Learn，《Canceling Pending I/O Operations》（官方同步取消示例：工作线程 `CreateFileA`，另一线程 `CancelSynchronousIo`，取消后 `GetLastError()==ERROR_OPERATION_ABORTED`；「no guarantee that underlying drivers correctly support cancellation」；开头以 `OpenFile` 阻塞在慢设备为例）。https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations
- [S21] Microsoft Learn（MSDN archive），《Win32 I/O Cancellation Support in Windows Vista》（「most useful when a create request has been issued that needs to be canceled, since the user will not yet have a handle to the file」）。https://msdn.microsoft.com/en-us/LIBRary/aa480216
- [S22] Microsoft Learn，《/STACK (Stack allocations)》（x86/x64 默认 reserve 1 MB、commit 4 KB）。https://learn.microsoft.com/en-us/cpp/build/reference/stack-stack-allocations?view=msvc-170
- [S23] Microsoft Learn，《CopyFileExW》（进度回调「called each time another portion of the file has been copied」；返回 `PROGRESS_CANCEL` 才取消；无超时参数）。https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-copyfileexw
- [S24] man7.org，《open(2)》（`O_NONBLOCK` 对普通文件和块设备无效；`EINTR` 出现在「blocked waiting to complete an open of a slow device」；NFS 只被提到 `O_SYNC`/`O_NDELAY`）。https://man7.org/linux/man-pages/man2/open.2.html
- [S25] The Open Group，《pthread_cancel》（取消何时生效由目标线程的 cancelability state/type 决定；取消处理相对调用方异步）。https://pubs.opengroup.org/onlinepubs/9699919799/functions/pthread_cancel.html

> 全部链接访问时点：2026-11-23。`learn.microsoft.com` 与 `man7.org` 的页面内容可能随版本更新；本文引用的是该时点的正文。
