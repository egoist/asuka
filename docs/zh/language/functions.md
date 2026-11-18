# 函数与闭包

```asuka
fn area(width: float, height: float = 1) -> float {
  return width * height
}

fn double(x: int) -> int = x * 2      // 只有一个表达式

area(3, height: 4)
let squares = [1, 2, 3].map(x => x * x)
let sorted = people.sorted((a, b) => a.age < b.age)
```

参数先按位置传，再按名字传。闭包写作 `x => expr`、`(a, b) => expr` 或 `x => { statements }`。闭包和周围的代码共享它捕获的变量：一方修改了，另一方也能看到。

```asuka
var n = 0
let inc = () => { n += 1 }
inc()
inc()                // n 现在是 2
```

状态也以同样的方式共享。只有 `spawn` 会复制它用到的东西（只能是普通的值，取它开始运行那一刻的值），因为它在另一个线程上运行。

闭包是否异步、是否会抛出错误，由它被传作的函数类型决定（`async fn(int) -> int`）。没有类型可依据时，比如存在 `let` 里的闭包，就由它的主体决定：其中有 `await` 就是异步的，有错误可能传出就会抛出错误。在闭包前写上 `async` 可以事先说明它是异步的，不管它现在有没有 `await`；这样以后再加上 `await`，调用它的地方也不受影响。这样的闭包不能传到需要不能等待的函数的地方。

```asuka
let refresh = async () => {
  await store.reload()
}
await refresh()
```

如果某个调用因为闭包主体里的代码而缺少 `await` 或 `try`，错误信息会指出是主体里的哪一处。

那些会当场调用闭包的内置方法，比如数组的 `map`、`filter`、`reduce`、`sorted(by:)` 等，都可以接受会抛出错误的闭包：这时调用本身也会抛出闭包抛出的错误，需要加 `try`，和 Swift 的 `rethrows` 一样。闭包不会抛出错误时，调用也不会。

```asuka
let counts = try lines.map(l => try parse(l))
let sorted = try names.sorted(by: (a, b) => try rank(a) < try rank(b))
```

函数、处理函数、闭包和方法的主体里也可以声明 `fn`。它像闭包一样能看到周围的东西，可以调用自己，也可以调用同一个代码块里紧挨着它声明的其他 `fn`（它们也能调用它）：

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

它和其他函数一样是一个值（`items.map(walk)`）。它的参数按顺序传入，不能带标签或默认值，它也不能是泛型的。

永远不会返回的函数（总是抛出错误、panic 或结束程序）用 `-> never` 表明这一点，`os.exit` 就是这样；调用它的地方会像 `return` 一样结束所在的代码块，它也可以出现在任何需要值的位置：`let port = text.toInt() ?? fail("not a port")`。

函数要调用自己，就这样写。如果把闭包存进一个变量，再通过这个变量调用自己（`var walk = …; walk = n => { … walk(c) … }`），闭包就持有这个变量，而变量又持有闭包：两者都永远不会被释放，因为没有什么会回收这样的循环引用，编译器也会对此给出警告。局部 `fn` 调用自己时不会持有自己。

## 接口与泛型

接口是一组方法和字段；一个类型只要具备这些方法和字段，就实现了这个接口（不需要额外声明）。

```asuka check
interface Shape { fn area() -> float }

fn total<T>(items: [T], size: fn(T) -> float) -> float {
  var sum = 0.0
  for i in items { sum += size(i) }
  return sum
}
```

接口本身也是一种类型：接口类型的值可以是任何具备这些方法的值，调用方法时调用的就是这个值自己的方法。

```asuka
let shapes: [Shape] = [Circle(radius: 1), Square(side: 2)]
let areas = shapes.map(s => s.area())
```

接口可以要求一个 `mut fn`（比如 prelude 里的 `Iterator<T>` 要求的 `mut fn next() -> T?`）：类型的 `mut fn` 满足它，签名相同的 `fn` 也满足它。反过来，`mut fn` 不能满足接口里的普通 `fn`，因为后者可以在任何值上调用。类型也可以通过[扩展](declarations.md#扩展)添加的方法获得接口，前提是这些扩展在当前位置可见。

泛型函数和泛型类型可以接收类型参数。类型参数的约束可以是程序自己的接口，也可以是 `Equatable`（能用 `==` 比较的类型：内置的值，以及各部分都能比较的结构体、枚举、元组、数组、map 和可选值）、`Comparable`（能用 `<` 排序的类型：整数、浮点数、字符串、时长、按成员顺序排序的不携带值的枚举，以及带有 `fn compare(other: T) -> int` 的结构体和枚举；结构体不会逐个字段地排序）和 `Hashable`（能做 map 键的类型：除浮点数以外的内置值，以及由它们组成的结构体、枚举、元组、可选值、数组、map 和集合，见[值与类型](types.md#值与类型)），多个约束用 `&` 连接。类型实参必须具备的东西，泛型代码里它的值也都能用：接口的方法和字段会针对每一个编译出的具体类型直接调用。

接口的方法也可以带方法体，这样的方法由接口提供。一个类型只要具备那些没有方法体的方法，就实现了这个接口；对于接口提供的方法，如果类型自己声明了签名相同的方法，就用它自己的，否则用接口的。在接口提供的方法里，`self` 就是这个接口的值，接口的方法和字段可以直接用名字访问：

```asuka check
interface Shape {
  fn area() -> float
  fn describe() -> string = "a shape of {area()}"
}

struct Square {
  side: float
  fn area() -> float = side * side
}

struct Circle {
  radius: float
  fn area() -> float = 3.14159 * radius * radius
  fn describe() -> string = "a circle of {radius}"
}

fn describeAll(shapes: [Shape]) -> [string] = shapes.map(s => s.describe())
```

接口提供的方法可以在接口类型的值上调用，也可以在以这个接口为约束的类型参数（如下文的 `T: Shape`）上调用；没有被当作 `Shape` 使用的 `Square` 并没有 `describe()`。如果两个接口各自提供了同名的方法，而某个类型自己没有这个方法，它就不能同时作为这两个接口使用：编译器会指出这一点，这时给这个类型声明自己的方法即可。

要判断一个接口值究竟是什么类型，可以用实现了该接口的类型的模式去匹配它：结构体的模式（`Circle(radius)`）、带类型的名字（`c: Circle`），或者写明类型的枚举成员（`Unit.big(n)`）。因为任何类型都可能实现某个接口，对接口值的 `match` 必须以 `_` 结尾：

```asuka
fn label(s: Shape) -> string = match s {
  Circle(radius: 0) => "a point"
  c: Circle => "a circle of {c.radius}"
  Square(side) => "a square of {side}"
  _ => s.describe()
}

if let c: Circle = shape { … }
let sq: Square = shape else { return }
```

模式里的类型必须实现了这个接口（没有 `area()` 的 `Point` 不可能是 `Shape`），而不携带值的枚举只有在声明了这个接口时（`enum Unit: Shape`）才算实现了它。具体类型的值不需要判断：拿 `c: Circle` 去匹配一个 `Circle` 会报错。对类型参数的值做这种判断时，结果在函数针对每个类型编译时就确定了，不可能匹配的分支不会出现在程序里。`catch e: ParseError` 也是同样的判断，只不过判断的是 `Error`（见[错误](errors.md)）。

泛型函数和泛型类型可以接收类型参数。类型参数的约束可以是程序自己的接口，也可以是 `Equatable`（能用 `==` 比较的类型：内置的值，以及各部分都能比较的结构体、枚举、元组、数组、map 和可选值）、`Comparable`（能用 `<` 排序的类型：整数、浮点数、字符串、时长、按成员顺序排序的不携带值的枚举，以及带有 `fn compare(other: T) -> int` 的结构体和枚举；结构体不会逐个字段地排序）和 `Hashable`（能做 map 键的类型：除浮点数以外的内置值，以及由它们组成的结构体、枚举、元组和可选值，见[值与类型](types.md#值与类型)），多个约束用 `&` 连接。类型实参必须具备的东西，泛型代码里它的值也都能用：接口的方法和字段会针对每一个编译出的具体类型直接调用。

```asuka
fn sortByArea<T: Shape>(xs: [T]) -> [T] = xs.sorted((a, b) => a.area() < b.area())

fn largestOnce<T: Shape & Equatable>(xs: [T]) -> [T] { … }

struct Labeled<T: Shape> {
  item: T
  fn label() -> string = "{item.area()}"
}
```

泛型函数的类型实参会从参数和对返回值的期望中推断出来；推断不出来时，用 `f<T>(…)` 显式写出：`json.decode<[Note]>(text)`、`flag.parse<Options>(args)`。

泛型会针对用到的每个类型各编译一份，所以它不能再以更大的类型回到自己身上：`fn depth<T>(x: T) -> int` 里调用 `depth([x])`，就需要 `depth<[int]>`，接着是 `depth<[[int]]>`，没有尽头；`struct Nested<T> { more: Nested<[T]>? }` 也是一样。编译器会指出出问题的位置。包含自身的类型要用同样的类型实参（`Tree<T>` 里的 `Tree<T>`）。

只要具备接口的方法和字段，类型实参就满足这个接口：可以是结构体或枚举（成员不携带值的枚举也可以），可以是要求了这些方法和字段的接口，也可以是带有相应约束的另一个类型参数。方法的参数必须和接口里的一样：参数更多的方法，哪怕多出来的参数都有默认值（接口要求 `greet(loud: bool)`，类型里却是 `greet(loud: bool, times: int = 1)`），也不算接口的方法，因为通过接口调用时只会传入接口自己的参数。`T: Error` 可以读取错误的 `message`。
