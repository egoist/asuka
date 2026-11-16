# 语言参考

Asuka 应用用 Asuka 语言编写。这是一门小巧的语言，专为使用各平台原生控件的界面而设计。它是静态类型的：发布构建时会预先编译；开发时则原地重新加载，所以保存后几毫秒内就能看到改动，所有的值和原生视图都会保留下来。

```asuka
component Counter {
  state count = 0

  SafeArea {
    justify: .center; align: .center; gap: 16; padding: 24

    Text("Count: {count}") { font: .largeTitle.bold() }
    Row {
      gap: 12
      Button("Reset") { variant: .gray; on press { count = 0 } }
      Button("Add") { variant: .filled; on press { count += 1 } }
    }
    if count >= 10 {
      Text("That's a lot") { color: .secondaryLabel }
    }
  }
}

app { Counter() }
```

这些页面按主题介绍这门语言的全部内容。初次了解可以先读[Asuka 速览](../language-tour.md)；元素本身（`Text`、`Button`、`List`、`Stack` ……）另有专门的页面：[组件](../components.md)。

## 各页内容

- [文件、包与导入](packages.md)：文件、包与导入；特定平台的文件；应用、程序与库
- [值与类型](types.md)：值与类型、运算符、内置方法、值语义
- [声明](declarations.md)：let、var、state 等声明；结构体与枚举
- [函数与闭包](functions.md)：函数与闭包；接口与泛型
- [控制流](control-flow.md)：if、match、循环与模式；特定平台的代码（@platform）
- [组件](components.md)：组件：组件体、处理函数、双向绑定、视图列表、effect 与 task
- [Store](stores.md)：在使用它的组件之间共享的状态
- [异步](async.md)：异步函数、task、流、spawn；需要加载的数据
- [错误](errors.md)：抛出、捕获，以及作为值的错误
- [样式](styles.md)：样式，以及给视图命名以便引用
- [动画](animation.md)：animate { }、动画中的值，以及逐帧的工作（onFrame）
- [绘制](drawing.md)：Canvas、Shape、路径与着色器
- [系统功能](system.md)：应用向系统请求的事（提示框、分享面板、触感反馈、窗口），以及它收到的事件（URL、文件、退出）
- [导航](navigation.md)：屏幕、标签页与导航栈；push、present、pop 与 dismiss
- [多语言文本](languages.md)：其他语言的文本
- [原生代码](native.md)：原生接口与原生组件
- [测试](tests.md)：测试块、mount，以及操作和检查应用

## 内存与性能

没有垃圾回收器，所以也就没有停顿。值采用引用计数，并在写入时复制。状态、store、effect 和 task 都属于创建它们的组件，并随它一起销毁。

- 一次变化只会运行读取了变化内容的那些绑定，也只把变化了的 prop 发给平台，每帧一次。
- 发布构建会预先编译，所以你发布的应用里没有解释器。
- 列表只构建屏幕上可见的部分，并复用单元格。

## 关键字

`app as async await bind break catch component continue defer do effect
else emit enum event extern false fn for if import in interface key let load
match native none on prop provide pub return screen self spawn state
store struct style task test throw throws true try type use var while`

`shader`（[着色器](../components.md#着色器)的声明）和 `at`（带路由的屏幕的模式）只在那个位置是关键字，在别处都是普通的名字。
