# モーションとジェスチャー

Asuka は、プラットフォームにアニメーションの仕組みがあればそれを使います。iOS では UIView のアニメーション、Android ではアニメーター、macOS では AppKit の暗黙的アニメーションと Core Animation、ブラウザーでは Web Animations API です。Windows と GTK にはビューのフレームをアニメーションさせる仕組みがないため、Windows と Linux では Asuka が同じカーブを使って、フレームごとに自前でアニメーションを進めます。`animate` の中で状態を変えると、その変更によって画面上で動くものがアニメーションします。このページでは、変更のアニメーション、表示と非表示のトランジション、共有要素、フレーム単位のアニメーション、タッチ・マウス・トラックパッドによるジェスチャー、触覚フィードバック、「視差効果を減らす」（Reduce Motion）について説明します。

## 変更のアニメーション

書き込みを `animate` で囲みます。すると、その書き込みで画面上の変わるものがアニメーションします。対象は、レイアウトによって動くフレームと、後述の見た目に関するプロパティです。

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

ボックスが大きくなり、その下にあるものもすべて一緒に下へずれていきます。レイアウトは一度だけ実行され、変わったフレームがそれぞれ新しい位置へアニメーションします。位置を自分でアニメーションさせる必要はありません。

同じ書き込みを `animate` なしで行うと、変更はすぐに反映されます。`animate` は、ほかの状態の書き込みと同じくメインスレッドで実行します（`spawn` のワーカーは先に結果を戻します）。`animate` はすぐに戻ります。`await animate(…)` はアニメーションが終わるまで待ちます。

## 再生時間、カーブ、繰り返し

`animate(Animation(…), body)` はアニメーションのタイミングを受け取ります。

- `duration` は `Duration` です（デフォルトは `350ms`、`0s` は 250ms を意味します）。
- `curve` は `.easeInOut`（デフォルト）、`.linear`、`.easeIn`、`.easeOut`、`.spring` のいずれかです。スプリングは減衰するばねで、`duration` が終わるまでに静止します。iOS では UIKit のスプリングアニメーションを、Android、Windows、Linux、Web では同じばねの補間関数を使います。`Animation.spring`、`.linear`、`.quick`（200ms）が用意されています。
- `damping` はスプリングの減衰比です。1 なら終点を越えずに静止し、それより小さいと行き過ぎてから戻ってきます（0 は 0.85 を意味し、少しだけ行き過ぎます）。`velocity` は開始時の速さで、1 秒あたりに変化量の何割進むかで表します（UIKit の `initialSpringVelocity`）。フリックの勢いを引き継ぐのに使います。
- `delay` は、開始までに待つ時間です。
- `done` は、アニメーションが論理的に完了したとき（`delay` と `duration` の後、繰り返すならその回ごと）に呼ばれます。`await animate(…)` が戻るのも同じときです。次のステップはここからつなげます。
- macOS の AppKit の暗黙的アニメーションには、スプリングも遅延もありません。弾むスプリングはカーブで行き過ぎる動きになり、遅延は無視されます。
- `repeat` は、指定した回数だけアニメーションを追加で繰り返します。`Animation.forever` なら、プロパティがまた変わるまで繰り返し続けます。`autoreverses` を指定すると、毎回、次の回の前に元へ戻ります。macOS と Windows は繰り返さず、アニメーションは一度だけ実行されます。

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
    // アニメーションなしで設定すると、パルスが止まる
    Button("Stop") { on press { on = false } }
  }
}
```

ジェスチャーの終わりをスプリングに引き継ぐことはよくあります。ジェスチャーの速度を残りの距離で割ったものをスプリングの `velocity` にすると、指が与えた速さのままビューが動き続けます。

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
        // 指に追従する
        dragX = g.translationX
        dragY = g.translationY
        return
      }
      let d = math.hypot(g.translationX, g.translationY)
      // 元の位置へ向かう速さ（戻る方向に沿った成分）
      let v = if d > 1 { -(g.velocityX * g.translationX + g.velocityY * g.translationY) / d / d } else { 0.0 }
      animate(Animation(duration: 600ms, curve: .spring, damping: 0.55, velocity: v), () => {
        dragX = 0
        dragY = 0
      })
    }
  }
}
```

連続するステップは、それぞれを await して（または `done` で）つなげます。次の例は、現れて、しばらく表示され、消えていくトーストです。

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

繰り返すアニメーションは、そのプロパティがアニメーションなしで再び設定されると止まります。Android で繰り返すのは不透明度と変形だけです。iOS、Linux、Web ではアニメーションするものすべてが繰り返します。macOS と Windows では一度だけ実行されます。

## アニメーションするもの

| 変更 | iOS | Android | macOS | Windows | Linux | Web |
| --- | --- | --- | --- | --- | --- | --- |
| フレーム（レイアウトによる位置とサイズ） | ○ | ○ | ○ | ○ | ○ | ○ |
| `opacity` | ○ | ○ | ○ | × | ○ | ○ |
| `transform` | ○ | ○ | ○ | ○ | ○ | ○ |
| `background`（色） | ○ | ○ | × | × | × | ○ |
| `radius` | ○ | ○ | ○ | × | × | × |
| `Shape` の `trimStart` と `trimEnd` | ○ | ○ | ○ | × | ○ | × |
| `Shape` のパス | ○ | × | ○ | × | × | × |

ほかのプロパティ（`borderColor`、`shadow`、`gradient`、文字色やフォント）はすぐに切り替わります。アニメーションするのは画面上にあるビューだけです。`animate` の間に現れたビューは、[トランジション](#表示と非表示のトランジション)を持っていない限り、アニメーションしながら現れることはありません。

`opacity: 0` までフェードしたビューも、レイアウトには残ります。ビューをフェードアウトさせてレイアウトから外すには、`Transition.opacity` トランジションを付けてビューを取り除きます。

## 変形

`transform` は、ビューを描画するときに、その中心を基準に移動・拡大縮小・回転させます。レイアウトはこれを考慮しないので、隣のビューは元の位置のままです。`Transform` には次のものがあります。

- `x`、`y`：オフセット（ポイント単位）。
- `scale`：両方の軸の拡大率（デフォルトの 1 が元の大きさ）。`scaleX`、`scaleY` は片方の軸だけを設定します。
- `rotation`：角度（度単位、時計回り）。

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

## 表示と非表示のトランジション

`transition` は、`animate` の間にビューが追加されたとき（`if` が true になる、`for` に項目が加わるなど）にどの状態から現れるか、そして `animate` の間に取り除かれたときにどの状態へ消えていくかを表します。取り除かれたビューは消え終わるまで画面に残り、その間はタッチを受け付けません。

- `.opacity` はフェードイン・フェードアウトします。
- `.pop` はフェードしながら 90% の大きさから拡大して現れます（ポップオーバーやバッジ向け）。
- `.slide` は 24 ポイント下からフェードインします（トーストや新しい行向け）。
- `Transition(…)` は独自のトランジションです。`fade`、`scale`（0 なら拡大縮小なし）、ポイント単位のオフセット `x`、`y` を指定でき、組み合わせることもできます（`Transition(fade: true, scale: 0.5, y: -8)`）。

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

トランジションは `animate` の中でだけ、その長さとカーブで実行されます。同じ `shown = true` でも、`animate` の外ではトーストがすぐに表示されます。また、ビューがトランジションで現れるのは、親がすでに画面上にあるときだけです。

## 共有要素

`shared` は、ビューがほかのビューと共有するキーで、SwiftUI の `matchedGeometryEffect` にあたります。`animate` の間に、あるキーを持つビューが消えて同じキーを持つ別のビューが現れると、新しいビューは古いビューの位置と大きさから自分の位置と大きさへと広がります。その間、古いビューも一緒に動きながらフェードアウトします。次の例では、行がカードに開き、アートワークとタイトルがそれに合わせて移動します（[`examples/gallery`](../../examples/gallery) より）。

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

- デスクトップと Web を含む、すべてのプラットフォームで動作します。
- 2 つのビューは同じ `animate` の中で変わる必要があります。一方が取り除かれ、もう一方が追加されるということです。
- キーはルートに属します。一つのキーを持てるのは、一度に一つのビューだけです。
- キーを持ったまま消えるビューに相手がいなければ、そのビューはフェードアウトします。

## プッシュする画面へのズーム

`zoomFrom` には、下にある画面のビューの `shared` キーを指定します。プッシュされた画面はそのビューから広がって現れ、ポップされるとそのビューへ縮んで戻ります。次の写真のグリッドのように、`List` のセルでも使えます。

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

- **iOS 18 以降**では UIKit のズームトランジションを使います。インタラクティブなポップのジェスチャーで、画面はビューへ縮んで戻ります。それより前のバージョンでは通常どおりプッシュします。
- **Android** では Material のコンテナートランスフォームを使います。画面はビューから角を丸めたまま自分の領域まで広がり、ビューの近くでは両者がクロスフェードします。予測型「戻る」ジェスチャーで、画面は縮んで戻ります。
- **macOS、Windows、Linux、Web** では通常どおりプッシュします。
- 下の画面にそのキーを持つビューがなければ、通常どおりプッシュします。

この場合 `animate` は必要ありません。プッシュはそれ自体がアニメーションするからです。詳しくは[ナビゲーション](navigation.md)を参照してください。

## 線が描かれていくシェイプ

`Shape` の `trimStart` と `trimEnd` は、輪郭のうち描画する範囲を 0 から 1 で表します。これらを `animate` の中で変えると、線が描かれていくようにアニメーションします。進捗リングやチェックマークに使えます。

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

## フレーム単位のアニメーション

動きを宣言するのではなく計算で求める場合（物理演算、ゲーム、値に追従するゲージなど）は、`frames()` を使います。`frames()` は、ディスプレイが描画するフレーム（1 秒に 60 回または 120 回）を、その時刻のストリームとして返します。タスクの中で `for await` で読みます。ループの中で書き込んだ内容は、そのフレームに反映されます。ループはタスクがキャンセルされるまで、つまりコンポーネントがなくなるまで続きます。何も読んでいない間は、フレームは要求されません。（`onFrame(fn)` は、フレームごとに関数を呼びます。戻り値のクロージャーを呼ぶと止まります。）

```asuka
component Spinner {
  state angle = 0.0

  task {
    var last: Duration? = none
    for await t in frames() {
      if let l = last {
        if !environment().reduceMotion {
          angle = (angle + (t - l).seconds() * 180) % 360 // 1 秒に半回転
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

動きは、フレームの数ではなくフレーム間の経過時間をもとに計算してください。フレームレートはデバイスによって異なります。

## ジェスチャー

`View` は、プラットフォームのジェスチャー認識器が認識したジェスチャーを受け取ります。

- `on press` と `on longPress`：タップと長押し。
- `on pan(g)`：ドラッグ。
- `on pinch(g)`：2 本の指を広げる・狭める操作。
- `on rotate(g)`：2 本の指をひねる操作。
- `on swipe(direction)`：すばやく払う操作。方向は `SwipeDirection`（`.left`、`.right`、`.up`、`.down`）で受け取ります。

マウスやトラックパッドでは、同じハンドラーが、各デスクトップでそれぞれに相当する操作を受け取ります。

| ジェスチャー | macOS | Windows | Linux | Web |
| --- | --- | --- | --- | --- |
| `on press` | クリック | クリック、またはフォーカスのあるビューでの Enter か Space | クリック、またはフォーカスのあるビューでの Enter か Space | クリック、またはフォーカスのあるビューでの Enter か Space |
| `on longPress` | 副ボタンのクリック、またはボタンの押し続け | 右クリック、またはボタンの押し続け | 副ボタンのクリック、またはボタンの押し続け | 右クリック、またはボタンやタッチの押し続け |
| `on pan(g)` | ボタンを押したままのドラッグ | ボタンを押したままのドラッグ | ボタンを押したままのドラッグ、タッチ、またはタッチパッド | ポインターを押したままのドラッグ |
| `on swipe(direction)` | 最後にすばやく動かすドラッグ、トラックパッドの 2 本指で払う操作、システムがアプリに任せている場合は 3 本指のスワイプ | 最後にすばやく動かすドラッグ | 最後にすばやく動かすドラッグ、タッチスクリーンで払う操作 | 最後にすばやく動かすドラッグ |
| `on pinch(g)` | トラックパッドのピンチ | タッチパッドのピンチ、Ctrl を押しながらのホイール、またはタッチスクリーン上の 2 本指 | タッチパッドかタッチスクリーンのピンチ、または Ctrl を押しながらのホイール | トラックパッドのピンチ、Ctrl を押しながらのホイール、またはタッチスクリーン上の 2 本指 |
| `on rotate(g)` | トラックパッドの回転 | タッチスクリーン上で 2 本指をひねる操作 | タッチパッドかタッチスクリーンの回転 | タッチスクリーン上で 2 本指をひねる操作 |

マウスのボタンを押し続けたら何が起こるかを試す人はあまりいません。そのため、重要な場面では、`on longPress` でできることをメニュー項目やボタンなど別の方法でもできるようにしてください。Windows と Linux では、Ctrl を押しながらホイールを 1 段回すごとに 10% ピンチし、ホイールが止まるとピンチが終わります。Mac のトラックパッドで 2 本指で払う操作は、引き続きビューの周りにあるものをスクロールします。上下のスワイプになるのは、周りにその方向へスクロールするものがない場合だけです。

パン、ピンチ、回転は、ジェスチャーの進行に合わせて `Gesture` を渡して関数を呼びます。

- `state`：`.began`、`.changed`、そして `.ended` の順に変わります。システムがタッチを奪った場合は `.cancelled` になります。
- `x`、`y`：タッチの位置（ビュー内の座標、ポイント単位）。
- `translationX`、`translationY`：指が触れてからパンで移動した距離。つまり `x - translationX` が、指が触れた位置です。
- `velocityX`、`velocityY`：パンの速さ（ポイント毎秒）。
- `scale`：ピンチ開始時からの拡大率（パンでは 1）。
- `rotation`：回転開始時からの角度（ラジアン、時計回り）。

ドラッグしたビューは変形によって指に追従し、指を離すとスプリングで元の位置へ戻ります。

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

拡大率と回転角はそのジェスチャーの開始時からの値です。そのため、前回のジェスチャーが終わったときの値を保存しておき、次のジェスチャーの値をそれに加えます。同じビューに付けたジェスチャーは同時に働くので、ピンチしながら回転させることもできます。

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

パン、ピンチ、回転がいったん始まると、タッチはそのジェスチャーのものになります。周りのスクロールビューがタッチを横取りすることはなく、中にある押せるビューも押されません。ボタンの上から始めたドラッグでボタンが押されないのと同じです。

スワイプやパンは、スクリーンリーダーがオンの状態では行えません。同じことを[アクセシビリティアクション](accessibility.md)としても提供してください。

## 押下とホバーの状態

`View` は操作の状態が変わるたびにそれを通知し、スタイルは 3 つの方法でその状態に追従できます。まずは目的を果たせる最初の方法から使ってください。

1. **バリアント**：ビュー自身の見た目を変えるためのものです。`pressed { … }`、`hovered { … }`、`dragging { … }`、`dragOver { … }` は、ビューがその状態にある間、スタイルのほかの部分の上に重ねて適用されます。CSS の `:active` や `:hover`、Tailwind の `active:` や `hover:` にあたります。
2. **`Ref`**：ほかのビューや、その状態に追従するあらゆるもののためのものです。ビューに `Ref` を渡し（`ref: card`）、必要な場所で `card.pressed` や `card.hovered` を読みます。たとえばほかのビューのスタイル、`Text`、`if` の中です。子が親に追従する（グループのホバー）のも、兄弟が隣のビューに追従する（ピアのホバー）のも、この方法で実現します。
3. **ハンドラー**：録音の開始など、スタイル以外のことのためのものです。`on pressedChange(on)` と `on hoverChange(on)` を使います。

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
    // カードのホバーにタイトルが追従する（グループのホバー）
    Text(title) {
      grow: 1
      color: if card.hovered { .accent } else { .label }
    }
  }
}
```

状態は次のとおりです。

- **Pressed** は、指またはマウスの主ボタンがビューの上で押されている間 true になります。
  - スクロールするコンテナーの中では少し遅れて true になるので、スクロールしただけで押下表示が一瞬出ることはありません。
  - 指を離したとき、指がビューの外へ出たとき（ビューの上へ戻ると再び押下状態になります）、またはスクロールや長押し、ほかのジェスチャーがタッチを引き継いだときに false に戻ります。
  - `on press` が呼ばれるのは、押下状態のまま指やボタンを離したときだけです。
- **Hovered** は、ポインターがビューまたはその子の上にある間 true になります。ポインターとは、マウス、トラックパッド、iPad のポインター、ホバー中の Apple Pencil やスタイラスのことです。タッチスクリーンだけではホバーは起きません。
- **Dragging** は、ビューがドラッグされている間（その `dragItems`）true になります。
- **DragOver** は、そのビューの `dropTypes` が受け付けるドラッグがビューの上にある間 true になります。

バリアントの組み合わせ方は次のとおりです。

- バリアントは、自分が指定したキーだけを設定します。それ以外はスタイルの値のままです。
- バリアントは hovered、pressed、dragging、dragOver の順に適用されます。したがって、押下状態はホバー状態より優先されます。
- どのバインディングとも同じく、キーそのものを状態に追従させることもできます。`hidden: !row.hovered` は、行がホバーされている間だけビューを表示します。

```asuka
component Person(name: string) {
  event remove

  let row = Ref()

  Row {
    ref: row
    Text(name) { grow: 1 }
    // 削除ボタンは行がホバーされている間だけ表示される
    View {
      hidden: !row.hovered
      label: "Delete"
      on press { emit remove }
      Symbol("trash")
    }
  }
}
```

スタイルは同じようにどんな状態にも追従できます（`background: if selected { .accent } else { .clear }`）。

ビューが追跡するのは、何かが読んでいる状態だけです。読んでいるものとは、そのビューのバリアントか、その `Ref` の読み取りです。

- `pressed` バリアントがなく、`pressed` を読むものもないビューは押下状態を通知せず、プラットフォームのハイライトがそのまま使われます。
- 押下状態を通知するビューは、押下表示を自分で描きます。プラットフォームのハイライト（Android のリップル、Windows のホバー時や押下時の塗り）は表示されません。

各状態は、変わるたびに一度だけ通知されます。ある状態のまま消えたビューはその状態を抜けるので、それに追従しているものが元に戻らなくなることはありません。

`on press` がない場合、押し続ける操作は「ホールド」になります。「押している間だけ録音する」がその例です。録音は副作用なので、ハンドラーの中で行います。

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

## ドラッグ＆ドロップ

`dragItems` を指定すると、ビューをドラッグできるようになります。タッチスクリーンでは、長押ししてから指を動かすとビューが持ち上がります。このときの長押しで `on longPress` が呼ばれることはありません。マウスでは、ボタンを押したまま動かすとドラッグが始まります。ビューの画像は指やポインターに追従し、アプリ内でも、ほかのアプリへでもドラッグできます。`dropTypes` を指定すると、ビューがドロップを受け付けるようになります。

`DragItem` は、テキスト、リンク（`kind: .link`）、ファイル（`path`）、アプリ独自の値（`value`、id などの文字列）のいずれかです。

- アプリ内では、ドロップ先は各項目を値も含めてそのまま受け取ります。
- ほかのアプリが受け取るのは、項目のテキスト、リンク、ファイルです。値しか持たない項目は、アプリの外へは渡りません。
- `mime` は項目の型で、ドロップ先はこれを見て受け付けるかどうかを決めます。テキスト、リンク、ファイルには自動的に設定されます。`value` にはアプリ独自の型を指定して、自分のアプリのドロップ先だけが受け取るようにしてください。

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
    // ドラッグ中は薄く表示する
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
    // タスクがドラッグされて上にある間は枠線を表示する
    dragOver { borderColor: .accent }
    on drop(d) {
      for it in d.items { emit move(it.value) }
    }
    children
  }
}
```

ドラッグ元：

- `dragItems` はバインディングなので、読んでいる値が変われば最新の状態に保たれます。ドラッグが運ぶのは、ドラッグ開始時点の項目です。
- `on dragPhase(phase)` は、ビューが持ち上がると `.began` を通知します。その後、ドロップ先が受け取った場合は `.dropped` を（移動の場合は、ドラッグしたものをここで取り除きます）、そうでなければ `.cancelled` を通知します。ドラッグ中の見た目には、`dragging { … }` バリアントか、その `Ref` の `dragging` を使います。

ドロップ先：

- `dropTypes` には MIME タイプを列挙します。完全一致（`"text/plain"`）、ファミリー（`"image/*"`）、すべて（`"*/*"`）のいずれかで指定します。
- これらの型の項目を一つも含まないドラッグは、ビューの上に来ても反応しません。
- ドロップ先が入れ子になっている場合、そのドラッグを受け付ける最も内側のドロップ先が受け取ります。
- `on dropOver(over)` は、ドラッグがビューの上に来たときと離れたときに通知します。`dragOver { … }` バリアントと同じ状態です。
- `on dropMove(x, y)` は、ドラッグがビュー上のどこにあるかを挿入位置として通知します。
- `on drop(d)` は、受け付けた項目とドロップされた位置を受け取ります。`d.local` は、ドラッグがアプリ内から来たかどうかを示します。

ほかのアプリからのドロップ（Finder や「ファイル」アプリからのファイル、「写真」アプリからの写真、ブラウザーからのリンクなど）は、項目の読み込みが終わってから届きます。

- iOS と Android では、ファイルはまずアプリのキャッシュにコピーされ、`d.copied` が true になります。アプリがファイルを保存する場所へ移動するか、使い終わったら削除してください。
- macOS、Windows、Linux では、`path` はユーザーのファイルそのものです。削除しないでください。
- ブラウザーでは、ドロップされたファイルはアプリのファイル（ページのメモリ上）にコピーされ、`d.copied` が true になります。

## キー

ビューは、キーボードのフォーカスが自分か自分の中のビューにある間、`keys` でキーを受け取ります。`keys` は `Shortcut` の配列で、それぞれがキーとその処理です。キーは、フォーカスのあるビューから外側へたどって最初にそのキーを持つビューへ届きます。これはフォーカスのあるコントロールやメニューバーより先なので、たとえば検索フィールドの上下キーで、その下の検索結果を移動できます。

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

キーは文字か名前付きのキー（`enter`、`escape`、`tab`、`space`、`delete`、`backspace`、`up`、`down`、`left`、`right`、`home`、`end`、`pageup`、`pagedown`、`f1` から `f12`、それにテレビのリモコンの再生／一時停止ボタン `playpause`。[テレビ](tv.md#リモコン)を参照）で、指定した修飾キーと完全に一致したときだけ反応します（`Shortcut(key: "d", primary: true, run: …)`、`Shortcut(key: "up", alt: true, run: …)`）。押し続けるとリピートします。修飾キーなしの文字を登録すると、ビューの中にあるフィールドへの入力を横取りしてしまいます。そのため修飾キーを付けるか、フィールドのない場所に置いてください。

コンポーネントの本体で `useKeys([…])` を呼ぶと、ウィンドウ独自のキーを登録します。キーが最後に届く先であり、何にもフォーカスがないときは最初に届く先になります。ゲームの矢印キーやビューアーの Space キーなどに使います。登録したキーは、それを呼んだコンポーネントと一緒に破棄されます。

キーはハードウェアキーボードから届きます。デスクトップ、iPad、Chromebook のキーボードや、ブラウザーでのキーボードです。スマートフォンには普通キーボードがないので、キーを唯一の操作手段にしないでください。テキストを変換中の入力メソッド（日本語や中国語のキーボード）は、そのキーを自分で処理します。`examples/launcher` では、上下キーで検索結果を移動できます。

## 触覚フィードバック

`device.haptic` はフィードバックを再生します。`.selection`（値が切り替わったとき）、`.light`、`.medium`、`.heavy`（衝撃）、`.success`、`.warning`、`.error`（結果）があります。

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

iOS では `UIFeedbackGenerator` のフィードバックを使います。Android には完全に対応するものがないため、Asuka は `performHapticFeedback` の定数のうち最も近いものを再生します。成功、警告、エラーは、Android 11 以降では confirm と reject、それより前では key press と long press になります。

Force Touch トラックパッドを備えた Mac では、`.selection` は `NSHapticFeedbackManager` の alignment に、それ以外は level change になります。トラックパッドは、指が触れている間にこれらを再生します。Windows と Linux には触覚フィードバックがないため、`device.haptic` は何もしません。ブラウザーでは、`navigator.vibrate` があれば（Android の Chrome）短く振動し、なければ何もしません。

## 視差効果を減らす

ユーザーが動きを減らすよう設定していると、`environment().reduceMotion` が true になります。これを読んでいるところは、ユーザーが設定を変えると追従します。

- **iOS**：「設定」>「アクセシビリティ」>「動作」>「視差効果を減らす」。
- **Android**：アニメーションをオフにしたとき（開発者向けオプション、または「ユーザー補助」>「アニメーションを削除」）。
- **macOS**：「システム設定」>「アクセシビリティ」>「ディスプレイ」>「視差効果を減らす」。
- **Windows**：「設定」>「アクセシビリティ」>「視覚効果」>「アニメーション効果」をオフ。
- **Linux**：GNOME のアニメーションをオフにしたとき（「設定」>「アクセシビリティ」>「アニメーションを減らす」（Reduce Animation）、GTK の `gtk-enable-animations`）。
- **Web**：ブラウザーの `prefers-reduced-motion`。システムの設定に従います。

Asuka がこれに対して自動で行うことは次のとおりです。

- **iOS**：トランジションは移動や拡大縮小をせず、その場でディゾルブします。UIKit 自身のトランジションもこの設定では同じ動きになります。共有要素（`shared`）も飛んでいかず、各ビューはそれぞれのトランジションで現れたり消えたりします。それ以外のアニメーションは実行されます。
- **Android**：システムのアニメーター再生時間スケールが 0 になるため、Asuka のものもプラットフォームのものも、すべてのアニメーションが即座に終わります。
- **macOS、Windows、Linux、Web**：`animate` の変更は、トランジションや共有要素も含めてすぐに反映されます。スタックのプッシュとポップは、macOS ではスライドせず、ブラウザーではクロスフェードし、Linux ではアニメーションをオフにしたときの libadwaita のナビゲーションと同じ動きになります。

残りは、装飾的かどうかを Asuka が判断できない動きです。たとえば `frames()` のループ（上のスピナーが止まるように）、繰り返すパルス、何かを遠くまで動かすアニメーションなどです。こうした動きは `reduceMotion` を読んで控えめにしてください。
