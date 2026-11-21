/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:svg
/** dyna:svg — SVG 1.1 to raster: renders an SVG document to PNG bytes or raw RGBA pixels, natively, with no external libraries.
 *  Both functions are asynchronous only: each call copies its input, then parses and rasterises on the shared worker pool (one
 *  render per pool thread, so parallel calls spread across the pool) and settles its Promise on the event loop (it runs as a fiber offloaded to the pool like dyna:async offload: calls beyond the pool queue wait in order, and with --io-threads 0 the render runs inline on the loop thread, settling the same way); there are no
 *  synchronous variants. Every failure is a rejection, never a synchronous throw: an input that is not a string or Uint8Array,
 *  a bad option type, or a document with no <svg> root rejects with TypeError; an out-of-range option, or an image or document
 *  over the built-in resource limits (64 Mpx output, 200000 nodes and others), rejects with RangeError; elements nested deeper than 1024 levels are not drawn.
 *  Supported: shapes, paths, transforms, viewBox and preserveAspectRatio, units, solid paint, linear and radial gradients with
 *  spread modes, patterns, clipPath, mask, opacity, markers, <use>/<symbol>, nested <svg>, <switch>, CSS (<style>, inline style,
 *  selectors), text in a built-in bitmap font (tspan, textPath, rotate, anchors), the SVG 1.1 filter primitives, and data: URI
 *  images (PNG, JPEG, SVG). External files are never read: an image href that is not a data: URI draws nothing.
 *  Rendering uses 4x supersampled anti-aliasing; output pixels are non-premultiplied RGBA8, rows top to bottom. */
declare module "dyna:svg" {
    /** Render options. width/height: output size in px, integers 1..65536 (one alone keeps the aspect ratio; neither = the
     *  document's own size). scale: extra uniform scale, > 0 and <= 65536 (default 1). background: fill painted under the
     *  drawing, "#RRGGBB" or "#RRGGBBAA" (the "#" is optional) or an integer 0xRRGGBBAA; omitted = transparent. */
    interface RenderOptions {
        width?: number;
        height?: number;
        scale?: number;
        background?: string | number;
    }
    /** Renders `svg` (the document text, or its UTF-8 bytes) to a PNG image: 8-bit RGBA, non-interlaced, zlib-compressed. */
    function render(svg: string | Uint8Array, opts?: RenderOptions): Promise<Uint8Array>;
    /** Renders `svg` to raw pixels: `data` holds width*height*4 bytes, non-premultiplied RGBA, row-major from the top-left. */
    function renderRGBA(svg: string | Uint8Array, opts?: RenderOptions): Promise<{ width: number; height: number; data: Uint8Array }>;
}
