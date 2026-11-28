# 自定义外观

Asuka 给应用提供两种搭建界面的方式，同一个页面里也可以两者混用：

- **平台控件**（`Button`、`Toggle`、`Picker`、`TextField`、`Sheet`……）：每个平台自己的控件。不设样式的应用看起来就和该平台上的其他应用一样，在 iOS 上是 UIKit，在 Android 上是 Material 3。见[组件](components.md)。
- **`std/base`**：只有行为、没有自带外观的组件。选择框、菜单、对话框、开关、标签页、滑块这些组件会处理好自己的状态、键盘与焦点、屏幕阅读器，以及浮在窗口上的位置；显示出来的只有应用给它们设的样式。因此应用在手机、桌面和浏览器里看起来都一样。

如果应用有自己的设计，比如每个控件都用上品牌的颜色、形状和动效，并且在哪里都一样，就像 Web 上的设计系统那样，那就选 `std/base`。它还提供了一些平台没有的控件：组合框、切换按钮组、范围滑块、窗口内的菜单栏和 toast。

如果平台控件做的不只是绘制，就继续用平台控件。比如 iOS 的日期选择器有滚轮，文本框有自动填充和系统的文本编辑，分享面板能连到平台上的其他应用。`std/base` 的 `Input` 底下就是一个设了样式的 `TextField`，所以输入、选择和键盘仍然是平台自己的。

## 一个完整的页面

`std/base` 随 Asuka 一起提供：写上 `import std/base`，它的组件就是 `base.Select`、`base.Switch`、`base.Dialog` 等。下面是一个用自己的主题做的设置卡片：

```asuka check
import std/base

/// 应用的外观：所有样式都从这里读取颜色和圆角。
store Theme(dark: bool) {
  pub let ink: Color = if dark { #f2f2f7 } else { #1c1c1e }
  pub let paper: Color = if dark { #18181c } else { #ffffff }
  pub let line: Color = if dark { #3a3a44 } else { #d1d1d6 }
  pub let accent: Color = #6d5dfc
  pub let corner: float = 10
}

style card {
  use t: Theme
  padding: 16
  gap: 12
  radius: t.corner
  background: t.paper
  border: 1
  borderColor: t.line
}

style field {
  use t: Theme
  direction: .row
  align: .center
  gap: 8
  padding: [8, 12]
  radius: t.corner
  border: 1
  borderColor: t.line
  focusVisible {
    border: 2
    borderColor: t.accent
  }
}

style option {
  use t: Theme
  padding: [6, 10]
  radius: t.corner - 4
  highlighted { background: t.accent.opacity(0.15) }
}

style track {
  use t: Theme
  width: 46
  height: 28
  radius: 14
  padding: 3
  background: t.line
  checked { background: t.accent }
}

style thumb {
  width: 22
  height: 22
  radius: 11
  background: #ffffff
  checked { margin: Edges(start: 18) }
}

style primary {
  use t: Theme
  padding: [8, 16]
  radius: 999
  background: t.accent
  pressed { opacity: 0.7 }
}

component Settings {
  use t: Theme
  state units = "metric"
  state sync = true
  state confirming = false

  Column {
    style: card

    base.Select {
      bind value: units
      labels: ["metric": unitName("metric"), "imperial": unitName("imperial")]
      base.SelectTrigger {
        style: field
        base.SelectValue {
          color: t.ink
          grow: 1
        }
        Symbol("chevron.down") { color: t.ink }
      }
      popup {
        base.SelectPopup {
          style: card
          padding: 4
          gap: 0
          for u in ["metric", "imperial"] key u {
            base.SelectItem(u) {
              style: option
              label: unitName(u)
              Text(unitName(u)) { color: t.ink }
            }
          }
        }
      }
    }

    Row {
      gap: 10
      align: .center
      base.Switch {
        style: track
        bind checked: sync
        label: "Sync"
        base.SwitchThumb { style: thumb }
      }
      Text("Sync") { color: t.ink }
    }

    base.Dialog {
      bind open: confirming
      base.DialogTrigger {
        style: primary
        Text("Reset…") { color: #ffffff }
      }
      backdrop { View { background: #00000066 } }
      popup {
        base.DialogPopup {
          style: card
          width: 300
          base.DialogTitle("Reset settings?") {
            color: t.ink
            font: .headline
          }
          base.DialogDescription("Units and sync go back to their defaults.") { color: t.ink }
          Row {
            gap: 8
            justify: .end
            base.DialogClose {
              style: option
              Text("Cancel") { color: t.ink }
            }
            base.Button {
              style: primary
              on press {
                units = "metric"
                sync = true
                confirming = false
              }
              Text("Reset") { color: #ffffff }
            }
          }
        }
      }
    }
  }
}

fn unitName(u: string) -> string = if u == "metric" { "Metric" } else { "Imperial" }

component Screen {
  provide Theme(dark: environment().dark)

  SafeArea {
    padding: 20
    Settings()
  }
}

app { Screen() }

test "units and a reset" {
  let app = mount(Screen())
  app.tap("Metric")
  app.tap("Imperial")
  app.expect("Imperial")
  app.tap("Reset…")
  app.expect("Reset settings?")
  app.tap("Reset")
  app.expectNo("Reset settings?")
  app.expect("Metric")
}
```

这张卡片在 iOS、Android、macOS、Windows、Linux 和 Web 上画出来都一样：颜色、圆角和间距都是应用自己定的。不同平台之间变化的，是用户对控件的预期：触摸屏上的点按和鼠标的点击、桌面上的键盘按键、Android 的返回键，以及每个平台自己的屏幕阅读器。

## 组成部分、插槽与状态

一个组件由几个部分组成。根组件持有状态（`Select`、`Dialog`、`Tabs`），应用在它里面摆放并设置各个部分的样式（`SelectTrigger`、`SelectValue`、`SelectPopup`、`SelectItem`）。每个部分只显示一个元素，所以它和 `View` 一样接受 `style:` 和各种样式属性，里面放什么由应用决定：一个符号、一张图片、一行文字都行。

- **值。** 根组件的值在绑定时（`bind value: units`、`bind checked: sync`、`bind open: confirming`）属于调用方，不绑定时归它自己。每次变化也会作为事件报告出来（`on valueChange(v)`、`on checkedChange(c)`、`on openChange(o)`）。
- **插槽。** 弹出内容的各个部分放在根组件的 `popup` 插槽里，根组件会把它显示在窗口上方、触发器旁边（对话框显示在中间，或者用 `side:` 贴在某条边上）。`backdrop` 是对话框后面盖住窗口的那一层。
- **状态。** 每个部分的状态都是应用可以设置样式的变体，就像给 `View` 写 `pressed { … }` 一样：`checked { … }`、`open { … }`、`highlighted { … }`、`selected { … }`、`disabled { … }`。样式里也可以写这些块（`style track { checked { … } }`），通过 ref 也能读取它们：写 `base.Switch as s`，然后读 `s.checked`。

`asuka doc std/base` 会列出每个部分的属性、事件和变体，[参考文档](std/base.md)里也有同样的内容。

## 主题

主题就是一个 store，里面放着样式要读取的值：颜色、圆角、间距、字体。样式用 `use` 取到它，`provide` 把它提供给一个页面里的组件：

```asuka
store Theme(dark: bool) {
  pub let ink: Color = if dark { #f2f2f7 } else { #1c1c1e }
  pub let accent: Color = #6d5dfc
}

style label {
  use t: Theme
  color: t.ink
}

component Screen {
  provide Theme(dark: environment().dark)
  Text("Hello") { style: label }
}
```

读取 `environment().dark` 就能在用户切换深色模式时跟着变化。如果主题本身是状态（比如用户在设置里选的），那么读取它的所有样式会一起切换。语义颜色（`.label`、`.accent`、`.separator`）也可以用，适合想在自己的形状里用平台颜色的场合。

## 组件一览

| 分组 | 组件 |
|---|---|
| 操作 | `Button`、`Toggle`、`ToggleGroup`、`Toolbar` |
| 选择 | `Checkbox`（`CheckboxGroup`）、`Switch`、`RadioGroup` 和 `Radio`、`Select`、`Listbox`、`Combobox` |
| 数值 | `Slider`、`NumberField`、`Progress`、`Meter` |
| 表单 | `Field`（及其标签、说明和错误）、`Input`、`Fieldset`、`Form` |
| 展开与收起 | `Collapsible`、`Accordion`、`Tabs` |
| 弹出 | `Dialog`、`AlertDialog`、`Popover`、`Tooltip`、`PreviewCard`、`Menu`、`ContextMenu`、`Menubar` |
| 其他 | `Avatar`、`ScrollArea`、`Separator`、toast（`Toasts`、`ToastViewport`） |

它们都是用 Asuka 写的，由 `View`、`Text`、`Overlay` 和样式属性搭成，就像 Base UI 是用 DOM 搭成的一样。

## 键盘、焦点与屏幕阅读器

它们的按键遵循各平台对同类控件的约定：

- 方向键在菜单、选择框、单选组或标签列表的项目之间移动，输入文字会跳到对应的项目。
- Escape 关闭弹出内容，Android 的返回键也一样。
- 弹出内容打开时焦点移进去，关闭时回到触发器。对话框打开期间焦点留在里面，窗口的其余部分对屏幕阅读器隐藏。
- Tab 在控件之间移动；一个单选组或一个标签列表只占一个 Tab 停靠点。
- `focusVisible { … }` 只在焦点是由键盘移动过来时才显示焦点框，点按时不显示。

每个部分都带有与自身相符的角色，所以各平台的屏幕阅读器（VoiceOver、TalkBack、讲述人、Orca 和浏览器的读屏）会把 `Switch` 读作开关，并说出它是开还是关。里面没有文字的开关或复选框要加上 `label:`，就像上面那个开关一样。见[无障碍](accessibility.md)。

## 动效

弹出内容在打开时创建、关闭时销毁。在 `animate { }` 里打开它，或者把它绑定到在 `animate { }` 里写入的状态上，它的视图声明的过渡就会播放：比如淡入并放大的菜单、从边缘滑出的抽屉。见[动画与手势](motion.md)。

## 测试

`std/base` 的组件和其他视图没什么不同，所以 `test` 块可以像用户那样操作它们：`app.tap("Metric")` 打开选择框，对某一项 `app.tap` 就选中它，`app.pressKey("down")` 在菜单里移动，`app.expect` 检查显示的内容。见[测试](testing.md)。

[`examples/base`](../../examples/base) 是一个有两套主题的设置页面，里面有表单、选择框、开关、复选框、提示、标签页、滑块、对话框和 toast。

## 自己动手写

`std/base` 的组件都是用每个应用都有的东西搭成的：带 `role`、`focusable`、`keys` 和无障碍属性的 `View`，以及用来显示浮在窗口上内容的 `Overlay`（[Overlay](components.md#overlay)）。它没有的控件也可以用同样的方式，在应用自己的包里写出来，用 `pub variant` 表示它的状态。见[组件声明的变体](language/components.md#组件声明的变体)。
