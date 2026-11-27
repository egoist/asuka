# preview-setting

A preview's setting it doesn't take, or not a value written as it is.

An error the compiler reports: the program doesn't build until it's fixed.

## Why

A preview's settings say how `asuka dev` shows it: `dark` (a bool), `fontScale` (the text size, a multiple of the default, from 0.5 to 4), `lang` (a language tag), `rtl` (a bool) and `width` and `height` (a frame's size, in points). The player applies them before the preview's code runs, so each is a value written as it is, not one the program computes; and a preview is listed by its name, a string without interpolations.

## How to fix

Name a setting the preview takes, and write its value as a literal: `(dark: true, fontScale: 1.5, lang: "ar")`.

## Wrong

```asuka
component Card {
  Text("Hi")
}

let big = 1.5

preview "Large text" (fontScale: big, theme: .dark) {
  Card()
}
```

## Right

```asuka
component Card {
  Text("Hi")
}

preview "Large text, dark" (fontScale: 1.5, dark: true) {
  Card()
}
```
