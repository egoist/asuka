# no-children

给不接受子视图的元素传了子视图，或者传的子视图不对。

编译器报告的错误：修正之前程序无法构建。

## 原因

元素声明了 `prop children: View`（或者本身是容器）才接受子视图；`Alert` 的代码块里只能放它的按钮和输入框，`List` 的代码块里只能有一个 `for`，它的每一项就是一行。

## 如何修正

把视图放进容器（`Column`、`Row`），或者放在这个元素旁边。`List` 里别的种类的行（标题、状态）也要作为 `for` 的一项：用一个枚举表示行的种类。

## 错误示例

```asuka
component Note {
  Text("Title") {
    Text("Body")
  }
}
```

## 正确示例

```asuka
component Note {
  Column {
    Text("Title")
    Text("Body")
  }
}
```
