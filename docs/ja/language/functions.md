# 関数とクロージャ

```asuka
fn area(width: float, height: float = 1) -> float {
  return width * height
}

fn double(x: int) -> int = x * 2      // 式が一つだけの関数

area(3, height: 4)
let squares = [1, 2, 3].map(x => x * x)
let sorted = people.sorted((a, b) => a.age < b.age)
```

引数は位置で渡すものが先、名前で渡すものが後です。クロージャは `x => expr`、`(a, b) => expr`、`x => { statements }` と書きます。クロージャは、キャプチャした変数を周りのコードと共有します。どちらかが変更すれば、もう一方にもその変更が見えます。

```asuka
var n = 0
let inc = () => { n += 1 }
inc()
inc()                // n は 2
```

状態も同じように共有されます。使うものをコピーするのは `spawn` だけです（値だけを、実行を始めた時点のまま受け取ります）。別のスレッドで動くためです。

クロージャが非同期かどうか、エラーを投げるかどうかは、渡す先の関数型（`async fn(int) -> int`）で決まります。型が決まっていないとき、たとえば `let` に入れたクロージャでは、本体で決まります。`await` があれば非同期になり、エラーが外に出うるならエラーを投げます。前に `async` を書くと、今 `await` しているかどうかにかかわらず非同期だと明示できます。あとで `await` を足しても、呼び出す側には何も変わりません。そうしたクロージャは、待てない関数を求める場所には渡せません。

```asuka
let refresh = async () => {
  await store.reload()
}
await refresh()
```

クロージャの本体のせいで呼び出しに `await` や `try` が足りないときは、エラーが本体のどこが原因かを示します。

`fn` は、関数、ハンドラー、クロージャ、メソッドの本体の中でも宣言できます。クロージャと同じように周りのものを参照でき、自分自身や、同じブロックで隣に宣言した `fn` を呼び出せます（隣の `fn` からも呼び出せます）。

```asuka
fn total(root: Node) -> int {
  var sum = 0
  fn walk(n: Node) {
    sum += n.size
    for c in n.children { walk(c) }
  }
  walk(root)
  return sum
}
```

ほかの関数と同じく値としても使えます（`items.map(walk)`）。引数は順番どおりに渡し、ラベルやデフォルト値は使えません。ジェネリックにもできません。

決して戻らない関数（必ずエラーを投げる、panic する、プログラムを終了する）は、`os.exit` のように `-> never` でそれを示します。その呼び出しは `return` と同じようにブロックを終わらせ、値が来る場所ならどこにでも書けます：`let port = text.toInt() ?? fail("not a port")`。

関数が自分自身を呼び出すときは、こう書きます。クロージャを変数に入れ、その変数を通して自分を呼び出す（`var walk = …; walk = n => { … walk(c) … }`）と、クロージャは変数を持ち、変数はクロージャを持つことになります。このような循環参照を回収する仕組みはないので、どちらも解放されません。コンパイラーはこれを警告します。ローカルな `fn` なら、自分自身を持たずに自分を呼び出せます。

## インターフェースとジェネリクス

インターフェースはメソッドとフィールドの集まりです。それらを持っている型は、そのインターフェースを満たします（何も宣言する必要はありません）。

```asuka
interface Shape { fn area() -> float }

fn total<T>(items: [T], size: fn(T) -> float) -> float {
  var sum = 0.0
  for i in items { sum += size(i) }
  return sum
}
```

インターフェースはそれ自体が型でもあります。インターフェース型の値には、そのメソッドを持つどんな値でも入れられ、メソッドを呼ぶとその値のメソッドが呼ばれます。

```asuka
let shapes: [Shape] = [Circle(radius: 1), Square(side: 2)]
let areas = shapes.map(s => s.area())
```

ジェネリックな関数と型は型パラメーターを取ります。型パラメーターの制約には、プログラム自身のインターフェースと、`Equatable`、`Comparable`、`Hashable` が使えます（構造体と列挙型は、フィールドが後の三つを満たしていれば自身も満たします）。複数の制約は `&` でつなぎます。型引数が持っていなければならないものは、ジェネリックなコードの中でその値から使えます。インターフェースのメソッドとフィールドは、コンパイルされる具体的な型ごとに直接呼び出されます。

```asuka
fn sortByArea<T: Shape>(xs: [T]) -> [T] = xs.sorted((a, b) => a.area() < b.area())

fn largestOnce<T: Shape & Hashable>(xs: [T]) -> [T] { … }

struct Labeled<T: Shape> {
  item: T
  fn label() -> string = "{item.area()}"
}
```

ジェネリック関数の型引数は、引数と、戻り値に期待される型から推論されます。推論できないときは `f<T>(…)` で明示します：`json.decode<[Note]>(text)`、`flag.parse<Options>(args)`。

インターフェースのメソッドとフィールドを持っていれば、型引数はそのインターフェースを満たします。構造体や列挙型（値を持たないケースだけの列挙型も含む）、それらを求めるインターフェース、同じように制約された別の型パラメーターがそうです。`T: Error` ならエラーの `message` を読めます。
