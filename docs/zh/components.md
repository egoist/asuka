# 组件

组件是一个显示视图的声明（见[语言](language-tour.md)）。本页按用途分类介绍 Asuka 提供的元素，以及标准库里用于 Markdown、SVG、Lottie、图表和着色器的几个包。只要平台自己有对应的控件，用的就是平台原生控件：iOS 上是 UIKit，Android 上是 Android Views 和 Material 3，macOS 上是 AppKit，Windows 上是绘制成 Windows 11（Fluent）风格的 Win32 控件，Linux 上是 GTK 4 和 libadwaita，浏览器里则是页面元素。视图如何确定尺寸和位置，请看[布局](layout.md)；页面、标签页和弹出面板（sheet）请看[导航](navigation.md)。

每个属性都是一个绑定：可以是一个值，也可以是读取状态的表达式，状态变化时视图随之更新。由用户修改的控件（输入框、开关）则用 `bind` 绑定它要写入的状态，比如 `bind text: query`。详见[核心概念](concepts.md)。

## 各平台的原生控件

| 组件 | iOS | Android | macOS | Windows | Linux | Web |
|---|---|---|---|---|---|---|
| `Text` | `UILabel` | `TextView` | `NSTextField`（作为标签） | 由 Asuka 绘制 | `GtkLabel` | 页面文本 |
| `Button` | `UIButton` | `MaterialButton` | `NSButton` | `BUTTON`，绘制成 Fluent 风格 | `GtkButton` | `<button>` |
| `TextField` | `UITextField`；多行时为 `UITextView` | `EditText` | `NSTextField`、`NSSecureTextField`；多行时为 `NSTextView` | `EDIT`，外框绘制成 Fluent 风格 | `GtkEntry`、`GtkPasswordEntry`；多行时为 `GtkTextView` | `<input>`；多行时为 `<textarea>` |
| `Toggle` | `UISwitch` | `MaterialSwitch` | `NSSwitch` | 绘制成拨动开关的 `BUTTON` 复选框 | `GtkSwitch` | `<input type="checkbox" switch>` |
| `Slider` | `UISlider` | Material `Slider` | `NSSlider` | trackbar 控件 | `GtkScale` | `<input type="range">` |
| `Segmented` | `UISegmentedControl` | Material 分段按钮 | `NSSegmentedControl` | 自行绘制，仿照 Windows Community Toolkit 的样式 | 相连的一组 `GtkToggleButton` | 单选组中的一组按钮 |
| `Stepper` | `UIStepper` | 两个 Material 图标按钮 | `NSStepper` | 两个按钮 | 相连的 − 和 + 按钮 | 两个按钮 |
| `Wheel` | `UIPickerView` | `NumberPicker` | `NSPopUpButton` | 下拉式 `COMBOBOX` | `GtkDropDown` | `<select>` |
| `ColorPicker` | `UIColorWell` | 色块加 Material 对话框 | `NSColorWell` 和颜色面板 | 色块加颜色对话框（`ChooseColor`） | `GtkColorDialogButton` | `<input type="color">` |
| `DatePicker` | `UIDatePicker` | 打开 Material 选择器的按钮 | `NSDatePicker` | 日期时间选择器（`SysDateTimePick32`） | 一个按钮，打开 `GtkCalendar` 和微调按钮 | `<input type="date">`、`time`、`datetime-local` |
| `Progress` | `UIProgressView` | `LinearProgressIndicator` | `NSProgressIndicator` | 绘制成 Fluent 风格 | `GtkProgressBar` | `<progress>` |
| `ProgressRing` | 仿照 SwiftUI 的圆形仪表绘制；没有值时是 `UIActivityIndicatorView` | `CircularProgressIndicator` | 16 和 32 点时是旋转样式的 `NSProgressIndicator`；其他尺寸或设置了 `color` 时，按它的样式自行绘制 | 自行绘制的 Fluent `ProgressRing` | 按 GNOME 的样式绘制；没有值时是 `AdwSpinner` | SVG 圆环 |
| `Activity` | `UIActivityIndicatorView` | `CircularProgressIndicator` | 旋转样式的 `NSProgressIndicator` | 自行绘制的 Fluent 进度环 | `AdwSpinner`（libadwaita 1.6 之前为 `GtkSpinner`） | 自行绘制的加载指示器 |
| `Image` | `UIImageView` | `ImageView` | `NSImageView` | 用 GDI+ 绘制 | 用 GDK 纹理绘制 | `<img>` |
| `lottie.View` | lottie-ios | lottie-android | lottie-ios | 暂不支持（一个空的框） | 暂不支持（一个空的框） | lottie-web |
| `Symbol` | SF Symbols | Material Symbols | SF Symbols | Segoe Fluent Icons | 图标主题中的图标，没有则用 Material Symbols | Material Symbols |
| `Video` | `AVPlayerViewController` | Media3 的播放器 | `AVPlayerView` | WebView2 中的 `<video>` | `GtkVideo`（GStreamer） | `<video>` |
| `WebView` | `WKWebView` | `WebView` | `WKWebView` | WebView2 | `WebKitWebView`（WebKitGTK 6.0） | `<iframe>` |
| `Scroll` | `UIScrollView` | `NestedScrollView` | `NSScrollView` | Asuka 自己的视图，配 Fluent 的滚动指示器 | `GtkScrolledWindow` | 可滚动的元素 |
| `List` | `UICollectionView` | `RecyclerView` | 由 Asuka 复用的行，放在 `NSScrollView` 中 | 由 Asuka 复用的行 | 由 Asuka 复用的行，放在 `GtkScrolledWindow` 中 | 由 Asuka 复用的行 |

下面各节会说明平台之间行为不一致的地方。

## 文本

### Text

`Text` 显示一行或一段文本。它就是平台的标签控件，默认会自动换行；`lines` 可以限制最多显示几行，超出的部分以省略号截断。

```asuka
component Inbox {
  state count = 3

  Column {
    gap: 8
    Text("Inbox") { font: .title2.bold() }
    Text("{count} unread") { color: .secondaryLabel }
    Text("A long description that stops after two lines.") { lines: 2 }
  }
}
```

样式中的 `font`、`color`、`textAlign` 和 `lines` 作用于文本本身。文本样式（`.body`、`.headline`、`.footnote` 等）会跟随用户设置的字号。

### RichText

`RichText` 显示带样式的文本，内容是一组 `Span`（文本片段）。片段中的 `"\n"` 会另起一段。片段里保持默认值的字段，会沿用 `RichText` 的样式。

```asuka
import platform/device

component About {
  RichText([
    Span(text: "asuka", font: .body.bold()),
    Span(text: " draws "),
    Span(text: "native", italic: true),
    Span(text: " views. Run "),
    Span(text: "asuka dev", mono: true),
    Span(text: ", or read "),
    Span(text: "the docs", link: "https://example.com/docs"),
    Span(text: ".\nA second paragraph, "),
    Span(text: "in red", color: .red),
    Span(text: "."),
  ]) {
    on link(url) { device.openURL(url) }
  }
}
```

- `Span` 包含它的 `text`、`font`、`italic`、`mono`、`underline`、`strike`、`color`、`background`（在片段覆盖的每一行上绘制，带圆角）和 `link`。
- 点击链接时会运行 `on link(url)`。没有设置时，点击链接不会有任何反应（在路由器里，指向应用自己 href 的链接会前往那里）。
- 设置 `role: .header` 后，屏幕阅读器会把它当作标题。

在 iOS 和 Android 上，每个段落都由平台自己的排版引擎（CoreText、StaticLayout）排版，并按内容和宽度缓存。因此往长文本末尾追加内容时（比如一条流式输出的回复），只有发生变化的那个段落会重新排版。在 macOS 上它是只读的 `NSTextView`（TextKit），在 Windows 上是只读的 rich edit 控件（`RICHEDIT50W`），在 Linux 上是用 Pango 属性设置样式的 `GtkLabel`，在浏览器中则是段落里的一组 span。[`examples/richtext`](../../examples/richtext) 演示了流式显示的效果。

如果只是纯文本，请用 `Text`：它就是平台的标签控件，行为完全是原生的。

**内嵌视图。** 片段的 `inline` 可以把一个组件放进文本中：组件坐落在基线上，随文字一起换行。文本更新时，只要 key 不变，它就还是同一个组件，状态也会保留下来。

```asuka
component Status {
  RichText([
    Span(text: "Status: "),
    Span(inline: Inline(key: "badge", build: () => Live())),
    Span(text: " since this morning."),
  ])
}

component Live {
  View {
    background: .green
    radius: 6
    padding: [2, 6]
    Text("live") {
      font: .caption
      color: #ffffff
    }
  }
}
```

### 可选择的文本

`Selectable` 让其中的 `RichText` 视图都可以被选择，而且是把它们当作一整段文本来选：长按选中一个词，拖动平台的选择手柄扩大选区，再用平台的编辑菜单复制。用鼠标时，拖动即可选择，用平台的复制快捷键复制。Linux 和 Web 上同样可以跨多个文本整体选择；macOS 和 Windows 上则是每个文本各选各的。

```asuka
component Answer(question: [Span], answer: [Span]) {
  Selectable {
    gap: 12
    RichText(question) { role: .header }
    RichText(answer)
  }
}
```

设置了 `selectable: true` 的 `Text` 本身就可以被选择；要跨多个文本选择时，请把 `RichText` 放进 `Selectable`。如果不经过选区直接复制内容，可以用 `device.copy(text)` 把它放到剪贴板。

### Markdown

`std/markdown` 把 Markdown（CommonMark，加上 GitHub 的表格、任务列表、删除线和自动链接）显示成平台原生的带样式文本：标题、强调、代码、引用、列表、表格、图片和链接，字号跟随用户的文字大小设置。文本里的原始 HTML 会按文字显示。

```asuka
import std/markdown

component Reply(text: string, writing: bool) {
  markdown.View(text) { streaming: writing; padding: 16 }
}
```

`streaming` 表示文本还在写，比如语言模型的回复正一点点流进来：这时文本的末尾会显示成它最可能的样子（代码围栏补上结尾、`**bold` 显示为粗体、正在写的链接显示为链接），而不是写了一半的语法。单独用 `markdown.heal(text)` 也能得到同样的处理。

它的外观和链接都由应用决定：

- `theme` 是一个 `markdown.Theme`：正文的字体和颜色，标题、链接、代码（及其背景）、引用和分隔线的样式，以及块与块之间的间距和每级缩进。没有设置的部分沿用平台的样式。
- `render` 用应用自己的方式绘制某个块：它接收一个 `markdown.Block`（包括 `kind`、纯文本 `text`、带样式的 `spans`、标题的 `level`、代码块的 `language`、图片的 `url`、表格的 `rows`），返回一个视图；返回 `none` 则使用 Markdown 自己的绘制。比如给代码块加上语法高亮、根据代码画出图表，或者用应用自己的图片视图。
- `on link(url)` 接收被点击的链接，目标地址保持原样（相对地址也一样），由应用决定打开什么。没有它时，网页、邮件和电话链接会用对应的应用打开，其他链接不做任何事。

```asuka
import std/markdown

component Code(text: string, language: string) {
  Column {
    background: .secondaryBackground; radius: 8; padding: 12
    Text(language) { font: .caption; color: .secondaryLabel }
    Text(text) { font: Font.callout.family("Menlo") }
  }
}

component Answer(text: string) {
  state opened = ""

  markdown.View(text) {
    theme: markdown.Theme(font: .callout, linkColor: .purple, spacing: 8)
    render: b => if b.kind == .code { Code(b.text, language: b.language) } else { none }
    on link(url) { opened = url }
  }
}
```

Markdown 由 Asuka 解析，所有平台都一样；每个块都用这门语言自己的视图绘制：每个段落是 [`Selectable`](#可选择的文本) 里的一个 [`RichText`](#richtext)，表格是网格，图片是 `Image`。文本每次变化都会重新解析，但块按位置保留：回复流式写入时，只有最后一两个块会重新绘制，前面的内容保持原有的布局和选区。参见 [`examples/markdown`](../../examples/markdown)。

## 按钮

`Button` 就是平台的按钮：在 iOS 上是使用配置（configuration）的 `UIButton`，在 Android 上是 Material 按钮，在 macOS 上是 `NSButton`，在 Windows 上是绘制成 Fluent 风格的 `BUTTON`，在 Linux 上是 `GtkButton`，在浏览器中是 `<button>`。

```asuka
component Actions {
  state saving = false

  Row {
    gap: 12
    wrap: true
    Button("Save") {
      variant: .filled
      disabled: saving
      on press { saving = true }
    }
    Button("Compose") {
      icon: "square.and.pencil"
      variant: .tinted
    }
    Button("") {
      icon: "trash"
      label: "Delete"
      variant: .gray
    }
  }
}
```

- 标题是按钮的文字；`icon` 是显示在文字前面的符号（见[符号](#符号与图标)）。有图标而标题为空的按钮只显示图标。
- `label` 是 VoiceOver 和 TalkBack 朗读的内容，默认就是标题。只有图标的按钮记得设置它。
- `tooltip` 是指针停在按钮上时显示的简短提示，适用于 iPad 的指针（`UIToolTipInteraction`）、Android 上的鼠标（长按也会显示，即 `tooltipText`），以及 macOS、Windows、Linux 和 Web（元素的 `title`）。只有图标的按钮应当设置一个，内容就是按钮的名称。任何视图上的 `tooltip` 对任意视图起同样的作用。在 iOS 上，只用触摸屏时提示永远不会出现，所以不能把它作为传达某个信息的唯一途径。
- `disabled` 按平台的方式禁用按钮。
- `font` 和 `color` 修改按钮的文字和色调。
- `href` 让按钮按下时前往某个路由（见[路由](navigation.md#路由)）。

`variant` 决定按钮的外观：

| Variant | iOS | Android | macOS | Windows | Linux | Web |
| --- | --- | --- | --- | --- | --- | --- |
| `.plain`（默认） | 普通 | 文本按钮 | 无边框，使用强调色 | subtle 样式，使用强调色 | 扁平，使用强调色 | 普通 |
| `.filled` | 填充 | 填充按钮 | 强调色的 push 按钮 | accent 按钮 | 建议操作（suggested action）样式 | 填充 |
| `.tinted` | 着色 | 色调按钮 | push 按钮，标题使用强调色 | 一层淡淡的强调色 | 一层淡淡的强调色 | 着色 |
| `.gray` | 灰色 | 色调按钮 | push 按钮 | 标准按钮 | 标准按钮 | 灰色 |
| `.bordered` | 带边框 | 描边按钮 | push 按钮 | 标准按钮 | 标准按钮 | 描边 |
| `.glass` | Liquid Glass（`glass()`，iOS 26；之前为灰色） | 色调按钮 | 玻璃边框，胶囊形，仅有图标时为圆形（macOS 26；之前为普通按钮） | 同 `.gray` | 标准按钮 | 同 `.gray` |
| `.glassProminent` | 着色的 Liquid Glass（`prominentGlass()`，iOS 26；之前为填充） | 填充按钮 | 以强调色着色的玻璃边框，作为主要操作（macOS 26；之前同 `.filled`） | 同 `.filled` | 建议操作样式 | 同 `.filled` |

玻璃按钮是胶囊形的；只有图标的玻璃按钮则是圆形，可以用作弹出面板的关闭按钮，和 iOS 26 系统自带的一样。

### 菜单

设置了 `menu` 的按钮被点击时会弹出其中的操作，而不是运行 `on press`（iOS 上是 `UIMenu`，Android 上是 `PopupMenu`，macOS 上是 `NSMenu`，Windows 上是弹出菜单，Linux 上是 `GtkPopoverMenu`，浏览器里是一个 popover）。每一项都是一个 `Action`：`.button(title:, icon:, role:, disabled:, checked:, run:)`，除标题外的字段都可省略。`role: .destructive` 会让它显示为红色。`run` 里可以 await（`run: () => { await save() }`）：它作为创建这个操作的代码的 task 运行，菜单不会等它，和事件处理函数一样；那段代码所在的栈、弹出面板和路由器就是它的，所以在里面可以用 `push` 和 `navigate`。`runAction(a)` 以同样的方式运行一个操作，供你自己做的菜单使用。

```asuka
import platform/device

component More {
  Button("") {
    icon: "ellipsis.circle"
    label: "More"
    menu: [
      .button(title: "Copy", icon: "doc.on.doc", run: () => device.copy("https://example.com")),
      .button(title: "Share", icon: "square.and.arrow.up", run: () => device.share("https://example.com")),
      .button(title: "Delete", icon: "trash", role: .destructive, run: () => {}),
    ]
  }
}
```

操作可以设为 `disabled`（变暗，不能选）或 `checked`（旁边打一个对勾），菜单打开期间两者都会跟随它们读取的状态变化。`.submenu(title:, icon:, items:)` 是一个子菜单。`.section(title:, items:)` 则是菜单里的一个分区，上下用分隔线隔开，顶部显示标题（相当于 SwiftUI 在 `Menu` 里放 `Section`）。包含已勾选项的分区就是一组单选：

```asuka
component Sorting {
  state sortBy = "Name"
  state locked = false

  fn sorted(by: string) -> Action = .button(title: by, checked: sortBy == by, run: () => { sortBy = by })

  Button("") {
    icon: "ellipsis.circle"
    label: "More"
    menu: [
      .section(title: "Sort by", items: [sorted("Name"), sorted("Date")]),
      .submenu(title: "Move to", icon: "folder", items: [
        .button(title: "Inbox", run: () => {}),
        .button(title: "Archive", run: () => {}),
      ]),
      .button(title: "Delete", role: .destructive, disabled: locked, run: () => {}),
    ]
  }
}
```

| | iOS | Android | macOS | Windows | Linux | Web |
|---|---|---|---|---|---|---|
| `disabled` | `.disabled` | 禁用的菜单项 | 禁用的菜单项 | 灰显的菜单项 | 禁用的 action | `disabled`、`aria-disabled` |
| `checked` | `state = .on` | 勾选的菜单项；分区里的单选显示为单选按钮 | `state = on` | `MF_CHECKED` | 带状态的 action（打勾） | `menuitemcheckbox` |
| `.submenu` | 嵌套的 `UIMenu` | `SubMenu` | 子菜单 | 子菜单 | GMenu 子菜单 | 子菜单（按指向它的方向键即可展开） |
| `.section` | `displayInline` 的 `UIMenu` | 用分隔线隔开的分组，标题是一个禁用的菜单项 | 分隔线加分区标题（macOS 14） | 分隔线加自绘的标题 | 带标签的 GMenu 分区 | 夹在分隔线之间、带标题的 `group` |

页面导航栏上的按钮（它的 `toolbar` 里的按钮）也一样：禁用的按钮会变暗，带 `menu` 的按钮点击后会展开菜单（相当于 SwiftUI 在工具栏里放一个 `Menu`）。警告框的按钮同样可以禁用，并在显示期间跟随 `disabled` 变化（比如一个要等用户填好名字才能点的 OK）。滑动操作两者都不显示。

`Menu` 可以给任意内容加上菜单。设置 `longPress: true` 后，它就是长按弹出的上下文菜单；用鼠标时（macOS、Windows、Linux、Web），则在右键点击时弹出：

```asuka
component Message {
  Menu {
    longPress: true
    actions: [
      .button(title: "Reply", icon: "message"),
      .button(title: "Delete", role: .destructive),
    ]
    View {
      padding: 16
      background: .secondaryBackground
      radius: 12
      Text("Long-press for a menu")
    }
  }
}
```

## 文本输入框

`TextField` 是文本输入框。`bind text` 是它的文本：用户输入时会写入这个状态，而在代码里写入状态，则会替换输入框中的文本。

```asuka
component SignIn {
  state email = ""
  state password = ""

  let pw = Ref()

  Column {
    gap: 12
    TextField {
      bind text: email
      placeholder: "Email"
      keyboard: .email
      contentType: "email"
      autocapitalize: .off
      autocorrect: .no
      returnKey: .next
      on submit { pw.focus() }
    }
    TextField {
      ref: pw
      bind text: password
      placeholder: "Password"
      secure: true
      contentType: "password"
      returnKey: .go
      on submit { pw.blur() }
    }
  }
}
```

- `secure` 隐藏输入的内容，用于密码。
- `keyboard`：`.default`、`.number`、`.decimal`、`.email`、`.url`、`.phone`。
- `returnKey` 是回车键上显示的文字：`.default`、`.done`、`.go`、`.next`、`.search`、`.send`。按下回车键会运行 `on submit`。
- `contentType` 说明输入框里填的是什么内容，供系统的自动填充使用：`"username"`、`"password"`、`"newPassword"`（平台会推荐一个强密码）、`"oneTimeCode"`（刚收到的短信验证码会出现在键盘上方，即 UIKit 的 `oneTimeCode` 和 Android 的短信验证码提示）、`"email"`、`"phone"`、`"name"`、`"address"`、`"postalCode"`、`"cardNumber"` 等。各平台会按自己的叫法来设置。
- `autocapitalize`：`.sentences`（默认）、`.off`、`.words`、`.characters`。`autocorrect`：`.default`、`.yes`、`.no`。
- `on focus` 和 `on blur` 跟随键盘的弹出和收起，`bind focused` 既跟随键盘也能移动它。`Ref` 的 `focus()` 唤起键盘，`blur()` 收起键盘。

桌面端没有可供切换的屏幕键盘，所以 `keyboard` 和 `returnKey` 在那里基本看不出效果，不过按回车仍然会运行 `on submit`。在 macOS 上，`contentType` 对应输入框的自动填充内容类型。在 Windows 上，`.number` 只允许输入数字。在 Linux 上，`keyboard`、`contentType`、`autocapitalize` 和 `autocorrect` 会告诉输入法这个输入框接受什么内容（即 GTK 的 input purpose 和 input hints）。在浏览器中，它们分别对应输入元素的 `inputmode`、`enterkeyhint`、`autocomplete`、`autocapitalize` 和 `spellcheck`，手机键盘会据此调整。

**多行文本。** `multiline: true` 把输入框变成文本区域，适合输入消息或评论：回车会换行，而不是运行 `on submit`。它会随文本增高，最多到 `lines` 行（0 表示不限），超出后开始滚动。

```asuka
component Composer {
  state message = ""

  Row {
    gap: 8
    align: .end
    TextField {
      bind text: message
      placeholder: "Message"
      multiline: true
      grow: 1
      lines: 5
    }
    Button("") {
      icon: "paperplane"
      label: "Send"
      variant: .filled
      disabled: message.isEmpty()
      on press { message = "" }
    }
  }
}
```

**只保留文字。** `plain: true` 去掉输入框的边框、背景和内边距，只留下文字和占位符，适合放进卡片或你自己设计的表单里，就像备忘录和提醒事项中的输入框那样。

```asuka
component NoteField {
  state note = ""

  TextField {
    bind text: note
    placeholder: "Note"
    multiline: true
    plain: true
  }
}
```

`SafeArea` 会为键盘留出内边距，所以放在它底部的输入框始终位于键盘上方。

## 控件

每个控件都用 `bind` 绑定它要修改的状态，也都接受 `disabled`。除了 `Picker` 和 `Toggle` 的 `title`，它们都不带标签：需要的话，在旁边放一个 `Text`。

```asuka
import std/time

component Settings {
  state wifi = true
  state volume = 0.5
  state period = 0
  state cups = 2.0
  state size = 1
  state milk = -1
  state tint: Color = .blue
  state when = time.now()

  Column {
    gap: 16
    Row {
      align: .center
      Text("Wi-Fi") { grow: 1 }
      Toggle { bind on: wifi }
    }
    Slider { bind value: volume }
    Segmented(["Day", "Week", "Month"]) { bind selected: period }
    Row {
      gap: 12
      align: .center
      Text("{cups.fixed(0)} cups") { grow: 1 }
      Stepper {
        bind value: cups
        min: 1
        max: 8
      }
    }
    Wheel(["Small", "Medium", "Large"]) { bind selected: size }
    Picker(["Whole", "Oat", "None"]) {
      bind selected: milk
      prompt: "Milk"
    }
    Row {
      gap: 12
      align: .center
      Text("Tint") { grow: 1 }
      ColorPicker {
        bind value: tint
        alpha: true
        title: "Tint"
      }
    }
    Row {
      gap: 12
      align: .center
      Text("Starts") { grow: 1 }
      time.Picker {
        bind value: when
        mode: .dateTime
        min: time.now()
      }
    }
  }
}
```

- **Toggle** 是开关，`bind on`。设置 `checkbox: true` 后它会变成复选框：Android 上是 `MaterialCheckBox`，Mac 和 Windows 上是各自的复选框，Linux 上是 `GtkCheckButton`，浏览器中是复选框；iOS 没有复选框，于是用一个选中后填上对勾的圆圈，和提醒事项里的一样。
- **Slider** 在 `min` 到 `max` 之间取一个 `float`（默认是 0 到 1）。
- **Segmented** 把几个选项并排摆出来，从中选一个，`selected` 是选中项的索引。它对应 UIKit 的分段控件和 Material 的分段按钮。Windows 没有这种控件，由 Asuka 自行绘制一个，仿照 Windows Community Toolkit 的 `Segmented`。
- **Stepper** 用减号和加号按 `step`（默认为 1）增减一个 `float`，范围从 `min` 到 `max`（默认是 0 到 100）。它在 iOS 上是 `UIStepper`，在 macOS 上是 `NSStepper`；在其他平台上则是两个按钮（Android 上是 Material 图标按钮，Linux 上是相连的按钮，和 GNOME 的微调按钮外观一致）。
- **Wheel** 在滚轮上从众多选项中选一个，对应 `UIPickerView` 和 Android 的 `NumberPicker`。桌面端和 Web 没有滚轮控件，所以在那里它是一个列出所有选项的弹出菜单（`NSPopUpButton`、绘制成 WinUI `ComboBox` 样式的下拉组合框、`GtkDropDown`、`<select>`）。
- **Picker** 从菜单中的众多选项里选一个，并显示选中的那一项（相当于菜单样式的 SwiftUI `Picker`，以及 Material 的 exposed dropdown menu）。`selected` 超出范围（-1，默认值）表示什么都没选，这时选择器显示它的 `prompt`；屏幕阅读器也用它来称呼选择器。在 Android 上，`prompt` 是输入框的标签，浮在选中项上方。不绑定时，选择器自己记住用户的选择。

  | | Picker |
  |---|---|
  | iOS | 一个普通的 `UIButton`，使用强调色并带弹出指示符，菜单里给选中项打勾（`changesSelectionAsPrimaryAction`） |
  | Android | 一个 exposed dropdown menu 样式的描边 `TextInputLayout`，里面是不能输入的 `MaterialAutoCompleteTextView`；TalkBack 会把它读作下拉列表 |
  | macOS | `NSPopUpButton`（标签是一个隐藏的菜单项，只在什么都没选时显示） |
  | Windows | 绘制成 WinUI 3 `ComboBox` 样式的下拉列表组合框（控件的填充色和描边，末端的 V 形箭头；列表在浮出面板上自绘，选中项开头有一条强调色的小竖条，在 Windows 11 上由 DWM 圆角处理）；什么都没选时，用次要文字颜色绘制标签，标签也是它在 UI Automation 中的名称 |
  | Linux | 基于 `GtkStringList` 的 `GtkDropDown`，标签以暗色文字显示在选中项的位置 |
  | Web | `<select>`，第一个选项是隐藏的占位项，显示标签 |
- **ColorPicker** 是一个色块，点击后打开平台的颜色选择器，选中的结果写入一个 `color`。`alpha` 允许用户选择不透明度，`title` 是选择器的标题。iOS 上是 `UIColorWell` 加系统的颜色选择器；Android 没有颜色选择器，所以是一个列出调色板的 Material 对话框，设置了 `alpha` 时再附带一个不透明度滑块。macOS 上是打开颜色面板的 `NSColorWell`，Linux 上是 `GtkColorDialogButton`，浏览器中是 `<input type="color">`。Windows 的颜色对话框不支持不透明度，因此 `alpha` 在那里不起作用。
- **DatePicker** 选取一个时刻，按设备所在的时区显示：`std/time` 的 `time.Picker` 绑定一个 `time.Time`（prelude 里的 `DatePicker` 绑定的是自 1970 年起的毫秒数）。`mode` 可以是 `.date`（默认）、`.time` 或 `.dateTime`；`min` 和 `max` 限定可选范围。

  | | 紧凑（默认） | `.section` |
  |---|---|---|
  | iOS | UIKit 的紧凑型日期选择器 | 直接展开的日历和时间，适合单独放在一个弹出面板里 |
  | Android | 显示日期和时间的按钮，点击后打开 Material 的选择器 | 同样的按钮 |
  | macOS | `NSDatePicker` 的文本框和步进器 | 它的日历和时钟 |
  | Windows | 日期时间选择器，日历以下拉方式展开 | 月历；`.dateTime` 时下方再加一个时间选择器（`.time` 仍为紧凑样式） |
  | Linux | 一个按钮，打开包含 GTK 日历以及时、分微调按钮的 popover | 直接展开的日历和微调按钮 |
  | Web | `<input type="date">`、`"time"`、`"datetime-local"` | 同样的输入元素 |

## 进度与加载

`Progress` 是进度条：它的 `value` 从 0 到 1；不知道进度时为 `none`。样式中的 `color` 是它的填充色。`ProgressRing` 是圆形的进度条：圆环从顶部开始顺时针填充，值变化时带有动画；为 `none` 时显示平台自己的不确定进度圆环。`Activity` 是一个加载指示器（spinner）。

```asuka
component Upload {
  state done = 0.4

  Column {
    gap: 12
    Progress { value: done }
    Progress()
    ProgressRing { value: done }
    ProgressRing {
      value: done
      width: 24
      color: .green
    }
    ProgressRing()
    Activity()
  }
}
```

圆环有自己的尺寸，每个平台各不相同（iOS 58 点，Android 48dp，Mac 和 Windows 32，Linux 和 Web 32）。样式里只设了一边时，另一边也随之相同，所以圆环始终是正方形。屏幕阅读器会把它读作进度指示器，并读出百分比。

| | 圆环 | 没有值时 |
|---|---|---|
| iOS | UIKit 没有确定进度的圆环，而 SwiftUI 的圆形 `ProgressView` 在 iOS 上会忽略它的值。所以圆环按 SwiftUI 圆形容量 `Gauge` 的样子绘制：强调色的圆头描边，压在 30% 强调色的轨道上，描边宽度是圆环尺寸的十分之一 | `UIActivityIndicatorView` |
| Android | Material 的 `CircularProgressIndicator`，按它的尺寸取 Material 3 对应的规格（extra small、small 或 medium：各自的轨道、内边距和间隙） | 同一个控件，不确定进度模式 |
| macOS | 没有 `color` 时，是 AppKit 自己尺寸（16 和 32 点）的 `NSProgressIndicator` 确定进度圆环。AppKit 只在这两个尺寸、而且只用强调色绘制它的圆环，所以其他尺寸或设置了 `color` 时，由 Asuka 绘制填满整个区域的圆环：16 到 32 点之间是 5 点宽的圆头描边（大于 32 点按 32 点圆环的比例，小于 16 点按 16 点的比例），压在 AppKit 的轨道上，颜色用 `color`，或者用从 AppKit 自己的指示器读出来的颜色（强调色、非活动窗口中的灰色，以及浅色和深色模式）。值的变化会像 AppKit 的 animator 那样用四分之一秒过渡过去，开启“减弱动态效果”时除外。两种情况下，VoiceOver 都会把它读作 AppKit 的指示器 | 16 和 32 点时是它的旋转指示器；其他尺寸则按该尺寸绘制它那 8 根辐条（缩放后带有 AppKit 的模糊效果，它本身不接受颜色），领头的那根用 `color` |
| Windows | Fluent 的 `ProgressRing`（WinUI 的）：一段没有轨道的圆弧。讲述人会把它读作进度条 | 转动的圆弧，和 `Activity` 一样 |
| Linux | 按 `AdwSpinner` 绘制圆环的方式绘制：15% 文字颜色的轨道，加上强调色的圆弧 | `AdwSpinner`（libadwaita 1.6 之前为 `GtkSpinner`） |
| Web | 一个 SVG 圆环，`role="progressbar"` 并带 `aria-valuenow` | 转动的圆弧；设置了 `prefers-reduced-motion` 时转得更慢 |

## 图片

`Image` 显示一张图片。平台会在主线程之外加载和解码图片，并缓存在内存中。iOS 和 Android 还会把通过 http(s) 获取的图片存到磁盘上，应用重启后依然有效；桌面端只缓存在内存里，浏览器则使用它自己的缓存。

```asuka
struct Photo {
  url: string
  blurHash: string
}

component Pictures(photo: Photo) {
  Column {
    gap: 12
    Image("logo.png") { width: 64 }
    Image("https://picsum.photos/id/1015/600/400") {
      placeholder: "placeholder.png"
      mode: .fill
      label: "A river in a valley"
      aspectRatio: 3.0 / 2
      radius: 12
      clip: true
    }
    Image(photo.url) {
      blurHash: photo.blurHash
      mode: .fill
      transition: 0.4
      aspectRatio: 1
      radius: 12
    }
  }
}
```

- 图片的来源可以是 http(s) URL、应用 `assets` 目录下的路径（`"logo.png"` 即 `assets/logo.png`），也可以是绝对路径，比如应用自己写入的文件，或者用户选取的照片。
- `mode` 决定图片如何填充它的框：`.fit`（默认，完整显示，必要时留出空边）、`.fill`（铺满整个框，超出部分裁掉）、`.stretch`（拉伸成框的宽高比）、`.center`（按原始尺寸居中）。
- `placeholder` 在来源加载完成之前显示，加载失败时也会显示。占位图请用资源文件（asset），这样能立即显示。记得给图片设定尺寸，让占位图有地方显示。
- `blurHash` 和 `thumbHash` 是用应用随图片保存的哈希生成的占位图（[BlurHash](https://blurha.sh)、[ThumbHash](https://evanw.github.io/thumbhash/)，后者为 base64 编码），和 expo-image 的一样。Asuka 会把哈希解码成一张小图，每个哈希只解码一次（最近用过的几百个会被保留），并立即显示在 `placeholder` 的位置上。BlurHash 本身不带形状信息：它按样式的 `aspectRatio` 解码，没有设置时解码成正方形并拉伸到整个框。ThumbHash 会保留原图的宽高比和透明度。它们都是绑定，因为列表的行在复用时会换成别的图片。
- `placeholderMode` 决定占位图如何填充框：`.fit`、`.fill`、`.stretch`、`.center`；默认和图片本身的方式一样。
- `transition` 是图片加载完成时淡入所用的秒数：在占位图上交叉淡入，没有占位图时从无到有地淡入。默认只在有占位图时淡入，时长取平台的短动画时长（0.25 秒；Android 上是 `config_shortAnimTime`）；设为 `-1` 则立即显示。
- `animated` 为 true 时播放动图，为 false 时让它停在当前帧。不设置时，动图会按文件规定的次数播放；但如果用户要求减少动态效果（“减弱动态效果”、Android 关闭动画、`prefers-reduced-motion`），它只显示第一帧。
- `label` 向屏幕阅读器描述这张图片；不设置时，图片被视为纯装饰。
- `on loaded(width, height)` 在图片加载完成后收到它的原始尺寸；`on error(message)` 在加载失败时收到错误信息。

没有设置尺寸的图片在加载完成前是空的。加载后，它采用原始尺寸，一个像素对应一个点，但宽度不会超过可用空间。如果只确定了一边（设置了 `width`，或在行中设置了 `grow`），它会保持宽高比；用 `aspectRatio` 则可以在加载之前就确定宽高比。

每个平台用它自己的解码器解码图片：

| | 能解码的格式 | 动图（GIF、APNG、动态 WebP）的播放 |
|---|---|---|
| iOS | ImageIO：PNG、JPEG、GIF、WebP、HEIC、AVIF | ImageIO 的 `CGAnimateImageDataWithBlock`，按文件规定的时长和循环次数播放（也支持 HEIF 序列） |
| Android | 先用 `BitmapFactory`，再用 `ImageDecoder`：PNG、JPEG、GIF、WebP、HEIF、AVIF | `AnimatedImageDrawable`：GIF 和 WebP（系统支持时也包括 AVIF 序列）；APNG 只显示第一帧 |
| macOS | `NSImage`：PNG、JPEG、GIF、WebP、HEIC、AVIF、SVG | 同 iOS |
| Windows | WIC：PNG、JPEG、GIF、BMP、TIFF、ICO，以及安装了对应编解码器时的 WebP、HEIF 和 AVIF（WebP 的随 Windows 11 自带；HEIF 需要 HEIF 和 HEVC 视频扩展，AVIF 需要 AV1 视频扩展） | 由 Asuka 逐帧播放；动态 WebP 的每一帧用 WIC 的 WebP 编解码器解码 |
| Linux | GDK 的加载器：PNG、JPEG、GIF，以及视安装情况而定的 WebP、AVIF、HEIF、SVG 和 JPEG XL | 由 Asuka 逐帧播放；动态 WebP 的每一帧用 GDK 的 WebP 加载器解码 |
| Web | 浏览器支持的格式 | 用 WebCodecs 的 `ImageDecoder` 在 canvas 上播放（也支持 AVIF 序列）；没有它的浏览器（Safari）由 `<img>` 自己播放，暂停时显示第一帧 |

只有在 Mac、Linux 和 Web 上，SVG 才能作为图片显示。要在所有平台上显示 SVG 图标，请用 [`std/svg`](#svg-图标) 以矢量方式绘制。

## 符号与图标

`Symbol` 显示一个符号。在 iOS 和 macOS 上它是 SF Symbol，在 Android 和 Web 上是对应的 Material Symbol，在 Windows 上是对应的 Segoe Fluent Icons 字形（Windows 11 之前为 Segoe MDL2 Assets），在 Linux 上是图标主题（Adwaita）中对应的符号图标，没有的话就用 Material Symbol。它按样式中的 `font` 像文字一样确定大小，因此会跟随字号变化；颜色取自样式中的 `color`（默认为标签颜色）。

```asuka
component Symbols {
  Row {
    gap: 14
    align: .center
    Symbol("bell")
    Symbol("star.fill") {
      color: .yellow
      font: .title
    }
    Symbol("archivebox") { color: .secondaryLabel }
  }
}
```

符号的名字就是 SF Symbol 的名称。按钮、菜单项、栏按钮和标签页也都接受它：`"house"`、`"magnifyingglass"`、`"gear"`、`"plus"`、`"xmark"`、`"trash"`、`"square.and.arrow.up"`、`"ellipsis.circle"` 等；名字后面加上 `".fill"` 就是它的填充样式（`"star.fill"`）。

iOS 和 macOS 能绘制任意 SF Symbol。其他平台只能绘制一组精选的符号，包括常用的那些以及更多：它们映射到 Material Symbols，在 Windows 上映射到 Segoe Fluent Icons 的字形。不在这组里的名称在这些平台上什么也不显示（日志中会有一条警告），所以像 `"archivebox"` 这样直接用字符串写的名称，要在你发布的每个平台上都检查一遍。在 Linux 上，以 `-symbolic` 结尾的名称会被当作图标主题中的名称，原样使用。

### SVG 图标

要用自己的图标，`std/svg` 会用平台的矢量绘图来画 SVG，不会栅格化。`svg.Icon` 的标记是一个字符串字面量，由编译器解析：读不懂的标记是编译错误，设备上不做任何解析。

```asuka
import std/svg

let download = svg.Icon(r"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" x2="12" y1="15" y2="3"/></svg>""")

component DownloadRow {
  Row {
    gap: 8
    svg.View(download) { color: .accent; width: 24; height: 24 }
    Text("Download")
  }
}
```

`color` 会画在 SVG 中使用 `currentColor` 的地方，以及 SVG 没有指定填充的地方；默认是文字颜色 `.label`。样式里没有宽高时，视图就是 SVG 自身的尺寸；宽高都给了时，图标会保持比例放进去。它能读懂常见图标集（Lucide、Material、Heroicons、Feather）用到的内容：路径、基本形状、分组、`use`、填充、描边和变换。它会跳过文字、渐变（用图标颜色绘制）、裁剪、蒙版、滤镜和 `fill-rule="evenodd"`。

`svg.Shape` 画应用自己生成的路径：每个 `svg.Layer` 是一条 `svg.Path`，可以填充（`fill`，沿框往下渐变到 `fade`）和描边（`stroke`、`width`、`cap`、`join`）。`svg.Point` 由相对框左上角的点数加上框尺寸的比例组成，所以框变大变小时形状会跟着变，应用不用重新计算：

```asuka
import std/svg

let tick = svg.Path().move(svg.Point(x: 4, fy: 0.5)).line(svg.Point(fx: 0.4, fy: 1, y: -4)).line(svg.Point(fx: 1, x: -4, y: 4))

component Done {
  svg.Shape([svg.Layer(path: tick, stroke: .green, width: 3, cap: .round, join: .round)]) {
    width: 32
    height: 32
  }
}
```

在 iOS 和 macOS 上用的是 Core Graphics，在 Android 上是 `Canvas`，在浏览器中是 `<svg>`，在 Windows 上是 GDI+，在 Linux 上是 Cairo。

## 视频

`Video` 用平台的播放器及其控件播放视频：iOS 上是 `AVPlayerViewController`，Android 上是 Media3 的 ExoPlayer，macOS 上是 `AVPlayerView`，Linux 上是 `GtkVideo`（GStreamer），浏览器中是 `<video>`。Win32 没有视频视图，所以在 Windows 上它是 WebView2 中的 `<video>`，使用 Edge 的播放控件。用 `Ref` 的 `play()`、`pause()` 和 `seek(seconds)` 控制它。

```asuka
component Clip {
  let v = Ref()

  Column {
    gap: 12
    Video("https://example.com/clip.mp4") {
      ref: v
      autoPlay: true
      muted: true
      loop: true
      radius: 12
    }
    Button("Start over") {
      on press {
        v.seek(0)
        v.play()
      }
    }
    Button("Pause") { on press { v.pause() } }
  }
}
```

- 视频的来源是 http(s) URL（视频文件或 HLS 流），或者应用 assets 中的文件。
- `mode` 为 `.fit`（默认，留出空边）或 `.fill`。
- `hideControls: true` 隐藏平台的播放控件：播放按钮、进度条、全屏，以及 iOS 上的 AirPlay 和画中画。
- 每次播放到结尾都会运行 `on end`；无法播放时运行 `on error(message)`。

它的尺寸规则和 `Image` 一样：在得知视频尺寸之前是空的（除非样式给了它尺寸），之后保持视频的宽高比。`seek` 的参数是从开头算起的秒数。

## Lottie 动画

`std/lottie` 用各平台自己的播放器播放 Lottie 动画（从 After Effects 导出的 Lottie JSON `.json` 或 dotLottie `.lottie`）：iOS 和 Mac 上是 lottie-ios，Android 上是 lottie-android，Web 上是 lottie-web。Windows 和 Linux 目前还没有可调用的播放器，在那里这个视图是一个空的框。

```asuka
import std/lottie

component Success {
  state playing = true
  state progress = 0.0
  state done = false

  Column {
    lottie.View("success.json") {
      bind playing: playing
      bind progress: progress
      overrides: [lottie.colors("Ring", light: #0a84ff, dark: #64d2ff)]
      width: 200
      on complete { done = true }
    }
    Slider { bind value: progress }
  }
}
```

- 它的来源是文件：http(s) URL、应用 assets 里的路径或绝对路径；也可以用 `data` 直接传文件内容。`animation` 按 ID 选择 dotLottie 中的某一个动画。
- `playing` 为 true 时播放，为 false 时停在当前位置；绑定后，播放完一遍它会变成 false。`loop` 会一遍又一遍地播放；否则播到结尾就停下，并报告 `complete`。`speed` 缩放帧率（负数表示倒放）。
- `progress` 是当前位置，从 0 到 1。绑定后，视图在播放时会写入它，应用写入的值会让动画跳过去：滑块可以拖动进度，手势可以驱动它。
- `segment`（帧，从 `from` 到 `to`）或 `marker`（按名字选的一个标记）是要播放的部分。`loaded` 会给出它的 `Info`：尺寸、帧数、帧率和标记。
- `fit` 是它填充框的方式：`.contain`（默认）、`.cover`、`.stretch` 或 `.center`。样式里没有尺寸时，它就是动画自身的尺寸。
- `overrides` 按 keypath 修改颜色和数值，用于主题或深色模式：`lottie.color(keypath, c)`（keypath 下的所有填充和描边）、`lottie.colors(keypath, light:, dark:)`、`lottie.opacity(keypath, v)` 和 `lottie.strokeWidth(keypath, w)`。keypath 是从根开始的图层、分组和形状的名字，用点分隔，`*` 表示任意一个名字，`**` 表示任意多个：`"Ring"`、`"Circle.Disc.Fill 1"`、`"**.Stroke 1"`。

导入这个包的应用会带上这些播放器一起构建：一个 Swift 包（lottie-spm）、一个 Gradle 库（`com.airbnb.android:lottie`）和 lottie-web 的脚本（视图第一次显示时才加载）；没导入的应用则不会带上。它是一个[原生包](native.md)，所以在手机上用 `asuka dev` 时，带有它的应用会单独构建运行，而不是在热重载模式下运行。

## 网页

`WebView` 用平台的 Web 视图显示网页，滚动、缩放和文本选择都是平台原生的：iOS 和 macOS 上是 `WKWebView`，Android 上是 `WebView`，Windows 上是 WebView2（Edge 的运行时），Linux 上是 WebKitGTK 6.0 的 `WebKitWebView`。Linux 上如果没有安装 WebKitGTK，它什么也不显示，并调用一次 `on error`，告诉你需要安装什么。在浏览器中它是一个 `<iframe>`，下面介绍的功能只有对应用自己站点的页面才能全部使用；参见 [Web](web.md#web-视图)。没有设置高度也没有 `grow` 时，它会扩展到填满可用空间。

```asuka
import platform/device

component Browser {
  state page = WebState()

  let web = Ref()

  Column {
    grow: 1
    Row {
      gap: 8
      padding: [8, 12]
      align: .center
      Button("") {
        icon: "chevron.backward"
        label: "Back"
        disabled: !page.canGoBack
        on press { web.goBack() }
      }
      Text(page.title) {
        grow: 1
        lines: 1
      }
      Button("") {
        icon: "arrow.clockwise"
        label: "Reload"
        on press { web.reload() }
      }
    }
    if page.loading { Progress { value: page.progress } }
    WebView("https://example.com") {
      ref: web
      block: ["doubleclick.net", "/ads/"]
      on state(s) { page = s }
      on message(m) { print("the page says {m}") }
      on newWindow(url) { device.openURL(url) }
    }
  }
}
```

显示什么：

- 它的 URL 是 http(s) URL，或者应用 assets 中的文件（这时页面里的相对链接也指向 assets）。`html` 则是直接显示的标记，用来代替 URL。
- `userAgent` 是请求页面时使用的 User-Agent，也是 `navigator.userAgent` 的值；留空时使用平台默认值。
- `preload` 是在每个页面自身的脚本之前、在每个 frame 中运行的脚本，可以用来拦截 `fetch` 或设置全局变量。
- `block` 阻止请求加载，就像广告拦截器那样。每一项可以是一个主机名，同时匹配它的子域名（`"doubleclick.net"`）；也可以包含 `/`，这时匹配 URL 中的一部分（`"/ads/"`）。

`preload`、`userAgent` 和 `block` 在视图创建时就确定了。

报告什么：

- 每个页面加载完成时，`on loaded(url)` 会收到它的 URL；页面加载失败时，`on error(message)` 会收到错误信息。
- `on state(s)` 像地址栏一样跟踪页面的状态：一个 `WebState`，包含 `url`、`title`、`loading`、`progress`（0 到 1）、`canGoBack` 和 `canGoForward`。
- `on newWindow(url)` 收到页面想在新窗口中打开的 URL（`window.open`、`target="_blank"` 的链接）。没有设置它时，那个页面会直接在当前视图中加载。
- `on download(d)` 收到页面无法显示、应当保存为文件的响应（比如指向 `.zip` 的链接、`Content-Disposition: attachment`），形式是一个 `WebDownload`，包含 `url`、`mime`、`filename` 和 `size`（未知时为 -1）。它本身并不会下载文件。
- `on request(url)` 收到页面发出的每个请求的 URL（脚本、图片、媒体、`fetch`）。**iOS 和 macOS 不支持**：`WKWebView` 不允许应用看到这些请求。

`Ref` 的 `goBack()`、`goForward()`、`reload()` 和 `stopLoading()` 分别用于在历史记录中后退、前进，重新加载页面，以及停止加载。

### 与页面通信

页面通过 `window.asuka.postMessage(text)` 给应用发消息，这会运行 `on message`。应用这边可以用 `Ref` 的 `run(script)` 在页面中执行脚本，它不返回任何结果；也可以用 `try await eval(script)`，它以 JSON 的形式返回值：最后一条语句的值，或者返回的 promise 解析出的值（`undefined` 时为 `null`），出错时则抛出脚本抛出的错误。

```asuka
component Page {
  let web = Ref()

  WebView {
    ref: web
    html: "<button onclick=\"asuka.postMessage('tapped')\">Tap</button>"
    on message(m) {
      let title = try? await web.eval("document.title")
      print("{m} {title ?? ""}")
    }
  }
}
```

`eval` 借助页面的 `eval` 来等待 promise，而页面的内容安全策略（CSP）如果不允许 `'unsafe-eval'`，就会拦下 `eval`。在这样的页面上，iOS、Android、macOS 和 Linux 会改用 `run` 的方式执行脚本，这种方式不受 CSP 限制：得到的仍是最后一条语句的值，出错时仍是脚本抛出的错误，只是不会等待 promise（Android 上得到 `{}`，其他平台报错），而且在 iOS 和 macOS 上，对象的键可能换了顺序。Windows 不需要 `eval`，照常等待 promise。在 Web 上，浏览器会对应用在页面里执行的脚本施加页面自己的策略，所以禁止 `eval` 的页面会同时拒绝 `run` 和 `eval`。

`try await webview.cookies(url)` 获取 Web 视图中某个 URL 的 cookie，形式是一个 `Cookie` 请求头。这样用户在页面里登录之后，你自己发出的请求也能带上这份登录状态。

[`examples/downloader`](../../examples/downloader) 用到了以上全部功能：一个带地址栏、预加载脚本、请求拦截和下载功能的浏览器。

## 形状

`Shape` 绘制一个轮廓，可以填充、描边，或者两者兼有。它的路径函数根据形状的尺寸生成轮廓；尺寸变化时，以及它读取的状态变化时，都会重新运行。形状的尺寸由样式决定（宽高、宽高比、`grow`）；都没有设置时，它是空的。

```asuka
component Triangle {
  Shape((w, h) => Path().move(w / 2, 0).line(w, h).line(0, h).close()) {
    fill: .orange
    width: 48
    height: 48
  }
}
```

`Path` 以点为单位，原点在形状的左上角。它的方法会往路径里添加内容并返回路径本身，方便链式调用：`move`、`line`、`quad`、`cubic`、`arc`（角度用弧度，从 x 轴起顺时针计算）、`close`，以及直接添加整个图形的 `rect`、`roundedRect` 和 `ellipse`。

`stroke` 和 `lineWidth`（默认为 1）用来描出轮廓，可以指定 `lineCap`（`.butt`、`.round`、`.square`）和 `lineJoin`（`.miter`、`.round`、`.bevel`）。`trimStart` 和 `trimEnd` 只画出轮廓的一部分，取值从 0 到 1：比如进度环，或者一条自己画出来的线。在 `animate` 中修改它们会产生动画（见[动效](motion.md)）。`dash` 把描边画成虚线：以点为单位的长度，依次是画的和空的（`[6, 4]`；个数为奇数时会重复一遍），和 SwiftUI 的 `StrokeStyle(dash:)` 一样。`dashPhase` 是虚线图案从哪里开始。[`examples/usage`](../../examples/usage) 就用它画了一条预测线。

```asuka
import std/math

fn ring(w: float, h: float) -> Path {
  let r = min(w, h) / 2 - 4
  return Path().arc(w / 2, h / 2, r, start: -math.pi / 2, end: 3 * math.pi / 2)
}

component ProgressCircle(done: float) {
  View {
    label: "Progress"
    width: 72
    height: 72
    Shape(ring) {
      stroke: .secondaryBackground
      lineWidth: 8
      absolute: true
      width: 100%
      height: 100%
    }
    Shape(ring) {
      stroke: .accent
      lineWidth: 8
      lineCap: .round
      trimEnd: done
      absolute: true
      width: 100%
      height: 100%
    }
  }
}
```

### Canvas

`Canvas` 用来绘制应用自己画的内容：图表、仪表盘、签名、游戏棋盘，任何没有现成视图可用的东西。它的函数通过一个 `Graphics` 绘制，坐标以点为单位，原点在左上角：

```asuka
component Level(level: float) {
  Canvas(g => {
    let w = g.width
    let h = g.height
    g.fill(g.path().roundedRect(0, 0, w, h, 12), .secondaryBackground)
    g.fill(g.path().roundedRect(0, 0, w * level, h, 12), .accent)
    g.text("{(level * 100).fixed(0)}%", w / 2, h / 2 + 6, CanvasText(weight: .semibold, align: .center))
  }) {
    height: 36
  }
}
```

- `g.path()` 返回一个待构建的路径（和 Shape 用的一样），在多次绘制之间复用。
- `fill` 和 `stroke`（带一个 `StrokeStyle`：线宽、线帽、连接方式、虚线）用颜色、`LinearGradient` 或 `RadialGradient`（圆心、半径，以及渐变起始的焦点）绘制路径；`text` 绘制一行文字，基线在 y，x 处是它的左边、中心或右边（`CanvasText` 的 `align`）。
- `save` 和 `restore` 成对使用，把 `translate`、`scale`、`rotate`（角度制，顺时针）、`transform`（任意仿射矩阵，包括倾斜）、`clip`、`opacity` 和 `evenOdd`（按奇偶规则填充和裁剪；默认是非零规则）括在中间。
- 颜色可以使用语义颜色（`.label`、`.separator`、`.accent`），它们会跟随深色模式变化。

布局完成后，Canvas 会绘制一次；之后尺寸变化，或函数读取过的状态变化时，它会重新绘制，就像一个 effect。每次绘制都会记录一个显示列表（display list），只有和上一次不同时才会发送出去，再由平台回放：iOS 和 macOS 上用 Core Graphics，Android 上用 `Canvas`，Windows 上用 GDI+，Linux 上用 GTK 的 snapshot（GSK），浏览器中用 2D `<canvas>`。只要这个 canvas 之前已经画过同样多的内容，记录过程就不会分配内存。Canvas 本身不响应点击：请把它包在设置了 `on press` 或 `on pan` 的 `View` 里，回调给出的坐标就在它的坐标系中。屏幕阅读器不会读取 Canvas：请给外层视图设置 `label`。

### 图表

`std/chart` 绘制图表：在数值轴上画柱、线、面积和点，标签用平台的文字（随用户的文字大小缩放）。它是用 Asuka 写的：刻度和图形只计算一次，由 `svg.Shape` 绘制，所以在每个平台上都一样。

```asuka
import std/chart

component Week(steps: [float], goal: [float]) {
  chart.View([
    chart.Series(name: "Steps", kind: .bar, values: steps),
    chart.Series(name: "Goal", kind: .line, values: goal, color: #ff9500),
  ]) {
    labels: ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]
    label: "Steps this week"
    height: 180
  }
}
```

- 一个系列的每个标签对应一个值。柱状系列并排摆放；面积、线和点画在它们上面。`smooth` 画一条单调曲线，不会超出它的值；`width` 是线的粗细或点的大小。
- 数值轴会根据数据自动调整，并向外取整到整刻度（柱状和面积图会一直延伸到零），除非用 `low` 和 `high` 指定范围；刻度标签会尽量简短（`2.5k`）。`grid: false` 会去掉图形后面的网格线。
- 颜色默认依次是应用的强调色和系统颜色（`chart.palette`）。
- 屏幕阅读器把整张图表当作一张图片来读：先读 `label`，再读每个系列的值。样式里没有高度时，它的高度是 200。

### 着色器

着色器用一段 GPU 程序给框里的每个像素上色：流动的渐变、噪声、光晕、波纹。用 `shader Name(params) { … }` 声明一个着色器，写法就是 Asuka 自己的语法，用的时候把它当成视图：参数就是它的 uniform，像组件的参数一样设置，它也像组件一样接受样式。着色器在应用编译时为所有平台编译好：写错了就是编译错误，并指出所在的行；设备上只编译平台自己的程序。

```asuka
/// 框中央的一团光晕，忽明忽暗。
shader Glow(tint: Color, level: float = 1) {
  let speed = 2

  fn fragment(pos: vec2) -> vec4 {
    let d = distance(pos / size, vec2(0.5))
    let g = smoothstep(0.5, 0, d) * (0.8 + 0.2 * sin(time * speed)) * level
    return vec4(tint.rgb * g, 1) * tint.a
  }
}

component Card(level: float) {
  Glow(tint: .accent, level: level) {
    animated: true
    height: 200
    radius: 16
    clip: true
    background: #0b1030
  }
}
```

- `fn fragment(pos: vec2) -> vec4` 根据 `pos`（像素中心相对框左上角的位置，单位是点）给出这个像素的颜色（预乘 alpha），红、绿、蓝和 alpha 都在 0 到 1 之间。在着色器里，`size` 是框的尺寸（点），`scale` 是每个点的像素数，`time` 是着色器显示以来的秒数。
- 参数可以是 `float`、`int`、`bool` 或 `Color`（在着色器里是一个 `vec4`，各通道在 0 到 1 之间）。参数变化时框会重新绘制。`animated: true` 让 `time` 走起来：显示器每画一帧，框就画一帧。
- 着色器里的值有 `bool`、`int`、`float`、`vec2` 到 `vec4`、`ivec2` 到 `ivec4` 以及 `mat2` 到 `mat4`，支持分量重组（`v.rgb`、`p.yx`）；颜色字面量是一个 `vec4`。它可以调用 GLSL 的函数（`sin`、`mix`、`smoothstep`、`dot`、`normalize` ……）和自己的函数（`fn wave(x: float) -> float = …`），可以有常量（`let`）、`let` 和 `var`、赋值、`if`、`while`、`for i in 0..<n`、`return`、`break` 和 `continue`。GPU 运行不了的东西（闭包、字符串、可选值、作为值的 `if`）都会报编译错误，并说明原因。
- 编译器还会报告着色器里 Android 运行不了的部分，在所有平台上一视同仁：AGSL 是 GLSL ES 1.00 的语言，所以不支持无符号整数、位运算、整数取余（浮点数的 `mod` 可以）、导数，以及上下界不是常量的循环。

在 iOS 和 macOS 上用的是 Metal（一个 `CAMetalLayer`），在 Android 13 及以上用的是 AGSL（一个 `RuntimeShader`；更早的版本上是一个空的框），在浏览器中是 WebGL 2（GLSL ES 3.00），在 Windows 上是 Direct3D 11（HLSL；没有 GPU 时用系统的软件光栅化器 WARP），在 Linux 上是 OpenGL（一个 `GtkGLArea`；没有 GPU 时用 Mesa 的软件光栅化器）。每个平台的程序都随应用一起编译，设备上只编译它自己的着色语言。在 Windows 上，每一帧都在主线程之外渲染，好了才显示。Linux 上没有 GL 时（`GDK_DISABLE=gl`），显示的是一个空的框。给框设一个 `background`，在第一帧出来之前先顶上。

## 容器

`View` 用来容纳其他视图。`Column` 和 `Row` 也是视图，分别把子视图从上到下、从起始边到结束边依次排列。`SafeArea` 是一个列，会为刘海、系统栏和键盘留出内边距。它们如何确定子视图的尺寸和位置，请看[布局](layout.md)。

如果一个视图没有外观、事件和无障碍相关的属性，它就不会生成自己的原生视图，子视图会直接放进最近的、确实有原生视图的那个祖先里。所以多层嵌套行和列不会带来任何开销。

```asuka
import platform/device

component Person {
  state open = false

  Row {
    gap: 12
    padding: 16
    background: .secondaryBackground
    radius: 12
    label: "Ada, 2 unread"
    on press { open = !open }
    on longPress { device.haptic(.medium) }
    Symbol("person.fill")
    Column {
      grow: 1
      gap: 2
      Text("Ada") { font: .headline }
      Text("2 unread") { color: .secondaryLabel }
    }
  }
}
```

`View`、`Row`、`Column` 和 `SafeArea` 接受：

- `on press` 和 `on longPress`。
- 手势，使用平台的手势识别器：`on pan(g)`、`on pinch(g)`、`on rotate(g)`（各自收到一个 `Gesture`）和 `on swipe(direction)`。参见[动效](motion.md)。
- 交互状态。视图自己的样式通过样式变体（`pressed { … }`、`hovered { … }`、`dragging { … }`、`dragOver { … }`）响应这些状态；其他视图则通过它的 `Ref`（`card.hovered`、`card.pressed`）跟随。副作用放在 `on pressedChange` 和 `on hoverChange` 里。拖放：`dragItems` 和 `dropTypes`，以及 `on drop(d)`。参见[动效](motion.md#按下与悬停状态)。
- `on layout(frame)`：视图布局完成后，以及之后每次位置或尺寸变化时，都会以视图的 `Frame` 运行它。
- `href`：按下时前往的路由（这样它就是一个链接：再加上 `role: .link`）。参见[路由](navigation.md#路由)。
- 无障碍：`label`、`hint`、`role`、`a11yHidden`、`actions` 和 `on adjust(by)`。参见[无障碍](accessibility.md)。

### Scroll

`Scroll` 沿一个方向滚动它的子视图。你需要给它一个尺寸，或者设置 `grow`，它才有可供滚动的范围。它的样式是滚动视图本身的框；子视图在里面按列排列（在里面放一个 `SafeArea` 或 `Column` 来设置内边距和间距）。

```asuka
component Page {
  state down = false

  let scroll = Ref()

  View {
    grow: 1
    Scroll {
      ref: scroll
      grow: 1
      on scroll(_, y) { down = y > 300 }
      SafeArea {
        padding: 20
        gap: 16
        for i in 0..<50 key i { Text("Line {i + 1}") }
      }
    }
    if down {
      View {
        absolute: true
        right: 20
        bottom: 20
        Button("") {
          icon: "arrow.up"
          label: "Back to top"
          variant: .filled
          on press { scroll.scrollTo(0, 0) }
        }
      }
    }
  }
}
```

- 滚动过程中会运行 `on scroll(x, y)`，参数是内容的偏移量（以点为单位）。
- `Ref` 的 `scrollTo(x, y, animated:)` 滚动到指定偏移（超出末尾就停在末尾）；`scrollToEnd(animated:)` 滚动到底部，横向滚动视图则滚到结束边。两者同样适用于 `List`。
- `horizontal: true` 改为横向滚动：

```asuka
component Tags(tags: [string]) {
  Scroll {
    horizontal: true
    Row {
      gap: 8
      padding: [0, 16]
      for t in tags key t { Text(t) }
    }
  }
}
```

**分页。** `paging: true` 让滚动只停在整页的位置，适合轮播图、新手引导的各个步骤、全屏视频流等。每个子视图就是一页，在滚动方向上和滚动视图一样大。`bind page` 是当前显示的页：滑动停稳时会写入它，反过来写入它也会滚动到对应的页。`PageControl` 显示当前在哪一页，每页一个圆点，点击圆点也会翻到那一页：

```asuka
component Carousel {
  state page = 0

  Column {
    gap: 8
    Scroll {
      horizontal: true
      paging: true
      bind page: page
      height: 200
      for i in 0..<3 key i { Text("Card {i + 1}") }
    }
    PageControl(3) { bind page: page }
  }
}
```

| | iOS | Android | macOS | Windows | Linux | Web |
|---|---|---|---|---|---|---|
| 分页 | `isPagingEnabled` | 每次滑动吸附到下一页，或回到原来的页 | 滑动结束时停在最近的一页 | 滚轮每滚一格翻一页（同 FlipView） | 滚轮每滚一格翻一页；用触控板或触摸屏滑动后停在某一页；方向键、Page Up 和 Page Down 每次翻一页 | CSS 滚动吸附 |
| `PageControl` | `UIPageControl` | 主题主色的圆点（Material 没有这种控件） | 强调色的圆点 | 圆点指示器（同 PipsPager） | 按 libadwaita 风格绘制的圆点，当前页的圆点更大、颜色更深 | 圆点，当前页使用标签颜色 |

页码从阅读的起始方向开始计算：在从右到左的布局中，第一页在最右边。滚动视图的尺寸变化时（比如旋转屏幕），它会停留在当前页。屏幕阅读器会在圆点上读出“第 2 页，共 3 页”，并且可以调节它们。

**下拉刷新。** 设置 `on refresh` 即可开启（`UIRefreshControl`、`SwipeRefreshLayout`）。刷新期间把 `refreshing` 设为 true；它变回 false 时，加载指示器随之隐藏。如果没有设置 `refreshing`，处理函数一返回指示器就会隐藏。在 macOS 上，用触控板或妙控鼠标滚过顶部就会拉出它，和 iOS 一样。Windows 和 Linux 按桌面的方式刷新：按 F5（Linux 上也可以按 Ctrl+R）刷新焦点所在的滚动视图，没有的话就刷新屏幕上最内层的那个，刷新期间顶部显示一个加载指示器。在浏览器里，用手指往下拉即可。

```asuka
component Feed {
  state refreshing = false

  Scroll {
    grow: 1
    refreshing: refreshing
    on refresh {
      refreshing = true
      await sleep(1s) // fetch
      refreshing = false
    }
    Text("Pull down")
  }
}
```

**键盘上方的工具栏。** `TextField` 的 `accessory { … }` 是输入框获得焦点时显示在键盘上方的一条栏，可以放字数统计和“完成”按钮、格式按钮、快捷回复等（类似 SwiftUI 的键盘工具栏）。它和窗口一样宽，高度由内容决定，随输入框一起创建，也随输入框一起销毁。

```asuka
component Note {
  state note = ""
  state focused = false

  TextField {
    bind text: note
    bind focused: focused
    multiline: true
    accessory {
      Row {
        padding: [6, 16]
        align: .center
        material: .bar
        Text("{note.count()} characters") { grow: 1 }
        Button("Done") { on press { focused = false } }
      }
    }
  }
}
```

在 iOS 上，它就是输入框的 `inputAccessoryView`：跟随键盘移动，交互式收起键盘时也一样。Android 没有对应的机制：这条栏会放在输入框所在的窗口里（输入框在弹出面板里时也是如此），贴在键盘顶部，逐帧跟随键盘。macOS、Windows 和 Linux 没有屏幕键盘，因此不会显示它；浏览器只在触摸屏上显示。

**富文本。** `bind rich` 把输入框变成编辑带样式文本的文本区域，适合邮件正文或带格式的消息：支持粗体、斜体、下划线、删除线、等宽代码和链接（片段原有的颜色和文本样式会原样保留）。它和 `RichText` 显示的是同一种 `[Span]`。用户的输入和格式操作会写入它；写入它则会替换文本。`Ref` 的 `toggleFormat(.bold)` 在选区不全是这种样式时为选区加上它，否则去掉（`format(.bold, on:)` 则明确设置或清除）；没有选区时，它作用于接下来输入的文字。`setLink(url)` 为选区加上链接（`""` 去掉链接；在光标处，作用于光标所在的链接）。`on formats(f)` 报告光标处的样式，或整个选区共有的样式，供工具栏显示。

```asuka
component Composer {
  state body = [Span(text: "Hi,\n\nThe "), Span(text: "1.0", font: .body.bold()), Span(text: " build is ready.")]
  state formats = TextFormats()

  let field = Ref()

  Column {
    grow: 1
    Button("") {
      icon: "bold"
      label: "Bold"
      background: if formats.bold { .secondaryBackground } else { .clear }
      on press { field.toggleFormat(.bold) }
    }
    TextField {
      ref: field
      bind rich: body
      plain: true
      grow: 1
      on formats(f) { formats = f }
    }
  }
}
```

| | iOS | Android | macOS | Windows | Linux | Web |
|---|---|---|---|---|---|---|
| 视图 | `UITextView` 及其 attributed text | `EditText` 及其 span | 富文本模式的 `NSTextView` | rich edit 控件（`RICHEDIT50W`），通过 RTF 读写 | `GtkTextView` 及其缓冲区中的 tag | `contenteditable` 元素 |
| 用户设置格式 | 编辑菜单中的 B/I/U | 实体键盘的 Ctrl+B、Ctrl+I、Ctrl+U | “格式”菜单，⌘B ⌘I ⌘U | Ctrl+B、Ctrl+I、Ctrl+U | Ctrl+B、Ctrl+I、Ctrl+U | 浏览器的 ⌘B ⌘I ⌘U（其他系统上为 Ctrl） |
| 撤销格式修改 | 支持 | 支持：使用输入框自己的历史记录（Android 自带的只记录文本），Ctrl+Z、Ctrl+Shift+Z、Ctrl+Y | 支持 | 支持 | 使用 GTK 的历史记录（Ctrl+Z、Ctrl+Shift+Z），只记录文本 | 支持：使用页面的编辑命令 |

[`examples/mail`](../../examples/mail) 中有一个带工具栏的邮件编辑器。

**内边距。** 文本或输入框的 `padding`（以及 `border`）位于它的框之内、内容的四周，和 CSS 一样：背景会铺满这部分区域，文字绘制在它的内侧，输入框的编辑区域也在它的内侧。

## 列表

`List` 使用平台的可复用列表（`UICollectionView`、`RecyclerView`），为每个数据项显示一行。桌面端和 Web 没有能把任意视图作为行的列表控件，所以在那里由 Asuka 在平台的滚动视图中自行复用行。只有屏幕上的单元格才会创建行；滚出屏幕的单元格会被复用来显示另一个数据项，并且只更新有差异的属性。[`examples/list10k`](../../examples/list10k) 有一万行。

```asuka
struct Message {
  id: int
  from: string
  subject: string
}

state messages = [Message(id: 1, from: "Ada", subject: "Lunch?"), Message(id: 2, from: "Grace", subject: "The report")]

component Inbox {
  List {
    for m in messages key m.id {
      Column {
        gap: 2
        padding: [12, 20]
        Text(m.from) { font: .headline }
        Text(m.subject) { color: .secondaryLabel }
      }
    }
  }
}
```

它的 `for` 接受数据项和每项的 key，并根据数据项（以及它的索引，`for (i, m) in messages.enumerated()`）构建一行。key 用来在列表前后两次变化之间认出每一项：某一项变了，就更新它的行；某一项移动了，它的行也跟着移动；平台会用动画呈现这些差异。行的绑定读取的是它的数据项，所以被复用的行会跟随它的新数据项。

列表会填满分配给它的空间：没有设置高度也没有 `grow` 时，它会自动扩展。行与行之间会绘制平台的分隔线。

`List` 接受：

- `swipeActions`：一行的操作，滑动该行时显示：写在行的顶层视图上（`swipeActions: [...]`），或者写在列表上，作为行索引的函数。在 iOS 上，它们是行的尾部操作，破坏性操作显示为红色，完整滑到底会执行第一个操作。在 Android 上，完整滑动会执行第一个操作，滑动时露出它的颜色和图标，就像 Material 的滑动删除。在 macOS、Windows、Linux 和 Web 上，它们会出现在行的上下文菜单中（右键点击；Linux 和 Web 上长按也可以），先列出前部操作，再列出尾部操作。请
- `on endReached`：最后几行即将显示时运行，对于列表的同一个长度只会触发一次。在这里加载更多数据。
- `on refresh` 和 `refreshing`：下拉刷新，用法与 `Scroll` 相同（Windows 和 Linux 上按 F5）。
- `on scroll(x, y)`：与 `Scroll` 相同。
- `appearance`：列表的外观。`.plain` 的行铺满整个宽度，彼此以分隔线隔开；`.insetGrouped` 是平台的分组列表（Linux 上是 libadwaita 的 boxed list）；`.bare` 没有分隔线，行的后面也没有任何背景，列表背后的内容会透出来，适合那些自己绘制卡片的行。bare 列表的行间距由行自己负责，而且行依然可以滑动。

```asuka
struct Message {
  id: int
  subject: string
}

fn fetch(from: int, n: int) -> [Message] = (from..<from + n).map(i => Message(id: i, subject: "Message {i + 1}"))

component Inbox {
  state messages = fetch(0, 30)

  List {
    on endReached {
      messages += fetch(messages.count(), 30)
    }
    for m in messages key m.id {
      Row {
        padding: [12, 20]
        swipeActions: [
          .button(title: "Delete", icon: "trash", role: .destructive, run: () => messages.removeWhere(x => x.id == m.id)),
          .button(title: "Archive", icon: "archivebox", run: () => messages.removeWhere(x => x.id == m.id)),
        ]
        Text(m.subject)
      }
    }
  }
}
```

`leadingSwipeActions` 通过反方向滑动显示，也就是从行的起始边滑出（比如邮件里的“未读”和“置顶”）：第一个操作紧贴那条边，也是完整滑动时执行的那个。iOS 会给它们着色；Android 把它们显示为 Material 的填充按钮。

`Ref` 的 `scrollToItem(index, animated:)` 滚动列表，让某一项显示出来；`scrollTo` 和 `scrollToEnd` 也同样适用。

### 聊天：锚定在底部的列表

`anchor: .bottom` 就是聊天界面用的列表：打开时显示最后一行；行数不够填满时，行贴着底部排列；只要停在末尾，就会一直保持在末尾，无论是末尾新增了行、行高发生了变化，还是列表变矮了（键盘弹出）。用户向上滚动就会离开末尾；滚回末尾，或调用 `scrollToEnd()`，又会重新粘住。`on startReached` 在最前面几行即将显示时调用，对每个不同的首项只触发一次：在这里加载更早的消息。

无论锚定在哪一端，在屏幕上可见的行之上添加或删除行，都不会让这些可见的行发生位移（即 React Native 的 `maintainVisibleContentPosition`）：无论是在顶部加载了更早的消息，还是删掉了上方的某一项，用户正在看的内容都不会跳走。只有在列表锚定的那一端，新增的行才会直接显示出来：从顶部开始阅读的列表是在顶部（比如来了一封新邮件），聊天列表则是在末尾。

```asuka
struct Message {
  id: int
  text: string
}

component Chat {
  state msgs: [Message] = []
  state next = 0

  let list = Ref()

  fn send(text: string) {
    next += 1
    msgs.push(Message(id: next, text: text))
    list.scrollToEnd() // one's own message shows, wherever the list was
  }

  Column {
    grow: 1
    List {
      ref: list
      anchor: .bottom
      on startReached { print("load older") }
      for m in msgs key m.id { Text(m.text) }
    }
    Button("Send") { on press { send("Hello") } }
  }
}
```

[`examples/chat`](../../examples/chat) 是一个完整的对话界面。

### 分组

`section` 把行分成若干组：它是从行索引到所在分组 key 的函数。相邻且 key 相同的行属于同一组，组的最上方是 `header` 返回的视图（比如聊天里的日期、通讯录里的首字母、邮箱里的“今天”和“昨天”）。`header` 拿到 key，并跟随它变化：组头本身也是一行，进入可视区域时才创建，之后会被复用到别的分组。

```asuka
struct Message {
  id: int
  day: string
  text: string
}

state msgs = [Message(id: 1, day: "Yesterday", text: "Hi"), Message(id: 2, day: "Today", text: "Hello")]

component Chat {
  List {
    section: i => msgs[i].day
    header: day => Text(day) {
      font: .footnote.semibold()
      padding: [6, 20]
    }
    for m in msgs key m.id { Text(m.text) }
  }
}
```

在 plain 或 bare 列表中，组头会停留在顶部，让本组的行从它下面滚过，直到被下一个组头推上去（即 UITableView 的 plain 样式；Android 没有这种机制，所以用的是浮在列表上方的一个单元格，绑定到当前位于顶部的分组；桌面端和 Web 上则就是组头行本身）。plain 列表的组头使用列表的背景；bare 列表的组头自己绘制背景（比如聊天中浮在消息上方的胶囊标签）。inset grouped 列表的每个分组各自成为一组，位于自己的组头之下，组头随分组一起滚动。

### 选择行

`selecting` 让列表进入选择模式（就像邮件里的“编辑”）：每一行显示平台的选择控件，点击一行会选中或取消选中它，而不再触发它的点击，行也不能滑动。行的 `selected` 表示它是否被选中（它会被跟踪，所以保存在状态里的选择会随时显示出来），`on select(on)` 接收用户的选择；应用没有采纳的选择会被恢复原样。行会因为选择控件占了位置而变窄。

```asuka
struct Message {
  id: int
  text: string
}

state msgs = [Message(id: 1, text: "Hi"), Message(id: 2, text: "Hello")]

component Mailbox {
  state selecting = false
  state chosen: [int] = []

  List {
    selecting: selecting
    for m in msgs key m.id {
      Row {
        padding: [12, 20]
        selected: chosen.contains(m.id)
        on select(on) {
          if on { chosen.push(m.id) } else { chosen.removeWhere(id => id == m.id) }
        }
        on selectBegin { selecting = true }
        Text(m.text)
      }
    }
  }
}
```

| | iOS | Android | macOS、Windows、Linux | Web |
|---|---|---|---|---|
| 选择控件 | UIKit 编辑模式下的圆圈 | 行末尾的 Material 复选框 | 高亮整行（使用强调色对应的选中颜色；Windows 上是 WinUI 列表的胶囊形指示条） | 行起始边的圆圈，选中时填上对勾，整行着色 |
| `on selectBegin` | 双指在行上拖动，并继续选中拖过的行 | 长按（同 Gmail） | — | — |
| 屏幕阅读器 | 每一行都可选择 | 每一行是一个复选框，以行的文字作为标签 | — | 每一行标明是否选中（`aria-selected`） |

### 网格

`columns` 把列表变成网格（类似 `LazyVGrid`、`GridLayoutManager`）：数据项按指定的列数逐行排列，每项宽度相同，横向和纵向都间隔 `gap`。每一行的高度取其中最高的那一项。网格没有分隔线，也不支持滑动操作。

```asuka
component Photos(ids: [int]) {
  List {
    columns: 3
    gap: 2
    for id in ids key id {
      Image("https://picsum.photos/id/{id}/300/300") {
        mode: .fill
        aspectRatio: 1
      }
    }
  }
}
```

### 在 Scroll 中用 List 还是 for

`List` 之外的 `for` 同样为每一项生成一个视图，key 的用法也一样，但它会一次性生成所有行，作为普通视图和兄弟视图并列。如果 `Scroll` 中只是页面的一小部分需要列表，比如几个标签、表单里的几行、标题下的一个小节，就用 `for`。

```asuka
component Tags(tags: [string]) {
  Scroll {
    grow: 1
    Column {
      padding: 20
      gap: 8
      Text("Tags") { font: .headline }
      for t in tags key t { Text(t) }
    }
  }
}
```

如果数据项很多或者数量没有上限（信息流、收件箱、搜索结果、照片），或者你需要滑动操作、分隔线、滚到末尾加载更多、网格布局，就用 `List`。`List` 自己就能滚动，不要把它放进 `Scroll`。

## 自定义外观

上面这些元素都是各平台自己的控件：不设置样式的应用在哪里看起来都是原生的，`Picker` 在 iOS 上是一个菜单，在 Windows 上是一个下拉列表。如果应用有自己的设计（每个控件都用品牌的配色、形状和动效，在手机上和浏览器里都一样），或者需要平台没有的控件（组合框、切换按钮组、范围滑块），`std/base` 提供的是只有行为、没有外观的组件：选择框、菜单、对话框、复选框、标签页、滑块等等。每个组件都会处理好自己的状态、键盘与焦点、屏幕阅读器，以及它浮在窗口上的位置；至于显示成什么样，完全取决于应用设置的样式。

```asuka
import std/base

style field {
  padding: [8, 12]
  radius: 8
  border: 1
  borderColor: .separator
  focusVisible { borderColor: .accent; border: 2 }
}

style menu {
  padding: 4
  radius: 10
  background: .background
  shadow: Shadow(radius: 12, y: 4)
}

style option {
  padding: [6, 10]
  radius: 6
}

component FruitPicker {
  state fruit = ""

  base.Select {
    bind value: fruit
    base.SelectTrigger {
      style: field
      base.SelectValue { placeholder: "Pick a fruit" }
    }
    popup {
      base.SelectPopup {
        style: menu
        for f in ["Apple", "Banana", "Cherry"] key f {
          base.SelectItem(f) {
            style: option
            highlighted { background: .accent.opacity(0.15) }
            selected { font: .headline }
            Text(f)
          }
        }
      }
    }
  }
}
```

一个组件由若干部件组成：一个持有状态的根（`Select`），以及由应用在根里排布、设置样式的部件（`SelectTrigger`、`SelectValue`、`SelectPopup`、`SelectItem`）。每个部件都接受 `style:` 和各种样式属性，它的状态是可以设置样式的变体，就像 `pressed { … }` 一样：`checked { … }`、`open { … }`、`highlighted { … }`、`selected { … }`、`disabled { … }`。根的值在绑定时（`bind value: fruit`）归调用方所有，否则归根自己所有。弹出层的部件放在根的 `popup` 插槽里，它们会显示在触发按钮旁边；`backdrop` 会把对话框后面的窗口调暗。

| 分类 | 组件 |
|---|---|
| 操作 | `Button`、`Toggle`、`ToggleGroup`、`Toolbar` |
| 选择 | `Checkbox`（`CheckboxGroup`）、`Switch`、`RadioGroup` 和 `Radio`、`Select`、`Listbox`、`Combobox` |
| 数值 | `Slider`、`NumberField`、`Progress`、`Meter` |
| 表单 | `Field`（它的标签、说明和错误信息）、`Input`、`Fieldset`、`Form` |
| 展开与收起 | `Collapsible`、`Accordion`、`Tabs` |
| 弹出层 | `Dialog`、`AlertDialog`、`Popover`、`Tooltip`、`PreviewCard`、`Menu`、`ContextMenu`、`Menubar` |
| 其他 | `Avatar`、`ScrollArea`、`Separator`、toast（`Toasts`、`ToastViewport`） |

它们的按键遵循各平台对同类控件的惯例：方向键在菜单、选择框或单选按钮组的选项之间移动，输入文字可以跳到对应的选项；Escape 会关闭弹出层，Android 上的返回键也一样；弹出层打开时焦点移进去，关闭时焦点回到触发按钮。`asuka doc std/base` 会列出每个部件以及它的属性和变体。

如果平台控件的作用不只是它的角色，就优先用平台控件：比如 iOS 日期选择器的滚轮、输入框的自动填充、分享面板。两者可以在同一个页面上混用。

### Overlay

`std/base` 的弹出层都由 `Overlay` 构成。`open` 为真时，它会把子视图显示在窗口之上、锚点旁边：

```asuka
component Hint {
  state open = false
  let anchor = Ref()

  Column {
    View {
      ref: anchor
      on press { open = !open }
      Text("What's this?")
    }
    Overlay {
      bind open: open
      anchor: anchor
      side: .bottom
      align: .start
      offset: 4
      on dismiss(reason) { print("closed by {reason}") }
      Text("Overlays are placed next to their anchor") {
        padding: 8
        background: .secondaryBackground
      }
    }
  }
}
```

| 键 | 作用 |
|---|---|
| `open` | 为真时显示子视图：打开时构建，关闭时销毁（在 `animate { }` 里会播放它们的过渡） |
| `anchor` | 它所靠近的视图（一个 `Ref`）；不设置时显示在窗口中（对话框、抽屉、toast） |
| `side`、`align` | 位于锚点的哪一侧（`.bottom`、`.top`、`.start`、`.end`，或 `.center` 覆盖在锚点上），以及沿这一侧的对齐方式（`.start`、`.center`、`.end`；`.stretch`：与锚点等长） |
| `offset`、`alignOffset` | 与锚点之间的间距，以及沿这一侧的偏移 |
| `flip` | 放不下时换到另一侧（默认为 true） |
| `edgeGap` | 与窗口边缘和安全区域之间的最小距离（默认为 8）；为了留在窗口内，它会沿所在的一侧移动，尺寸也不会超过剩余的空间 |
| `matchWidth` | 至少和锚点一样宽 |
| `modal` | 只有它能收到按压，屏幕阅读器也只能看到它 |
| `dismissOn` | 什么会关闭它：`.outsidePress`（不包括按在锚点上）、`.escape`、`.back`、`.focusOut`、`.anchorHidden`（锚点滚出视野）；默认全部启用 |
| `backdrop { … }` | 它打开时覆盖在后面窗口上的内容 |
| `on place(placed)` | 它最终所在的一侧、锚点中点沿这一侧的位置（箭头该放的地方），以及它可用的空间 |

锚点移动时（滚动、窗口大小变化、键盘弹出），浮层会跟着移动，空间不够时会翻到另一侧，在所有平台上都一样。它位于锚点所在的窗口中；锚点在弹出面板（sheet）里时，它显示在面板之上。

## 自定义视图

如果某个平台有 Asuka 没有提供的控件，你可以自己实现一个 `native component`：iOS 和 macOS 上用 Swift，Android 上用 Kotlin，Windows 和 Linux 上用 C，Web 上用 JavaScript。参见[原生代码](native.md)。
