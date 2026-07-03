//! A Rust guest for Ladybird's wasm-dom host interface.
//!
//! Build (see ../README.md):
//!   RUSTFLAGS="-C link-args=--export-table" \
//!     cargo build --release --target wasm32-unknown-unknown
//!
//! _start builds a heading and a button; every click appends "click #N from
//! Rust" — the counter and the decimal formatting live entirely in the module.

#![no_std]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    // Trap; the host logs it and the renderer carries on.
    core::arch::wasm32::unreachable()
}

mod dom {
    #[link(wasm_import_module = "dom")]
    extern "C" {
        pub fn get_element_by_id(ptr: *const u8, len: usize) -> i32;
        pub fn create_element(ptr: *const u8, len: usize) -> i32;
        pub fn append_child(parent: i32, child: i32) -> i32;
        pub fn set_attribute(node: i32, nptr: *const u8, nlen: usize, vptr: *const u8, vlen: usize) -> i32;
        pub fn text_content_set(node: i32, ptr: *const u8, len: usize) -> i32;
        pub fn add_event_listener(node: i32, tptr: *const u8, tlen: usize, callback: i32, user_data: i32) -> i32;
    }
}

// Typed wrappers: this thin layer is exactly what a real binding crate would
// generate from the interface description.
#[derive(Clone, Copy, PartialEq)]
struct Node(i32);

impl Node {
    fn by_id(id: &str) -> Option<Node> {
        let handle = unsafe { dom::get_element_by_id(id.as_ptr(), id.len()) };
        (handle != 0).then_some(Node(handle))
    }

    fn create(tag: &str) -> Node {
        Node(unsafe { dom::create_element(tag.as_ptr(), tag.len()) })
    }

    fn append(&self, child: Node) {
        unsafe { dom::append_child(self.0, child.0) };
    }

    fn set_attribute(&self, name: &str, value: &str) {
        unsafe { dom::set_attribute(self.0, name.as_ptr(), name.len(), value.as_ptr(), value.len()) };
    }

    fn set_text(&self, text: &str) {
        unsafe { dom::text_content_set(self.0, text.as_ptr(), text.len()) };
    }

    fn on(&self, event: &str, callback: extern "C" fn(i32, i32), user_data: i32) {
        // On wasm32 a function pointer *is* its index in the indirect function
        // table, which the host resolves at registration time.
        let index = callback as usize as i32;
        unsafe { dom::add_event_listener(self.0, event.as_ptr(), event.len(), index, user_data) };
    }
}

static mut CLICKS: u32 = 0;

fn format_click_line(n: u32, buf: &mut [u8; 32]) -> usize {
    let prefix = b"click #";
    buf[..prefix.len()].copy_from_slice(prefix);
    let mut digits = [0u8; 10];
    let mut i = digits.len();
    let mut value = n;
    loop {
        i -= 1;
        digits[i] = b'0' + (value % 10) as u8;
        value /= 10;
        if value == 0 {
            break;
        }
    }
    let digit_count = digits.len() - i;
    buf[prefix.len()..prefix.len() + digit_count].copy_from_slice(&digits[i..]);
    let suffix = b" from Rust";
    let end = prefix.len() + digit_count + suffix.len();
    buf[prefix.len() + digit_count..end].copy_from_slice(suffix);
    end
}

extern "C" fn on_click(_event: i32, _user_data: i32) {
    let count = unsafe {
        CLICKS += 1;
        CLICKS
    };
    let mut buf = [0u8; 32];
    let len = format_click_line(count, &mut buf);

    let li = Node::create("li");
    li.set_text(unsafe { core::str::from_utf8_unchecked(&buf[..len]) });
    if let Some(out) = Node::by_id("out") {
        out.append(li);
    }
}

#[no_mangle]
pub extern "C" fn _start() {
    let Some(root) = Node::by_id("root") else { return };

    let heading = Node::create("h2");
    heading.set_text("built by Rust");
    heading.set_attribute("id", "rust-heading");
    root.append(heading);

    let button = Node::create("button");
    button.set_text("increment");
    button.set_attribute("id", "rust-btn");
    root.append(button);

    let list = Node::create("ol");
    list.set_attribute("id", "out");
    root.append(list);

    button.on("click", on_click, 0);
}
