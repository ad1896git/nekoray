# 运行参数

- `-many` 跳过单实例检查。Unix 上若同一配置目录的启动记录仍属于存活的 GUI，会拒绝启动核心；请关闭原 GUI 或使用不同的 `-appdata` 目录。
- `-appdata` 开启后配置文件会指定目录，未指定目录则使用共享目录，无法多开和自动升级。
- `-flag_reorder` 进行重新整理配置文件的顺序，并删除损坏和孤立的配置。

On Unix, `-many` cannot start another core for a configuration whose launch
record identifies a live owning GUI (PID and process start time both match).
The second GUI displays a refusal; it leaves the incumbent core and record alone.
Use separate `-appdata` directories for concurrent instances. GUI identity in the
0600 record is observation-only metadata and is never used for signaling.
Legacy records, or records with missing/incomplete owner metadata, retain the
existing authenticated Exit and bounded wait/refusal recovery path. They cannot
identify a live legacy GUI; close older instances before launching with `-many`.
