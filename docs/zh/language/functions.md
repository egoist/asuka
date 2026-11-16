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

```asuka
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

泛型函数和泛型类型可以接收类型参数。类型参数的约束可以是程序自己的接口，也可以是 `Equatable`、`Comparable` 和 `Hashable`（当结构体和枚举的字段都满足后三者之一时，它们自身也就满足），多个约束用 `&` 连接。类型实参必须具备的东西，泛型代码里它的值也都能用：接口的方法和字段会针对每一个编译出的具体类型直接调用。

```asuka
fn sortByArea<T: Shape>(xs: [T]) -> [T] = xs.sorted((a, b) => a.area() < b.area())

fn largestOnce<T: Shape & Hashable>(xs: [T]) -> [T] { … }

struct Labeled<T: Shape> {
  item: T
  fn label() -> string = "{item.area()}"
}
```

泛型函数的类型实参会从参数和对返回值的期望中推断出来；推断不出来时，用 `f<T>(…)` 显式写出：`json.decode<[Note]>(text)`、`flag.parse<Options>(args)`。

只要具备接口的方法和字段，类型实参就满足这个接口：可以是结构体或枚举（成员不携带值的枚举也可以），可以是要求了这些方法和字段的接口，也可以是带有相应约束的另一个类型参数。`T: Error` 可以读取错误的 `message`。
