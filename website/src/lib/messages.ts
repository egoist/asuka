// The words of the site's own pages, in each language. (No React here: the
// Markdown downloads, built by the Vite config, take the site's description.)

import type { Locale } from "./locales.ts"

const en = {
  siteTitle: "Asuka — Native widgets in Asuka, on phones, desktops and the web",
  siteDescription:
    "Build apps in Asuka, a language made for interfaces, with each platform's own widgets on iOS, Android, macOS, Windows, Linux and the web. Each platform shows the app with its own widgets (UIKit, Material 3, AppKit, Win32, GTK 4 and libadwaita, the DOM), updated by fine-grained state, with live reload as you save and release builds for every platform. Or give it one look everywhere: std/base's components have the behavior, the keyboard and screen readers, and take the app's style.",
  nav: { docs: "Docs", references: "References", examples: "Examples", releases: "Releases", menu: "Navigation menu", license: "MIT License" },
  reference: {
    language: "Language",
    languageAbout: "The language apps are written in, a topic a page.",
    errors: "Error codes",
    errorsAbout: "Every error and warning's code: why the rule is there, and how to fix what breaks it.",
    std: "Standard library",
    platform: "Platform SDK",
    stdAbout: "The language's own packages, the same on every platform: import std/<name>.",
    platformAbout: "What wraps a device, an operating system's service or a store: import platform/<name>.",
    stdIntro:
      "The standard library is the language's own: JSON, time, URLs, files, HTTP, SQLite, the store, Markdown, charts… the same on every platform. The `asuka` CLI carries it, so a package needs no `asuka add`: import it and use it. Each page is the package's API and its documentation, from its source; `asuka doc std/<name>` prints the same in a terminal.",
    platformIntro:
      "The platform SDK wraps a device (the camera, location, Bluetooth, the sensors), an operating system's service (notifications, the share sheet, the Dock and the tray, the keychain) or a store (purchases, Apple Pay, reviews), with each platform's own APIs. The `asuka` CLI carries it: import a package and use it. Each page is the package's API and its documentation, from its source; `asuka doc platform/<name>` prints the same in a terminal.",
    package: "Package",
    about: "What it is",
    source: "View the source on GitHub",
    english: "",
  },
  language: { label: "Language", auto: "Auto" },
  theme: { label: "Theme", system: "System", light: "Light", dark: "Dark" },
  search: {
    button: "Search docs…",
    title: "Search the docs",
    description: "Find a page or a section of the docs.",
    placeholder: "Search the docs…",
    failed: "The search index could not be loaded.",
    loading: "Loading…",
    empty: (query: string) => `No results for “${query}”`,
  },
  docs: {
    overview: "Overview",
    introduction: "Introduction",
    more: "More",
    menu: "Menu",
    documentation: "Documentation",
    previous: "Previous episode",
    next: "Next episode",
    edit: "Edit this page on GitHub",
    copyCode: "Copy code",
    copyMarkdown: "Copy as Markdown",
    copied: "Copied",
    apiReference: "API reference",
    sync: "Sync",
  },
  notFound: {
    label: "Error 404 / Pattern unknown",
    title: "This page does not exist",
    text: "It may have moved. The docs, or their search, will have what you are looking for.",
    docs: "Read the docs",
    home: "Home",
    pageTitle: "Page not found · Asuka",
  },
}

export type Messages = typeof en

const zh: Messages = {
  siteTitle: "Asuka — 用 Asuka 写原生控件应用，手机、桌面、Web 全覆盖",
  siteDescription:
    "用 Asuka——一门专为界面设计的语言——开发应用，在 iOS、Android、macOS、Windows、Linux 和 Web 上使用各平台自己的控件（UIKit、Material 3、AppKit、Win32、GTK 4 与 libadwaita、DOM）。界面采用细粒度更新，保存即重载，每个平台都能直接打出发布构建。也可以在所有平台上使用同一种外观：std/base 的组件负责行为、键盘和屏幕阅读器，样式由应用决定。",
  nav: { docs: "文档", references: "参考", examples: "示例", releases: "版本发布", menu: "导航菜单", license: "MIT 许可证" },
  reference: {
    language: "语言",
    languageAbout: "编写应用所用的语言，每页一个主题。",
    errors: "错误代码",
    errorsAbout: "每个错误和警告的代码：这条规则为什么存在，违反时如何修正。",
    std: "标准库",
    platform: "平台 SDK",
    stdAbout: "语言自带的包，在每个平台上都一样：import std/<name>。",
    platformAbout: "封装设备、操作系统服务或应用商店的包：import platform/<name>。",
    stdIntro:
      "标准库是语言自带的库：JSON、时间、URL、文件、HTTP、SQLite、store、Markdown、图表……在每个平台上都一样。`asuka` 命令行工具自带标准库，所以不需要 `asuka add`，直接 import 就能用。每个页面都是该包的 API 及其文档，从源码生成；在终端里运行 `asuka doc std/<name>` 能看到同样的内容。",
    platformIntro:
      "平台 SDK 封装了设备（相机、定位、蓝牙、传感器）、操作系统的服务（通知、分享面板、Dock 和托盘、钥匙串）以及应用商店（内购、Apple Pay、评分），在每个平台上使用该平台自己的 API。`asuka` 命令行工具自带平台 SDK：直接 import 就能用。每个页面都是该包的 API 及其文档，从源码生成；在终端里运行 `asuka doc platform/<name>` 能看到同样的内容。",
    package: "包",
    about: "用途",
    source: "在 GitHub 上查看源码",
    english: "API 参考由源码中的注释生成，目前只有英文版。",
  },
  language: { label: "语言", auto: "自动" },
  theme: { label: "主题", system: "跟随系统", light: "浅色", dark: "深色" },
  search: {
    button: "搜索文档…",
    title: "搜索文档",
    description: "查找文档中的页面或章节。",
    placeholder: "搜索文档…",
    failed: "搜索索引加载失败。",
    loading: "加载中…",
    empty: (query: string) => `没有找到与“${query}”相关的内容`,
  },
  docs: {
    overview: "概览",
    introduction: "简介",
    more: "更多",
    menu: "目录",
    documentation: "文档",
    previous: "上一话",
    next: "下一话",
    edit: "在 GitHub 上编辑此页",
    copyCode: "复制代码",
    copyMarkdown: "复制为 Markdown",
    copied: "已复制",
    apiReference: "API 参考",
    sync: "同步率",
  },
  notFound: {
    label: "Error 404 / 图形不明",
    title: "页面不存在",
    text: "它可能已经换了地方。去文档里看看，或者搜索一下，应该能找到你要的内容。",
    docs: "阅读文档",
    home: "首页",
    pageTitle: "页面不存在 · Asuka",
  },
}

const ja: Messages = {
  siteTitle: "Asuka — Asuka で書く、ネイティブ UI のアプリ。スマートフォンにも、デスクトップにも、Web にも",
  siteDescription:
    "インターフェースのための言語 Asuka で書いたアプリを、iOS、Android、macOS、Windows、Linux、Web で、各プラットフォーム純正のウィジェット（UIKit、Material 3、AppKit、Win32、GTK 4 と libadwaita、DOM）で動かせます。画面はきめ細かな状態の更新で必要な部分だけが書き換わり、保存するたびにライブでリロードされます。リリースビルドもすべてのプラットフォーム向けに作れます。どこでも同じ見た目にすることもできます。std/base のコンポーネントが振る舞い、キーボード、スクリーンリーダーを受け持ち、スタイルはアプリが付けます。",
  nav: { docs: "ドキュメント", references: "リファレンス", examples: "サンプル", releases: "リリース", menu: "ナビゲーションメニュー", license: "MIT ライセンス" },
  reference: {
    language: "言語",
    languageAbout: "アプリを書く言語を、トピックごとのページで。",
    errors: "エラーコード",
    errorsAbout: "すべてのエラーと警告のコード。ルールがある理由と、破ったときの直し方。",
    std: "標準ライブラリ",
    platform: "プラットフォーム SDK",
    stdAbout: "言語に付属するパッケージで、どのプラットフォームでも同じです：import std/<name>。",
    platformAbout: "デバイス、OS のサービス、ストアを包むパッケージです：import platform/<name>。",
    stdIntro:
      "標準ライブラリは言語に付属するライブラリです。JSON、時刻、URL、ファイル、HTTP、SQLite、store、Markdown、チャートなどがあり、どのプラットフォームでも同じように動きます。`asuka` CLI に含まれているので `asuka add` は要らず、import するだけで使えます。各ページはパッケージの API とそのドキュメントで、ソースから生成されています。ターミナルで `asuka doc std/<name>` を実行しても同じ内容が表示されます。",
    platformIntro:
      "プラットフォーム SDK は、デバイス（カメラ、位置情報、Bluetooth、センサー）、OS のサービス（通知、共有シート、Dock とトレイ、キーチェーン）、ストア（課金、Apple Pay、レビュー）を、それぞれのプラットフォーム自身の API で包みます。`asuka` CLI に含まれているので、import するだけで使えます。各ページはパッケージの API とそのドキュメントで、ソースから生成されています。ターミナルで `asuka doc platform/<name>` を実行しても同じ内容が表示されます。",
    package: "パッケージ",
    about: "内容",
    source: "GitHub でソースを見る",
    english: "API リファレンスはソースのコメントから生成されており、現在は英語のみです。",
  },
  language: { label: "言語", auto: "自動" },
  theme: { label: "テーマ", system: "システムに合わせる", light: "ライト", dark: "ダーク" },
  search: {
    button: "ドキュメントを検索…",
    title: "ドキュメントを検索",
    description: "ドキュメントのページやセクションを探します。",
    placeholder: "ドキュメントを検索…",
    failed: "検索インデックスを読み込めませんでした。",
    loading: "読み込み中…",
    empty: (query: string) => `「${query}」に一致する結果はありません`,
  },
  docs: {
    overview: "概要",
    introduction: "はじめに",
    more: "その他",
    menu: "目次",
    documentation: "ドキュメント",
    previous: "前回",
    next: "次回",
    edit: "GitHub でこのページを編集",
    copyCode: "コードをコピー",
    copyMarkdown: "Markdown としてコピー",
    copied: "コピーしました",
    apiReference: "API リファレンス",
    sync: "シンクロ率",
  },
  notFound: {
    label: "Error 404 / パターン不明",
    title: "このページは存在しません",
    text: "移動した可能性があります。ドキュメントか、その検索で探しているものが見つかるはずです。",
    docs: "ドキュメントを読む",
    home: "ホーム",
    pageTitle: "ページが見つかりません · Asuka",
  },
}

export const messages: Record<Locale, Messages> = { en, zh, ja }
