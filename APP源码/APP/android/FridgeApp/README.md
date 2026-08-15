# FridgeApp

这是智能药箱冰箱的安卓端控制 App，使用原生 Kotlin 和 Android 系统控件实现。

## 打开方式

1. 打开 Android Studio。
2. 选择 `Open`。
3. 打开本目录：`android/FridgeApp`。
4. 等 Gradle Sync 完成后，连接安卓手机运行 `app`。

## 默认连接配置

- 服务器地址：`http://101.245.87.7:10087`
- Token：`fridge-app-token`

App 首页右上角点“设置”可以修改服务器地址和 Token。

服务器 `/etc/fridge-deepseek.env` 里要保持：

```text
FRIDGE_HTTP_HOST=0.0.0.0
FRIDGE_HTTP_PORT=10087
FRIDGE_APP_TOKEN=fridge-app-token
```

云服务器安全组需要放行 `10087/TCP`，原来的 W800 仍然使用 `10086/TCP`。
