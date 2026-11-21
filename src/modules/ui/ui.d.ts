/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:ui
/** dyna:url — WHATWG URL and URLSearchParams (the same classes as the globals), IDNA 2008 / UTS #46, Punycode, form encoding. */
declare module "dyna:ui" {
    /** A colour as 0xRRGGBBAA. */
    type Color = number;
    /** A length in logical points: a number of points, "fit" (to content), "grow" (share free space), "N%" (of the parent's inner size) or { grow: weight } (weight 0 does not grow). */
    type Size = number | "fit" | "grow" | `${number}%` | { grow: number };
    type Rect = { x: number; y: number; width: number; height: number };
    /** Layout and style of a container; every length is in logical points and is multiplied by the Context's scale. */
    type BoxOptions = {
        width?: Size; height?: Size; minWidth?: number; maxWidth?: number; minHeight?: number; maxHeight?: number;
        /** A number, [x, y] or [left, top, right, bottom]. */
        padding?: number | [number, number] | [number, number, number, number];
        /** Space between children; rows default to the theme's padX, columns to its spacing. */
        gap?: number;
        alignX?: "start" | "center" | "end"; alignY?: "start" | "center" | "end";
        /** Clips the children and scrolls them (wheel, drag on touch, scrollbar); needs an id. */
        scroll?: boolean | "x" | "y" | "both";
        /** A row whose children flow onto further lines when they do not fit (flex-wrap). */
        wrap?: boolean;
        fill?: Color; border?: Color; borderWidth?: number; radius?: number;
        /** Keys the container's retained state (scroll offset) and scopes the ids of what it holds. */
        id?: string;
        /** Takes the box out of flow: "parent" at (x, y) inside its parent; "popup" relative to its parent, above everything, kept inside the safe area; "screen" at (x, y) on the screen, free to cover the insets (edge-to-edge bars, shades); "safe" at (x, y) on the screen, moved inside the safe area. */
        float?: "parent" | "popup" | "screen" | "safe"; x?: number; y?: number;
        /** Stacking order among floating boxes; equal values keep declaration order. */
        z?: number;
    };
    /** Theme values; lengths are logical points, colours 0xRRGGBBAA. */
    type Theme = {
        /** Font index: -1 picks ProggyClean for 13 px text and Inter for every other size; 0 is ProggyClean, 1 Inter, then loadFont() results. */
        font: number; fontSize: number; padX: number; padY: number; spacing: number; windowPad: number; indent: number; radius: number;
        borderWidth: number; labelWidth: number; scrollbarWidth: number;
        /** The smallest height of anything touchable (44 suits fingers, 0 a mouse). */
        minTarget: number;
        /** How far a touch moves before it becomes a scroll instead of a tap. */
        dragSlop: number;
        /** Seconds a touch holds still before it raises the context action. */
        longPress: number;
        /** Multiplies every text size (an accessibility text scale). */
        textScale: number;
        text: Color; textDim: Color; onAccent: Color; bg: Color; panel: Color; frame: Color; frameHot: Color; frameActive: Color; button: Color;
        buttonHot: Color; buttonActive: Color; accent: Color; accentHot: Color; border: Color; title: Color; titleActive: Color; selection: Color;
        popup: Color; shade: Color; tooltip: Color; link: Color; scrollbar: Color; scrollbarHot: Color;
        /** The border and message colour of a field given { error }. */
        error: Color;
    };
    /** Colour sets for theme(): dark (the default) and light. */
    const themes: { readonly dark: Readonly<Partial<Theme>>; readonly light: Readonly<Partial<Theme>> };
    /** Metric sets for theme(): compact (mouse, the default) and touch (44 pt targets, roomier padding). */
    const densities: { readonly compact: Readonly<Partial<Theme>>; readonly touch: Readonly<Partial<Theme>> };
    type Geometry = {
        /** 8 floats per vertex in device pixels: x, y, r, g, b, a (0..1), u, v. */
        vertices: Float32Array;
        indices: Uint32Array;
        /** Draw ranges of indices: texture is "atlas" (glyphs and solid shapes, alpha only) or an image handle; clip is in device pixels. */
        batches: { texture: "atlas" | number; first: number; count: number; clip: Rect }[];
    };
    /** An immediate-mode UI: frame(fn) runs fn, lays out what it declared, and draws it to the target only when the result changed. */
    class Context {
        /** Without a target the Context draws in memory (pixels() reads it); with a dyna:sdl Renderer or dyna:raylib Window it draws there and takes size, scale and safe-area insets from it; present: false draws inside the caller's frame (sdl: between its own draws; raylib: inside rl.frame() or RenderTexture#draw()) without clearing or presenting; touch "auto" applies the touch density on Android and iOS. */
        constructor(opts: { width?: number; height?: number; scale?: number; insets?: [number, number, number, number]; touch?: boolean | "auto"; target?: object; present?: boolean; clock?: "monotonic" | "manual"; clear?: Color });
        /** Runs fn(this) to declare the UI, lays it out and draws it; returns whether anything visible changed. */
        frame(fn: (ui: Context) => void): boolean;
        /** Blocks until input arrives or the next timed redraw is due (animations, tooltips, long presses, flings), at most seconds; returns the seconds it waited at most. */
        idle(seconds?: number): number;
        /** RGBA of the last frame in device pixels (pixelWidth x pixelHeight), rasterised in memory for any target. */
        pixels(): Uint8Array;
        /** Rasterises the last frame into target (exactly pixelWidth * pixelHeight * 4 bytes) without allocating, and returns target. */
        pixelsInto(target: Uint8Array): Uint8Array;
        /** The last frame as triangles for a custom renderer. */
        geometry(): Geometry;
        /** The box of the widget or block declared just before (a row/column/box/wrap with an id, group, card, property, panel, scroll, split, popup, modal, drawer, sheet, bar; a tree or menu answers its header), as laid out by the previous frame (inside frame()) or this one (after it), in logical points; null before it has been laid out. */
        lastRect(): Rect | null;
        /** The box of item `item` of the composite declared just before (tabs, segmented, radio, combo items while open, list/log/table rows, navBar/navRail, breadcrumbs, stepper, pagination by page, datePicker by day, rating by star, accordion headers, tagInput chips, timePicker hour/minute/AM-PM, autocomplete suggestions while shown, scaffold destinations); with column, a table cell (item -1 with a column is that column's header); null when that item is not laid out or not visible. */
        lastRect(item: number, column?: number): Rect | null;
        /** Counts for the last frame (elements, text bytes, draw commands, glyph quads, vertices, hit boxes, text runs and their glyph records, cached glyphs, atlas rows) and totals (frames, frames reused because their input was unchanged). */
        stats(): { elements: number; textBytes: number; commands: number; glyphQuads: number; vertices: number; hitBoxes: number; textRuns: number; runRecords: number; glyphs: number; atlasHeight: number; frames: number; reusedFrames: number };
        /** The glyph atlas the "atlas" texture samples: one alpha byte per texel. */
        atlas(): { width: number; height: number; version: number; alpha: Uint8Array };
        /** Resizes a Context without a target (logical size, optional scale and insets). */
        resize(width: number, height: number, opts?: { scale?: number; insets?: [number, number, number, number] }): void;
        /** Reads the theme, or merges the given values into it (between frames). */
        theme(values?: Partial<Theme>): Theme | undefined;
        /** Loads a TRUSTED TrueType/OpenType font from a path or bytes and returns its index for theme({ font }). */
        loadFont(source: string | Uint8Array): number;
        /** Registers RGBA pixels (width * height * 4 bytes) and returns a handle for image(), iconButton(), avatar() and navigation icons. */
        addImage(rgba: Uint8Array, width: number, height: number): number;
        updateImage(handle: number, rgba: Uint8Array): void;
        removeImage(handle: number): void;
        /** Synthetic pointer input in logical points; buttons is 1 left, 2 right, 4 middle; kind "touch" has no hover and scrolls by dragging. */
        pointer(state: { x?: number; y?: number; buttons?: number; wheelX?: number; wheelY?: number; time?: number; kind?: "mouse" | "touch" | "pen" }): void;
        /** Synthetic key input by name: left right up down home end backspace delete return tab escape pageup pagedown a c v x y z space. */
        key(name: string, down?: boolean, mods?: { shift?: boolean; ctrl?: boolean; alt?: boolean; super?: boolean }): void;
        /** Synthetic text input (what a keyboard or IME commits). */
        type(text: string): void;
        /** An IME's uncommitted text, drawn at the caret of the field being edited. */
        composition(text: string, cursor?: number): void;
        /** Sets the clock of a { clock: "manual" } Context, in seconds. */
        setTime(seconds: number): void;
        /** Logical size of the drawable area. */
        readonly width: number;
        readonly height: number;
        readonly pixelWidth: number;
        readonly pixelHeight: number;
        /** Device pixels per logical point. */
        readonly scale: number;
        /** "compact" below 600 points wide, "medium" below 840, else "expanded". */
        readonly sizeClass: "compact" | "medium" | "expanded";
        /** The safe-area insets of the last frame in logical points: [left, top, right, bottom]. */
        readonly insets: [number, number, number, number];
        /** Seconds until a timed redraw is due (Infinity when nothing animates). */
        readonly wakeIn: number;
        /** The caret of the field being edited, in logical points, or null. */
        readonly textInput: Rect | null;
        /** Whether the last text field saw Return this frame. */
        readonly submitted: boolean;
        /** Whether the last text field holds keyboard focus. */
        readonly editing: boolean;
        /** The row the last list or table activated (Return or double click) this frame, or -1. */
        readonly activated: number;
        /** The payload of the drag in progress (this frame), or null. */
        readonly dragging: string | number | null;
        /** A horizontal container; children are centred vertically. */
        row(opts: BoxOptions | ((ui: Context) => void), fn?: (ui: Context) => void): void;
        column(opts: BoxOptions | ((ui: Context) => void), fn?: (ui: Context) => void): void;
        /** A column without default gap or alignment. */
        box(opts: BoxOptions | ((ui: Context) => void), fn?: (ui: Context) => void): void;
        /** A full-width row whose children wrap onto further lines (chips, cards, toolbars). */
        wrap(opts: BoxOptions | ((ui: Context) => void), fn?: (ui: Context) => void): void;
        /** Scopes the ids of the widgets fn declares (for lists of identical labels). */
        id(key: string | number, fn: (ui: Context) => void): void;
        /** Empty space; an omitted length grows. */
        spacer(width?: number, height?: number): void;
        /** Text after "##" in any label is not shown but keys the widget. label, text, heading, bullet, badge, image and progress have a box for lastRect() and tooltip(), keyed by the first 32 bytes and the length of their text: when two in one id scope share both, the first one answers. */
        label(text: string): void;
        text(text: string, opts?: { color?: Color; wrap?: boolean }): void;
        heading(text: string): void;
        bullet(text: string): void;
        separator(title?: string): void;
        link(text: string): boolean;
        badge(text: string, color?: Color): void;
        image(handle: number, width?: number, height?: number, tint?: Color): void;
        progress(fraction: number, width?: number): void;
        /** An animated busy indicator; it asks for the next frame itself (wakeIn). */
        spinner(size?: number): void;
        skeleton(width?: number, height?: number): void;
        avatar(name: string, opts?: { image?: number; size?: number }): void;
        /** True on the frame the button is clicked, or activated by Return/Space while keyboard-focused. */
        button(label: string): boolean;
        iconButton(label: string, image: number, size?: number): boolean;
        /** Each returns the new value; the value passed in is never mutated. */
        toggle(label: string, on: boolean): boolean;
        checkbox(label: string, checked: boolean): boolean;
        switch(label: string, on: boolean): boolean;
        /** Returns the selected state (true every frame while selected), not "clicked". */
        selectable(label: string, selected: boolean): boolean;
        chip(label: string, selected: boolean): boolean;
        radio(label: string, items: string[], selected: number, opts?: { horizontal?: boolean }): number;
        segmented(label: string, items: string[], selected: number): number;
        tabs(label: string, items: string[], selected: number): number;
        /** Up/Down move the highlight and Return picks it while the popup is open; search adds a filter field (case-insensitive substring, cleared each time it opens); error/help draw a line below. */
        combo(label: string, items: string[], selected: number, opts?: { search?: boolean; error?: string; help?: string }): number;
        slider(label: string, value: number, min: number, max: number, opts?: { step?: number; decimals?: number; vertical?: boolean; length?: number }): number;
        /** Returns the same array while unchanged, a new [low, high] when dragged. */
        range(label: string, value: [number, number], min: number, max: number, opts?: { step?: number; decimals?: number }): [number, number];
        /** Dragging sideways changes the value by speed per point moved (1 by default). */
        drag(label: string, value: number, opts?: { speed?: number; min?: number; max?: number; decimals?: number }): number;
        spin(label: string, value: number, opts?: { step?: number; min?: number; max?: number; decimals?: number }): number;
        /** Returns the text after this frame's edits (caret, selection, clipboard, undo); the same string while unchanged. */
        /** Returns the text after this frame's edits (caret, selection, clipboard, undo); the same string while unchanged; error draws the field's border and a message below it in theme error, help a dim line; lastRect() stays the field. */
        textField(label: string, text: string, opts?: { hint?: string; maxLength?: number; width?: number; password?: boolean; readOnly?: boolean; error?: string; help?: string }): string;
        textArea(label: string, text: string, opts?: { hint?: string; maxLength?: number; width?: number; rows?: number; readOnly?: boolean; error?: string; help?: string }): string;
        search(label: string, text: string, opts?: { hint?: string; width?: number; error?: string; help?: string }): string;
        /** A text field with the caller's suggestions below it while focused; a click, or Down/Up and Return, replaces the text with one (caret at the end) and hides them until the text changes; Escape hides them. */
        autocomplete(label: string, text: string, suggestions: string[], opts?: { hint?: string; width?: number; error?: string; help?: string }): string;
        /** Hour and minute fields (Up/Down step them, two typed digits set one) with an AM/PM button when h12; minutes step by step (1 by default; a step must divide 60, others are a RangeError) and wrap on that grid; returns the same object while unchanged. */
        timePicker(label: string, time: { hour: number; minute: number }, opts?: { step?: number; h12?: boolean; error?: string; help?: string }): { hour: number; minute: number };
        /** Chips with a close button and a field: every comma-separated piece of a frame's input is added at once, and Return adds the rest (trimmed; duplicates of a tag or of another piece are skipped), Backspace in the empty field removes the last tag; returns the same array while unchanged. */
        tagInput(label: string, tags: string[], opts?: { hint?: string; error?: string; help?: string }): string[];
        color(label: string, rgba: Color): Color;
        rating(label: string, value: number, opts?: { max?: number }): number;
        /** Returns the same object while unchanged; firstDay 0 is Sunday, 1 (the default) Monday. */
        datePicker(label: string, date: { year: number; month: number; day: number }, opts?: { firstDay?: number }): { year: number; month: number; day: number };
        pagination(label: string, page: number, pages: number): number;
        /** Returns the index of the crumb clicked this frame, or -1 (the last is the current page). */
        breadcrumbs(label: string, items: string[]): number;
        /** Returns the step clicked this frame, or -1. */
        stepper(label: string, items: string[], current: number): number;
        /** A virtualised list: only visible rows read items[]; returns the selected index. */
        list(label: string, items: string[], selected: number, opts?: { rows?: number; follow?: boolean }): number;
        /** A virtualised read-only list that follows its end while scrolled to it. */
        log(label: string, lines: string[], opts?: { rows?: number; follow?: boolean }): void;
        /** A virtualised table: cell(row, column) runs only for visible rows; headers sort, grips resize; Return or a double click activates the selected row. selectedColumn (-1 for none) selects cells: a click picks one, arrows/Home/End move it. edit (with selectedColumn) shows that cell as a focused text field: result edit is its text, committed on Return, on a click elsewhere in the table, or when focus leaves it, cancelled on Escape; on the committing frame selected/column name the edited cell and a clicked cell becomes selected on the next frame. A selected row, selectedColumn or sort column out of range reads as -1; column is -1 without selectedColumn. */
        table(label: string, opts: { columns: { name: string; width?: number }[]; rows: number; cell: (row: number, column: number) => string; visibleRows?: number; selected?: number; selectedColumn?: number; edit?: string; sort?: { column?: number; descending?: boolean } }): { selected: number; column: number; sortColumn: number; descending: boolean; activated: boolean; edit: string | undefined; committed: boolean; cancelled: boolean };
        plot(label: string, values: ArrayLike<number>, opts?: { min?: number; max?: number; height?: number }): void;
        bars(label: string, values: ArrayLike<number>, opts?: { min?: number; max?: number; height?: number }): void;
        /** A label column of theme labelWidth and fn's widgets beside it. */
        property(label: string, fn: (ui: Context) => void): void;
        group(title: string, fn: (ui: Context) => void): void;
        card(title: string, opts: { width?: number } | ((ui: Context) => void), fn?: (ui: Context) => void): void;
        panel(label: string, opts: { width?: number; height?: number; border?: boolean } | ((ui: Context) => void), fn?: (ui: Context) => void): void;
        scroll(label: string, opts: { width?: number; height?: number; border?: boolean } | ((ui: Context) => void), fn?: (ui: Context) => void): void;
        collapsing(label: string, defaultOpen?: boolean): boolean;
        /** Section headers of which at most one is open; fn(ui, index) declares the section that was open when the frame began; returns the open index (-1 for none), changed by a click on a header. */
        accordion(label: string, sections: string[], open: number, fn: (ui: Context, index: number) => void): number;
        /** Returns whether the node is open; fn declares the children (none makes a leaf). */
        tree(label: string, opts?: { open?: boolean; leaf?: boolean } | ((ui: Context) => void), fn?: (ui: Context) => void): boolean;
        /** A floating, draggable window; returns false the frame its close button is clicked. */
        window(title: string, opts: { x?: number; y?: number; width?: number; height?: number; closable?: boolean; collapsible?: boolean; resizable?: boolean; fixed?: boolean } | ((ui: Context) => void), fn?: (ui: Context) => void): boolean;
        /** Two panes split at ratio (0.05..0.95) with a draggable handle; returns the ratio. */
        split(label: string, ratio: number, opts: { vertical?: boolean } | ((ui: Context) => void), first: (ui: Context) => void, second?: (ui: Context) => void): number;
        /** Opens popup(label) at the pointer; both calls must be in the same id scope (a container with an id, or id(), between them names another popup). */
        openPopup(label: string): void;
        /** A popup at the pointer where openPopup() was called; closes on an outside press or Escape. */
        popup(label: string, fn: (ui: Context) => void): boolean;
        /** Opens on a right press or a long touch over the widget declared just before it. */
        contextMenu(label: string, fn: (ui: Context) => void): boolean;
        /** Opens modal(label); modal, drawer and sheet names are screen-wide, so the open call may sit inside any container. */
        openModal(label: string): void;
        modal(label: string, opts: { width?: number } | ((ui: Context) => void), fn?: (ui: Context) => void): boolean;
        /** Closes the innermost popup, modal, drawer or sheet being declared. */
        closePopup(): void;
        /** Shown for the widget declared just before it: on hover after delay seconds, or while a touch holds it. */
        tooltip(text: string, opts?: { delay?: number }): void;
        /** Opens drawer(label); the name is screen-wide, like a modal's. */
        openDrawer(label: string): void;
        drawer(label: string, opts: { side?: "left" | "right"; width?: number } | ((ui: Context) => void), fn?: (ui: Context) => void): boolean;
        /** Opens sheet(label); the name is screen-wide, like a modal's. */
        openSheet(label: string): void;
        /** A bottom sheet over a shade; tapping the shade closes it. */
        sheet(label: string, fn: (ui: Context) => void): boolean;
        /** A transient message at the bottom; callable between frames too. */
        toast(message: string, opts?: { duration?: number }): void;
        /** A floating action button above the bottom bars; a label without an icon makes it extended. */
        fab(label: string, opts?: { icon?: number }): boolean;
        /** Declared directly in frame() these dock to the screen edge (outside the safe area insets) and stack. */
        menubar(fn: (ui: Context) => void): void;
        appBar(title: string, fn: (ui: Context) => void): void;
        navBar(label: string, items: (string | { label: string; icon?: number })[], selected: number): number;
        /** A vertical navigation column that fills its parent's height; it does not dock, so declare it first in a row beside the content. */
        navRail(label: string, items: (string | { label: string; icon?: number })[], selected: number): number;
        /** Adaptive navigation: a navBar docked at the bottom below 600 points wide, else a navRail left of fn's content; returns the selected destination. */
        scaffold(label: string, items: (string | { label: string; icon?: number })[], selected: number, fn: (ui: Context) => void): number;
        /** Makes the widget declared just before it draggable with payload (a TypeError when no widget precedes it in the frame, or the payload is ""); compare dropTarget()'s result with null, since 0 is a payload; true while it is being dragged. A mouse drag starts past dragSlop, a touch one after a long press (or a move no scroll container takes); the release is not a click, Escape cancels; label is drawn beside the pointer. */
        dragSource(payload: string | number, opts?: { label?: string }): boolean;
        /** Makes the widget or block declared just before it a drop target (a TypeError when none precedes it in the frame): highlighted while a drag is over it; returns the payload on the frame a drag is released over it (the smallest target under the pointer wins), else null. */
        dropTarget(): string | number | null;
        /** Declared directly in frame() these dock to the screen edge (outside the safe area insets) and stack. */
        toolbar(fn: (ui: Context) => void): void;
        statusbar(fn: (ui: Context) => void): void;
        menu(label: string, fn: (ui: Context) => void): boolean;
        menuItem(label: string, opts?: { shortcut?: string; checked?: boolean; enabled?: boolean }): boolean;
        /** Frees the context's native state; refused inside frame(). */
        close(): void;
        dispose(): void;
        readonly closed: boolean;
    }
}
