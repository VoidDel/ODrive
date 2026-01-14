# ODrive GUI

ODrive 电机控制器的桌面配置和控制应用程序。

## 功能特性

- **国际化 (i18n)**：完整支持英文和中文界面
- 带实时绘图的交互式仪表板
- 电机配置的分步设置向导
- 实时参数调整和监控
- 电机和编码器校准工具

## 语言支持

GUI 支持多种语言。点击标题栏中的语言按钮可在以下语言之间切换：
- **English** (英文)
- **中文** (默认)

您的语言偏好会自动保存，重新打开应用程序时会恢复。

## 系统要求

Python 依赖：`pip install flask flask_socketio flask_cors odrive`

如果不想使用默认的 odrive python 包，可以通过命令行参数传递模块路径。

Windows 10 示例：
```
./odrive_gui_win.exe C:/Users/<你的用户名>/ODrive/tools C:/Users/<你的用户名>/ODrive/Firmware
```

第一个参数是本地 odrivetool 版本的路径，第二个是 fibre 的路径。

## 开发和测试说明

### 项目设置
```
npm install
```

### 开发模式编译和热重载
```
npm run serve
```

### 代码检查和修复
```
npm run lint
```

### 运行 Electron 版本的 GUI
```
npm run electron:serve
```

### 将 Electron 应用打包为可执行文件
```
npm run electron:build
```

### 为所有平台构建 Electron 应用
```
npm run electron:build -- -mwl
```

### 从源码运行
在开发分支上，可能有未发布的依赖项更改（如 fibre 或 ODrive 枚举）。使用此命令从仓库中启动带依赖项的 GUI：
```
npm run electron:serve -- ../tools/
```

### 为树莓派和其他 ARM 平台设备构建

需要先安装 PhantomJS 依赖：
```
sudo apt install phantomjs
```

安装后，可以使用 `npm run electron:build` 为 ARM 构建 AppImage。

## 国际化 (i18n)

GUI 使用 [vue-i18n](https://kazupon.github.io/vue-i18n/) 进行国际化。

### 翻译文件

翻译文件位于 `src/locales/` 目录：
- `en.json` - 英文翻译
- `zh.json` - 中文翻译
- `index.js` - i18n 配置

### 添加新语言

1. 在 `src/locales/` 中创建新的翻译文件（例如 `ja.json` 用于日语）
2. 从 `en.json` 复制结构并翻译所有字符串
3. 在 `src/locales/index.js` 中导入并添加新的语言环境：
   ```javascript
   import ja from './ja.json'

   export default {
     en,
     zh,
     ja  // 在这里添加您的新语言
   }
   ```
4. 如需要，在 `src/App.vue` 中添加语言切换器选项

### 翻译覆盖范围

- ✅ 应用程序标题和导航
- ✅ 启动/设置页面
- ✅ 仪表板控件和按钮
- ✅ 向导（所有页面、选择和工具提示）
- ✅ 校准消息
- ✅ 错误标签和状态消息
- ✅ 绘图控件

### 技术术语翻译

专业电机控制术语的中文翻译：
- axis → 轴
- encoder → 编码器
- calibration → 校准
- pole pairs → 极对数
- phase resistance → 相电阻
- phase inductance → 相电感
- Hall effect → 霍尔效应
- closed loop control → 闭环控制
- position control → 位置控制
- velocity control → 速度控制
- torque control → 力矩控制

## 自定义配置
参见 [配置参考](https://cli.vuejs.org/config/)。

## 许可证

请参阅主仓库的 LICENSE 文件。
