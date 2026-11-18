# 标准库与平台 SDK

Asuka 自带两组包。它们都随 `asuka` 命令行工具一起提供，不需要 `asuka add`：导入就能用。

- **标准库**，`std/...`：语言自己的库（JSON、时间、URL、正则表达式、文件、HTTP、SQLite、store、Markdown、图表……），在每个平台上都一样。
- **平台 SDK**，`platform/...`：封装设备（相机、定位、蓝牙、传感器）、操作系统服务（通知、分享面板、Dock 和托盘、钥匙串）或应用商店（购买、Apple Pay、评分）的包。

```asuka
import std/http
import std/store

state units = store.read("units", "metric")
```

导入旧版本的 `asuka/...` 路径时，编译器会告诉你这个包现在叫什么（`asuka/camera is platform/camera now`）。

标准库的大多数包不需要平台代码：它们用 Asuka 写成，Asuka 表达不了的部分由运行时自己的 C 完成，所以在每个平台上的行为都一样（`std/json`、`std/math` 和 `std/embed` 是编译器的一部分）。其余的包（以及平台 SDK 的大多数包）和别的[原生包](native.md)一样：每个平台用自己的 API 来实现（iOS 和 macOS 上用 URLSession，Android 上用它自己的类，Web 上用浏览器的 API，Windows 和 Linux 上用 C），所以它们的行为和平台原生应用里的一致。

## 标准库：`std/`

| 包 | 内容 |
|---|---|
| [`std/json`](std/json.md) | `json.decode<T>(text)` 和 `json.encode(value, indent:)`：带类型的 JSON，依据编译器对 `T` 的了解来读写 |
| [`std/math`](std/math.md) | `sqrt`、`pow`、`exp`、`log`、`log2`、`log10`、`floor`、`ceil`、`round`、`sin`、`cos`、`tan`、`atan`、`atan2`、`clamp`、`random`、`randomInt`、`seed`、`pi` |
| [`std/embed`](std/embed.md) | 把包里的文件编进应用：`embed.text(path)`、`bytes(path)`、`json<T>(path)`（按 `T` 读取，编译时检查）、`texts(glob)` 和 `files(glob)`（`[路径: 文本]`、`[路径: 字节]`） |
| [`std/time`](std/time.md) | `time.now()`、`time.date(2026, 3, 7, hour: 9, zone: .utc)`、`time.unix(s)`、`time.parse(rfc3339)`：得到一个 `Time`，即带时区（`.local`、`.utc`、`.fixed(seconds:)`）的时刻，以及它在挂钟上的 `year`、`month`、`day`、`hour`、`minute`、`second`、`nanosecond`、`weekday`。它的方法有：`format("EEE d MMM, HH:mm")`（Unicode 的日期模式，英文输出）、`rfc3339()`、`add(d)`、`sub(t)`（得到 `Duration`）、`addDate(years:, months:, days:)`、`isBefore`、`isAfter`、`compare`、`sameDay`、`startOfDay`、`truncate(d)`、`inZone`、`utc`、`local`、`unix`、`unixMilli`、`unixNano`。另有 `time.monotonic()`（只会向前走的 `Duration`）、`time.since(t)`、`time.ticks(1s)` 和 `time.ticker(1s)`（时刻的流） |
| [`std/decimal`](std/decimal.md) | `Decimal`：精确的十进制数，用于金额。`Decimal.parse("19.99")`（得到 `Decimal?`）、`Decimal.of(1999, exponent: -2)`、`.zero`、`.one`；`+`、`-`、`*` 精确计算（也可以 `price * 3`），`/` 保留 18 位有效数字，还有 `%`、`-x`、`<` 等比较；`divided(by:, places:, mode:)` 和 `rounded(places:, mode:)` 按 `RoundingMode`（`down`、`up`、`ceiling`、`floor`、`halfUp`、`halfDown`、`halfEven`）舍入；`string()`、`formatted(currency:)`（按当前语言的方式书写，同 `i18n.formatCurrency`）、`int()`、`float()`、`abs()`、`sign()`。结果的位数超出 int 能容纳的范围时，抛出 "integer overflow" |
| [`std/url`](std/url.md) | `url.parse(text)`：得到 `URL?`，含 `scheme`、`user`、`host`、`port`、`path`、`rawQuery`、`fragment`，以及 `query(name)`、`queryAll(name)`、`text()` 和 `resolve(ref)`（RFC 3986）；还有 `url.resolve(base, ref)`、`escape`、`unescape`、`pathEscape`、`pathUnescape`、`encodeQuery(map)`（参数名排序）、`parseQuery(raw)` |
| [`std/regex`](std/regex.md) | `regex.compile(pattern)`（模式有误时抛出错误）：得到 `Regex`，有 `matches`、`find`、`findAll`、`findRange`、`findSubmatch`、`findAllSubmatches`、`replace(text, with: "$1")`、`replaceEach(text, f:)`、`split`、`groupNames`；以及 `regex.quote(text)`。语法是 RE2 的，匹配时间是线性的（不回溯） |
| [`std/encoding`](std/encoding.md) | `base64Encode`、`base64URLEncode`（不补齐）、`base64Decode`（两种字母表都接受）、`hexEncode`、`hexDecode` |
| [`std/crypto`](std/crypto.md) | `sha256`、`sha512`、`sha1`、`hmacSHA256(key:, data:)`、`randomBytes(n)`（来自系统的随机源）、`uuid()`（第 4 版）、`equal(a, b)`（常数时间比较），以及 Ed25519（RFC 8032）：`ed25519Verify(publicKey, message, signature)`、`ed25519Sign(privateKey, message)`、`ed25519PublicKey(privateKey)`、`ed25519NewKey()` |
| [`std/html`](std/html.md) | `escape(text)` 和 `unescape(text)`（数字引用，以及文本中常用的命名引用） |
| [`std/random`](std/random.md) | `random.int(1, 6)`（包含两端）、`float()`、`chance(p)`、`pick(items)`、`shuffled(items)`、`seed(n)` |
| [`std/fs`](std/fs.md) | `fs.appDir()`（应用自己的目录）、`join([…])`、`clean`、`dir`、`base`、`ext`；以及会阻塞、应放在 `spawn { }` 里调用的：`readDir`（返回 `Entry`：`name`、`isDir`、`size`、`modified`）、`stat`、`read`、`readText`、`write`、`writeText`、`append`、`appendText`、`copy`、`rename`、`mkdirAll`、`remove`、`removeAll`、`exists`。失败时抛出 `FsError`（`.notFound(path)`、`.exists`、`.denied`、`.notDirectory`、`.isDirectory`、`.notEmpty`、`.failed(path, reason)`） |
| [`std/os`](std/os.md) | 程序运行时所在的进程（[程序](programs.md)）：`os.args()`、`program()`、`env(name)`、`environ()`、`cwd()`、`exit(code)`；stdin：`readLine()`、`readAll()`、`lines()`（一个 stream，在另一个线程上读取）；`eprint(text)`、`writeTo(.stderr, text)`、`terminal(.stdout)`。`os.fake(args:, env:, stdin:)` 和 `FakeOS.run(main)`：在测试里像真正的进程那样运行程序的 main（得到退出码、stdout 和 stderr）。手机和 Web 上的应用：没有参数和 stdin，`exit` 会抛出错误 |
| [`std/flag`](std/flag.md) | `flag.parse<T>(os.args())`：根据编译器对 `T` 的了解，把命令行读成一个选项结构体（有默认值的字段是 `--字段名`，文档注释是帮助，第一个词 `-x` 是短名；没有默认值的是位置参数）或一个子命令枚举；支持 `--help`、`--version`，出错时给出建议并以 2 退出。`tryParse<T>`（`FlagError`）、`help<T>()` |
| [`std/command`](std/command.md) | 运行其他程序，从不经过 shell：`command.run(program, args, cwd:, env:, stdin:, check:)`（返回 `Output`：`code`、`stdout`、`stderr`、`text()`），`status(…)`（使用当前程序的标准流），`start(…)`（返回 `Process`：`lines()`、`stdout()`、`write`、`closeStdin`、`wait`、`output`、`kill`）；等待被取消时会结束该程序。`bundled(name)`：应用自带的程序（asuka.toml 的 `[bin]`）的路径。底层是 fork 和 exec、CreateProcess；测试中用 `command.fake()`。可用于程序和桌面应用；手机和 Web 上为 `unsupported` |
| [`std/i18n`](std/i18n.md) | 应用显示的语言：`i18n.locale()`、`setLocale(tag)`、`languages()`、`preferredLanguages()`、`isRTL()`、`autonym(tag)`、`region()`；按这种语言的方式书写数字和日期：`formatNumber`、`formatInt`、`formatPercent`、`formatDecimal`、`formatCurrency(amount, currency)`、`formatDate(t, date:, time:)`。应用自己的文案用 `tr.key(…)`（见[多语言](i18n.md)） |
| [`std/http`](std/http.md) | `http.get`、`http.post`、`http.postJSON`、`http.request`、`http.download`（边收边写进文件，并报告进度）、`http.stream`（边发边读的响应体：片段、`lines()`、`events()`）、`http.events`（服务器发送事件，可按需重连）、`http.loopback`（回环接口上的服务器，接收登录的重定向）：请求经由平台自己的客户端发出，所以代理、受信任的证书、缓存和 cookie 都和系统里的表现一致。`Response` 有 `status`、`url`、`headers`、`body`、`text()`、`json<T>()` 和 `ok()` |
| [`std/websocket`](std/websocket.md) | `websocket.connect(url, headers:, protocols:)`：得到一个 `WebSocket`，可以 `send(text)`、`sendBinary(data)`，从 `messages` 读取消息（`.text`、`.binary`），用 `close(code:, reason:)` 关闭；由各平台自己的客户端承载（URLSession、OkHttp、浏览器、WinHTTP、libsoup） |
| [`std/store`](std/store.md) | `store.read(key, default)`、`store.write(key, value)`、`store.remove(key)`、`store.flush()`、`store.dir()`：以 JSON 形式跨启动保存的小数据，存放在平台的偏好设置里（UserDefaults、SharedPreferences、`localStorage`，在 Windows 和 Linux 上是一个文件，改动后稍等片刻才写入） |
| [`std/watch`](std/watch.md) | `watch.path(dir, ignore:, recursive:, attributes:)` 返回一个 `Watch`：它的 `root`，以及 `events`，一个由一批批 `Event` 组成的流（`op`：create、write、remove、rename、attrib、overflow；`path`、`oldPath`）。基于各个桌面系统自己的文件事件；手机和 Web 上不支持 |
| [`std/sqlite`](std/sqlite.md) | `sqlite.open(name)`（应用数据目录里的一个文件、一个绝对路径，或 `":memory:"`）返回一个 `DB`：`migrate(steps)`、`exec(sql, args)` 和 `query<T>(sql, args)`（异步，在主线程之外运行；查询到的行直接解码成 `T`，结构体的字段按列名对应）、`watch<T>(sql, args)`（一个 `Live<T>` store：`rows`、`loading`、`error`；参数变化时、它读取的表被写入时都会重新运行）、`changed()`、`close()`。SQLite 编译进应用（见[库文件](data.md#库文件)）；在 Web 上运行在页面的一个 worker 里 |
| [`std/storage`](std/storage.md) | `storage.Value(key, default)`：一个 store，它的 `value` 保存在设备上（用的是 std/store），会被跟踪，变化时写入；`reset()`；`storage.flush()`、`storage.dirName()` |
| [`std/lottie`](std/lottie.md) | `lottie.View(source)`（`bind playing`、`bind progress`、`loop`、`speed`、`segment`、`marker`、`fit`、`overrides`；`complete`、`loaded`）：用各平台的播放器播放 Lottie 和 dotLottie；Windows 和 Linux 上暂时没有（[组件](components.md)） |
| [`std/svg`](std/svg.md) | `svg.Icon(markup)` 在应用编译时解析，`svg.View(icon) { color }` 显示它；`svg.Shape(layers)`：应用自己画的路径，会跟随它的框（[组件](components.md)） |
| [`std/base`](std/base.md) | 只有行为、没有外观的组件，由应用设置样式，在各平台上表现一致：`Select`、`Combobox`、`Menu`、`Dialog`、`Popover`、`Tooltip`、`Tabs`、`Checkbox`、`Switch`、`RadioGroup`、`Slider`、`Field` 和 `Form`、toast 等。每个组件都会处理好自己的状态、键盘与焦点、屏幕阅读器，以及它浮在窗口上的位置（[自定义外观](components.md#自定义外观)） |
| [`std/chart`](std/chart.md) | `chart.View(series) { labels, label, low, high, grid, format }`；`chart.Series(name:, kind: .bar \| .line \| .area \| .points, values:, smooth:, color:)`：用 Asuka 写成，通过 `svg.Shape` 绘制（[组件](components.md)） |
| [`std/markdown`](std/markdown.md) | `markdown.View(text) { streaming }` 和 `markdown.heal(text)`：把 Markdown 显示为平台自己的富文本，流式输入时边接收边修补（[组件](components.md)） |

## 平台 SDK：`platform/`

这些包封装的是各个平台自己的能力，所以不同平台上能做的事也不同：某个平台上没有的功能，包会明确说明（抛出 `unsupported` 错误，或者给出一个替代结果），[各平台对比](parity.md)页面列出了它们的差异。

| 包 | 内容 |
|---|---|
| [`platform/integrity`](platform/integrity.md) | `integrity.supported()`、`generateKey()`、`attestKey(id, challenge)`、`assertion(id, data)`（iOS 和 Mac 上的 App Attest）；`prepare(cloudProjectNumber:)`、`token(requestHash:)`（Play Integrity） |
| [`platform/js`](platform/js.md) | `js.eval(script, timeout:)`：在平台自己的引擎里离屏运行 JavaScript（WKWebView、Android 的 WebView、WebView2、WebKitGTK、沙盒化的 frame），以 JSON 返回它的值；`js.JsError.timeout` |
| [`platform/apps`](platform/apps.md) | `apps.list()`、`apps.search(apps, query)`（启动器式的排序：完全相同、开头相同、某个单词的开头或各单词首字母、任意位置、字母按顺序出现；不区分大小写和重音）、`apps.icon(app, size)`（一个 PNG 文件的路径）、`apps.open(app)`：macOS、Windows 和 Linux 上已安装的应用；在手机和网页上抛出 `AppsError.unsupported` |
| [`platform/dock`](platform/dock.md) | `dock.setProgress(Progress(value:, state:))` 和 `dock.requestAttention(.critical)`：应用图标上的进度和提醒，在 macOS 的程序坞、Windows 的任务栏和 Linux 的启动器上（桌面环境提供 Unity 启动器 API 时）；在手机和网页上什么也不做 |
| [`platform/hotkey`](platform/hotkey.md) | `hotkey.Hotkey(Shortcut(key:, primary:, shift:, alt:, control:, mods:)) { description; on press; on error(e) }`：显示期间有效的全局快捷键（RegisterEventHotKey、RegisterHotKey、GlobalShortcuts 门户）；在手机和网页上报 `.notAvailable` |
| [`platform/tray`](platform/tray.md) | `tray.Tray(id) { image, symbol, template, title, tooltip, menu: [Action]; on click }`：显示期间放在托盘里的图标（Mac 的菜单栏、Windows 的通知区域、StatusNotifierItem）；`tray.DockMenu { items }`：程序坞图标的菜单、跳转列表的任务、启动器的快捷列表；`tray.anchor(id)`：浮出 `Window` 的锚点；`async supported()`。在手机和网页上什么也不显示 |
| [`platform/update`](platform/update.md) | 桌面应用的自动更新，Sparkle 式：`update.UpdateWindow { icon:, interval:, automaticChecks: }`（后台检查；安装更新、跳过此版本、稍后提醒、版本说明、下载进度、立即重新启动），`update.AppMenu()` 和 `update.CheckForUpdates()`（给 `MenuBar` 用的“检查更新…”），`checkForUpdates()`；`check()`（返回 `Release` 或 none）、`Release.install(progress:)`、`relaunch()`、`unsupported()`、`updater`（更新器状态，一个 store）、`setChannel`、`setFeed`、`compareVersions`、`onInstalled`，以及用户的选择（`setAutomaticChecks`、`setAutomaticDownloads`、`lastCheck`）；测试用的 `fakeHost`。更新用 `asuka update keygen` 的密钥签名；应用自己下载更新（能用增量包时就用增量包）并安装。在手机上返回 `.notAvailable`；Web 上则重新加载页面（见[发布](shipping.md#自动更新)） |
| [`platform/agerange`](platform/agerange.md) | `agerange.request(13, 18)`：不需要出生日期就能得到用户的年龄段（iOS 26 的 Declared Age Range、Play 的 Age Signals）：一个 `Range`（`status`、`lower`、`upper`、`source`）；其他平台上是 `AgeRangeError.notAvailable` |
| [`platform/appearance`](platform/appearance.md) | `appearance.current()`（会被追踪：`accent` 为 `Color?`、`increaseContrast`、`reduceTransparency`；只有在有东西读取时才监视设置）、`appearance.now()`、`appearance.changes()`（一个流）。强调色来自 Mac、Windows、Linux 的设置门户、Android 的 Material You、CSS 的 AccentColor；iOS 没有。`appearance.setColorScheme(.dark)`：不管系统是浅色还是深色，都让应用使用浅色或深色（`.system`：跟随系统） |
| [`platform/accessibility`](platform/accessibility.md) | `accessibility.announce(text)`（让屏幕阅读器读出文本）、`accessibility.focus(ref)`（把屏幕阅读器的焦点移到该视图）：UIAccessibility、Android 的无障碍事件、NSAccessibility、UI Automation、GTK 的无障碍接口，Web 上是 `aria-live` 区域和焦点 |
| [`platform/biometric`](platform/biometric.md) | `biometric.check()`（`kind`、`enrolled`、`passcode`）、`biometric.authenticate(reason:, cancel:, noPasscode:)`：LocalAuthentication、BiometricPrompt、Windows Hello、WebAuthn 的平台认证器；`BiometricError.canceled`、`.notAvailable`、`.failed` |
| [`platform/device`](platform/device.md) | `device.info()`（`model`、`system`、`systemVersion`、`appVersion`、`build`）、`device.openURL(url)`（用能处理它的应用打开）、`device.share(text)`、`device.shareFiles(paths, mime:, uti:, title:, from:)`（分享面板）、`device.haptic(.success)`、`device.copy(text)`、`device.browse(url)`（在应用内打开：SFSafariViewController、Custom Tab）、`device.pickPhotos(max:)`、`device.pickFiles(types:, multiple:)`（系统的选择器，无需权限；副本放在应用的缓存里）、`device.clipboardText()`、`clipboardHas()`、`clipboardImage()`、`copyImage(path)`、`clipboardChanges()`（一个流）、`device.keepAwake(on)` |
| [`platform/dialog`](platform/dialog.md) | `dialog.open(...)`、`dialog.save(name:, ...)`、`dialog.chooseFolder(...)`、`chooseFolders`：系统的文件对话框（NSOpenPanel 和 NSSavePanel、通用项目对话框、GtkFileDialog、File System Access 选择器），返回用户自己的路径；`dialog.Filter(name:, types:, extensions:)`。手机上，open 是文档选择器。`dialog.fake()`：由测试来回答的对话框 |
| [`platform/login`](platform/login.md) | `login.set(on)`（在 macOS 上，用户允许之前抛出 `LoginError.needsApproval`）、`login.enabled()`、`login.openedAtLogin()`：用户登录时启动应用（SMAppService、Run 注册表项、XDG 自动启动）；手机上的应用做不到 |
| [`platform/network`](platform/network.md) | `network.status()`（会被追踪：`connected`、`type`、`metered`、`constrained`；只有在有人读取时才监听网络）、`network.current()`、`network.changes()`（一个流）。iOS 和 macOS 上用 NWPathMonitor，Android 上用 ConnectivityManager，Windows 上用网络列表管理器，Linux 上用 GNetworkMonitor 和 NetworkManager，网页上用 `navigator.onLine` 和 Network Information API |
| [`platform/host`](platform/host.md) | `host.call(module, method, args)` 和 `host.send`：按名称调用宿主自己的模块，参数采用各模块的小端编码（`u8`、`u32`、`f64`、`str`、`readU32`、`readStr` 等）；`host.next(module, method)` 是模块长轮询给出的内容组成的流；`host.unsupported(e)` 判断宿主是否表示没有这个模块。基于宿主模块的那些包（notify、push、icloud、photos 等）都建立在它之上。供平台 SDK 的包使用；应用要调用自己的平台代码，请用[原生包](native.md) |
| [`platform/recent`](platform/recent.md) | `recent.add(path)`、`recent.clear()`、`recent.list()`：用户最近打开的文档，出现在“最近使用”菜单和程序坞菜单、跳转列表、最近使用的文件中 |
| [`platform/notify`](platform/notify.md) | 本地通知：`request()`、`show(n)`、`schedule(n)`（在 `n.after` 之后显示）、`cancel(id)`、`showing()`、`remove(ids)`、`removeThread(t)`、`removeAll()`、`setBadge(n)`；`setCategories`（操作、回复）、`setChannels`、`setChannelGroups`（Android）；`Notification(…).from(person, conversation)`（消息）；`present(decide)`（应用在前台时通知怎么显示）；`responses()`（点按、操作和回复组成的流；`r.defer()` 让应用继续运行，直到这项工作完成）。各平台用的都是平台自己的通知：UserNotifications、Android 的通知、Windows 的 toast、Linux 的通知、浏览器的通知 |
| [`platform/push`](platform/push.md) | `push.register()`（令牌：APNs 的、FCM 的、Web 的订阅）、`push.opened()` 和 `push.received()`（`Message` 组成的流；`Message.from(person, conversation)`）；`push.rewrite(f)`：在每条推送显示之前修改它（iOS 的 Notification Service Extension、Android 的数据消息） |
| [`platform/background`](platform/background.md) | `background.start(title:, text:, progress:, actions:, onAction:)` 返回一个 `Task`（`update(…)`、`end()`）：在后台继续进行的长时间工作（Android 的前台服务及其通知；iOS 上多给约 30 秒；桌面端不让系统休眠） |
| [`platform/calendar`](platform/calendar.md) | `calendar.request()`（`Access`）、`calendars()`、`events(from:, to:, calendars:)`、`add(Event)`（系统的表单）：EventKit、Calendar Provider |
| [`platform/compose`](platform/compose.md) | `compose.mail(to:, cc:, bcc:, subject:, body:, html:, attachments:)` 和 `compose.message(to:, body:, …)` 显示系统的编写界面，返回一个 `Result`（cancelled、sent、saved、unknown）；`check()` |
| [`platform/contacts`](platform/contacts.md) | `contacts.pick()`（系统的选择器：返回一个 `Contact` 或 none）、`request()`、`list(name:, limit:)`、`add(Contact)` |
| [`platform/library`](platform/library.md) | `library.save(path, collection:, name:, folder:, mime:, move:)`：把文件存进照片、相册、下载或音乐，并返回存到了哪里 |
| [`platform/liveactivity`](platform/liveactivity.md) | `liveactivity.start(Content)` 返回一个 `Activity`（`update(c, alert:)`、`end(c, dismiss:)`）、`events()`（开始、结束、推送令牌）、`enabled()`：iOS 的实时活动、Android 16 的实时更新 |
| [`platform/periodic`](platform/periodic.md) | `periodic.schedule(name, every:, network:, charging:)`、`cancel(name)`、`runs(name)`（`Run` 组成的流，`run.finish(ok)`）：iOS 的后台任务、WorkManager |
| [`platform/photos`](platform/photos.md) | 经授权访问照片图库：`request()`、`current()`、`list(screenshots:, offset:, limit:)`、`thumbnail(id, size:)`、`export(id)`、`manageLimited()` |
| [`platform/widget`](platform/widget.md) | `widget.set(kind, Entry)`、`setAll(kind, entries)`、`clear(kind)`：应用的小组件显示的内容 |
| [`platform/crash`](platform/crash.md) | `crash.reports()`（一个流：先是已保存的报告，之后每来一个就给一个；`Report`：`id`、`kind`、`fatal`、`time`、`message`、`text`、`threads`、带面包屑和值的 `context`）、`breadcrumb(message)`、`set(key, value)`、`record(e)`、`delete(id)`。运行记录在每个平台上都由 Asuka 保存（Asuka 自己的致命错误），再加上各平台报告的情况：未捕获的异常、iOS 和 Mac 的 MetricKit 诊断、Android 进程的退出原因、桌面上的致命信号和未处理的异常、未捕获的 JavaScript 错误 |
| [`platform/icloud`](platform/icloud.md) | iOS 和 macOS 上的 iCloud：`account()` 和 `files(scope)`（会被跟踪：iCloud 改动它们时随之更新）、`Value(key, default)`（保存在 iCloud 键值存储里的 store）、`documents()`、`container(id)`、`readFile`、`writeFile`、`delete`、`rename`、`download`、`evict`、`conflicts(path)`、`resolve(path, keeping:)`。其他平台：没有账户、列表为空、“icloud: not available” |
| [`platform/document`](platform/document.md) | `document.open(path, read:, write:)` 返回一个 `Document<T>` store：`content`、`state`（loading、saved、edited、saving、conflict、failed）、`error`、`file`、`name`；`edit(c)`（稍后自动保存）、`save()`、`saveAnyway()`、`saveAs(path)`、`rename(name)`、`moveTo(dir)`、`revert()`、`unsaved()`、`discard()`；无法原地保存的更改保留在恢复副本中（`recoveryDir()`）。在 iOS 和 macOS 上对文件做协调访问。`Documents(types) { editor: }`：桌面上每个文档一个窗口，带“文件”菜单（新建、打开…、最近使用、关闭、存储、复制、存储为…、重新命名…、移到…、复原），在更改可能丢失时询问，并重新打开上次运行未保存的文档；手机和浏览器上是文档列表（复制、重新命名、删除）；菜单文字有六种语言 |
| [`platform/power`](platform/power.md) | `power.status()`（会被追踪：`battery`、`onBattery`、`level`、`charging`、`lowPower`、`thermal`）、`power.events()`、`suspends()`、`resumes()`、`locks()`、`unlocks()`（流）、`power.idleTime()`：IOKit 和 ProcessInfo、UIDevice、BatteryManager 和 PowerManager、GetSystemPowerStatus 和会话通知、UPower 和 logind、Battery Status API |
| [`platform/preview`](platform/preview.md) | `preview.show(sources, start:, edit:)`：在系统的查看器里显示文件（路径、资源、URL）（iOS 和 Mac 上是快速查看；其他平台上是文件对应的应用；Web 上是新标签页）；`preview.PreviewError.noViewer` |
| [`platform/print`](platform/print.md) | `print.file(source, job:, orientation:)`、`print.html(markup, ...)`：系统的打印对话框，返回 `print.Outcome`；`print.pdf(markup, page:, landscape:, margins:)`：把标记生成 PDF，返回它的路径；`print.Page.a4`、`letter`……、`print.regionPage(locale)`。它的名字和 prelude 的 `print` 相同：`import platform/print as printing` |
| [`platform/review`](platform/review.md) | `review.request()`：商店的评分提示（iOS 和 Mac 上的 StoreKit、Play 的应用内评价），是否显示由商店的配额决定；Windows、Linux 和 Web 上什么也不做。`review.appStoreURL(id)`、`review.playStoreURL(id)` 用于“给我们评分”按钮 |
| [`platform/screen`](platform/screen.md) | `screen.displays()`（会被追踪：`id`、`name`、`bounds`、`workArea`、`scale`、`refreshRate`、`primary`）、`screen.primary`、`nearest`、`matching`；`screen.lock(orientation)`、`screen.setBars(style:, hideStatus:, hideNavigation:)`、`screen.preventCapture(on)`、`screen.screenshots()`（一个流） |
| [`platform/secure`](platform/secure.md) | `secure.read(key)`、`secure.write(key, value)`、`secure.remove(key)`：由平台加密的少量机密（钥匙串、Android Keystore、凭据管理器、通过 libsecret 访问的用户密钥环、用不可导出的 WebCrypto 密钥加密的 IndexedDB） |
| [`platform/sharesheet`](platform/sharesheet.md) | 应用自己的分享面板，显示在发起分享的应用上方（iOS 的分享扩展、Android 的一个 Activity）：`sharesheet.active()`（应用作为面板运行：由 app 块显示它）、`done()`、`cancel()`、`openApp()`；分享的内容交给 `onShare` |
| [`platform/shell`](platform/shell.md) | `shell.reveal(path)`、`shell.open(path)`、`shell.trash(path)`、`shell.beep()`：文件管理器提供的功能（NSWorkspace、资源管理器和回收站、GIO 和 FileManager1）；`shell.ShellError.noApp` |
| [`platform/splash`](platform/splash.md) | `splash.hold()`、`splash.hide()`：在应用准备好之前保持 asuka.toml 的启动画面（iOS、Android；其他平台什么也不做） |
| [`platform/tracking`](platform/tracking.md) | `tracking.request()`、`tracking.current()`：iOS 和 Mac 上 App Tracking Transparency 的提示（`.notAsked`、`.restricted`、`.denied`、`.allowed`）；Windows 的广告 ID 设置；浏览器的 Global Privacy Control 或 Do Not Track；Android 和 Linux 没有提示，返回 `.allowed` |
| [`platform/webauth`](platform/webauth.md) | `webauth.open(url, callbackScheme:, ephemeral:)`：在系统的界面里登录（ASWebAuthenticationSession、Custom Tab；WebView2 和 WebKitGTK 窗口；弹出窗口），返回回调 URL；`WebAuthError.canceled`；取消任务会关闭它 |
| [`platform/webview`](platform/webview.md) | `webview.cookies(url)`：应用的 Web 视图（`WebView`）为某个 URL 保存的 cookie，形式是一个 `Cookie` 请求头，供应用自己的请求使用（Web 上：站点自己的 cookie，即 `document.cookie`） |
| [`platform/location`](platform/location.md) | `location.authorization()`（可追踪）、`request()`、`requestAlways()`、`requestPrecise(purpose)`、`current()`、`positions()`（流）、`placeName(lat, lon)`、`openSettings()`、`supported()`；后台位置更新（`startUpdates(…)`、`updates()`、`stopUpdates()`、`updating()`）和区域监控（`monitor(region)`、`crossings()`、`stopMonitoring(id)`、`monitored()`），这些事件会一直保留到被读取，应用处理期间还能保持运行（`hold()`）。基于 Core Location、Android 的 LocationManager、WinRT 的 Geolocator、位置门户或 GeoClue，以及 Geolocation API |
| [`platform/sensors`](platform/sensors.md) | `sensors.available(sensor)`；`accelerometer(every:)`、`gyroscope`、`magnetometer`、`motion`（姿态、重力、用户加速度）、`pressure`、`light`、`stepUpdates()`（都是流）；`steps(from:, to:)`；`simulate(json)`。基于 Core Motion、SensorManager、Generic Sensor API 和 iio-sensor-proxy |
| [`platform/purchase`](platform/purchase.md) | 应用内购买：`declare(items)`、`products(items)`、`buy(item, options)`、`Transaction.finish()`、`events()`（从应用启动起的流）、`owned()`（可追踪：`has(id)`）、`entitlements()`、`restore()`、`statuses(group)`、`manageSubscriptions(id)`、`redeemCode()`、`requestRefund(t)`、`storefront()`；`Product` 的 `amount` 是 `Decimal`。基于 StoreKit 2、Play Billing、Microsoft Store 和 Digital Goods API |
| [`platform/pay`](platform/pay.md) | Apple Pay 与 Google Pay：`available(request)`、`pay(request)`（配送联系人、配送方式、卡片、优惠码和授权都有异步处理函数，返回 `Update`，或 `throw Invalid(field:, message:)`）、`setUp()`，以及钱包自己的 `PayButton`；金额是 `std/decimal` 的 `Decimal` |
| [`platform/appleauth`](platform/appleauth.md) | 通过 Apple 登录：`signIn(scopes:, nonce:, state:)`、`credentialState(user)`、`available()`，以及 Apple 的 `SignInButton`。仅限 iOS 和 macOS |
| [`platform/nfc`](platform/nfc.md) | NFC 标签：`state()`（可追踪）、`start(alert:, poll:)` 得到 `Session`（`next()`、`setAlert`、`fail`、`stop`）、`Tag`（`message`、`ndef`、`write`、`makeReadOnly`、`transceive`）、`opened()`，以及 NDEF 的 `Message` 和 `Record`（`encode()`、`decode(bytes)`）。基于 Core NFC、Android 的读卡器模式和 Web NFC |
| [`platform/bluetooth`](platform/bluetooth.md) | 低功耗蓝牙：`state()`（可追踪）、`request()`、`scan()`（流）、`choose(filters)`、`connect(device)`（返回 `Connection` store：`state`、`services()`、`characteristic(s, c)`），特征值的 `read`、`write`、`notifications()`；`serve(services)`、`advertise(…)`；`backgroundScan(services)`、`restored()`（iOS 的状态恢复、Android 的后台扫描）、`stopBackgroundScan()`；以及蓝牙 SIG 定义的 UUID。基于 Core Bluetooth、Android 的 BLE、WinRT、BlueZ 和 Web Bluetooth |
| [`platform/audio`](platform/audio.md) | `audio.play(source)`，以及 `audio.Player(source)`：一个 store，有 `play`、`pause`、`seek`、`setVolume`、`setRate`、`setLoop`、`status()` 和 `release()`。基于 AVPlayer、Media3 的 ExoPlayer、`<audio>`、MCI、GStreamer（[设备](device.md)） |
| [`platform/nowplaying`](platform/nowplaying.md) | `nowplaying.NowPlaying`：应用正在播放的内容，显示在锁屏和系统的媒体控件上，以及它们发回的命令（媒体键、耳机的按键）；给它一个 `audio.Player` 时会跟随它（[设备](device.md)） |
| [`platform/speech`](platform/speech.md) | `speech.speak(text, options:)`、`stop`、`pause`、`resume`、`speaking`、`voices()`：用系统的语音朗读文本（[设备](device.md)） |
| [`platform/record`](platform/record.md) | `record.audio(maxDuration:, quality:, background:)` 以及 `stopAudio`、`pauseAudio`、`resumeAudio`、`audioStatus()`：录制麦克风的声音；`record.start()` 和 `stop()`：录制屏幕上 `camera.View` 的视频（[设备](device.md)） |
| [`platform/camera`](platform/camera.md) | `camera.View`（`facing`、`mode`、`scan`、`codes`；`code`、`error`）：实时的相机画面；`camera.capture()`、`camera.take()`（系统的相机界面）、`camera.record()`（[设备](device.md)） |
| [`platform/scan`](platform/scan.md) | `scan.file(path, kinds:)`：识别图片里的条形码和二维码（[设备](device.md)） |
| [`platform/vision`](platform/vision.md) | `vision.analyze(path, text:, faces:, codes:, languages:)`：在设备上识别图片里的文字（逐行、逐词）、人脸和码，以及它们各自的位置（[设备](device.md)） |
| [`platform/transcribe`](platform/transcribe.md) | `transcribe.check(language)`、`transcribe.file(path, language:)`：把录音转成文字，只在设备上进行（做不到的地方返回 `notAvailable`：Web、Linux、较旧的 Android）（[设备](device.md)） |
| [`platform/imageedit`](platform/imageedit.md) | `imageedit.edit(source, actions, format:, quality:)`（缩放、裁剪、旋转、翻转、填充、像素化）、`imageedit.size`、`imageedit.videoFrame`：处理图片和视频帧，输出 JPEG 或 PNG（[设备](device.md)） |
| [`platform/maps`](platform/maps.md) | `maps.View`（`bind region`、`markers`、`mapType`、`showsUser`、`interactive`；`markerPress`、`press`、`regionChange`）：Apple 地图、Google 地图、OpenStreetMap（[设备](device.md)） |

如果某个平台没有原生包的代码（比如由 `asuka test` 运行的应用，它没有任何平台代码），用到它的地方会抛出 `unsupported` 错误；这时 `std/store` 什么也不保存，读取时返回默认值。

`asuka test` 让每次运行都可重复：`time.now()` 从 2026-01-01T00:00:00Z 开始，只随测试的时间（`sleep`）前进，本地时区是 UTC，每个测试都有一个属于自己的空目录作为 `fs.appDir()`。Web 上应用还没有自己的目录，所以 `std/fs` 的调用在那里会抛出错误。

设备相关的包（`location`、`sensors`、`purchase`、`pay`、`appleauth`、`nfc`、`bluetooth`）都为测试提供了替身：`location.fake()`、`purchase.fake(products)` 等会把一个由测试驱动的 Asuka 实现换给这个包，代替平台的实现（移动设备位置、批准一笔购买、轻触一个标签、添加一个外设），参见[测试](testing.md#平台服务与原生包)。

## 标准库之外的包

标准库和平台 SDK 都没有的功能，可以来自一个[包](packages.md)（`asuka add`）；如果是还没有人封装过的平台 API，就写一个你自己的原生包（[原生代码](native.md)）。
