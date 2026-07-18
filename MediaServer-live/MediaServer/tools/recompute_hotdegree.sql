-- 热度批量重算脚本（已不是必需项）
-- hotdegree 现在由 CLogic::PlayReportRq 在收到播放上报时实时重算（见 clogic.cpp），
-- 不再依赖这个脚本 + crontab 定时任务。这个文件保留下来只是作为离线维护工具：
--   - 想一次性把全站视频的热度按最新公式重刷一遍时手动跑一次
--   - 或者怀疑某些视频的 hotdegree 因为历史原因跟播放数据对不上时，用它兜底重新对齐
--
-- 只聚合"最近30天"的互动数据，天然实现时间衰减：互动发生超过30天就自动不再计入热度，
-- 不需要在公式里搞指数衰减数学。这一点跟 PlayReportRq 里实时重算的逻辑保持一致。
--
-- 手动执行方式：
--   mysql -u root -p123456 -D MediaServer < /path/to/recompute_hotdegree.sql
--
-- 权重说明（可按需调整，需要和 clogic.cpp::PlayReportRq 里的权重保持一致）：
--   5  = 每次播放的基础分（弱信号，说明至少点开看了）
--   20 = 完播率权重，乘以播放次数（强信号，越接近1说明内容质量越高）

UPDATE t_VideoInfo v
LEFT JOIN (
    SELECT videoId,
           COUNT(*) AS playCount30d,
           AVG(watchSeconds / totalSeconds) AS avgCompletion30d
    FROM t_VideoPlayLog
    WHERE playTime > NOW() - INTERVAL 30 DAY
      AND totalSeconds > 0
    GROUP BY videoId
) p ON p.videoId = v.videoId
SET v.hotdegree = ROUND(
    5  * IFNULL(p.playCount30d, 0) +
    20 * IFNULL(p.avgCompletion30d, 0) * IFNULL(p.playCount30d, 0)
);
