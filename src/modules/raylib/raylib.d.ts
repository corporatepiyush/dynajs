/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:raylib
/** dyna:raylib — raylib 6 (opt-in: CONFIG_RAYLIB=y, ./install.sh --with-raylib): one window per process, main thread only. Colours are 0xRRGGBBAA integers. */
declare module "dyna:raylib" {
    /** A colour as 0xRRGGBBAA, e.g. 0xff0000ff for opaque red (see rgba() and colors). */
    type Color = number;
    type Rect = { x: number; y: number; width: number; height: number };
    type Vec2 = { x: number; y: number };
    type Vec3 = { x: number; y: number; z: number };
    type Camera2D = { offset?: Vec2; target?: Vec2; rotation?: number; zoom?: number };
    type Camera3D = { position?: Vec3; target?: Vec3; up?: Vec3; fovy?: number; projection?: "perspective" | "orthographic" };
    /** raylib's named colours (RAYWHITE, RED, BLANK, ...) as 0xRRGGBBAA. */
    const colors: Readonly<Record<string, Color>>;
    /** Keyboard codes by raylib name without the KEY_ prefix (A, SPACE, ESCAPE, F1, KP_0, ...). */
    const keys: Readonly<Record<string, number>>;
    /** LEFT, RIGHT, MIDDLE, SIDE, EXTRA, FORWARD, BACK. */
    const mouseButtons: Readonly<Record<string, number>>;
    /** Gamepad buttons by raylib name without the GAMEPAD_BUTTON_ prefix. */
    const gamepadButtons: Readonly<Record<string, number>>;
    /** LEFT_X, LEFT_Y, RIGHT_X, RIGHT_Y, LEFT_TRIGGER, RIGHT_TRIGGER. */
    const gamepadAxes: Readonly<Record<string, number>>;
    /** The raylib version this binary was compiled against ("6.2"). */
    function version(): string;
    /** raylib's own log threshold (default "warning"). */
    function setLogLevel(level: "all" | "trace" | "debug" | "info" | "warning" | "error" | "fatal" | "none"): void;
    /** Packs channels 0..255 into a Color; alpha defaults to 255. */
    function rgba(r: number, g: number, b: number, a?: number): Color;
    /** Opens the default audio device; idempotent, throws when none exists. */
    function initAudio(): void;
    /** Closes the audio device and unloads every Sound, Music and AudioStream. */
    function closeAudio(): void;
    function audioReady(): boolean;
    /** Volume 0..1 for everything played. */
    function setMasterVolume(volume: number): void;
    /** Fills the current target (window frame or RenderTexture); every draw call needs an open Window. */
    function clearBackground(color: Color): void;
    function drawPixel(x: number, y: number, color: Color): void;
    /** thick defaults to 1 (0..4096). */
    function drawLine(x1: number, y1: number, x2: number, y2: number, color: Color, thick?: number): void;
    function drawCircle(cx: number, cy: number, radius: number, color: Color): void;
    function drawCircleLines(cx: number, cy: number, radius: number, color: Color): void;
    function drawEllipse(cx: number, cy: number, rx: number, ry: number, color: Color): void;
    function drawRectangle(x: number, y: number, width: number, height: number, color: Color): void;
    /** thick defaults to 1 (0..4096). */
    function drawRectangleLines(x: number, y: number, width: number, height: number, color: Color, thick?: number): void;
    /** roundness 0..1, segments 0..1024 (0 picks a count from the radius). */
    function drawRectangleRounded(x: number, y: number, width: number, height: number, roundness: number, segments: number, color: Color): void;
    /** Vertices in counter-clockwise order; a clockwise triangle draws nothing (raylib culls it). */
    function drawTriangle(x1: number, y1: number, x2: number, y2: number, x3: number, y3: number, color: Color): void;
    /** A regular polygon of 3..4096 sides. */
    function drawPoly(cx: number, cy: number, sides: number, radius: number, rotation: number, color: Color): void;
    /** Draws inside mode3D(). */
    function drawCube(x: number, y: number, z: number, width: number, height: number, length: number, color: Color): void;
    function drawCubeWires(x: number, y: number, z: number, width: number, height: number, length: number, color: Color): void;
    function drawSphere(x: number, y: number, z: number, radius: number, color: Color): void;
    /** An XZ plane centred at (x, y, z). */
    function drawPlane(x: number, y: number, z: number, width: number, length: number, color: Color): void;
    function drawLine3D(x1: number, y1: number, z1: number, x2: number, y2: number, z2: number, color: Color): void;
    /** slices 1..1024 around the origin on the XZ plane. */
    function drawGrid(slices: number, spacing: number): void;
    function drawFPS(x: number, y: number): void;
    /** The default font; fontSize is an integer 1..4096. */
    function drawText(text: string, x: number, y: number, fontSize: number, color: Color): void;
    /** Width in pixels of text in the default font. */
    function measureText(text: string, fontSize: number): number;
    /** font null uses the default font; spacing is extra pixels between glyphs. */
    function drawTextEx(font: Font | null, text: string, x: number, y: number, fontSize: number, spacing: number, color: Color): void;
    function measureTextEx(font: Font | null, text: string, fontSize: number, spacing: number): { width: number; height: number };
    /** Draws a Texture or RenderTexture at its own size (a RenderTexture upright); tint defaults to white. */
    function drawTexture(texture: Texture | RenderTexture, x: number, y: number, tint?: Color): void;
    /** src (texture pixels, top-left origin even for a RenderTexture) scaled into dst, rotated in degrees about dst.xy + origin. */
    function drawTexturePro(texture: Texture | RenderTexture, src: Rect, dst: Rect, originX?: number, originY?: number, rotation?: number, tint?: Color): void;
    /** Any image raylib decodes (PNG, BMP, TGA, JPG, GIF, QOI, HDR ...) as an RGBA texture; each side up to 16384, 2^26 pixels in all. */
    function loadTexture(path: string): Texture;
    /** The same formats decoded into a CPU-side RGBA Image (no Window needed). */
    function loadImage(path: string): Image;
    /** A TTF/OTF font rasterised at size 1..1024 (default 32) for ASCII. */
    function loadFont(path: string, size?: number): Font;
    /** WAV, OGG, MP3, FLAC or QOA decoded into memory (needs initAudio()). */
    function loadSound(path: string): Sound;
    /** The same formats streamed from the file; call update() every frame while it plays. */
    function loadMusic(path: string): Music;
    /** Decodes audio to interleaved f32 samples in -1..1 without an audio device (at most 2^28 samples). */
    function loadWave(path: string): { sampleRate: number; channels: number; samples: Float32Array };
    /** Keyboard state from the last frame's event poll; key is 0..511 (see keys). */
    function isKeyDown(key: number): boolean;
    function isKeyPressed(key: number): boolean;
    function isKeyReleased(key: number): boolean;
    /** The next queued key press, 0 when none. */
    function getKeyPressed(): number;
    /** The next queued typed character, "" when none. */
    function getCharPressed(): string;
    function mousePosition(): Vec2;
    function mouseDelta(): Vec2;
    /** Wheel movement this frame (positive is away from the user). */
    function mouseWheel(): number;
    /** button is 0..6 (see mouseButtons). */
    function isMouseButtonDown(button: number): boolean;
    function isMouseButtonPressed(button: number): boolean;
    function isMouseButtonReleased(button: number): boolean;
    /** gamepad is 0..3. */
    function isGamepadAvailable(gamepad: number): boolean;
    /** null when no pad is connected there. */
    function gamepadName(gamepad: number): string | null;
    /** axis 0..5 (see gamepadAxes); -1..1. */
    function gamepadAxis(gamepad: number, axis: number): number;
    /** button 0..17 (see gamepadButtons). */
    function isGamepadButtonDown(gamepad: number, button: number): boolean;
    /** True in the frame the button went down. */
    function isGamepadButtonPressed(gamepad: number, button: number): boolean;
    /** True in the frame the button came up. */
    function isGamepadButtonReleased(gamepad: number, button: number): boolean;
    /** Axes the pad reports, 0 when none is connected there. */
    function gamepadAxisCount(gamepad: number): number;
    /** The last gamepad button pressed on any pad (see gamepadButtons), 0 when none. */
    function getGamepadButtonPressed(): number;
    /** Motor intensities 0..1 for seconds 0..60; a pad without rumble ignores it. */
    function setGamepadVibration(gamepad: number, leftMotor: number, rightMotor: number, seconds: number): void;
    /** Wheel movement this frame on both axes (x is horizontal scrolling). */
    function mouseWheelV(): Vec2;
    /** An annulus between the two radii from startAngle to endAngle degrees; segments 0..4096 (0 picks a count). */
    function drawRing(cx: number, cy: number, innerRadius: number, outerRadius: number, startAngle: number, endAngle: number, segments: number, color: Color): void;
    /** A filled pie slice from startAngle to endAngle degrees; segments 0..4096 (0 picks a count). */
    function drawCircleSector(cx: number, cy: number, radius: number, startAngle: number, endAngle: number, segments: number, color: Color): void;
    /** A rectangle whose colour blends between its four corners. */
    function drawRectangleGradient(x: number, y: number, width: number, height: number, topLeft: Color, bottomLeft: Color, topRight: Color, bottomRight: Color): void;
    /** Connected segments through 2..65536 points (x, y pairs); thick above 0 draws them that wide. */
    function drawLineStrip(points: Float32Array, color: Color, thick?: number): void;
    /** A convex polygon fanned from the first of 3..65536 points, given counter-clockwise. */
    function drawTriangleFan(points: Float32Array, color: Color): void;
    /** A screen point in the world of a mode2D() camera (no Window needed). */
    function screenToWorld2D(x: number, y: number, camera: Camera2D): Vec2;
    /** A world point on the screen of a mode2D() camera (no Window needed). */
    function worldToScreen2D(x: number, y: number, camera: Camera2D): Vec2;
    /** A world point on the window of a mode3D() camera. */
    function worldToScreen(x: number, y: number, z: number, camera: Camera3D): Vec2;
    function showCursor(): void;
    function hideCursor(): void;
    /** Shows the cursor and frees it from disableCursor(). */
    function enableCursor(): void;
    /** Hides the cursor and locks it to the window, for mouse-look (mouseDelta keeps reporting motion). */
    function disableCursor(): void;
    function isCursorHidden(): boolean;
    function isCursorOnScreen(): boolean;
    function setMouseCursor(cursor: "default" | "arrow" | "ibeam" | "crosshair" | "pointer" | "resize-ew" | "resize-ns" | "resize-nwse" | "resize-nesw" | "resize-all" | "not-allowed"): void;
    /** Clipboard text ("" when empty). */
    function getClipboardText(): string;
    function setClipboardText(text: string): void;
    /** The connected monitors; sizes are the current video mode, physical sizes in millimetres. */
    function monitors(): { name: string; x: number; y: number; width: number; height: number; physicalWidth: number; physicalHeight: number; refreshRate: number }[];
    /** Touch points down this frame (0 on a desktop without a touchscreen). */
    function touchCount(): number;
    /** Touch point index 0..7 in window coordinates, null past touchCount(). */
    function touchPoint(index: number): { x: number; y: number; id: number } | null;
    /** The gesture raylib recognised from touch or the left mouse button this frame. */
    function gesture(): "none" | "tap" | "doubletap" | "hold" | "drag" | "swipe-right" | "swipe-left" | "swipe-up" | "swipe-down" | "pinch-in" | "pinch-out";
    /** Measurements of the gesture in progress: hold seconds, drag and pinch vectors, angles in degrees. */
    function gestureDetails(): { holdDuration: number; drag: Vec2; dragAngle: number; pinch: Vec2; pinchAngle: number };
    /** The process's one raylib window (1..16384 a side); closing it unloads every Texture, RenderTexture and Font. fps 0 means uncapped. */
    class Window {
        constructor(width: number, height: number, title: string, opts?: { hidden?: boolean; resizable?: boolean; highDensity?: boolean; vsync?: boolean; msaa?: boolean; undecorated?: boolean; topmost?: boolean; transparent?: boolean; fps?: number });
        /** True once the user closed the window or pressed the exit key (ESC by default). */
        readonly shouldClose: boolean;
        readonly size: { width: number; height: number };
        /** Framebuffer size in pixels (larger than size on a high-density display with highDensity). */
        readonly renderSize: { width: number; height: number };
        readonly scaleDPI: Vec2;
        readonly focused: boolean;
        readonly minimized: boolean;
        readonly fps: number;
        /** Seconds the last frame took. */
        readonly frameTime: number;
        /** Seconds since the window opened. */
        readonly time: number;
        setTitle(title: string): void;
        /** size then reports width x height. On macOS windows are always high-density (highDensity is implied), so size stays in points and renderSize in pixels across resizes. */
        setSize(width: number, height: number): void;
        setPosition(x: number, y: number): void;
        /** 0..1000; frame() then waits out the rest of each frame's slot. */
        setTargetFPS(fps: number): void;
        /** The key that sets shouldClose (0 disables it). */
        setExitKey(key: number): void;
        /** Runs draw between BeginDrawing and EndDrawing (which presents and polls input); draw must be synchronous and frame() cannot nest. Closing the window inside it takes effect when it returns. */
        frame(draw: () => void): void;
        /** Runs draw with a 2D camera, inside frame() or RenderTexture#draw(); zoom must be above 0. */
        mode2D(camera: Camera2D, draw: () => void): void;
        /** Runs draw with a 3D camera (fovy in (0, 180), default 45, perspective). */
        mode3D(camera: Camera3D, draw: () => void): void;
        /** Runs draw with a blend mode, inside frame() or RenderTexture#draw(); it does not nest in itself. */
        blendMode(mode: "alpha" | "additive" | "multiplied" | "add-colors" | "subtract-colors" | "alpha-premultiply", draw: () => void): void;
        /** Runs draw clipped to a rectangle of the target in pixels (not camera coordinates), inside frame() or RenderTexture#draw(); it does not nest in itself. */
        scissor(x: number, y: number, width: number, height: number, draw: () => void): void;
        readonly fullscreen: boolean;
        readonly maximized: boolean;
        readonly hidden: boolean;
        /** True in the frame after the window changed size. */
        readonly resized: boolean;
        readonly position: Vec2;
        /** Index into monitors() of the monitor the window is on. */
        readonly monitor: number;
        /** The state changes below run outside frame() and RenderTexture#draw(). */
        toggleFullscreen(): void;
        toggleBorderless(): void;
        maximize(): void;
        minimize(): void;
        restore(): void;
        focus(): void;
        setResizable(on: boolean): void;
        setTopmost(on: boolean): void;
        /** false removes the title bar and border. */
        setDecorated(on: boolean): void;
        /** Keeps frames running while the window is minimized or unfocused. */
        setAlwaysRun(on: boolean): void;
        /** Lets clicks fall through to what is behind the window. */
        setMousePassthrough(on: boolean): void;
        /** 0..1. */
        setOpacity(opacity: number): void;
        /** Limits user resizing; 1..16384 a side. */
        setMinSize(width: number, height: number): void;
        setMaxSize(width: number, height: number): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** An RGBA GPU texture from loadTexture() or Image#toTexture(); it keeps its Window alive. */
    class Texture {
        readonly width: number;
        readonly height: number;
        /** Uploads RGBA bytes for the rect (whole pixels inside the texture) or the whole texture. */
        update(pixels: Uint8Array, rect?: Rect): void;
        setFilter(mode: "point" | "bilinear" | "trilinear"): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** An offscreen RGBA target (each side up to 16384, 2^26 pixels); it keeps its Window alive. */
    class RenderTexture {
        constructor(width: number, height: number);
        readonly width: number;
        readonly height: number;
        /** Runs draw with this texture as the target; it cannot nest inside another draw(), mode2D() or mode3D(). */
        draw(draw: () => void): void;
        /** The texture's pixels as top-to-bottom RGBA rows. */
        readPixels(): { width: number; height: number; pixels: Uint8Array };
        setFilter(mode: "point" | "bilinear" | "trilinear"): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** A CPU-side RGBA image (no Window needed until toTexture()); each side up to 16384, 2^26 pixels in all. */
    class Image {
        /** fill is a Color (default transparent) or exactly width*height*4 RGBA bytes. */
        constructor(width: number, height: number, fill?: Color | Uint8Array);
        readonly width: number;
        readonly height: number;
        /** A copy of the RGBA bytes. */
        pixels(): Uint8Array;
        /** The extension picks the format: .png, .bmp, .qoi or .tga. */
        export(path: string): void;
        resize(width: number, height: number): void;
        /** The rect must lie inside the image. */
        crop(x: number, y: number, width: number, height: number): void;
        flipVertical(): void;
        flipHorizontal(): void;
        copy(): Image;
        toTexture(): Texture;
        /** CPU drawing straight into the pixels (no Window needed, except drawText): whole-pixel coordinates within +-65536, clipped to the image. */
        clear(color: Color): void;
        drawPixel(x: number, y: number, color: Color): void;
        /** thick 1..4096 (default 1). */
        drawLine(x1: number, y1: number, x2: number, y2: number, color: Color, thick?: number): void;
        drawRectangle(x: number, y: number, width: number, height: number, color: Color): void;
        /** An outline thick pixels wide (1..4096, default 1) inside the rectangle. */
        drawRectangleLines(x: number, y: number, width: number, height: number, color: Color, thick?: number): void;
        drawCircle(cx: number, cy: number, radius: number, color: Color): void;
        drawCircleLines(cx: number, cy: number, radius: number, color: Color): void;
        drawTriangle(x1: number, y1: number, x2: number, y2: number, x3: number, y3: number, color: Color): void;
        /** Alpha-blends another Image (or this one) at x, y, multiplied by tint (default white). */
        drawImage(source: Image, x: number, y: number, tint?: Color): void;
        /** Text in the default font, which needs an open Window. */
        drawText(text: string, x: number, y: number, fontSize: number, color: Color): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** A rasterised font from loadFont(); it keeps its Window alive. */
    class Font {
        readonly baseSize: number;
        readonly glyphCount: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** A decoded sound from loadSound(); closeAudio() unloads it. */
    class Sound {
        readonly playing: boolean;
        play(): void;
        stop(): void;
        pause(): void;
        resume(): void;
        /** 0..1. */
        setVolume(volume: number): void;
        /** 0.01..16, 1 is unchanged. */
        setPitch(pitch: number): void;
        /** 0 left .. 1 right. */
        setPan(pan: number): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** Streamed audio from loadMusic(); update() it every frame while it plays. */
    class Music {
        readonly playing: boolean;
        /** Length in seconds. */
        readonly length: number;
        /** Seconds played so far; seek() sets it, and update() then subtracts what it buffers ahead. */
        readonly played: number;
        play(): void;
        stop(): void;
        pause(): void;
        resume(): void;
        /** Refills the stream's buffers. */
        update(): void;
        setVolume(volume: number): void;
        setPitch(pitch: number): void;
        setPan(pan: number): void;
        /** seconds must not pass length. */
        seek(seconds: number): void;
        setLooping(on: boolean): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** Raw f32 output, 8000..192000 Hz, 1 or 2 channels (needs initAudio()). */
    class AudioStream {
        constructor(sampleRate: number, channels: number);
        readonly playing: boolean;
        /** True when the stream wants its next buffer. */
        readonly processed: boolean;
        play(): void;
        stop(): void;
        pause(): void;
        resume(): void;
        /** Queues whole frames of interleaved samples; refused until processed is true. */
        update(samples: Float32Array): void;
        setVolume(volume: number): void;
        setPitch(pitch: number): void;
        setPan(pan: number): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}
