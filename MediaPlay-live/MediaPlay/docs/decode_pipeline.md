# 音视频解码流程详解

本文只讲解码这一件事，从"打开文件"到"拿到可以播放的原始数据"，逐步说清楚每个环节在做什么、为什么要这么做。其他功能（同步、跳转、录制）不在本文范围内。

---

## 一、先搞清楚几个概念

### 视频文件里装的是什么

你下载的一个 `.mp4` 文件，并不是直接存着一帧一帧的图片和一段段的音频波形。那样太大了——一个 1920×1080 的帧，如果存原始 RGB 数据需要约 6MB，一秒 30 帧就是 180MB，一部两小时的电影要 1.3TB，根本没法存也没法传。

所以视频文件里实际存的是**压缩过的数据**。图像经过 H.264、H.265 这类算法压缩，体积缩小几十甚至几百倍；音频经过 AAC、MP3 压缩，同样大幅缩小。

`.mp4` 这个文件本身是一个**容器格式**，它的工作就像一个"打包箱"：把压缩好的视频数据、压缩好的音频数据，加上时间信息、字幕等，打包放在一起，并在文件头部记录"视频用的什么压缩算法、音频用的什么压缩算法、各有多少条流"这类元数据。

除了 MP4，常见的容器格式还有 MKV、AVI、FLV、TS 等，它们的打包方式各不相同，但核心都是同样的思路。

### 什么是解码

**解码**就是把上面说的压缩数据还原成原始数据的过程：

- 视频解码：把 H.264 比特流 → 还原成一帧帧的图像（YUV 像素格式）
- 音频解码：把 AAC 比特流 → 还原成一段段的 PCM 音频采样数据

还原出来的原始数据才能送给显卡显示、送给声卡播放。

### Packet 和 Frame 的区别

这两个词在 FFmpeg 里反复出现，必须分清楚：

- **AVPacket（包）**：从文件里读出来的**压缩数据片段**。一个视频 packet 通常是一个编码帧（一张压缩图像）；一个音频 packet 可能包含一到多个编码帧（比如 AAC 每帧 1024 个采样）。Packet 还没有解码，不能直接播放。
- **AVFrame（帧）**：解码器处理 packet 之后输出的**原始数据**。视频 frame 包含 YUV 像素数据；音频 frame 包含 PCM 采样数据。Frame 才是可以直接操作的数据。

数据流向是单向的：`文件 → Packet → Frame → 播放`，Packet 是中间的压缩态，Frame 是解压后的原始态。

---

## 二、解码前的准备工作

解码正式开始之前，需要做三件事：打开文件、找到流、找到解码器。对应代码在 `videoplayer.cpp` 的 `VideoPlayer::run()` 函数开头部分。

### 第一步：打开文件，拿到 AVFormatContext

```cpp
AVFormatContext *pFormatCtx = avformat_alloc_context();
avformat_open_input(&pFormatCtx, file_path, NULL, NULL);
avformat_find_stream_info(pFormatCtx, NULL);
```

`AVFormatContext` 是 FFmpeg 里最顶层的结构体，代表一个已打开的媒体文件（或网络流）。可以把它理解成一个"文件描述符"，后续所有操作都要带着它。

`avformat_open_input` 做的事情是：读取文件头部，识别出这是什么容器格式（MP4？MKV？），解析容器头的元数据。

`avformat_find_stream_info` 是一个"探测"步骤。有些容器格式（比如早期的 AVI）在文件头里不完整地记录编解码参数，FFmpeg 需要实际读取文件开头的若干帧，从数据本身推断出"视频的分辨率是多少、音频的采样率是多少"这些信息。调用完这个函数之后，`pFormatCtx->streams[i]->codec` 里的参数才是可靠的。

### 第二步：找到视频流和音频流的索引

文件里可能有好几条"流"：一条视频、一条或多条音频（不同语言）、字幕流等。每条流有一个索引编号（0、1、2……）。我们需要找到视频流的编号和音频流的编号，代码里是 `videoStream` 和 `audioStream`。

`find_stream_index` 函数（`videoplayer.cpp:779`）就是遍历 `pFormatCtx->nb_streams`，看每条流的 `codec->codec_type` 是 `AVMEDIA_TYPE_VIDEO` 还是 `AVMEDIA_TYPE_AUDIO`，记下对应的索引。

找到索引之后，就能通过 `pFormatCtx->streams[videoStream]` 拿到视频流的详细信息，包括它用的是什么压缩格式（`codec_id`，比如 `AV_CODEC_ID_H264`）。

### 第三步：找到解码器并打开它

有了 `codec_id`，就能找到对应的解码器：

```cpp
pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
avcodec_open2(pCodecCtx, pCodec, NULL);
```

`avcodec_find_decoder` 在 FFmpeg 内部维护的解码器注册表里查找，根据 codec_id 返回对应的 `AVCodec` 结构体。H.264 就返回 H.264 解码器，AAC 就返回 AAC 解码器，不需要你手动指定名字，FFmpeg 自动匹配。

`avcodec_open2` 才是真正"打开"解码器的操作：它为解码器分配工作内存、初始化内部状态，之后这个解码器才能接受 packet 并解码。注意 `avcodec_find_decoder` 只是找到解码器的"说明书"，`avcodec_open2` 才是"组装机器、通电开机"。

---

## 三、PacketQueue：为什么需要一个缓冲队列

做好准备工作之后，接下来要读 packet、解码、输出。但这里有一个问题：**读 packet 和解码这两件事不能在同一个线程里按顺序串行做**。

原因是：读 packet 涉及磁盘 I/O（或网络 I/O），速度不稳定，有时快有时慢；解码是 CPU 密集计算，也需要时间。如果串行，解码在等 I/O，I/O 在等解码，互相拖累，播放就会卡顿。

解决方案是用**生产者-消费者模式**：

- 一个线程（读线程）专门负责从文件读 packet，往队列里放；
- 另一个线程（解码线程）从队列里取 packet，解码输出。

两个线程各跑各的，队列作为缓冲区解耦它们之间的速度差异。这个队列就是 `PacketQueue`。

### PacketQueue 的结构

`PacketQueue`（`PacketQueue.h:13`）是一个单向链表：

```
first_pkt → [packet1] → [packet2] → [packet3] → NULL
                                          ↑
                                       last_pkt
```

每个节点是一个 `AVPacketList`，包含一个 `AVPacket` 和一个指向下一节点的指针。入队从尾部插，出队从头部取。

结构体里还有两个重要字段：`mutex`（互斥锁）和 `cond`（条件变量）。这两个是 SDL 提供的同步原语，用来保证两个线程同时访问队列时不出问题。

### 入队（packet_queue_put）

读线程调用这个函数，把一个 packet 放入队列尾部。核心逻辑（`PacketQueue.cpp:13`）：

在操作链表之前先加锁（`SDL_LockMutex`），确保解码线程此时不会同时来取数据，避免链表指针被并发修改破坏。插入完成后，调用 `SDL_CondSignal` 发一个信号，通知"队列里有新数据了"——万一解码线程正在等待数据，收到这个信号就会醒来。最后解锁。

有一个细节：入队前调用了 `av_dup_packet`，对 packet 做一次深拷贝。原因是 `av_read_frame` 返回的 packet，其 `data` 指针指向 FFmpeg 内部的缓冲区，下次调用 `av_read_frame` 时这块内存会被覆盖。要让 packet 在队列里安全地排队等待，必须把数据拷贝到独立的堆内存上。

### 出队（packet_queue_get）

解码线程调用这个函数，从队列头部取走一个 packet。有一个 `block` 参数：

- `block = 0`（非阻塞）：队列空就立刻返回 0，不等待。本项目的视频线程和音频解码都用非阻塞模式。
- `block = 1`（阻塞）：队列空就调用 `SDL_CondWait` 挂起，直到有新 packet 入队才醒来。

为什么音频解码用非阻塞？因为音频回调函数是 SDL 的硬件驱动线程触发的，有严格的时间要求，不能让它卡在等待数据上；如果队列暂时空了，直接返回 -1，回调函数会填充一段静音数据顶替。

---

## 四、视频解码详解

视频解码发生在 `video_thread` 函数（`videoplayer.cpp:45`）里，这是一个独立的 SDL 线程。

### 第一步：从队列取出压缩数据（packet）

```cpp
if (packet_queue_get(is->videoq, packet, 0) <= 0) {
    // 队列暂时为空，等一下再试
    SDL_Delay(1);
    continue;
}
```

用非阻塞方式从视频队列取一个 packet。如果返回 0 说明队列里暂时没有数据，睡 1ms 再来。如果文件真的读完了并且队列也空了，才会退出循环。

### 第二步：解码——把压缩数据变成 YUV 帧

```cpp
ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture, packet);
```

这是解码的核心调用（`videoplayer.cpp:109`）。把压缩的 `packet` 喂给解码器，解码器输出一帧原始图像到 `pFrame`。

`got_picture` 是一个输出参数，值为 1 表示这次调用成功输出了一帧；为 0 则表示没有输出帧——这在 B 帧场景下正常发生。B 帧（双向预测帧）依赖它前面和后面的帧来解码，解码器需要先收到"后面的帧"才能输出"中间的 B 帧"，所以有时候喂入一个 packet，解码器内部缓存着，没有立刻输出。

`avcodec_decode_video2` 成功后，`pFrame` 里包含的图像数据是 **YUV 格式**，不是 RGB。这需要进一步转换才能在屏幕上显示。

### 为什么视频压缩后存的是 YUV 而不是 RGB

这是视频压缩里一个重要的设计选择，和人眼感知有关。

人眼对**亮度**变化非常敏感，对**颜色**变化相对不敏感。YUV 格式正是利用了这一点：

- **Y**（亮度分量）：每个像素都有，完整保留亮度信息；
- **U、V**（色度分量）：只存 1/4 的信息（每 4 个像素共用一组 UV），人眼几乎察觉不到差别。

这就是 YUV420P 格式的"420"的含义。同样分辨率下，YUV420P 的数据量只有 RGB24 的一半，并且编码器在此基础上再做压缩，效率大幅提升。

所以从解码器出来的数据是 YUV，是因为视频从一开始编码进文件时就是 YUV 的形式。

### 第三步：YUV → RGB 格式转换（sws_scale）

屏幕显示需要 RGB（或 ARGB）格式，所以要把 YUV 数据转换一下：

```cpp
sws_scale(
    img_convert_ctx,
    (uint8_t const * const *)pFrame->data,   // YUV 数据
    pFrame->linesize,
    0, pCodecCtx->height,
    pFrameRGB->data,                          // 转换后的 RGB 数据放这里
    pFrameRGB->linesize
);
```

`sws_scale` 是 `libswscale` 库提供的核心函数，负责色彩空间转换和图像缩放（这里尺寸没变，只做颜色格式转换）。

它的上下文 `img_convert_ctx` 是在 `video_thread` 入口处创建的（`videoplayer.cpp:61`），只创建一次，整个播放过程复用：

```cpp
img_convert_ctx = sws_getContext(
    width, height, pCodecCtx->pix_fmt,   // 输入：原始 YUV 格式
    width, height, AV_PIX_FMT_RGB32,     // 输出：RGB32 格式
    SWS_BICUBIC,                          // 插值算法（缩放时用，这里只是格式转换）
    NULL, NULL, NULL
);
```

目标格式选 `AV_PIX_FMT_RGB32`（即 32 位 ARGB，高字节固定为 0xFF）而不是 RGB24 的原因：Qt 的 `QImage::Format_RGB32` 直接对应这个格式，不需要再做任何转换就能用来构建 QImage；而且 4 字节对齐的内存访问比 3 字节的 RGB24 效率更高。

### 第四步：封装成 QImage，通过信号发给 UI

```cpp
QImage tmpImg((uchar*)out_buffer_rgb, pCodecCtx->width, pCodecCtx->height,
              QImage::Format_RGB32);
QImage image = tmpImg.copy();
is->m_player->SendGetOneImage(image);
```

`tmpImg` 只是对 `out_buffer_rgb` 这块内存的一个"包装视图"，不拥有数据。如果直接把 `tmpImg` 发出去，下一帧 `sws_scale` 就会把 `out_buffer_rgb` 的内容覆盖掉，UI 线程还没来得及显示就已经被改了。所以必须调用 `.copy()` 做一次深拷贝，复制出一块独立的内存，再通过 Qt 信号发给 UI 线程显示。

---

## 五、音频解码详解

音频解码分两层：`audio_callback` 是外层，`audio_decode_frame` 是内层。

### 为什么音频解码用"回调"而不是用独立线程

视频解码用了一个独立的 SDL 线程（`video_thread`），可以自己控制节奏。但音频不行。

声卡（音频硬件）有自己的缓冲区，它按固定频率消耗数据——比如采样率 44100Hz、缓冲区 1024 个采样，那么大约每 23ms 声卡就消耗一次缓冲区的数据，消耗完了就需要应用程序补充新数据。如果应用程序没有及时补充，声卡就会播放出噪声或者静音。

SDL 的解决方案是：你注册一个回调函数，SDL 内部用一个专门的线程，在声卡需要数据的时候自动调用你的回调函数，把 `stream`（声卡缓冲区）填满。你不需要自己管这个线程，SDL 帮你管，你只需要在回调里提供数据就行。

这就是为什么注册 SDL 音频设备时要指定 `callback = audio_callback`（`videoplayer.cpp:426`）。

### audio_callback 在做什么（videoplayer.cpp:621）

SDL 调用回调时会传入三个参数：`stream`（需要填充的声卡缓冲区指针）、`len`（要填充多少字节）。

`audio_callback` 要做的事情只有一件：**把 `len` 字节的 PCM 音频数据写入 `stream`**。

但解码出来的一帧音频数据不一定正好等于 SDL 每次要求的 `len`。为了处理这个尺寸不匹配的问题，`VideoState` 里维护了一个内部缓冲区 `audio_buf`，配合两个游标：`audio_buf_size`（当前缓冲里有多少有效数据）和 `audio_buf_index`（已经拷贝给 SDL 多少字节了）。

逻辑是这样的：每次回调进来，检查内部缓冲是否还有剩余数据没有拷完。如果有，继续从上次中断的位置往 `stream` 里拷；如果内部缓冲已经用完了，就调用 `audio_decode_frame` 解码新的一帧数据，填入 `audio_buf`，再继续拷给 SDL。

如果 `audio_decode_frame` 返回 -1（队列为空），就往 `audio_buf` 里填 1024 字节的零（静音），保证 SDL 不会拿到垃圾数据。

最后用 `SDL_MixAudioFormat` 而不是 `memcpy` 来写数据，是因为 SDL 混音函数支持音量调节，未来可以方便地加入软件音量控制。

### audio_decode_frame 在做什么（videoplayer.cpp:667）

这个函数负责从 `audioq` 取一个 packet，解码出 PCM 数据，转换格式后放入 `audio_buf`。

**取 packet：**

```cpp
if (packet_queue_get(audioq, &pkt, 0) <= 0) {
    return -1;
}
```

非阻塞取包。队列空就返回 -1，`audio_callback` 会填充静音。

**解码：**

```cpp
int ret = avcodec_decode_audio4(aCodecCtx, audioFrame, &got_picture, &pkt);
if (ret < 0) {
    exit(0);  // 真正的解码器错误，硬退出
}
```

`avcodec_decode_audio4` 是音频版本的解码函数，把压缩 packet 解码成原始 PCM 数据，存入 `audioFrame`。

注意这里的错误处理：`ret < 0` 时直接 `exit(0)` 而不是返回 -1，因为这是解码器本身出了问题（比如损坏的码流导致解码器崩溃），属于无法恢复的错误。与之不同，队列为空返回 -1 是"暂时没有数据"的正常状态。

**一个 packet 里有多帧音频：**

代码里有 `while(audio_pkt_size > 0)` 循环（`videoplayer.cpp:702`）。

视频通常一个 packet 对应一帧；但音频 packet（尤其是 AAC）可能包含多个编码帧，每次 `avcodec_decode_audio4` 调用只消耗一部分 packet 数据（返回值 `ret` 是本次消耗的字节数），用 `audio_pkt_size -= ret` 减去消耗量，循环直到整个 packet 处理完。不过本项目 `audio_decode_frame` 解码出第一帧就 `return data_size` 出去了，剩余帧留给下次回调再处理——虽然这里有 while 循环，但实际只处理第一帧，略有浪费但逻辑正确。

### 音频格式转换（swr_convert）

解码器输出的 `audioFrame` 里的数据格式不一定是 SDL 能直接播放的格式。比如 AAC 解码后是 `AV_SAMPLE_FMT_FLTP`——float 平面格式（左右声道数据分开存，且是 32 位浮点数），而 SDL 设备配置的是 `AUDIO_S16SYS`（16 位有符号整数，交错存储）。需要转换：

```cpp
swr_ctx = swr_alloc_set_opts(NULL,
    wanted_frame.channel_layout,           // 目标声道布局（如立体声）
    (AVSampleFormat)wanted_frame.format,   // 目标采样格式（S16）
    wanted_frame.sample_rate,              // 目标采样率（如 44100）
    audioFrame->channel_layout,            // 源声道布局
    (AVSampleFormat)audioFrame->format,    // 源采样格式（如 FLTP）
    audioFrame->sample_rate,               // 源采样率
    0, NULL
);
swr_init(swr_ctx);
swr_convert(swr_ctx, &audio_buf, AVCODEC_MAX_AUDIO_FRAME_SIZE,
            (const uint8_t **)audioFrame->data, audioFrame->nb_samples);
swr_free(&swr_ctx);
```

`swr_alloc_set_opts` 告诉重采样器"输入是什么格式，输出是什么格式"；`swr_init` 初始化（计算内部滤波器系数等）；`swr_convert` 才是真正做转换的那一步，把 `audioFrame->data` 里的数据转换格式后写入 `audio_buf`。

这里有一个值得注意的低效之处：每解码一帧音频都重新创建 `swr_ctx`，用完就 `swr_free`，然后下一帧再重建。`swr_alloc_set_opts` + `swr_init` 是有初始化开销的（计算重采样滤波器系数等）。正确的做法是在打开音频流时建好一个 `swr_ctx` 一直复用，只要参数不变就不需要重建。本项目是学习代码，逻辑清晰优先，性能其次。

### "采样格式"到底是什么

音频的原始数据（PCM）是一串数字，代表每个时刻声波的振幅。这些数字可以用不同的数值类型来表示：

- `AV_SAMPLE_FMT_U8`：8 位无符号整数，精度低，声音质量差
- `AV_SAMPLE_FMT_S16`：16 位有符号整数，CD 品质，SDL 最常用
- `AV_SAMPLE_FMT_FLTP`：32 位浮点数，平面格式（左右声道分开）——很多现代编码器内部使用，精度高但字节数多

"平面格式"（Planar，后缀 P）和"交错格式"（Packed）的区别：

- 平面：左声道数据全在一块，右声道数据全在另一块，即 `LLLLLLRRRRRR`
- 交错：左右声道交替存放，即 `LRLRLRLR`

SDL 要求交错格式（S16SYS），所以从解码器出来的平面格式需要 `swr_convert` 转换。

---

## 六、三个线程的必要性

这部分从整体角度解释为什么要三个线程，而不是用一个线程把所有事情串行做完。

**读线程（VideoPlayer::run via QThread）** 负责从文件读 packet，分发到视频队列和音频队列。它的速度取决于磁盘/网络 I/O，不稳定。

**视频线程（video_thread via SDL_CreateThread）** 负责从视频队列取 packet、解码、YUV→RGB 转换、发 QImage 信号。解码是 CPU 密集计算，可能比较慢。

**音频回调（SDL 内部线程）** 每约 23ms 被 SDL 触发一次，必须在这个时间窗口内完成从队列取 packet、解码、格式转换、填入 stream 的全部工作，时间要求严格。

如果只用一个线程串行：读 packet 时 I/O 阻塞，音频回调就没有及时得到数据，出现音频卡顿；或者音频解码占用时间太长，读 packet 跟不上，视频队列为空，视频卡顿。

三个线程相互独立，队列作为缓冲，各自以自己的节奏运行，互不阻塞。读线程读快了就让队列积压一些（流控限制在约 4MB~13MB），读慢了队列暂时空也没关系，解码线程等一下就好。这种设计的核心思想就是：**用空间（队列缓冲）换取时间（各线程无阻塞运行）**。

---

## 七、解码流程中的数据流向总结

用文字描述一帧视频从文件到屏幕的完整路径：

读线程调用 `av_read_frame` 从文件读出一个压缩的视频 packet，复制数据后放入视频队列 `videoq`。视频线程从 `videoq` 取出这个 packet，调用 `avcodec_decode_video2` 让解码器解压，得到一帧 YUV 格式的原始图像（存在 `pFrame`）。然后调用 `sws_scale` 把 YUV 转换成 RGB32 格式（存在 `pFrameRGB` / `out_buffer_rgb`）。把 RGB 数据封装成 `QImage`，深拷贝后通过 Qt 信号发给 UI 线程，UI 线程在控件里把它画出来。

一帧音频从文件到声卡的完整路径：

读线程调用 `av_read_frame` 读出一个压缩的音频 packet，放入音频队列 `audioq`。SDL 内部线程每约 23ms 调用一次 `audio_callback`，回调里调用 `audio_decode_frame` 从 `audioq` 取 packet，用 `avcodec_decode_audio4` 解码成 PCM（存在 `audioFrame`），再用 `swr_convert` 把采样格式转换成 SDL 要求的 S16 交错格式，写入 `audio_buf`。最后 `audio_callback` 用 `SDL_MixAudioFormat` 把 `audio_buf` 里的数据拷进 SDL 的 `stream` 缓冲区，SDL 把 `stream` 送给声卡硬件播放。
