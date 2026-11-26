# HEN-K Android 数据层 daemon

## 架构
- 独立 root daemon（不注入目标进程），/proc/<pid>/mem 读写
- 内核后端接口预留（rw_backend: pm / kp / ko），ACE 强化后切 APatch kpatch 或自研 .ko
- 快照 500ms 写 /data/local/tmp/esp_state.json（悬浮窗 app 消费）

## 偏移复用说明
iOS 实测的引擎偏移全量平移（arm64 LP64 同 ABI）：
UObject Class@0x10 / UWorld PersistentLevel@0x30 / Level.Actors@0x98 /
pawn PS@0x2c0 PC@0x2d8 胶囊@0x310(+0x200 C2W) 骨骼@0x300(+0x600 24骨) /
PC CMgr@0x388 / POV loc@0x1ef0 yaw@0x1f08 pitch@0x1f0c fov@0x1f14 /
本地 pawn@PC+0x2d0。安卓侧不吻合时改 AO_* 宏。

## Termux 部署
1. 手机装 Termux，pkg install clang
2. adb push 或下载本目录文件
3. bash build.sh
4. 游戏进对局后: su -c "./gnyx_daemon"   （后台跑: nohup）
5. 验证: cat /data/local/tmp/esp_state.json

## 扫描链（无硬编码依赖，首次自动锁定）
GWorld（.data/.bss 堆指针→UWorld 结构互指验证）→ PC（GameInstance.LocalPlayers[0]）
→ GNames（"None" 特征，失败则 pawn 识别退化结构启发式）→ pawn 表 → POV → 矩阵+本地标尺验证
