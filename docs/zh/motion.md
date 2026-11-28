# 动效与手势

只要平台自带动画机制，Asuka 就直接用它：iOS 上是 UIView 动画，Android 上是 animator，macOS 上是 AppKit 的隐式动画和 Core Animation，浏览器里是 Web Animations API。Windows 和 GTK 没有针对视图 frame（位置与尺寸）的动画，所以在 Windows 和 Linux 上由 Asuka 自己逐帧推进，用的也是同样的曲线。你只要在 `animate` 里修改状态，这次修改在屏幕上引起的移动就会以动画呈现。本页依次介绍：为变化添加动画、进入与退出过渡、共享元素、逐帧动画、触摸/鼠标/触控板手势、触感反馈，以及减弱动态效果。

## 为变化添加动画

把写入操作包进 `animate`，这次写入在屏幕上带来的变化就会以动画呈现：包括布局移动的 frame，以及下文列出的那些视觉属性。

```asuka
component Details {
  state expanded = false

  Column {
    gap: 12
    padding: 20
    Button("More") {
      on press { animate(() => { expanded = !expanded }) }
    }
    View {
      height: if expanded { 240 } else { 80 }
      radius: 16
      background: .secondaryBackground
    }
  }
}
```

方块变高时，它下面的内容也会跟着往下滑：布局只运行一次，每个变了的 frame 都会以动画移到新位置。你不需要自己去给位置做动画。

同样的写入如果不放在 `animate` 里，就会立刻生效。和所有状态写入一样，`animate` 也在主线程上运行（`spawn` 的工作线程要先把结果交回来）。它会立刻返回；`await animate(…)` 则会一直等到动画结束。

## 时长、曲线与重复

`animate(Animation(…), body)` 接受动画的时间参数：

- `duration` 是一个 `Duration`（默认 `350ms`；`0s` 表示 250ms）。
- `curve` 可以是 `.easeInOut`（默认）、`.linear`、`.easeIn`、`.easeOut` 或 `.spring`。弹簧动画是一个带阻尼的弹簧，会在 `duration` 结束时停稳：iOS 上用的是 UIKit 的弹簧动画，Android、Windows、Linux 和 Web 上则用同一个弹簧的插值器。`Animation.spring`、`.linear` 和 `.quick`（200ms）是现成的。
- `damping` 是弹簧的阻尼比：为 1 时不越过终点就停下，小于 1 时会冲过头再弹回来（0 表示 0.85，稍有回弹）。`velocity` 是弹簧的初速度，以“每秒走完整段变化的几分之几”来表示（即 UIKit 的 `initialSpringVelocity`），用来延续一次快速滑动（fling）的速度。
- `delay` 是开始前等待的时长。
- `done` 会在动画逻辑上完成时调用（即 `delay` 加上每一次重复的 `duration` 都过去之后），也就是 `await animate(…)` 返回的时候；可以在这里接上下一步。
- macOS 上 AppKit 的隐式动画既没有弹簧也没有延迟：回弹明显的弹簧动画会改用一条会冲过头的曲线，延迟则直接跳过。
- `repeat` 表示再额外重复多少次；`Animation.forever` 会一直重复，直到这些属性再次改变。设置了 `autoreverses` 时，每一轮都会先倒放回去，再开始下一轮。macOS 和 Windows 不支持重复：动画只运行一次。

```asuka
component Pulse {
  state on = false

  Row {
    gap: 16
    View {
      width: 24
      height: 24
      radius: 12
      background: .red
      transform: if on { Transform(scale: 1.6) } else { Transform() }
      opacity: if on { 0.35 } else { 1 }
    }
    Button("Pulse") {
      on press {
        animate(Animation(duration: 800ms, curve: .easeInOut, repeat: Animation.forever, autoreverses: true), () => { on = true })
      }
    }
    // 不带动画地设置：脉冲随即停止。
    Button("Stop") { on press { on = false } }
  }
}
```

手势结束时，往往接着播放一个弹簧动画。把手势的速度除以剩下的距离，就得到弹簧的 `velocity`，这样视图就能保持手指甩出去的速度：

```asuka
import std/math

component Flung {
  state dragX = 0.0
  state dragY = 0.0

  View {
    width: 64
    height: 64
    radius: 32
    background: .accent
    transform: Transform(x: dragX, y: dragY)
    on pan(g) {
      if g.state != .ended {
        // 跟随手指。
        dragX = g.translationX
        dragY = g.translationY
        return
      }
      let d = math.hypot(g.translationX, g.translationY)
      // 沿返回路径、朝向原位的速度。
      let v = if d > 1 { -(g.velocityX * g.translationX + g.velocityY * g.translationY) / d / d } else { 0.0 }
      animate(Animation(duration: 600ms, curve: .spring, damping: 0.55, velocity: v), () => {
        dragX = 0
        dragY = 0
      })
    }
  }
}
```

前后相接的几步动画可以依次 await 来串起来（也可以用 `done`）。比如一个 toast 提示：滑入、停留一会儿、再消失：

```asuka
component Toast {
  state shown = false

  async fn show() {
    await animate(() => { shown = true })
    await animate(Animation(delay: 1500ms), () => { shown = false })
  }

  Column {
    Button("Save") { on press { await show() } }
    if shown {
      Text("Saved") { transition: .slide }
    }
  }
}
```

要停止一个重复动画，只需不带动画地再次设置它的属性。Android 上只有透明度和变换能重复；iOS、Linux 和 Web 上所有能动画的属性都能重复；macOS 和 Windows 则只运行一次。

## 哪些变化会动画

| 变化 | iOS | Android | macOS | Windows | Linux | Web |
| --- | --- | --- | --- | --- | --- | --- |
| frame（位置和尺寸，来自布局） | 是 | 是 | 是 | 是 | 是 | 是 |
| `opacity` | 是 | 是 | 是 | 否 | 是 | 是 |
| `transform` | 是 | 是 | 是 | 是 | 是 | 是 |
| `background`（纯色） | 是 | 是 | 否 | 否 | 否 | 是 |
| `radius` | 是 | 是 | 是 | 否 | 否 | 否 |
| `Shape` 的 `trimStart` 和 `trimEnd` | 是 | 是 | 是 | 否 | 是 | 否 |
| `Shape` 的路径 | 是 | 否 | 是 | 否 | 否 | 否 |

其他属性（`borderColor`、`shadow`、`gradient`、文字颜色和字体）都会立即改变。只有已经在屏幕上的视图才会动画；在 `animate` 期间新出现的视图不会以动画进入，除非它设置了[过渡](#进入与退出)。

淡出到 `opacity: 0` 的视图仍然占着布局。想让一个视图淡出并离开布局，就用 `Transition.opacity` 过渡把它移除。

## 变换

`transform` 会在绘制时以视图中心为原点，对它进行平移、缩放和旋转。布局看不到变换，相邻的视图都待在原处不动。`Transform` 包括：

- `x`、`y`：偏移量，单位为点。
- `scale`：同时缩放两个轴（默认值 1 表示原始大小）；`scaleX`、`scaleY` 只缩放其中一个轴。
- `rotation`：角度，顺时针方向。

```asuka
component Flip {
  state flipped = false

  View {
    height: 120
    radius: 20
    background: .accent
    transform: if flipped { Transform(scale: 1.04, rotation: 6) } else { Transform() }
    on press { animate(Animation(duration: 500ms, curve: .spring), () => { flipped = !flipped }) }
  }
}
```

## 进入与退出

`transition` 描述的是一个状态：视图在 `animate` 期间被添加时（比如 `if` 变为 true，或 `for` 里新增了一项），会从这个状态进入；在 `animate` 期间被移除时，会退出到这个状态。被移除的视图会一直留在屏幕上，直到退出动画结束，在此期间它不响应触摸。

- `.opacity`：淡入淡出。
- `.pop`：淡入的同时从 90% 放大进入（适合弹出框、角标）。
- `.slide`：从下方 24 点处淡入（适合 toast、新增的行）。
- `Transition(…)`：自定义过渡，包括 `fade`、`scale`（0 表示不缩放），以及以点为单位的偏移 `x`、`y`。它们可以组合使用：`Transition(fade: true, scale: 0.5, y: -8)`。

```asuka
import platform/accessibility

component SaveToast {
  state shown = false

  async fn save() {
    animate(() => { shown = true })
    accessibility.announce("Saved")
    await sleep(1500ms)
    animate(() => { shown = false })
  }

  View {
    grow: 1
    Button("Save") { on press { await save() } }
    if shown {
      View {
        absolute: true
        left: 20
        right: 20
        bottom: 20
        background: .secondaryBackground
        radius: 16
        padding: [14, 18]
        transition: .slide
        Text("Saved")
      }
    }
  }
}
```

过渡只在 `animate` 内部运行，并沿用它的时长和曲线。同样的 `shown = true` 如果不在 `animate` 里，toast 会直接出现。另外，只有父视图已经在屏幕上时，子视图才会以过渡进入。

## 共享元素

`shared` 是一个键，让两个视图彼此对应，作用类似 SwiftUI 的 `matchedGeometryEffect`。在 `animate` 期间，如果一个带这个键的视图离开、另一个带同样键的视图出现，新视图就会从旧视图的位置和大小长成自己的样子，旧视图则跟着一起移动并淡出。下面的例子里，一行展开成一张卡片，封面和标题随之移动（取自 [`examples/gallery`](../../examples/gallery)）：

```asuka
component Matched {
  state open = false

  fn toggle() { animate(Animation(duration: 500ms, curve: .spring), () => { open = !open }) }

  if open {
    View {
      gap: 12
      align: .center
      on press { toggle() }
      Art(240, 24)
      Text("Night Drive") {
        font: .title2
        shared: "album-title"
      }
    }
  } else {
    Row {
      gap: 12
      align: .center
      on press { toggle() }
      Art(56, 10)
      Text("Night Drive") {
        font: .headline
        shared: "album-title"
      }
    }
  }
}

component Art(size: float, corner: float) {
  View {
    width: size
    height: size
    radius: corner
    background: .orange
    shared: "album"
  }
}
```

- 所有平台都支持，包括桌面端和 Web。
- 两个视图必须在同一次 `animate` 中变化：一个被移除，一个被添加。
- 键属于整个根节点：同一时刻，每个键只能有一个视图持有。
- 带键离开却找不到对应视图的视图，会直接淡出。

## 缩放进入新页面

`zoomFrom` 指定下层页面上某个视图的 `shared` 键。新页面推入时会从这个视图中放大展开，弹出时再缩回去。`List` 的单元格也可以用，比如下面这个照片网格：

```asuka
component Photos {
  let ids = [10, 11, 12, 13, 14, 15]

  List {
    columns: 3
    gap: 2
    for id in ids key id {
      View {
        label: "Photo"
        aspectRatio: 1
        shared: "photo-{id}"
        on press { push(Photo(id)) }
        Image("https://picsum.photos/id/{id}/300/300") {
          mode: .fill
          grow: 1
        }
      }
    }
  }
}

screen Photo(id: int) {
  title: "Photo"
  zoomFrom: "photo-{id}"
  Image("https://picsum.photos/id/{id}/1200/1200") {
    mode: .fit
    grow: 1
  }
}
```

- **iOS 18 及以上**使用 UIKit 的缩放过渡，交互式返回手势会把页面缩回到那个视图里。更早的版本按普通方式推入。
- **Android** 使用 Material 的容器变换（container transform）：页面从视图处长到自己的边界，带圆角，两者在靠近视图的地方交叉淡化。预测性返回手势会把它缩回去。
- **macOS、Windows、Linux 和 Web** 按普通方式推入。
- 如果下层页面上没有带这个键的视图，也按普通方式推入。

这里不需要 `animate`：推入本身就带动画。详见[导航](navigation.md)。

## 自己画出来的图形

`Shape` 的 `trimStart` 和 `trimEnd` 决定轮廓绘制出来的那一段，取值从 0 到 1。在 `animate` 里修改它们，描边就会一点点画出来，适合做进度环、对勾之类的效果。

```asuka
import std/math

fn ring(w: float, h: float) -> Path {
  let r = min(w, h) / 2 - 4
  return Path().arc(w / 2, h / 2, r, start: -math.pi / 2, end: 3 * math.pi / 2)
}

component Ring {
  state done = 0.3

  Column {
    gap: 12
    Shape(ring) {
      stroke: .accent
      lineWidth: 8
      lineCap: .round
      trimEnd: done
      width: 72
      height: 72
    }
    Button("Finish") {
      on press { animate(Animation(duration: 800ms), () => { done = 1 }) }
    }
  }
}
```

## 逐帧动画

如果运动是算出来的而不是声明出来的（物理模拟、游戏、跟随数值变化的仪表盘），可以用 `frames()`：它是显示器绘制的帧（每秒 60 或 120 帧），以一个由帧时间组成的流给出，在 task 里用 `for await` 读取。循环里写入的内容会在这一帧显示出来。它会一直运行，直到 task 被取消，也就是组件离开时。没有任何东西读取它时，就不会请求帧。（`onFrame(fn)` 则是在每一帧调用一个函数，直到调用它返回的闭包为止。）

```asuka
component Spinner {
  state angle = 0.0

  task {
    var last: Duration? = none
    for await t in frames() {
      if let l = last {
        if !environment().reduceMotion {
          angle = (angle + (t - l).seconds() * 180) % 360 // 每秒转半圈
        }
      }
      last = t
    }
  }

  View {
    a11yHidden: true
    transform: Transform(rotation: angle)
    Symbol("gear") {
      font: .largeTitle
      color: .accent
    }
  }
}
```

运动要根据两帧之间的时间来计算，而不是根据帧数：不同设备的帧率并不相同。

## 手势

`View` 可以接收手势，识别工作交给平台自己的手势识别器：

- `on press` 和 `on longPress`：轻点，以及按住不放。
- `on doubleTap(x, y)`：快速连点两下，以及第二下落在哪里。
- `on pan(g)`：拖动。
- `on pinch(g)`：双指张开或捏合。
- `on rotate(g)`：双指旋转。
- `on swipe(direction)`：快速轻扫，并带上方向 `SwipeDirection`：`.left`、`.right`、`.up` 或 `.down`。

使用鼠标或触控板时，同样这些处理函数会收到桌面端对应的操作：

| 手势 | macOS | Windows | Linux | Web |
| --- | --- | --- | --- | --- |
| `on press` | 单击 | 单击；或在获得焦点的视图上按 Enter、空格 | 单击；或在获得焦点的视图上按 Enter、空格 | 单击；或在获得焦点的视图上按 Enter、空格 |
| `on longPress` | 辅助点按，或按住鼠标键不放 | 右键单击，或按住鼠标键不放 | 辅助点击，或按住鼠标键不放 | 右键单击，或按住鼠标键、手指按住不放 |
| `on doubleTap` | 双击 | 双击 | 双击 | 双击，或连点两下 |
| `on pan(g)` | 按住鼠标键拖动 | 按住鼠标键拖动 | 按住鼠标键拖动、在触摸屏或触控板上拖动 | 按下指针拖动 |
| `on swipe(direction)` | 结束时速度很快的拖动；触控板上的双指轻扫；系统留给应用处理时的三指轻扫 | 结束时速度很快的拖动 | 结束时速度很快的拖动；触摸屏上的轻扫 | 结束时速度很快的拖动 |
| `on pinch(g)` | 触控板捏合 | 触控板捏合、Ctrl 加滚轮，或触摸屏上的双指操作 | 触控板或触摸屏捏合，或 Ctrl 加滚轮 | 触控板捏合、Ctrl 加滚轮，或触摸屏上的双指操作 |
| `on rotate(g)` | 触控板旋转 | 触摸屏上的双指旋转 | 触控板或触摸屏旋转 | 触摸屏上的双指旋转 |

很少有人会按住鼠标键不放来看看会发生什么，所以在重要的地方，`on longPress` 的功能也要提供另一个入口，比如一个菜单项或一个按钮。在 Windows 和 Linux 上，Ctrl 加滚轮每滚一格捏合 10%，滚轮停下后捏合就结束。在 Mac 触控板上双指轻扫时，视图外围能滚动的内容照样会滚动：只有外围在那个方向上没有可滚动的内容时，才会触发向上或向下的轻扫。

拖动、捏合和旋转在进行过程中，会不断用一个 `Gesture` 调用对应的函数：

- `state`：依次为 `.began`、`.changed`，最后是 `.ended`；如果触摸被系统接管，则是 `.cancelled`。
- `x`、`y`：触摸点在视图中的位置，单位为点。
- `translationX`、`translationY`：拖动从手指按下以来移动的距离，因此 `x - translationX` 就是按下的位置。
- `velocityX`、`velocityY`：拖动的速度，单位为点每秒。
- `scale`：捏合开始以来的缩放比例（拖动时为 1）。
- `rotation`：旋转开始以来转过的角度，单位为弧度，顺时针方向。

被拖动的视图通过变换跟随手指，松手后弹回原位：

```asuka
component Puck {
  state dragX = 0.0
  state dragY = 0.0

  View {
    width: 64
    height: 64
    radius: 32
    background: .accent
    transform: Transform(x: dragX, y: dragY)
    on pan(g) {
      if g.state == .ended || g.state == .cancelled {
        animate(Animation(duration: 450ms, curve: .spring), () => {
          dragX = 0
          dragY = 0
        })
        return
      }
      dragX = g.translationX
      dragY = g.translationY
    }
  }
}
```

缩放和旋转都是从这次手势开始时算起的，所以要记住上一次手势结束时的值，下一次在它的基础上累加。同一个视图上的多个手势可以同时生效：一边捏合一边旋转。

```asuka
import std/math

component Turner {
  state angle = 0.0
  state base = 0.0
  state scale = 1.0
  state baseScale = 1.0

  View {
    label: "A square to turn"
    width: 160
    height: 160
    radius: 24
    background: .purple
    transform: Transform(scale: scale, rotation: angle)
    on rotate(g) {
      angle = base + g.rotation * 180 / math.pi
      if g.state == .ended || g.state == .cancelled { base = angle }
    }
    on pinch(g) {
      scale = min(max(baseScale * g.scale, 0.5), 2)
      if g.state == .ended || g.state == .cancelled { baseScale = scale }
    }
  }
}
```

拖动、捏合或旋转一旦开始，就独占了这次触摸：外层的滚动视图不会把它抢走，里面可按压的视图也不会收到按压，就像从按钮上开始的拖动不会触发按钮一样。

开着屏幕阅读器时，用户无法完成轻扫或拖动。所以请把它们的功能也作为[无障碍操作](accessibility.md)提供出来。

### 单击与双击

同时有 `on press` 和 `on doubleTap` 的视图，会像平台那样把两者区分开（UIKit 的 `require(toFail:)`、Android 的 `onSingleTapConfirmed`）：单击的 press 会等到不可能再有第二下，也就是过了系统的双击时限才运行；双击只运行 `on doubleTap`。只有 `on press` 的视图则立刻响应，不会等待。

```asuka
component Post {
  state liked = false
  state open = false

  View {
    height: 240
    radius: 16
    background: .secondaryBackground
    justify: .center
    align: .center
    label: "Photo"
    on press { open = true }
    on doubleTap(x, y) { liked = true }
    Symbol(if liked { "heart.fill" } else { "heart" }) { color: .red }
  }
}
```

### 哪些拖动算拖动手势

有三个键决定拖动手势怎样被识别，作用类似 React Native Gesture Handler 的 `activeOffsetX` 和 `simultaneousWithExternalGesture`：

- `panAxis: .horizontal`（或 `.vertical`）：只有沿这个方向的拖动才算。先沿另一个方向移动的拖动会交给外层，所以一行能横向拖动，所在的列表照样能上下滚动。
- `panMinDistance`：拖动要移动多少点之后才开始（不设置时用平台的触摸阈值）。
- `panWithScroll: true`：拖动手势和外层的滚动视图一起跟随这次拖动，而不是由拖动手势独占触摸（比如一个拖动时也滚动内容的面板把手）。

```asuka
component SwipeCard {
  state dx = 0.0

  View {
    height: 96
    radius: 16
    background: .accent
    panAxis: .horizontal
    panMinDistance: 16
    transform: Transform(x: dx)
    on pan(g) {
      dx = if g.state == .ended || g.state == .cancelled { 0 } else { g.translationX }
    }
  }
}
```

## 按下与悬停状态

`View` 会在交互状态变化时报告出来，样式可以通过三种方式跟随这些状态。从第一种开始，能满足需求就用它：

1. **变体（variant）**，用于视图自身的外观。`pressed { … }`、`hovered { … }`、`dragging { … }` 和 `dragOver { … }` 会在视图处于对应状态时叠加在其余样式之上。它们相当于 CSS 的 `:active` 和 `:hover`，或者 Tailwind 的 `active:` 和 `hover:`。
2. **`Ref`**，用于其他视图，以及其他任何需要跟随状态的东西。给视图设置一个 `Ref`（`ref: card`），然后就可以在任何需要的地方读取 `card.pressed` 或 `card.hovered`，比如另一个视图的样式、`Text` 或 `if`。子视图跟随父视图（相当于 group 的 hover）、兄弟视图跟随相邻视图（相当于 peer 的 hover），都是这么实现的。
3. **处理函数**，用于样式之外的事情，比如开始录音：`on pressedChange(on)` 和 `on hoverChange(on)`。指针在视图上移动时，`on hover(x, y)` 会带着它的位置（以视图左上角为原点的点数）运行，可以用来画图表的十字准线或放大镜。

```asuka
component Card(title: string) {
  event open

  let card = Ref()

  Row {
    ref: card
    padding: 16
    radius: 12
    background: .secondaryGroupedBackground
    on press { emit open }
    hovered { background: .secondaryBackground }
    pressed {
      background: .separator
      transform: Transform(scale: 0.97)
    }
    // 标题跟随卡片的悬停状态：相当于 group 的 hover。
    Text(title) {
      grow: 1
      color: if card.hovered { .accent } else { .label }
    }
  }
}
```

各个状态的含义：

- **Pressed**：手指或鼠标主键在视图上按下时为 true。
  - 在可滚动的容器里，它会稍晚一些才变为 true，免得一滚动就闪一下。
  - 手指抬起、移出视图（移回来会再次按下），或者触摸被滚动、长按或其他手势接管时，它就会变回 false。
  - 只有在按下状态中松开，才会触发 `on press`。
- **Hovered**：指针位于视图或其子视图上方时为 true。指针可以是鼠标、触控板、iPad 的指针，或者悬停的 Apple Pencil、手写笔。单纯的触摸屏永远不会产生悬停。
- **Dragging**：视图正在被拖动（通过它的 `dragItems`）时为 true。
- **DragOver**：有一个会被它的 `dropTypes` 接收的拖动经过它上方时为 true。

变体的组合规则：

- 变体只设置它写明的键，其余的键保持样式原有的值。
- 变体按 hovered、pressed、dragging、dragOver 的顺序应用，所以按下状态优先于悬停状态。
- 和所有绑定一样，一个键也可以直接跟随某个状态：`hidden: !row.hovered` 让视图只在这一行被悬停时显示。

```asuka
component Person(name: string) {
  event remove

  let row = Ref()

  Row {
    ref: row
    Text(name) { grow: 1 }
    // 鼠标悬停在这一行上时，才显示删除按钮。
    View {
      hidden: !row.hovered
      label: "Delete"
      on press { emit remove }
      Symbol("trash")
    }
  }
}
```

样式也可以用同样的方式跟随任何状态：`background: if selected { .accent } else { .clear }`。

视图只追踪有人读取的状态，也就是它的变体用到的，或者从它的 `Ref` 读取的状态。

- 如果一个视图既没有 `pressed` 变体，也没有人读取 `pressed`，它就不会报告按下状态，并保留平台自带的高亮效果。
- 视图一旦报告按下状态，就由它自己负责绘制，平台自带的高亮（Android 的水波纹、Windows 的悬停和按下填充）会被关掉。

每个状态每变化一次只报告一次。视图在某个状态中消失时会退出该状态，因此跟随它的东西不会卡在那里。

没有 `on press` 时，按下不放就只是单纯的按住，比如“按住录音”。录音属于副作用，所以要放在处理函数里：

```asuka
import platform/record

component HoldToRecord {
  View {
    label: "Hold to record"
    padding: 16
    pressed { background: .red }
    on pressedChange(on) {
      if on {
        try? await record.audio(maxDuration: 60s)
      } else {
        record.stopAudio()
      }
    }
    Symbol("mic")
  }
}
```

## 拖放

`dragItems` 让视图可以被拖动。在触摸屏上，长按后再移动就能把它拿起来，而且这次长按不会再触发 `on longPress`。用鼠标时，按住鼠标键移动就能拖动它。视图的图像会跟着手指或指针移动，既可以在应用内拖动，也可以拖到其他应用里。`dropTypes` 则让视图可以接收拖放。

一个 `DragItem` 可以是一段文本、一个链接（`kind: .link`）、一个文件（`path`），或者应用自己的一个值（`value`，一个字符串，比如 id）：

- 在应用内部，放下时拿到的就是原样的条目，连同值一起。
- 其他应用拿到的是条目的文本、链接或文件。只带有值的条目不会传到其他应用。
- `mime` 是条目的类型，拖放目标根据它来决定是否接收。文本、链接和文件会自动填好类型。请给 `value` 设置一个应用专属的类型，这样只有应用自己的目标会接收它。

```asuka
struct Task {
  id: string
  title: string
  done: bool = false
}

component TaskCard(t: Task) {
  View {
    dragItems: [DragItem(value: t.id, mime: "application/x-myapp-task", text: t.title)]
    padding: 12
    // 拖动时变淡。
    dragging { opacity: 0.4 }
    Text(t.title)
  }
}

component DoneColumn {
  prop children: View
  event move(id: string)

  View {
    dropTypes: ["application/x-myapp-task"]
    gap: 8
    padding: 8
    border: 2
    borderColor: .clear
    // 有任务拖到上方时显示描边。
    dragOver { borderColor: .accent }
    on drop(d) {
      for it in d.items { emit move(it.value) }
    }
    children
  }
}
```

拖动源：

- `dragItems` 是一个绑定，所以它读取的内容变化时会自动更新。一次拖动携带的是开始拖动那一刻的条目。
- `on dragPhase(phase)` 在视图被拿起时报告 `.began`。之后要么报告 `.dropped`，表示有目标接收了它（如果是移动操作，就把被拖动的东西删掉）；要么报告 `.cancelled`。拖动中的外观请用 `dragging { … }` 变体或它的 `Ref` 的 `dragging`。

拖放目标：

- `dropTypes` 列出 MIME 类型：可以是精确类型（`"text/plain"`）、一类类型（`"image/*"`）或任意类型（`"*/*"`）。
- 如果一次拖动里没有这些类型的条目，它就根本不会进入这个视图。
- 目标嵌套时，由最内层愿意接收的那个目标获得这次拖动。
- `on dropOver(over)` 报告拖动进入和离开视图，与 `dragOver { … }` 变体是同一个状态。
- `on dropMove(x, y)` 报告拖动在视图上的位置，可以当作插入点。
- `on drop(d)` 拿到它接收的条目以及放下的位置。`d.local` 表示这次拖动来自应用自身。

从其他应用拖进来的内容（访达或“文件”App 里的文件、“照片”里的照片、浏览器里的链接），会等条目加载完成后再送达：

- 在 iOS 和 Android 上，文件会先被复制到应用的缓存目录，此时 `d.copied` 为 true。请把文件移到应用存放它们的地方，或者用完后删掉。
- 在 macOS、Windows 和 Linux 上，`path` 就是用户的原文件，不要删除它。
- 在浏览器里，放下的文件会被复制到应用的文件中（属于这个页面，存放在内存里），此时 `d.copied` 为 true。

## 按键

键盘焦点位于某个视图（它本身或它里面的视图）时，这个视图可以通过 `keys` 接收按键：它是一组 `Shortcut`，每个包含一个按键和要执行的操作。按键会从获得焦点的视图开始向外查找，交给最近的那个处理它的视图，而且优先于获得焦点的控件和菜单栏。这样，搜索框的上下方向键就可以在下面的结果中移动：

```asuka
component Search {
  prop results: [string] = []
  event open(result: string)
  state query = ""
  state sel = 0

  fn move(by: int) { sel = max(0, min(sel + by, results.count() - 1)) }

  TextField {
    bind text: query
    keys: [Shortcut(key: "down", run: () => move(1)), Shortcut(key: "up", run: () => move(-1))]
    on submit {
      if sel < results.count() { emit open(results[sel]) }
    }
  }
}
```

一个按键可以是一个字符，也可以是一个具名按键（`enter`、`escape`、`tab`、`space`、`delete`、`backspace`、`up`、`down`、`left`、`right`、`home`、`end`、`pageup`、`pagedown`、`f1` 到 `f12`，以及电视遥控器的播放/暂停键 `playpause`：见[电视](tv.md#遥控器)），修饰键必须与给定的完全一致：`Shortcut(key: "d", primary: true, run: …)`、`Shortcut(key: "up", alt: true, run: …)`。按住不放会重复触发。不带修饰键的字符会截走视图内输入框的输入，所以请给它加上修饰键，或者把它放在没有输入框的地方。

在组件体里调用 `useKeys([…])`，给窗口设置它自己的按键：按键最后才会到达这里；而在没有任何东西获得焦点时，则最先到达这里，比如游戏的方向键或查看器的空格键。它们的生命周期跟随调用它的那个组件。

按键来自硬件键盘，也就是桌面电脑、iPad 或 Chromebook 上的键盘，以及浏览器收到的键盘输入。不要让按键成为完成某件事的唯一方式，因为手机通常没有键盘。输入法正在组字时（比如日文或中文输入法），按键归输入法处理。`examples/launcher` 就用上下方向键在搜索结果中移动。

## 触感反馈

`device.haptic` 播放触感反馈：`.selection`（数值切换了一格），`.light`、`.medium` 和 `.heavy`（碰撞），以及 `.success`、`.warning` 和 `.error`（操作结果）。

```asuka
import platform/device

component Pager {
  state page = 0

  View {
    grow: 1
    on swipe(d) {
      if d == .left {
        page += 1
      } else if d == .right {
        page = max(page - 1, 0)
      }
      device.haptic(.selection)
    }
    on longPress { device.haptic(.medium) }
    Text("Page {page + 1}")
  }
}
```

在 iOS 上，它们对应 `UIFeedbackGenerator` 的各种反馈。Android 没有完全对应的反馈：Asuka 会从 `performHapticFeedback` 的常量里挑最接近的来播放。成功、警告和错误在 Android 11 及以上对应 confirm 和 reject，更早的版本则用按键或长按的反馈代替。

在配有 Force Touch 触控板的 Mac 上，`.selection` 对应 `NSHapticFeedbackManager` 的 alignment，其他几种对应它的 level change；只有手指放在触控板上时才能感受到。Windows 和 Linux 没有触感反馈，`device.haptic` 在那里什么也不做。在浏览器里，如果浏览器支持 `navigator.vibrate`（Android 上的 Chrome），它会短促地振动一下，其他浏览器则什么也不做。

## 减弱动态效果

用户要求减少动态效果时，`environment().reduceMotion` 为 true。用户更改设置时，读取它的地方也会随之更新：

- **iOS**：设置 > 辅助功能 > 动态效果 > 减弱动态效果。
- **Android**：关闭动画（在开发者选项中，或在无障碍 > 移除动画中）。
- **macOS**：系统设置 > 辅助功能 > 显示 > 减弱动态效果。
- **Windows**：设置 > 辅助功能 > 视觉效果，关闭“动画效果”。
- **Linux**：关闭 GNOME 的动画（设置 > 无障碍 > 减少动画（Reduce Animation）；即 GTK 的 `gtk-enable-animations`）。
- **Web**：浏览器的 `prefers-reduced-motion`，它跟随系统设置。

Asuka 自己会做的处理：

- **iOS**：过渡改为原地溶解，不再移动或缩放，和 UIKit 自己的过渡在这种设置下的表现一致；共享元素（`shared`）也不再飞过去，视图按各自的过渡出现和消失。其他动画照常运行。
- **Android**：系统的动画时长缩放为 0，因此所有动画，无论是 Asuka 的还是平台的，都会立即结束。
- **macOS、Windows、Linux 和 Web**：`animate` 里的变化会立即生效，过渡和共享元素也一样。导航栈的推入和弹出在 macOS 上不再滑动，在浏览器里改为交叉淡化，在 Linux 上则和关闭动画后的 libadwaita 导航表现一致。

剩下需要你自己处理的，是 Asuka 无法判断是否只起装饰作用的动效：`frames()` 循环（就像上面的加载图标那样停下来）、反复播放的脉冲、让某样东西移动很远的动画。读取 `reduceMotion`，相应地减少这些动效。
