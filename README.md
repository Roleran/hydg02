# hydg02

HY-DG02 主控上的应用程序。这个仓库只放应用，不放 OpenWrt 编译树。

基础功能按《HY-DG02 整机功能列表》做：4G 注册、注册成功后上报一次位置、语音、对讲、LED、红外/雷达、两路 RS485、远程重启、恢复出厂、固件升级。注册成功之前不做现场动作。

## 放进固件仓库

在 `mt7628dan-openwrt` 里执行一次：

```sh
git clone <这个仓库的地址> package/hydg02
```

`package/hydg02/` 已经写在固件仓库的 `.gitignore` 里，克隆进来不会混进固件提交。应用有改动时，进 `package/hydg02` 执行 `git pull`，再回到固件仓库编译。

`mt7628dan_config/debug.diffconfig` 增加一行：

```text
CONFIG_PACKAGE_hydg02=y
```

然后在固件仓库：

```sh
./build.sh
```

产物烧进模组。单分区用 `MT7628DAN-DEV.dts`，不要用还按 8MB 槽位写的 A/B 配置。

板上更新这个包、不整包重刷时，在固件仓库编出 ipk 后拷到板子安装。整包烧录仍然走 `./build.sh`。

## 现在已经写了什么

能编过、能在 PC 上对协议样例的部分：

- 配置文件和板上节点路径
- N32 帧的组包、CRC 和拆包
- 注册报文 `0x81` 的组包
- 开机进程。注册、拨号、播报、亮屏、485 还没接上，所以进程只记录状态并等待

PC 上检查组帧。这一步不连板子，只核对协议文档里的样例字节：

```sh
cd src
make
./hydg02 --selftest
```

Windows 上如果 `make` 不在路径里，用已装的 gcc：

```sh
gcc -Wall -Wextra -std=c99 -o hydg02.exe *.c
hydg02.exe --selftest
```

看到 `selftest ok` 再往下写。失败时会打印哪一帧的哪一个字节和文档不一致。

## 接下来按这个顺序写

1. 4G：打开 `at_dev`，确认 `net_if` 拿到地址，读出 IMEI 和 ICCID。
2. MQTT：连上平台，发出 `0x81`，收到成功和 CID 后才进入业务。
3. 定位：注册成功后读一次 `gps_dev`。
4. 屏和雷达：打开 `n32_dev`，波特率 115200，用已经写好的帧收发。
5. 485：两个节点原样转发。
6. 语音和对讲：ALSA，播放前拉高功放 GPIO。格式是 WAV、MP3、PCM、OPUS。
7. 升级：校验 MD5 后走 `sysupgrade`。

星闪、视频、人车和烟火识别不进这一版。
