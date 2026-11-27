# preview-setting

预览的设置项不存在，或者它的值不是直接写出来的。

编译器报告的错误：修正之前程序无法构建。

## 原因

预览的设置项决定 `asuka dev` 怎样显示它：`dark`（bool）、`fontScale`（文字大小，默认大小的倍数，0.5 到 4）、`lang`（语言标签）、`rtl`（bool），以及 `width` 和 `height`（框的尺寸，单位是点）。播放器在预览的代码运行之前就应用这些设置，所以每个值都要直接写出来，不能是程序算出来的；预览按名字列出，名字是不带插值的字符串。

## 如何修正

写预览支持的设置项，并把值写成字面量：`(dark: true, fontScale: 1.5, lang: "ar")`。

## 错误示例

```asuka
component Card {
  Text("Hi")
}

let big = 1.5

preview "Large text" (fontScale: big, theme: .dark) {
  Card()
}
```

## 正确示例

```asuka
component Card {
  Text("Hi")
}

preview "Large text, dark" (fontScale: 1.5, dark: true) {
  Card()
}
```
