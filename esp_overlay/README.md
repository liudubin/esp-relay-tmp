# esp-overlay (esp-android → iOS 移植渲染端)

高能英雄 ESP 渲染 dylib：把 Apr-YT/esp-android 的安卓渲染框架移植到 iOS。
与 esp_data.dylib 共存（同渠道 TweakInject 注入 AClient），自枚举 pawn、每帧直读矩阵。

## 安装（设备端 Filza 直装）

deb 已在 Minis 沙盒内：

```
/var/mobile/Containers/Data/Application/C3DAFE35-28C1-4E29-BBF3-B8565EBA6012/Documents/alpine-rootfs/var/minis/shared/esp-overlay-v1.deb
```

md5: 02d7c62295f06aae2462cbf6d5d1d40d
Filza 打开 → 点 deb → 安装（Sileo/Zebra 也可以装）→ 重启游戏。

## 验证判据

- 进对局后左上角 HUD：`M 0x…/0x… YS±1 DROP n` = 矩阵偏移已从 esp_state.json 拿到并锁定
- `M --scanning--` = esp_data 还没锁定矩阵（等它写入 esp_state.json）
- 敌人红框 + 绿血条 + 头顶名字 + 黄色骨骼折线
- ysign 未锁定前 40 帧投票（本地 pawn 投影 ndc.y），翻转后 YS±1 锁定

## 构建（Minis 沙盒内）

```sh
cd /var/minis/workspace/gnyx_android/esp_overlay
sh build.sh   # 产出 esp_overlay.dylib + com.esp.overlay_0.1.0_iphoneos-arm64.deb
```

## 架构对照

| 安卓 esp-android | iOS esp_overlay |
|---|---|
| 内核驱动 ioctl 读内存 | vm_read_overwrite(mach_task_self) |
| GWorld→Actors 遍历 | data 线程 500ms 枚举（CharacterMovement 类名过滤 + 影子过滤）|
| ViewProjection 每帧重读 | cmgr+mv_off / cmgr+mp_off 每帧直读 + last-good 1s |
| W2S 双步投影 | project_pos()（clip=V^T·pos；ndc=P·clip/w）|
| DrawBox/DrawHealth/DrawName | CAShapeLayer + CATextLayer |
| ANativeWindow+Vulkan+ImGui | UIWindow(Alert+99) + CALayer 全量重建 + CADisplayLink 120Hz |
| uinput 自瞄 | 未移植（iOS 无 uinput）|
