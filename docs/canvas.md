# Canvas 2D

`CanvasRenderingContext2D` (`src/web-api/js/canvas.js`) is the interface; the drawing is Skia's, on a raster surface that each context has (`src/paint/Canvas.cpp`,
`CanvasSurface`), reached through a few natives that take numbers (`src/paint/CanvasNatives.cpp`: one call, `__solarCv(id, operation, a ... h)`, for most of what the
context does). Only the window program (`solar`) has them: the command line program and the tests have no drawing, and `getContext("2d")` answers null there.

* **Showing it**: the canvas element's box is drawn by a `DrawCanvas` item of the display list, which asks the surface for the picture that was last published. What the page
  draws is published by one task of the loop for all the drawing there was meanwhile, which also tells layout the element changed. (The page thread draws; the window thread
  reads an image that does not change, so there is no lock around drawing.)
* **Paths** are kept in the canvas's coordinates (each point is mapped by the matrix of the call), as the specification says, and are taken back to the space of the matrix of
  the moment they are filled or stroked, so a line width follows the matrix of then. A `Path2D` is a list of numbers (`PathWriter::AddCommands` plays it back) and has no
  matrix; `addPath` with a matrix puts the matrix in the list.
* **Text** uses the system's fonts through the font database (`font` is parsed as the CSS shorthand); it is not shaped (no ligatures, no right-to-left reordering, no fallback for
  missing glyphs), and `letterSpacing`, `wordSpacing` and `filter` are kept and given back but have no effect.
* **Images**: an image element's file is fetched by the program without CORS (`src/web-api/ImageLoad.cpp`) and decoded by Skia; `src/web-api/js/image.js` gives `<img>`
  `naturalWidth`, `complete`, `decode()` and the load and error events. A document's own images are loaded by layout; script that asks for the size of one gets a load of its own.
  SVG is not decoded for canvas. There is no tainting of a canvas that has a picture of another origin (`getImageData` and `toDataURL` always work).
* **Others**: `OffscreenCanvas` (also in workers), `ImageBitmap` and `createImageBitmap` (images, canvases, videos, blobs, `ImageData`, with a crop; no resize options),
  `toDataURL` and `toBlob` (PNG, JPEG, WebP), `getImageData` and `putImageData`, gradients (linear, radial, conic), patterns, shadows, dashes, all the blend modes, `roundRect`,
  hit testing. Not done: `bitmaprenderer`, WebGL (`getContext("webgl")` answers null), color spaces other than sRGB, `ctx.filter`.
