//! A Zig guest for Ladybird's wasm-dom host interface.
//!
//! Build (see ../README.md):
//!   zig build-exe guest.zig -target wasm32-freestanding -O ReleaseSmall \
//!     -fno-entry --export=_start --export-table -femit-bin=zig-guest.wasm
//!
//! _start kicks off dom.fetch of a text file and a 1ms dom.set_timeout; the
//! fetch callback writes the body into the page, the timer callback stamps a
//! marker attribute. Everything async goes through the host's wakeup ABI.

const dom = struct {
    extern "dom" fn get_element_by_id(ptr: [*]const u8, len: u32) i32;
    extern "dom" fn create_element(ptr: [*]const u8, len: u32) i32;
    extern "dom" fn append_child(parent: i32, child: i32) i32;
    extern "dom" fn set_attribute(node: i32, nptr: [*]const u8, nlen: u32, vptr: [*]const u8, vlen: u32) i32;
    extern "dom" fn text_content_set(node: i32, ptr: [*]const u8, len: u32) i32;
    extern "dom" fn fetch(url: [*]const u8, len: u32, callback: i32, user_data: i32) i32;
    extern "dom" fn response_status(response: i32) i32;
    extern "dom" fn response_read(response: i32, dst: [*]const u8, cap: u32) i32;
    extern "dom" fn set_timeout(ms: i32, callback: i32, user_data: i32) i32;
};

fn byId(id: []const u8) i32 {
    return dom.get_element_by_id(id.ptr, id.len);
}

fn setText(node: i32, text: []const u8) void {
    _ = dom.text_content_set(node, text.ptr, text.len);
}

fn setAttr(node: i32, name: []const u8, value: []const u8) void {
    _ = dom.set_attribute(node, name.ptr, name.len, value.ptr, value.len);
}

// On wasm32 a function pointer is its index in the indirect function table.
fn tableIndex(comptime f: fn (i32, i32) callconv(.c) void) i32 {
    return @intCast(@intFromPtr(&f));
}

var buffer: [512]u8 = undefined;

fn onFetch(response: i32, user_data: i32) callconv(.c) void {
    _ = user_data;
    const out = byId("zig-out");
    if (response == 0) {
        setText(out, "network failure");
        return;
    }
    if (dom.response_status(response) == 200)
        setAttr(out, "data-status", "ok");
    // Deliberately probe with a tiny capacity first to use the retry ABI.
    const full_len: u32 = @intCast(dom.response_read(response, &buffer, 4));
    const len: u32 = @intCast(dom.response_read(response, &buffer, full_len));
    const li = dom.create_element("li".ptr, 2);
    setText(li, buffer[0..len]);
    _ = dom.append_child(out, li);
}

fn onTimeout(arg: i32, user_data: i32) callconv(.c) void {
    _ = arg;
    if (user_data == 99)
        setAttr(byId("zig-out"), "data-timer", "fired");
}

export fn _start() void {
    const root = byId("zig-root");
    const heading = dom.create_element("h2".ptr, 2);
    setText(heading, "built by Zig");
    setAttr(heading, "id", "zig-heading");
    _ = dom.append_child(root, heading);

    const list = dom.create_element("ol".ptr, 2);
    setAttr(list, "id", "zig-out");
    _ = dom.append_child(root, list);

    const url = "../../data/wasm-dom/message.txt";
    _ = dom.fetch(url.ptr, url.len, tableIndex(onFetch), 0);
    _ = dom.set_timeout(1, tableIndex(onTimeout), 99);
}
