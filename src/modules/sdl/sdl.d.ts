/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:sdl
/** dyna:sdl — SDL3 windows, 2D rendering, input events and audio playback; present only in a CONFIG_SDL3=y build. Video calls must run on the main thread (never in an os.Worker). */
declare module "dyna:sdl" {
    type Subsystem = "video" | "audio" | "events" | "joystick" | "gamepad";
    type Rect = { x: number; y: number; width: number; height: number };
    /** One polled event; fields beyond type/timestamp depend on the type ("other" covers kinds not mapped here). */
    interface Event {
        type: "quit" | "keydown" | "keyup" | "textinput" | "textediting" | "dropfile" | "mousemotion" | "mousebuttondown" | "mousebuttonup" | "mousewheel" | "windowshown" | "windowhidden" | "windowmoved" | "windowresized" | "windowfocusgained" | "windowfocuslost" | "windowclose" | "windowexposed" | "windowminimized" | "windowmaximized" | "windowrestored" | "windowmouseenter" | "windowmouseleave" | "windowpixelsizechanged" | "windowdisplayscalechanged" | "fingerdown" | "fingerup" | "fingermotion" | "fingercanceled" | "gamepadadded" | "gamepadremoved" | "gamepadbuttondown" | "gamepadbuttonup" | "gamepadaxis" | "user" | "other";
        /** Milliseconds since SDL started. */
        timestamp: number;
        windowId?: number;
        key?: number;
        keyName?: string;
        scancode?: number;
        mod?: number;
        repeat?: boolean;
        text?: string;
        /** textediting: the selected part of the IME composition text (-1 when unset). */
        start?: number;
        length?: number;
        /** dropfile: the dropped file's path. */
        file?: string;
        x?: number;
        y?: number;
        xrel?: number;
        yrel?: number;
        buttons?: number;
        button?: number;
        clicks?: number;
        data1?: number;
        data2?: number;
        code?: number;
        /** finger*: the touch device and the finger on it; x, y are 0..1 of the window, dx, dy the motion in the same units. */
        touchId?: number;
        fingerId?: number;
        dx?: number;
        dy?: number;
        /** finger*: 0..1. */
        pressure?: number;
        /** gamepad*: the gamepad's id (a Gamepad opens it). */
        which?: number;
        /** gamepadaxis: the axis name and its value, -1..1 (triggers 0..1). gamepadbutton*: `button` is the button's name. */
        axis?: string;
        value?: number;
    }
    /** System cursor names, the CSS cursor vocabulary. */
    type Cursor = "default" | "text" | "wait" | "crosshair" | "progress" | "nwse-resize" | "nesw-resize" | "ew-resize" | "ns-resize" | "move" | "not-allowed" | "pointer" | "nw-resize" | "n-resize" | "ne-resize" | "e-resize" | "se-resize" | "s-resize" | "sw-resize" | "w-resize";
    /** Shows the mouse cursor (needs video). */
    function showCursor(): void;
    /** Hides the mouse cursor (needs video). */
    function hideCursor(): void;
    /** Whether the mouse cursor is shown (needs video). */
    function cursorVisible(): boolean;
    /** Switches to a system cursor; quit() frees the cursors it made (needs video). */
    function setCursor(name: Cursor): void;
    /** Ids of the connected gamepads (needs init(["gamepad"])). */
    function gamepads(): number[];
    /** Connects a software gamepad with every standard button and axis, for tests and input injection; returns its id (needs init(["gamepad"])). */
    function attachVirtualGamepad(name?: string): number;
    /** Disconnects a gamepad attachVirtualGamepad() made; any other id is refused. */
    function detachVirtualGamepad(id: number): void;
    /** Battery state; seconds and percent are null when the platform does not say. */
    function powerInfo(): { state: "error" | "unknown" | "battery" | "no-battery" | "charging" | "charged"; seconds: number | null; percent: number | null };
    /** The desktop's light/dark preference. */
    function systemTheme(): "unknown" | "light" | "dark";
    /** The user's languages in order of preference, as "en-US" or "fr". */
    function preferredLocales(): string[];
    /** Logical CPU cores. */
    function cpuCount(): number;
    /** Physical memory in MiB. */
    function systemRAM(): number;
    /** The directory the application runs from (with a trailing separator), or null when SDL cannot tell. */
    function basePath(): string | null;
    /** Starts subsystems (default ["video"]); repeated calls only add what is missing. Video refuses to start off the main thread. */
    function init(subsystems?: Subsystem[]): void;
    /** Shuts down every subsystem init() started; refused while a Window, AudioStream or Gamepad is still open. */
    function quit(): void;
    /** The subsystems currently running. */
    function wasInit(): Subsystem[];
    /** The linked SDL library's version. */
    function version(): { major: number; minor: number; patch: number; revision: string };
    /** "macOS", "Linux", "Windows", ... */
    function platform(): string;
    /** The last SDL error text ("" when none). */
    function getError(): string;
    /** Video drivers compiled into SDL; set SDL_VIDEODRIVER (e.g. "dummy" for headless) before init to pick one. */
    function videoDrivers(): string[];
    /** The running video driver, or null before init(["video"]). */
    function currentVideoDriver(): string | null;
    /** Audio drivers compiled into SDL; SDL_AUDIODRIVER picks one. */
    function audioDrivers(): string[];
    /** The running audio driver, or null before init(["audio"]). */
    function currentAudioDriver(): string | null;
    /** Milliseconds since SDL was first used. */
    function ticks(): number;
    /** Connected displays (needs video). */
    function displays(): { id: number; name: string | null; x: number; y: number; width: number; height: number; refreshRate: number; pixelDensity: number }[];
    /** The next queued event, or null; main thread only. Call it every frame (e.g. from setInterval) to keep windows responsive. */
    function pollEvent(): Event | null;
    /** Drains up to `max` queued events (default 256, at most 65536). */
    function pollEvents(max?: number): Event[];
    /** Queues a synthetic event that pollEvent() returns with the same fields (input injection, tests); unknown keys and fields of another event type are refused by name, only own enumerable fields are read, finger x/y are 0..1, dx/dy -1..1, pressure 0..1, button 1..5. */
    function pushEvent(event: { type: "user" | "quit" | "keydown" | "keyup" | "mousemotion" | "mousebuttondown" | "mousebuttonup" | "mousewheel" | "fingerdown" | "fingerup" | "fingermotion" | "fingercanceled"; code?: number; windowId?: number; key?: number; scancode?: number; mod?: number; repeat?: boolean; x?: number; y?: number; xrel?: number; yrel?: number; buttons?: number; button?: number; clicks?: number; touchId?: number; fingerId?: number; dx?: number; dy?: number; pressure?: number }): void;
    /** The display name of a keycode ("" for an unknown one, including codes that are no character). */
    function keyName(key: number): string;
    /** The keycode for a key name (0 when unknown). */
    function keyFromName(name: string): number;
    /** Pointer position relative to the focused window, and the pressed-button mask (needs video). */
    function mouseState(): { x: number; y: number; buttons: number };
    /** Sets an SDL hint by name (e.g. "SDL_RENDER_DRIVER"); true when SDL accepted it. Most hints must be set before init. */
    function setHint(name: string, value: string): boolean;
    /** Clipboard text ("" when empty; needs video). */
    function getClipboardText(): string;
    /** Whether the clipboard holds text (needs video). */
    function hasClipboardText(): boolean;
    /** Replaces the clipboard text (needs video). */
    function setClipboardText(text: string): void;
    /** Playback devices, or recording devices with { recording: true }: SDL's id and display name for each (needs init(["audio"])). */
    function audioDevices(opts?: { recording?: boolean }): { id: number; name: string }[];
    /** Decodes a WAV file to interleaved f32 samples in -1..1 (at most 256 MiB of decoded audio). */
    function loadWAV(path: string): { freq: number; channels: number; samples: Float32Array };
    /** Loads a BMP or PNG file into an RGBA Texture of the renderer (each side 1..16384). */
    function loadTexture(renderer: Renderer, path: string): Texture;
    /** A window; width and height are 1..16384. Closing it also closes its Renderer and that renderer's Textures. */
    class Window {
        constructor(title: string, width: number, height: number, opts?: { resizable?: boolean; hidden?: boolean; borderless?: boolean; highDensity?: boolean; fullscreen?: boolean; alwaysOnTop?: boolean });
        readonly id: number;
        readonly title: string;
        /** Size in window coordinates. */
        readonly size: { width: number; height: number };
        /** Size in pixels (larger than size on a high-density display). */
        readonly pixelSize: { width: number; height: number };
        readonly position: { x: number; y: number };
        setTitle(title: string): void;
        setSize(width: number, height: number): void;
        setPosition(x: number, y: number): void;
        show(): void;
        hide(): void;
        raise(): void;
        setFullscreen(on: boolean): void;
        /** Content scale of the window's display (2 on a typical high-density screen). */
        readonly displayScale: number;
        /** Whether textinput and textediting events are being delivered to this window. */
        readonly textInputActive: boolean;
        /** Smallest size the user can resize to; 0 removes the limit. */
        setMinimumSize(width: number, height: number): void;
        /** Largest size the user can resize to; 0 removes the limit. */
        setMaximumSize(width: number, height: number): void;
        /** Opacity 0..1; a video driver without window opacity throws. */
        setOpacity(opacity: number): void;
        /** Starts delivering textinput (and IME textediting) events for this window; a text field needs it. */
        startTextInput(): void;
        /** Stops textinput and textediting events for this window. */
        stopTextInput(): void;
        /** Where the text being edited sits (cursor is the caret's x offset inside rect), so an IME can place its candidate window; null clears it. */
        setTextInputArea(rect: Rect | null, cursor?: number): void;
        /** The window's flags as SDL reports them now (a window manager may apply a request later; sync() waits for it). */
        readonly state: { fullscreen: boolean; hidden: boolean; borderless: boolean; resizable: boolean; minimized: boolean; maximized: boolean; mouseGrabbed: boolean; inputFocus: boolean; mouseFocus: boolean; highDensity: boolean; relativeMouse: boolean; alwaysOnTop: boolean; occluded: boolean };
        maximize(): void;
        minimize(): void;
        restore(): void;
        /** Asks the desktop to draw attention to the window briefly. */
        flash(): void;
        /** Waits until pending size, position and state requests have been applied; throws when the window manager does not apply them in time. */
        sync(): void;
        setResizable(on: boolean): void;
        setBordered(on: boolean): void;
        setAlwaysOnTop(on: boolean): void;
        /** Confines the mouse to the window. */
        setMouseGrab(on: boolean): void;
        /** Hides the cursor and reports only relative motion (xrel/yrel), for mouse-look. */
        setRelativeMouseMode(on: boolean): void;
        /** Captures system shortcuts (Alt-Tab and the like) while the window has focus, where the platform allows it. */
        setKeyboardGrab(on: boolean): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** A 2D renderer for one window (SDL allows one per window); it keeps its Window alive. */
    class Renderer {
        constructor(window: Window, opts?: { driver?: string; vsync?: boolean });
        /** The backend in use ("metal", "opengl", "software", ...). */
        readonly name: string;
        readonly outputSize: { width: number; height: number };
        /** Channels are integers 0..255; alpha defaults to 255. */
        setDrawColor(r: number, g: number, b: number, a?: number): void;
        clear(): void;
        fillRect(x: number, y: number, width: number, height: number): void;
        drawRect(x: number, y: number, width: number, height: number): void;
        drawLine(x1: number, y1: number, x2: number, y2: number): void;
        drawPoint(x: number, y: number): void;
        /** Draws a Texture of this renderer; src/dst default to the whole texture / whole target. With opts, it turns `angle` degrees clockwise about `center` (dst coordinates, default the middle of dst) and mirrors by `flip`. */
        drawTexture(texture: Texture, src?: Rect | null, dst?: Rect | null, opts?: { angle?: number; center?: { x: number; y: number }; flip?: "none" | "horizontal" | "vertical" | "both" }): void;
        /** Reads back what has been drawn as RGBA rows (call it before present(), which may discard the frame). */
        readPixels(rect?: Rect): { width: number; height: number; pixels: Uint8Array };
        /** Draws into a { target: true } Texture of this renderer until setTarget(null) returns to the window. */
        setTarget(texture: Texture | null): void;
        /** Limits drawing to rect in viewport coordinates; null removes the clip. */
        setClipRect(rect: Rect | null): void;
        /** Maps drawing to rect of the current target; null restores the whole target. */
        setViewport(rect: Rect | null): void;
        /** Scales drawing coordinates; each factor is in (0, 1024]. */
        setScale(scaleX: number, scaleY: number): void;
        /** Blending for fill/draw calls (a Texture keeps its own mode). */
        setBlendMode(mode: "none" | "blend" | "add" | "mod" | "mul"): void;
        /** Turns vsync on or off; a driver without vsync throws. */
        setVSync(on: boolean): void;
        /** Triangles from 8 floats per vertex (x, y, then r, g, b, a and u, v in 0..1); indices, or else the vertices taken in threes, form them. */
        geometry(vertices: Float32Array, indices?: Int32Array | null, texture?: Texture | null): void;
        /** Draws text in SDL's built-in 8x8 font in the draw colour (characters outside ASCII show as '?'). */
        debugText(x: number, y: number, text: string): void;
        /** Draws at a fixed width x height and fits that to the output (mode default "letterbox"); "disabled" with 0, 0 turns it off. Events keep window coordinates: map them with windowToRender. */
        setLogicalPresentation(width: number, height: number, mode?: "disabled" | "stretch" | "letterbox" | "overscan" | "integer"): void;
        /** A window point in render coordinates (after logical presentation, viewport and scale). */
        windowToRender(x: number, y: number): { x: number; y: number };
        /** A render point in window coordinates. */
        renderToWindow(x: number, y: number): { x: number; y: number };
        /** Shows the frame. */
        present(): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** An RGBA texture owned by a Renderer (blend mode "blend" by default); it keeps its Renderer alive. */
    class Texture {
        /** { target: true } makes a texture Renderer.setTarget can draw into. */
        constructor(renderer: Renderer, width: number, height: number, opts?: { target?: boolean });
        readonly width: number;
        readonly height: number;
        /** True for a texture created with { target: true }. */
        readonly target: boolean;
        /** Uploads RGBA bytes (width*height*4 of the rect, or of the whole texture); the rect must lie inside the texture. */
        update(pixels: ByteView, rect?: Rect): void;
        setAlphaMod(alpha: number): void;
        setColorMod(r: number, g: number, b: number): void;
        setBlendMode(mode: "none" | "blend" | "add" | "mod" | "mul"): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** Playback, or recording with { recording: true }, on the default device or an audioDevices() id of that kind (needs init(["audio"])); starts paused. format "f32" uses Float32Array samples, "s16" Int16Array; channels interleave. */
    class AudioStream {
        constructor(opts?: { freq?: number; channels?: number; format?: "f32" | "s16"; recording?: boolean; device?: number });
        /** Bytes queued and not yet played. */
        readonly queued: number;
        /** Bytes ready to read: captured audio on a recording stream. */
        readonly available: number;
        /** True for a recording stream. */
        readonly recording: boolean;
        /** Queues interleaved samples of the stream's format (playback streams only). */
        put(samples: Float32Array | Int16Array): void;
        /** Reads up to maxSamples captured samples, whole frames only (recording streams only; default and maximum 2^24). */
        get(maxSamples?: number): Float32Array | Int16Array;
        resume(): void;
        pause(): void;
        /** Drops everything queued. */
        clear(): void;
        /** Marks the end of the queued data so it plays out fully. */
        flush(): void;
        /** Volume multiplier 0..16 (1 is unchanged). */
        setGain(gain: number): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** An opened gamepad (needs init(["gamepad"])); button and axis names are SDL's ("a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright"; "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"). */
    class Gamepad {
        constructor(id: number);
        readonly id: number;
        readonly name: string;
        /** SDL's controller family, e.g. "xbox360", "ps5", "standard". */
        readonly type: string;
        readonly connected: boolean;
        /** True for a gamepad from attachVirtualGamepad(). */
        readonly virtual: boolean;
        button(name: string): boolean;
        /** -1..1; triggers read 0..1. */
        axis(name: string): number;
        /** Vibrates the two motors (intensity 0..1) for ms milliseconds (at most 60000); false when the device cannot. */
        rumble(low: number, high: number, ms: number): boolean;
        /** Presses or releases a button of a virtual gamepad; SDL drops presses while a window exists and none has focus unless the hint SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS is "1". */
        setVirtualButton(name: string, down: boolean): void;
        /** Moves an axis of a virtual gamepad, value -1..1. */
        setVirtualAxis(name: string, value: number): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}
