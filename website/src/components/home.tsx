import { Link } from "@tanstack/react-router"
import { ArrowRightIcon } from "lucide-react"
import type { ReactNode } from "react"

import { InstallCommand } from "@/components/install-command"
import { LiveCounter } from "@/components/live-counter"
import { Wordmark } from "@/components/logo"
import { Translation } from "@/components/translation"
import type { HomeData } from "@/lib/home"
import { alternates, messages, routes, type Locale } from "@/lib/i18n"
import { site } from "@/lib/site"
import { cn } from "@/lib/utils"

// The platforms an app runs on, each with the widgets it is shown with: the
// units, of which Asuka is the pilot.
const platforms = ["iOS", "Android", "macOS", "Windows", "Linux", "Web"]

// The six features' numerals, as a title card numbers episodes.
const numerals = ["壱", "弐", "参", "肆", "伍", "陸"]

/** The words of the home page, in a language. */
interface Copy {
  toolkits: string[]
  hero: {
    status: string
    framework: string
    title: string
    subtitle: string
    lead: string
  }
  start: string
  spec: { label: string; lines: { line: string; note: string; red?: boolean }[] }
  translation: { label: string; aside: string; heading: ReactNode }
  units: { label: string; aside: ReactNode; heading: ReactNode; unit: (n: string) => string; active: string; sync: string }
  hello: {
    label: string
    aside: string
    heading: ReactNode
    source: string
    run: string
    every: string
    runs: [string, string][]
    note: (examples: ReactNode) => ReactNode
    examples: string
  }
  episodes: { label: string; aside: string; heading: string; features: { title: string; text: string; href: string }[] }
  finale: { label: string; motto: ReactNode; begin: string; docs: string }
  runsOn: string
}

const en: Copy = {
  toolkits: ["UIKit", "Android Views · Material 3", "AppKit", "Win32, drawn as Windows 11", "GTK 4 · libadwaita", "The DOM"],
  hero: {
    status: "Language: Asuka / Widgets: native / Sync 100.0%",
    framework: "Framework:",
    title: "Native widgets, in Asuka.",
    subtitle: "UIKit, Material, AppKit, Win32, GTK and the DOM. Not a canvas, not a web view.",
    lead: "Write one app in Asuka, a language made for interfaces, for phones, desktops and the web. Each platform shows it with its own controls, its text input and its accessibility, and only what changed is updated.",
  },
  start: "Get started",
  spec: {
    label: "Specification",
    lines: [
      { line: "One language.", note: "Asuka, made for interfaces" },
      { line: "Six platforms.", note: "iOS · Android · macOS · Windows · Linux · Web" },
      { line: "Zero web views.", note: "Each platform's own widgets", red: true },
    ],
  },
  translation: {
    label: "Translation",
    aside: "Pick a platform",
    heading: (
      <>
        Asuka in.
        <br />
        <span className="text-unit">Native out.</span>
      </>
    ),
  },
  units: {
    label: "Deployment",
    aside: (
      <>
        Pilot: Asuka
        <br />
        Units: 6 / 6 active
      </>
    ),
    heading: (
      <>
        One pilot.
        <br />
        <span className="text-unit">Six units.</span>
      </>
    ),
    unit: (n) => `Unit ${n}`,
    active: "Active",
    sync: "Sync 100%",
  },
  hello: {
    label: "Episode 00 / Hello",
    aside: "One file. Every platform.",
    heading: (
      <>
        Hello, <span className="text-unit">native.</span>
      </>
    ),
    source: "Source",
    run: "Run it",
    every: "Every platform",
    runs: [
      ["asuka new hello && cd hello", ""],
      ["asuka dev", "this computer: Mac, PC or Linux"],
      ["asuka dev --platform ios", "the iOS simulator"],
      ["asuka dev --platform android", "an emulator or a phone"],
      ["asuka dev --platform web", "your browser"],
      ["asuka test", "the file's test blocks"],
    ],
    note: (examples) => (
      <>
        Save a file and the app reloads in milliseconds, its state kept, on a phone too; its tests live beside it. See {examples}: the gallery shows every
        component.
      </>
    ),
    examples: "the examples",
  },
  episodes: {
    label: "Six episodes",
    aside: "Read on in the docs",
    heading: "What it does",
    // What the README says, in short, each with the page that says more.
    features: [
      {
        title: "The platform's own widgets",
        text: "Navigation, lists, fields, pickers, sheets and menus that look and feel as each platform's do, because they are each platform's, with its motion and gestures.",
        href: "/docs/components",
      },
      {
        title: "A language for interfaces",
        text: "Components declare what they show, state says what changes, async and await, tests beside the code. The compiler checks what a binding reads and what runs on the main thread.",
        href: "/docs/language",
      },
      {
        title: "Fine-grained updates",
        text: "Components run once. A change to state updates only the props that read it, once a frame, without diffing a tree.",
        href: "/docs/concepts",
      },
      {
        title: "Live as you save",
        text: "asuka dev runs the app on your computer, the simulator, an emulator, a phone or a browser, and reloads it on save with the app's state kept.",
        href: "/docs/development",
      },
      {
        title: "Batteries, and C",
        text: "A standard library (std/: SQLite with live queries, HTTP, JSON, Markdown) and a platform SDK (platform/: the camera, photos, location, notifications, purchases, Bluetooth), and any C library through extern.",
        href: "/docs/stdlib",
      },
      {
        title: "Ready to ship",
        text: "asuka build makes a signed .ipa for the App Store, an .aab for Google Play, a notarized Mac app and its .dmg, a signed Windows .exe, Linux's .deb, .rpm, AppImage and Flatpak manifest, and a static site.",
        href: "/docs/shipping",
      },
    ],
  },
  finale: {
    label: "Next episode",
    // After NERV's motto, from Browning: God's in his heaven, all's right with the world.
    motto: (
      <>
        <Tall>Asuka&rsquo;s in its core.</Tall>
        <Tall className="mt-[0.08em]">
          <span className="text-unit">All&rsquo;s native</span> on
        </Tall>
        <Tall className="mt-[0.08em]">every screen.</Tall>
      </>
    ),
    begin: "Begin",
    docs: "Read the docs",
  },
  runsOn: "Runs on",
}

const zh: Copy = {
  toolkits: ["UIKit", "Android Views · Material 3", "AppKit", "Win32，Windows 11 外观", "GTK 4 · libadwaita", "DOM"],
  hero: {
    status: "语言：Asuka / 控件：原生 / 同步率 100.0%",
    framework: "框架：",
    title: "跨平台原生控件，用 Asuka 写。",
    // No breaks inside a phrase: Chinese breaks between any two characters.
    subtitle: "UIKit、Material、AppKit、Win32、GTK 和 DOM。不是 GPU 自绘，也不是 Web View。",
    lead: "用 Asuka——一门专为界面设计的语言——写一个应用，就能跑在手机、桌面和 Web 上。每个平台都用自己的控件、输入法和无障碍功能来呈现它，只更新真正变化的部分。",
  },
  start: "快速上手",
  spec: {
    label: "规格",
    lines: [
      { line: "一种语言。", note: "Asuka，为界面而生" },
      { line: "六个平台。", note: "iOS · Android · macOS · Windows · Linux · Web" },
      { line: "零 Web View。", note: "全是各平台自己的控件", red: true },
    ],
  },
  translation: {
    label: "转译",
    aside: "选一个平台看看",
    heading: (
      <>
        写的是 Asuka，
        <br />
        <span className="text-unit">跑的是原生。</span>
      </>
    ),
  },
  units: {
    label: "出击",
    aside: (
      <>
        驾驶员：Asuka
        <br />
        机体：6 / 6 启动
      </>
    ),
    heading: (
      <>
        一名驾驶员，
        <br />
        <span className="text-unit">六台机体。</span>
      </>
    ),
    unit: (n) => `${n} 号机`,
    active: "运行中",
    sync: "同步率 100%",
  },
  hello: {
    label: "第 00 话 / Hello",
    aside: "一个文件，所有平台。",
    heading: (
      <>
        你好，<span className="text-unit">原生。</span>
      </>
    ),
    source: "源码",
    run: "运行",
    every: "全平台",
    runs: [
      ["asuka new hello && cd hello", ""],
      ["asuka dev", "本机：Mac、PC、Linux"],
      ["asuka dev --platform ios", "iOS 模拟器"],
      ["asuka dev --platform android", "Android 模拟器或手机"],
      ["asuka dev --platform web", "浏览器"],
      ["asuka test", "文件里的测试块"],
    ],
    note: (examples) => <>保存文件，应用几毫秒内就会重新加载，状态原样保留，在手机上也是如此；测试就写在代码旁边。更多用法请看{examples}，其中的 gallery 展示了全部组件。</>,
    examples: "示例",
  },
  episodes: {
    label: "全六话",
    aside: "详见文档",
    heading: "它能做什么",
    features: [
      {
        title: "平台原生控件",
        text: "导航、列表、输入框、选择器、弹出面板和菜单，外观和手感都与各平台一致——因为它们本来就是平台自己的控件，动画和手势也是。",
        href: "/zh/docs/components",
      },
      {
        title: "为界面而生的语言",
        text: "组件声明自己显示什么，状态说明什么会变；有 async 和 await，测试就写在代码旁边。编译器会检查绑定读取了什么、什么代码在主线程上运行。",
        href: "/zh/docs/language",
      },
      {
        title: "细粒度更新",
        text: "组件只运行一次。状态变化时，只有读取它的属性才会更新，每帧一次，无需比对整棵树。",
        href: "/zh/docs/concepts",
      },
      {
        title: "保存即生效",
        text: "asuka dev 能在你的电脑、iOS 模拟器、Android 模拟器、手机或浏览器上运行应用，保存后自动重载，应用状态不丢失。",
        href: "/zh/docs/development",
      },
      {
        title: "开箱即用，还能调 C",
        text: "标准库（std/：支持实时查询的 SQLite、HTTP、JSON、Markdown）和平台 SDK（platform/：相机、相册、定位、通知、内购、蓝牙），还能通过 extern 直接调用任何 C 库。",
        href: "/zh/docs/stdlib",
      },
      {
        title: "随时上架",
        text: "asuka build 能打出提交 App Store 的签名 .ipa、提交 Google Play 的 .aab、经过公证的 Mac 应用及其 .dmg、签名的 Windows .exe、Linux 的 .deb、.rpm、AppImage 与 Flatpak 清单，还有静态网站。",
        href: "/zh/docs/shipping",
      },
    ],
  },
  finale: {
    label: "下回预告",
    // 化用 NERV 的箴言（出自勃朗宁）：神在天堂，世间一切安好。
    motto: (
      <>
        <Tall>Asuka 在核心，</Tall>
        {/* Chinese characters fill the em, so the lines need more room than Latin's. */}
        <Tall className="mt-[0.22em]">
          万屏<span className="text-unit">皆原生。</span>
        </Tall>
      </>
    ),
    begin: "开始",
    docs: "阅读文档",
  },
  runsOn: "可运行于",
}

const ja: Copy = {
  toolkits: ["UIKit", "Android Views · Material 3", "AppKit", "Win32（Windows 11 の外観）", "GTK 4 · libadwaita", "DOM"],
  hero: {
    status: "言語：Asuka / ウィジェット：ネイティブ / シンクロ率 100.0%",
    framework: "フレームワーク：",
    title: "全機種純正部品、Asuka で。",
    // Breaks only after punctuation (break-keep), as in Chinese.
    subtitle: "UIKit、Material、AppKit、Win32、GTK、そして DOM。キャンバス描画でも、Web View でもない。",
    lead: "インターフェースのための言語 Asuka でアプリを一つ書けば、スマートフォンでも、デスクトップでも、Web でも動きます。表示するのは各プラットフォーム純正のコントロールで、文字入力もアクセシビリティもそのまま。更新されるのは、変わった部分だけです。",
  },
  start: "はじめる",
  spec: {
    label: "仕様",
    lines: [
      { line: "言語は一つ。", note: "Asuka、インターフェースのための言語" },
      { line: "舞台は六つ。", note: "iOS · Android · macOS · Windows · Linux · Web" },
      { line: "Web View はゼロ。", note: "すべて各プラットフォーム純正のウィジェット", red: true },
    ],
  },
  translation: {
    label: "変換",
    aside: "プラットフォームを選択",
    heading: (
      <>
        書くのは Asuka。
        <br />
        <span className="text-unit">動くのはネイティブ。</span>
      </>
    ),
  },
  units: {
    label: "出撃",
    aside: (
      <>
        パイロット：Asuka
        <br />
        機体：6 / 6 起動
      </>
    ),
    heading: (
      <>
        パイロットは一人。
        <br />
        <span className="text-unit">機体は六機。</span>
      </>
    ),
    // Unit 01 is 初号機, the first; then 弐号機, 参号機 and on, as the units are named.
    unit: (n) => `${["初", "弐", "参", "四", "伍", "六"][Number(n) - 1] ?? n}号機`,
    active: "稼働中",
    sync: "シンクロ率 100%",
  },
  hello: {
    label: "第 00 話 / Hello",
    aside: "ファイルは一つ。動くのは全プラットフォーム。",
    heading: (
      <>
        こんにちは、<span className="text-unit">ネイティブ。</span>
      </>
    ),
    source: "ソース",
    run: "実行",
    every: "全プラットフォーム",
    runs: [
      ["asuka new hello && cd hello", ""],
      ["asuka dev", "このコンピューター：Mac、PC、Linux"],
      ["asuka dev --platform ios", "iOS シミュレーター"],
      ["asuka dev --platform android", "Android エミュレーターか実機"],
      ["asuka dev --platform web", "ブラウザー"],
      ["asuka test", "ファイルのテストブロック"],
    ],
    note: (examples) => (
      <>
        ファイルを保存すると、アプリは数ミリ秒でリロードされ、状態もそのまま残ります。スマートフォンでも同じです。テストはコードのすぐ隣に書けます。ほかの使い方は{examples}
        をどうぞ。gallery ではすべてのコンポーネントを見られます。
      </>
    ),
    examples: "サンプル",
  },
  episodes: {
    label: "全六話",
    aside: "続きはドキュメントで",
    heading: "できること",
    features: [
      {
        title: "純正のウィジェット",
        text: "ナビゲーション、リスト、入力欄、ピッカー、シート、メニュー。見た目も手触りも、モーションもジェスチャーも各プラットフォームのものと同じです。各プラットフォームのウィジェットそのものなのですから。",
        href: "/ja/docs/components",
      },
      {
        title: "インターフェースのための言語",
        text: "コンポーネントは表示するものを宣言し、状態は変わるものを表します。async と await があり、テストはコードの隣に。バインディングが何を読み、何がメインスレッドで動くかはコンパイラーが確かめます。",
        href: "/ja/docs/language",
      },
      {
        title: "きめ細かな更新",
        text: "コンポーネントは一度しか実行されません。状態が変わると、それを読むプロパティだけが、フレームごとに一度だけ更新されます。ツリーの差分は取りません。",
        href: "/ja/docs/concepts",
      },
      {
        title: "保存すれば、すぐ動く",
        text: "asuka dev はアプリをコンピューター、シミュレーター、エミュレーター、実機、ブラウザーで動かし、保存するたびに状態を保ったままリロードします。",
        href: "/ja/docs/development",
      },
      {
        title: "全部入り、C も",
        text: "標準ライブラリ（std/：ライブクエリ付きの SQLite、HTTP、JSON、Markdown）とプラットフォーム SDK（platform/：カメラ、写真、位置情報、通知、アプリ内課金、Bluetooth）。そして extern で、どんな C ライブラリでも呼べます。",
        href: "/ja/docs/stdlib",
      },
      {
        title: "すぐにリリース",
        text: "asuka build は、App Store 向けの署名済み .ipa、Google Play 向けの .aab、公証済みの Mac アプリとその .dmg、署名済みの Windows .exe、Linux の .deb、.rpm、AppImage と Flatpak マニフェスト、そして静的サイトを作ります。",
        href: "/ja/docs/shipping",
      },
    ],
  },
  finale: {
    label: "次回予告",
    // 上田敏が訳したブラウニングの一節、エヴァの結びの言葉「神は天にいまし、すべて世は事も無し」から。
    motto: (
      <>
        <Tall>Asuka はコアにいまし、</Tall>
        {/* Kana and kanji fill the em, so the lines need more room than Latin's. */}
        <Tall className="mt-[0.22em]">すべて画面は</Tall>
        <Tall className="mt-[0.22em]">
          <span className="text-unit">ネイティブ</span>なり。
        </Tall>
      </>
    ),
    begin: "発進",
    docs: "ドキュメントを読む",
  },
  runsOn: "動作環境：",
}

const copies: Record<Locale, Copy> = { en, zh, ja }

export function homeHead(locale: Locale) {
  const t = messages[locale]
  return {
    meta: [
      { title: t.siteTitle },
      { name: "description", content: t.siteDescription },
      { property: "og:title", content: t.siteTitle },
      { property: "og:description", content: t.siteDescription },
    ],
    links: alternates("/"),
  }
}

/** The button that starts the story: Unit red, two corners cut. */
function Start({ locale, className, children }: { locale: Locale; className?: string; children: ReactNode }) {
  return (
    <Link
      to={routes[locale].page}
      params={{ _splat: "getting-started" }}
      className={cn(
        "chamfer inline-flex h-12 items-center gap-2 bg-unit px-6 font-mono text-[13px] font-semibold tracking-[0.14em] text-white uppercase transition-[filter] hover:brightness-110 focus-visible:outline-2 focus-visible:outline-offset-2",
        className
      )}
    >
      {children} <ArrowRightIcon className="size-4" />
    </Link>
  )
}

/** Mincho, stretched up as a title card stretches it. */
function Tall({ children, className }: { children: ReactNode; className?: string }) {
  return <span className={cn("block origin-bottom-left scale-y-[1.14]", className)}>{children}</span>
}

/** A section's heading: a numbered label over a line of mincho. */
function Heading({ label, children, aside }: { label: string; children: ReactNode; aside?: ReactNode }) {
  return (
    <div className="inner flex items-end justify-between gap-6 pt-24 pb-10">
      <div>
        <p className="label flex items-center gap-2 text-signal-ink">
          <span className="size-1.5 bg-signal" />
          {label}
        </p>
        <h2 className="display mt-4 text-[clamp(2.5rem,6vw,4.5rem)] leading-[0.98]">{children}</h2>
      </div>
      {aside && <div className="label hidden text-right md:block">{aside}</div>}
    </div>
  )
}

export function Home({ locale, data }: { locale: Locale; data: HomeData }) {
  const t = copies[locale]
  const units = platforms.map((name, i) => ({ name, toolkit: t.toolkits[i]! }))
  return (
    <main>
      <Hero locale={locale} t={t} />
      <Ticker units={units} t={t} />
      <Spec t={t} />
      <section className="border-b">
        <Heading label={t.translation.label} aside={t.translation.aside}>
          {t.translation.heading}
        </Heading>
        <div className="inner pb-24">
          <Translation />
        </div>
      </section>
      <Units units={units} t={t} />

      <section className="bleed border-t">
        <Heading label={t.hello.label} aside={t.hello.aside}>
          {t.hello.heading}
        </Heading>
        <div className="inner pb-24">
          <LiveCounter code={data.main} />
          <div className="mt-4 grid border lg:grid-cols-[1.35fr_1fr] lg:divide-x">
            <div className="min-w-0">
              <p className="label flex items-center justify-between border-b px-4 py-3 sm:px-6">
                <span className="flex items-center gap-2">
                  <span className="size-1.5 bg-unit" /> {t.hello.run}
                </span>
                <span>{t.hello.every}</span>
              </p>
              <pre className="overflow-x-auto px-4 py-5 font-code text-[13.5px] leading-[1.7] sm:px-6">
                {t.hello.runs.map(([command, comment]) => (
                  <span key={command} className="block">
                    <span className="text-signal-ink select-none">$ </span>
                    {command.padEnd(comment ? 30 : 0)}
                    {comment && <span className="text-muted-foreground"># {comment}</span>}
                  </span>
                ))}
              </pre>
            </div>
            <p className="flex items-end border-t px-4 py-5 text-sm leading-6 text-muted-foreground sm:px-6 lg:border-t-0">
              <span>
                {t.hello.note(
                  <a href={`${site.repo}/tree/main/examples`} className="text-foreground underline decoration-unit/50 underline-offset-4 hover:decoration-unit">
                    {t.hello.examples}
                  </a>
                )}
              </span>
            </p>
          </div>
        </div>
      </section>

      <Episodes t={t} />
      <Finale locale={locale} t={t} />
    </main>
  )
}

/** The title card's two lines in Chinese and Japanese, and their sizes: see Hero. */
const cards = {
  zh: {
    top: "跨平台原生控件，",
    topSize: "text-[calc(100cqi/8.1)]",
    bottom: (
      <>
        用 <span className="text-unit">Asuka</span> 写。
      </>
    ),
    bottomSize: "text-[calc(100cqi/9)]",
  },
  ja: {
    top: "全機種純正部品、",
    topSize: "text-[calc(100cqi/8.1)]",
    bottom: (
      <>
        <span className="text-unit">Asuka</span> で。
      </>
    ),
    bottomSize: "text-[calc(100cqi/7.8)]",
  },
}

/**
 * The title card, in the page's column and within a laptop's screen.
 *
 * In English, NATIVE WIDGETS, set to the column's full width, IN ASUKA.
 * flush right under it and larger, the subtitle in the space it leaves. The
 * sizes come from the lines' widths in Shippori Mincho B1 ExtraBold: NATIVE
 * WIDGETS, is 10.23em, IN ASUKA. 5.74em (at 100cqi/9.5, 60% of the column).
 *
 * In Chinese, whose characters are each an em wide, likewise: 跨平台原生控件，
 * (8em) across the column, 用 Asuka 写。 (6.69em in Noto Serif SC, at
 * 100cqi/9: 74%) flush right under it, the subtitle beside it. In Japanese,
 * in kanji as a title card sets them, the same: 全機種純正部品、 (8em: every
 * platform's genuine parts) across the column, Asuka で。 (5.44em, at
 * 100cqi/7.8: 70%) under it, larger.
 */
function Hero({ locale, t }: { locale: Locale; t: Copy }) {
  // Line height after the size: tailwind-merge drops a leading- that comes before a text- size.
  const big = (size: string) => cn("block origin-bottom-left scale-y-[1.14] whitespace-nowrap", size, "leading-[0.9]")
  const subtitle = (size: string) => cn("display text-balance", size, "leading-snug")
  const card = locale === "en" ? undefined : cards[locale]
  return (
    <section className="relative overflow-hidden border-b">
      <div className="scanlines pointer-events-none absolute inset-0" />
      <div className="relative flex items-center justify-between border-b px-4 py-3 sm:px-10">
        <p className="label flex items-center gap-2 text-foreground/80">
          <span className="size-1.5 animate-pulse bg-unit" />
          {__ASUKA_VERSION__}
        </p>
        <p className="label hidden text-signal-ink sm:block">{t.hero.status}</p>
      </div>

      <div className="relative px-4 pt-8 pb-10 sm:px-10 md:pt-10 md:pb-12">
        <div className="flex items-start justify-between">
          <p className="display text-lg text-unit-ink sm:text-xl">{t.hero.framework}</p>
          <p lang="ja" className="display text-lg tracking-[0.3em] text-unit sm:text-xl">
            明日香
          </p>
        </div>

        <div className="@container mt-3">
          <h1 className="sr-only">{t.hero.title}</h1>
          {card ? (
            <>
              <p aria-hidden className={cn("display", big(card.topSize))}>
                {card.top}
              </p>
              <div className="mt-[1.6cqi] flex items-end justify-between gap-6">
                <p className={cn(subtitle("text-[clamp(1.2rem,2.4cqi,1.8rem)]"), "hidden max-w-[17em] pb-[0.8cqi] break-keep sm:block")}>{t.hero.subtitle}</p>
                <p aria-hidden className={cn("display ml-auto", big(card.bottomSize), "origin-bottom-right")}>
                  {card.bottom}
                </p>
              </div>
            </>
          ) : (
            <>
              <p aria-hidden className={cn("display", big("text-[calc(100cqi/10.4)]"))}>
                NATIVE WIDGETS,
              </p>
              <div className="mt-[1.6cqi] flex items-end justify-between gap-6">
                <p className={cn(subtitle("text-[clamp(1.2rem,2.5cqi,1.9rem)]"), "hidden max-w-[26ch] pb-[0.8cqi] sm:block")}>{t.hero.subtitle}</p>
                <p aria-hidden className={cn("display ml-auto", big("text-[calc(100cqi/9.5)]"), "origin-bottom-right")}>
                  IN <span className="text-unit">ASUKA.</span>
                </p>
              </div>
            </>
          )}
        </div>

        <div className="mt-6 grid grid-cols-1 gap-6 border-t-2 border-foreground pt-6 md:mt-8 lg:grid-cols-[minmax(0,1fr)_auto] lg:items-center lg:gap-10">
          <div>
            <p className={cn(subtitle("text-[1.5rem]"), "mb-3 break-keep sm:hidden")}>{t.hero.subtitle}</p>
            <p className="max-w-xl text-base leading-7 text-foreground/70">{t.hero.lead}</p>
          </div>
          <div className="flex flex-wrap items-center gap-3">
            <Start locale={locale}>{t.start}</Start>
            <InstallCommand className="h-12" />
          </div>
        </div>
      </div>
      <div className="hazard relative h-3" />
    </section>
  )
}

type Unit = { name: string; toolkit: string }

/** Warning tape that runs past, fading out at its ends and held while pointed at: every platform and the widgets it gets. */
function Ticker({ units, t }: { units: Unit[]; t: Copy }) {
  const run = (
    <span className="flex shrink-0 items-center">
      {units.map((u) => (
        <span key={u.name} className="flex items-center gap-4 px-6">
          <span className="font-bold">{u.name}</span>
          <span className="opacity-70">{u.toolkit}</span>
          <span className="pl-6">Λ</span>
        </span>
      ))}
    </span>
  )
  return (
    <div className="overflow-hidden border-b bg-signal py-3 font-mono text-[13px] tracking-[0.16em] text-[#0e0d10] uppercase">
      <p className="sr-only">
        {t.runsOn} {units.map((u) => `${u.name} (${u.toolkit})`).join(", ")}.
      </p>
      <div className="ticker-track" aria-hidden>
        <div className="ticker flex w-max">
          {run}
          {run}
        </div>
      </div>
    </div>
  )
}

/** The spec, as three lines of a title card. */
function Spec({ t }: { t: Copy }) {
  return (
    <section className="bleed border-b">
      <div className="inner py-20 md:py-28">
        <p className="label">{t.spec.label}</p>
        <dl className="mt-8">
          {t.spec.lines.map(({ line, note, red }) => (
            <div key={line} className="relative flex flex-col gap-2 border-b py-5 first:border-t md:py-7">
              <dd className="label md:absolute md:top-7 md:right-0 md:max-w-56 md:text-right">{note}</dd>
              <dt className={cn("display text-[clamp(2.6rem,8.6vw,7rem)] leading-[0.95] md:whitespace-nowrap", red && "text-unit")}>
                <Tall>{line}</Tall>
              </dt>
            </div>
          ))}
        </dl>
      </div>
    </section>
  )
}

/** One pilot, six units: a status panel for each platform. */
function Units({ units, t }: { units: Unit[]; t: Copy }) {
  return (
    <section className="bleed">
      <Heading label={t.units.label} aside={t.units.aside}>
        {t.units.heading}
      </Heading>
      <div className="inner pb-24">
        <ol className="grid gap-4 sm:grid-cols-2 lg:grid-cols-3">
          {units.map((u, i) => (
            <li key={u.name} className="hud scanlines relative border bg-card p-5 [--hud:var(--unit)]">
              <div className="label flex items-center justify-between">
                <span>{t.units.unit(String(i + 1).padStart(2, "0"))}</span>
                <span className="flex items-center gap-1.5 text-signal-ink">
                  <span className="size-1.5 animate-pulse rounded-full bg-signal" /> {t.units.active}
                </span>
              </div>
              <p className="display mt-8 text-5xl leading-none">{u.name}</p>
              <p className="mt-3 font-mono text-[13px] text-foreground/70">{u.toolkit}</p>
              <div className="mt-8 flex items-center gap-3">
                <div className="h-2 flex-1 bg-foreground/10">
                  <div className="sync-fill h-full origin-left bg-signal" />
                </div>
                <span className="label text-foreground">{t.units.sync}</span>
              </div>
            </li>
          ))}
        </ol>
      </div>
    </section>
  )
}

/** The features, six episodes, each with its numeral writ large behind it. */
function Episodes({ t }: { t: Copy }) {
  return (
    <section className="bleed border-t">
      <Heading label={t.episodes.label} aside={t.episodes.aside}>
        {t.episodes.heading}
      </Heading>
      <div className="border-t">
        <ol className="mx-auto grid max-w-6xl gap-px bg-border sm:grid-cols-2 lg:grid-cols-3">
          {t.episodes.features.map((feature, i) => (
            <li key={feature.title} className="bg-background">
              <a href={feature.href} className="group relative flex h-full flex-col overflow-hidden px-5 pt-7 pb-10 transition-colors hover:bg-code sm:px-8">
                <span className="absolute top-0 left-0 h-0.5 w-0 bg-unit transition-[width] duration-300 group-hover:w-full" />
                <span
                  lang="ja"
                  aria-hidden
                  className="display pointer-events-none absolute -right-3 -bottom-10 text-[11rem] leading-none text-unit/12 transition-colors group-hover:text-unit/25"
                >
                  {numerals[i]}
                </span>
                <span className="relative flex items-center justify-between">
                  <span className="display text-[15px] text-unit-ink" lang="ja">
                    第{numerals[i]}話
                  </span>
                  <span className="label">Episode {String(i + 1).padStart(2, "0")}</span>
                </span>
                <h3 className="display relative mt-8 flex items-start justify-between gap-4 text-[1.75rem] leading-tight">
                  {feature.title}
                  <ArrowRightIcon className="mt-2 size-4 shrink-0 text-muted-foreground transition-transform group-hover:translate-x-0.5 group-hover:text-unit-ink" />
                </h3>
                <p className="relative mt-3 max-w-sm text-sm leading-6 text-foreground/65">{feature.text}</p>
              </a>
            </li>
          ))}
        </ol>
      </div>
    </section>
  )
}

/**
 * The preview of the next episode: the motto, where to begin, and the name.
 * Its overflow is clipped, not hidden: a section that hides its overflow is a
 * scroll container, which the wordmark's view() timeline would follow.
 */
function Finale({ locale, t }: { locale: Locale; t: Copy }) {
  return (
    <section className="bleed relative overflow-clip border-t">
      <div className="hazard h-3" />
      <div className="scanlines pointer-events-none absolute inset-0" />
      <div className="inner relative pt-24 pb-16 md:pt-32">
        <p className="label text-signal-ink">{t.finale.label}</p>
        <p className="display mt-6 text-[clamp(2.6rem,8vw,6.5rem)] leading-[0.98]">{t.finale.motto}</p>
        <div className="mt-12 flex flex-wrap items-center gap-3">
          <Start locale={locale}>{t.finale.begin}</Start>
          <Link
            to={routes[locale].docs}
            className="inline-flex h-12 items-center border px-6 font-mono text-[13px] tracking-[0.14em] uppercase transition-colors hover:border-foreground/40"
          >
            {t.finale.docs}
          </Link>
        </div>
      </div>
      <div className="relative px-[2vw] pt-10 pb-[2vw]" aria-hidden>
        <Wordmark lambda="var(--unit)" className="wordmark-slam h-auto w-full text-foreground" />
      </div>
    </section>
  )
}
