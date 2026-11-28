// Cursor: the pointer's shape over a view, CSS's cursor on its element,
// which the views inside it inherit unless they set their own (a text
// field keeps its I-beam: style.css). Auto: the element's own (the
// property removed).

/** cursorCSS is the CSS cursor of a Cursor value (ops.Cursor's order); "" for Auto and what it doesn't know. */
export function cursorCSS(v) {
  return names[v] ?? "";
}

const names = [
  "", // Auto
  "default", // Arrow
  "pointer",
  "text",
  "crosshair",
  "move",
  "grab",
  "grabbing",
  "not-allowed",
  "wait",
  "progress",
  "help",
  "context-menu",
  "copy",
  "alias",
  "ew-resize", // ResizeHorizontal
  "ns-resize", // ResizeVertical
  "nwse-resize", // ResizeDiagonal
  "nesw-resize", // ResizeAntiDiagonal
  "col-resize", // ResizeColumn
  "row-resize", // ResizeRow
  "zoom-in",
  "zoom-out",
  "none", // Hidden
];
