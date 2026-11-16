# 测试

应用的测试就是写在代码旁边的 `test` 块。`mount` 在没有设备、窗口和显示器的情况下挂载一个组件，测试像用户一样，根据屏幕上显示的内容来操作它：

```asuka
component Counter {
  state count = 0
  Column {
    Text("Count: {count}")
    Button("Add") { on press { count += 1 } }
  }
}

test "Add counts" {
  let app = mount(Counter())
  app.tap("Add")
  app.tap("Add")
  app.expect("Count: 2")
}
```

```sh
asuka test                    # 当前目录下的应用或包
asuka test --run Add          # 名称中包含 Add 的测试
asuka test path/to/app
```

```
ok   Add counts
FAIL Remove empties the list (app.asuka:41)
     the screen doesn't show "No items"
     View 0,0 390x844
       Text 0,0 390x20 "Item 1"
asuka: 1 of 2 tests failed
```

只要能运行命令行工具的地方都能运行测试，CI 中也一样，不需要 iOS 模拟器或 Android 模拟器：`asuka test` 会把包连同其中的测试一起编译，然后在这台电脑上运行；第一次运行时会构建测试运行器，这需要一个 C 编译器。有测试失败时，它以状态码 1 退出。如果要使用各平台真实的视图，参见下文的[在各平台上测试](#在各平台上测试)。

`asuka run file.asuka` 会编译单个文件并以同样的方式运行：先运行其中的测试（如果有的话），再运行它的应用，并打印屏幕上显示的内容。想试一个组件或函数时，不必为此创建一个应用。

## 工作原理

`mount` 把视图挂载到一个代替平台的替身上。它会记下应用创建的视图，但不会真的创建平台的视图。除此之外，一切都和应用真实运行时一样：它的状态、effect 和 task，Asuka 的布局、导航、列表绑定行、弹出面板（sheet）和警告框。

应用会像在 iPhone 上一样运行（iOS，390 × 844 点）。`asuka test --platform android`（或 `mac`、`windows`、`linux`、`web`）可以让它以为自己运行在另一个平台上，用来测试应用按平台区分的行为：测试也会为那个平台编译，包含各个包为该平台准备的文件，`@platform` 也是那个平台。

唯一和平台不同的是文字的测量方式：不管什么字体，每个字符 8 点宽，每行 20 点高。视图的位置来自 Asuka 真实的布局计算，所以视图之间的相对位置是可信的，但精确到点的尺寸并不是。

时间只在没有其他东西可以运行时才会推进：测试里的 `sleep(5s)`，或者某个 task 的定时器，都不会真的花时间；而一个等待某件永远不会发生的事的测试会立即失败（“the test never finished”）。

## 屏幕上显示什么

测试用用户看到的文字来指称界面元素：一段文本、按钮的标题、输入框的文字或占位文字、无障碍标签、页面的标题、标签页、警告框的按钮。文字需要完全匹配。

```asuka
app.expect("Inbox")       // 不显示时测试失败
app.expectNo("Message 1") // 显示时测试失败
print(app.dump())         // 屏幕上显示的内容，每行一个视图
```

只有用户能看到的内容才算数：

- 每个导航栈最上面的页面（它下面的页面不显示）；
- 选中的标签页的内容，以及标签栏中每个标签页的标题；
- 显示在其余内容之上的弹出面板、警告框和弹出框（popover）；
- 列表中在其高度内放得下的行：Asuka 按平台的请求绑定列表的行，`asuka test` 也一样，就像集合视图（collection view）那样；
- 不包括任何隐藏的内容。

检查失败时，消息里会附上屏幕内容：每个显示出来的视图，连同它的类型、frame 和文字，让你看到那里实际显示的是什么；`asuka test` 还会指出测试所在的位置（`app.asuka:41`）。

## 像用户一样操作

每个操作在返回之前都会让应用稳定下来：它改变的内容已经完成布局，列表已经绑定，应用的 task 投递的工作也已经运行。

```asuka
app.tap("Add")                      // 按钮、行、标签页、开关、分段控件的段、菜单项
app.type("ada@example.com", into: "Email")  // 占位文字或标签为 Email 的输入框
app.tapLink("https://asuka.dev")     // RichText 中指向它的链接
app.share([SharedItem(kind: .link, text: "https://asuka.dev")])  // 另一个应用分享了它：onShare 会收到
await app.idle()                    // 直到没有任何东西要运行：task、定时器、加载
```

点按会落在手指会落下的地方。行里的文字会点按这一行，按钮里的文字会点按这个按钮。带菜单的视图会打开菜单，下一次 `tap` 就从菜单中选择。

## 平台服务与原生包

[原生包](native.md)的接口由平台实现，而 `asuka test` 没有这些实现：调用它们会抛出错误，就像在没有这项服务的平台上一样。通过 store 或 `provide` 把组件要用的东西交给它，测试则提供自己的版本：

```asuka
test "archiving removes the row" {
  let app = mount(Inbox(), provide: Mailbox.fake([sample]))
  await app.idle()
  app.tap("Archive")
  app.expectNo("Hello")
}
```

## 在各平台上测试

`asuka test` 测试的是应用的行为：显示什么，以及用户操作时会发生什么。每个平台如何绘制它，是平台自己的事。要在各平台上查看你的应用：

- `asuka dev --platform ios|android|web` 会在 iOS 模拟器、Android 模拟器、手机或浏览器中运行应用，并在保存时重新加载。
- 在桌面端，`ASUKA_SNAPSHOT=out.png` 会在窗口显示后把它保存为 PNG，不抢占焦点，然后退出（`ASUKA_DUMP=1` 还会在旁边写出视图树）。`ASUKA_SCRIPT` 会先执行一些步骤（`tap:Add`、`type:hello`）：参见[开发](development.md#快照与脚本)。
- 在 Linux 上，在 Xvfb 下（`xvfb-run -a dbus-run-session`）运行，就能在没有显示器的环境（比如 CI）中做到这一点。
