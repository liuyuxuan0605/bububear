# 音视频同步原理与实现

## 一、音视频同步的核心问题

### 1.1 为什么会出现音视频不同步

音频和视频是两条独立的数据流，从编码、解码到渲染都走各自的通道。以下几类问题会让它们在时间轴上发生偏移：

**（1）解码速度不一致**  
视频解码（尤其是 H.264/H.265）计算量远高于音频解码（AAC/MP3）。在低性能设备上，视频解码耗时可能远超一帧的播放时间，导致视频越来越落后于音频。

**（2）PTS（Presentation Timestamp）缺失或不连续**  
容器格式中某些帧可能没有 PTS，或者由于 B 帧重排导致 PTS 不单调递增。播放端必须能处理这类异常，否则会出现跳帧或卡顿。

**（3）缓冲区延迟不等**  
音频播放有硬件缓冲区（如 SDL `AUDIO_BUFFER_SIZE = 1024` 个采样），从解码到实际出声有固定延迟。视频送显的延迟则受 OpenGL 渲染流水线、显示器 vsync 等影响，两者延迟特性不同。

**（4）Seek（跳转）后的时钟漂移**  
Seek 后解码器会跳到关键帧（I 帧），但关键帧的时间戳不一定精确等于目标时间。如果音频和视频在 Seek 后恢复的起点不一致，就会出现短暂的不同步。

**（5）帧率和采样率的数值误差**  
时间戳使用 `AVRational` 有理数表示，转换为浮点时存在精度损失，长时间播放会累积误差。

**（6）重复帧（repeat_pict）**  
某些编码格式（如 MPEG-2）允许一帧显示多次，此时视频时钟需要额外补偿，否则会走得比实际慢。

---

### 1.2 三种同步策略对比

| 策略 | 主时钟 | 优点 | 缺点 |
|------|--------|------|------|
| 以音频为主时钟 | 音频 PTS | 感知最准，人耳对声音延迟极敏感 | 视频需丢帧/等待，画面可能抖动 |
| 以视频为主时钟 | 视频 PTS | 画面流畅，无跳帧 | 音频可能出现卡顿或加速，体验差 |
| 以外部时钟为主 | 系统时钟 | 解耦音视频，适合网络直播 | 实现复杂，累积误差需主动纠正 |

**结论：绝大多数本地播放器（包括本项目）都选择音频为主时钟。**

---

## 二、为什么以音频为主时钟

### 2.1 人的感知特性

人类对音频不连续极为敏感，哪怕 50ms 的卡顿或加速都能被清晰感知；而对视频的短暂丢帧（相邻两帧合并显示）往往不易察觉。因此，**保证音频流畅 > 保证视频流畅**。

### 2.2 音频输出由硬件驱动，天然具有时钟性质

音频播放依赖声卡硬件，SDL 会以固定频率（`sample_rate / buffer_size` 次/秒）调用 `audio_callback` 来索取数据。这个回调频率由硬件晶振驱动，**天然等价于一个高精度时钟**，不会因 CPU 负载抖动。

本项目中 SDL 配置为：

```cpp
// videoplayer.cpp:409
wanted_spec.freq    = pAudioCodecCtx->sample_rate; // 如 44100 Hz
wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;        // 1024 个采样
// 回调间隔 ≈ 1024/44100 ≈ 23ms，极为稳定
wanted_spec.callback = audio_callback;
```

### 2.3 音频时钟的更新点是解码时刻，精度高

`audio_clock` 在每次解码一帧音频时更新，单位为微秒：

```cpp
// videoplayer.cpp:728-730
if (pkt.pts != AV_NOPTS_VALUE) {
    is->audio_clock = pkt.pts * av_q2d(is->audio_st->time_base) * 1000000;
}
```

由于音频帧持续时间短（单帧约 23ms），时钟分辨率高，视频拿到的参考值足够精确。

---

## 三、本项目的音视频同步实现

### 3.1 先理解两个时钟是什么

整个同步机制只依赖 `VideoState` 里的两个 `double` 变量（`videoplayer.h:36,50`）：

```cpp
double audio_clock;  // 音频主时钟，单位：微秒
double video_clock;  // 视频从时钟，单位：微秒
```

**`audio_clock` 的含义**：每次音频解码完一帧，就把这帧的 PTS 写进来（`videoplayer.cpp:730`）：

```cpp
is->audio_clock = pkt.pts * av_q2d(is->audio_st->time_base) * 1000000;
```

说白了，`audio_clock` 就是"音频解码到哪里了"。SDL 的声卡硬件会持续不断地把音频缓冲区里的数据播出去，每播完一块就再来要新数据，触发一次 `audio_callback`，`audio_callback` 再调 `audio_decode_frame` 解码并更新 `audio_clock`。所以 `audio_clock` 是在稳定地、持续地向前推进的。

**`video_clock` 的含义**：每次视频解码完一帧，把这帧的 PTS 写进来（通过 `synchronize_video` 更新）。它代表"视频解码到哪里了"。

---

### 3.2 情况一：有音频流时，视频怎么跟着音频走

整个项目的播放架构是三条并行线程：**读线程**负责从文件里读包分发到两个队列，**音频**由 SDL 回调驱动（可以理解成一条隐形的音频线程），**视频线程**负责解码和送显。

同步的核心代码全在视频线程开头的这个 `while` 循环里（`videoplayer.cpp:99-107`）：

```cpp
while (1) {
    if (is->quit) break;
    if (is->audioq->size == 0) break;  // 音频队列空了就别等了

    audio_pts = is->audio_clock;
    video_pts = is->video_clock;

    if (video_pts <= audio_pts) break; // 视频没超前，可以解码显示
    SDL_Delay(5);                      // 视频超前了，等 5ms 再看
}
// ↓ 跳出循环之后才去解码这帧视频、送显
ret = avcodec_decode_video2(...);
```

**具体发生了什么，一步一步说：**

视频线程从视频队列里取出一个数据包，但它不立刻解码，而是先拿当前的 `video_clock`（视频时间）和 `audio_clock`（音频时间）比一下。

- 如果 `video_clock > audio_clock`，说明视频跑得比音频快，如果现在就显示这帧，画面会超前于声音，嘴还没动声音就出来了。所以视频线程就 `SDL_Delay(5)` 睡 5 毫秒，然后再醒来再比一次，如此反复，一直等到音频时钟追上来。

- 如果 `video_clock <= audio_clock`，说明音频已经追上来了，或者视频本来就落后于音频，那直接解码显示就行。

这就是同步的全部秘密：**视频主动等音频，音频由硬件驱动自己跑，视频只能跟着**。

有一个细节要注意：`if (is->audioq->size == 0) break`。这行的意思是，如果音频队列里已经没有数据了（比如文件快读完了，或者只有纯视频流），就不要等了，直接显示。否则 `audio_clock` 不再推进，视频线程会一直卡在那个 `while` 里出不来。

---

### 3.3 情况二：没有音频流时，视频靠什么驱动

有音频流的时候，音频硬件回调天然地提供了一个节奏，视频跟着它走就行。但如果文件根本没有音频流，就没有 `audio_clock` 可以参考。

这时项目在初始化阶段做了一个判断（`videoplayer.cpp:376-388`）：如果有音频流，创建视频线程；如果没有音频流，不创建视频线程，改为注册一个 SDL 定时器。

```cpp
if (m_videoState.audioStream != -1) {
    // 有音频：创建视频线程，靠音频时钟同步
    SDL_CreateThread(video_thread, "video_thread", &m_videoState);
} else {
    // 无音频：按帧率算出每帧间隔，用定时器定时触发显示
    double fps      = av_q2d(m_videoState.video_st->r_frame_rate);
    double interval = 1.0 / fps;  // 例如 25fps → 40ms
    SDL_AddTimer((Uint32)(interval * 1000), timer_callback, &m_videoState);
}
```

`timer_callback` 每隔 40ms（以 25fps 为例）被 SDL 自动调用一次，每次调用就解码并显示一帧，然后返回同样的间隔值，下次继续。

这种方式的缺点是它只是"按固定节奏播"，并没有真正对齐视频帧的 PTS，如果某帧解码耗时超过 40ms，就会产生卡顿，并且误差会累积。但对于没有音频的纯视频文件，这是最简单可用的方案。

---

### 3.4 视频时钟的维护：synchronize_video 做了什么

上面说的同步逻辑依赖 `video_clock` 准确地反映当前视频进度，`synchronize_video`（`videoplayer.cpp:21-42`）就负责维护这个值，它在每帧解码后被调用。

它处理两件事：

**第一件事：帧没有 PTS 怎么办。**  
理论上每帧都应该有 PTS，但实际编码中有些帧的 PTS 是无效值（`AV_NOPTS_VALUE`）。遇到这种情况，就用当前 `video_clock` 的值作为这帧的 PTS，相当于"假定这帧紧跟在上一帧后面"。

```cpp
if (pts != 0) {
    is->video_clock = pts;  // 有 PTS，用它更新时钟
} else {
    pts = is->video_clock;  // 没有 PTS，时钟值作为这帧的 PTS
}
```

**第二件事：把时钟向前推进一帧的时长。**  
每处理完一帧，视频时钟要往前推一帧的时间，这样下一帧进来比较时才能拿到正确的"当前位置"。同时还要处理 `repeat_pict`（重复帧）：如果这帧需要显示 N 次，时钟就要多推进相应时间，否则时钟走得比实际慢，视频会越来越落后。

```cpp
double frame_delay = av_q2d(is->video_st->codec->time_base);
frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
is->video_clock += frame_delay;
```

---

### 3.5 情况三：Seek 跳转后同步是怎么恢复的

Seek 是同步最容易出问题的地方，需要理解清楚整个流程。

**为什么 Seek 后同步会乱？**

假设你正在播放第 5 分钟的内容，然后拖动进度条跳到第 1 分钟。此时 `audio_clock` 大约是 300 秒（5分钟），`video_clock` 也差不多。跳转之后解码器重新从第 1 分钟的关键帧开始解码，新解出来的帧 PTS 只有约 60 秒。但如果两个时钟还停留在 300 秒，视频线程的同步判断 `video_clock > audio_clock` 永远是 true，视频线程就会永远等下去，画面冻住。

还有另一个问题：Seek 之前队列里可能还存着第 5 分钟的旧数据包，解码器内部也缓存着旧帧。如果不清除这些，第 5 分钟的旧帧和第 1 分钟的新帧会混在一起解码，导致花屏。

**项目的解决方案，分四步：**

**第一步：读线程调用 `av_seek_frame` 跳转**（`videoplayer.cpp:499`）

告诉 FFmpeg 把文件读取位置移到最近的关键帧（`AVSEEK_FLAG_BACKWARD` 表示跳到目标时间之前最近的关键帧）。注意关键帧不一定精确在目标时间，比如你要跳到第 63 秒，最近的关键帧可能在第 60 秒。

**第二步：清空队列，发送 FLUSH 包**（`videoplayer.cpp:507-514`）

清掉音频队列和视频队列里的旧数据，然后往队列里各塞一个特殊的"FLUSH 包"（内容是字符串 `"FLUSH"`）。这个 FLUSH 包的作用是作为信号——让音频线程和视频线程知道"跳转发生了，你们要清理解码器缓存"。

**第三步：视频线程和音频线程收到 FLUSH 包，清理解码器并重置时钟**（`videoplayer.cpp:92-97`）

```cpp
if (strcmp((char*)packet->data, FLUSH_DATA) == 0) {
    avcodec_flush_buffers(is->video_st->codec); // 清解码器内部缓存，防花屏
    av_free_packet(packet);
    is->video_clock = 0;  // 重置视频时钟！
    continue;
}
```

这里 `video_clock = 0` 非常关键。清零之后，`video_clock < audio_clock` 几乎肯定成立（因为音频也重新从关键帧时间开始更新 `audio_clock`），视频线程不会再卡在等待循环里，可以正常解码推进了。

**第四步：音视频各自丢弃关键帧到目标时间之间的帧**（`videoplayer.cpp:117-128`，`737-745`）

前面说了，Seek 落到的关键帧不一定精确等于你要的时间。比如你要跳到第 63 秒，关键帧在第 60 秒，那第 60-63 秒的帧就是多余的，必须丢掉，否则会出现短暂的"时间倒退"画面。

```cpp
// 视频线程：如果这帧的 PTS 还没到目标时间，丢掉继续取下一帧
if (video_pts < is->seek_time) {
    av_free_packet(packet);
    continue;  // 不解码，不显示，直接扔掉
} else {
    is->seek_flag_video = 0;  // 到了，开始正常播
}

// 音频线程同理：PTS 还没到目标时间，丢掉这帧音频
if (is->audio_clock < is->seek_time) {
    break;  // 跳出，继续取下一个包
} else {
    is->seek_flag_audio = 0;
}
```

经过这四步，音视频都从目标时间重新对齐，同步恢复正常。

---

### 3.6 缓冲区背压控制

读线程在读包时会检查队列大小，防止内存溢出（`videoplayer.cpp:472-481`）：

```cpp
#define MAX_AUDIO_SIZE (1024*16*25*10)   // ~10 MB
#define MAX_VIDEO_SIZE (1024*255*25*2)   // ~12.7 MB

if (m_videoState.audioq->size > MAX_AUDIO_SIZE) {
    SDL_Delay(10); continue;  // 队列满了就停止读包，等消耗
}
if (m_videoState.videoq->size > MAX_VIDEO_SIZE) {
    SDL_Delay(10); continue;
}
```

这是一个简单的背压机制：读太快会撑爆内存，所以队列超过阈值时读线程主动暂停。音视频线程继续消耗队列，等队列缩小后读线程再继续读。

---

## 四、音视频同步相关面试题

### 基础概念

**Q1：什么是 PTS 和 DTS？它们有什么区别？**  
PTS（Presentation Timestamp）是帧的显示时间，DTS（Decoding Timestamp）是帧的解码时间。对于 B 帧，解码顺序和显示顺序不同，DTS < PTS；对于 I/P 帧，DTS == PTS。播放时以 PTS 为准，解码时以 DTS 为准。

**Q2：为什么以音频为主时钟，而不是视频？**  
人耳对声音的时间感知精度约为 10ms，对视频帧率变化的感知阈值约为 30ms 以上。音频不连续会被立刻感知，而丢弃视频帧几乎不被察觉。此外，音频播放由硬件晶振驱动，天然稳定，适合作为参考时钟。

**Q3：av_q2d 的作用是什么？**  
`av_q2d(AVRational r)` 将 FFmpeg 的有理数时间基（如 1/44100、1/90000）转换为 double 浮点秒数，方便做时间比较和计算。

**Q4：time_base 是什么？不同流的 time_base 一样吗？**  
`time_base` 是时间戳的最小单位，`pts * time_base = 实际时间（秒）`。音频流和视频流通常有不同的 time_base（例如音频为 1/44100，视频为 1/90000），比较时必须先统一换算。

---

### 同步机制

**Q5：本项目视频同步的具体方式是什么？**  
视频线程在取下一个包之前，先进入等待循环，不断比较 `video_clock` 和 `audio_clock`。若视频超前于音频，则 `SDL_Delay(5ms)` 等待音频推进；若视频落后，则直接解码显示（相当于丢弃等待，追赶音频）。

**Q6：如果视频落后于音频怎么办？本项目有做主动丢帧吗？**  
本项目没有显式的主动丢帧逻辑。当 `video_pts <= audio_pts` 时直接跳出等待循环解码并显示，若解码速度不足，视频会持续落后，表现为卡顿。更完整的实现（如 FFmpeg 的 ffplay）会在差值超过阈值时主动跳帧。

**Q7：没有音频流的视频文件怎么同步？**  
退化为纯视频模式，使用 SDL 定时器以视频帧率为间隔触发帧解码和显示，不依赖 `audio_clock`。

**Q8：Seek 后为什么必须重置 video_clock 为 0？**  
若不重置，向左 Seek 后 `video_clock` 仍是 Seek 前的大值，而 `audio_clock` 已经重置到新位置（更小的值），导致 `video_pts > audio_pts` 的条件持续成立，视频线程进入死等循环，无法推进。

---

### 进阶问题

**Q9：audio_clock 的值精确吗？实际音频输出时刻和 audio_clock 有差距吗？**  
不完全精确。`audio_clock` 在音频**解码时**更新，但解码完的数据要先进入 SDL 的内部音频缓冲区（`SDL_AUDIO_BUFFER_SIZE = 1024` 个采样，约 23ms），才真正被播放出来。因此实际出声时刻比 `audio_clock` 落后约 23ms。更精确的做法是在 `audio_clock` 基础上减去缓冲中尚未播放的音频对应的时长，即：

```
real_audio_clock = audio_clock - (audio_buf_size - audio_buf_index) / bytes_per_second
```

**Q10：repeat_pict 是什么？不处理它会有什么后果？**  
`repeat_pict` 表示该帧需要重复显示的次数（常见于 MPEG-2 的 pulldown 处理）。若忽略 `repeat_pict`，视频时钟推进速度会比实际慢，导致视频越播越落后于音频。

**Q11：B 帧对同步有什么影响？**  
B 帧依赖其前后的帧，解码顺序（DTS）早于显示顺序（PTS）。解码器输出帧时 PTS 不连续，同步代码需要用 `pFrame->best_effort_timestamp` 或 `pFrame->pts`，而不是 `DTS`，否则时钟计算错误。

**Q12：如果音频队列为空（audio_decode_frame 返回 -1），audio_clock 会如何变化？**  
`audio_decode_frame` 在 `packet_queue_get` 取不到包（队列为空）时返回 -1；注意真正的解码错误（`avcodec_decode_audio4 < 0`）会直接 `exit(0)` 而不是返回 -1。`audio_callback` 收到 -1 后填充 1024 字节静音，但不更新 `audio_clock`（更新发生在 `audio_decode_frame` 内部成功解码后）。时钟停止推进，视频线程因 `video_pts <= audio_pts` 始终成立而不再等待，会以最快速度解码视频——实质上退化为"尽力而为"模式。

**Q13：多线程下 audio_clock 和 video_clock 的读写存在竞争吗？本项目如何处理？**  
存在竞争：`audio_clock` 在音频回调线程（SDL 内部线程）写，在视频线程读；`video_clock` 在视频线程读写。本项目使用的是 `double` 类型，x86 上对齐的 double 读写具有一定的原子性，但严格来说不是线程安全的。更严谨的做法是用 `SDL_LockAudioDevice` 保护 `audio_clock` 的读取，或将时钟改为原子类型。

**Q14：直播场景下为什么通常用外部时钟而非音频时钟？**  
直播网络抖动会导致音频包延迟到达，`audio_clock` 会停滞，以它为主时钟会让视频也停滞，表现为画面卡住。用系统时钟（墙钟）可以让播放速度与真实时间绑定，配合抖动缓冲区（jitter buffer）独立吸收网络抖动，同步更稳健。
