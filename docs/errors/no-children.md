# no-children

Children given to an element that takes none, or not those.

An error the compiler reports: the program doesn't build until it's fixed.

## Why

An element takes children when it declares `prop children: View` (or is a container); an `Alert`'s block holds its buttons and fields only, and a `List`'s one `for`, whose items are its rows.

## How to fix

Put the views in a container (`Column`, `Row`), or beside the element. A row of another kind in a `List` (a header, a status) is an item of its `for`: an enum of the kinds of row.

## Wrong

```asuka
component Note {
  Text("Title") {
    Text("Body")
  }
}
```

## Right

```asuka
component Note {
  Column {
    Text("Title")
    Text("Body")
  }
}
```
