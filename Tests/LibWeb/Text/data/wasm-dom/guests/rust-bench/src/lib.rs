//! DOM benchmark guest. Identical workload code compiles against either the
//! native "dom" host interface (default) or a "glue" JS import object with the
//! same ABI (--features glue), so the two binaries differ only in which module
//! name their imports resolve against.
//!
//! Build (see ../README.md):
//!   RUSTFLAGS="-C link-args=--export-table" cargo build --release --target wasm32-unknown-unknown
//!   RUSTFLAGS="-C link-args=--export-table" cargo build --release --target wasm32-unknown-unknown --features glue

#![no_std]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    core::arch::wasm32::unreachable()
}

macro_rules! host_imports {
    ($module:literal) => {
        #[link(wasm_import_module = $module)]
        extern "C" {
            pub fn get_element_by_id(ptr: *const u8, len: usize) -> i32;
            pub fn create_element(ptr: *const u8, len: usize) -> i32;
            pub fn append_child(parent: i32, child: i32) -> i32;
            pub fn set_attribute(node: i32, nptr: *const u8, nlen: usize, vptr: *const u8, vlen: usize) -> i32;
            pub fn get_attribute(node: i32, nptr: *const u8, nlen: usize, dst: *const u8, cap: usize) -> i32;
            pub fn text_content_set(node: i32, ptr: *const u8, len: usize) -> i32;
            pub fn text_content_get(node: i32, dst: *const u8, cap: usize) -> i32;
            pub fn release(handle: i32);
            pub fn intern(ptr: *const u8, len: usize) -> i32;
            pub fn now() -> f64;
        }
    };
}

#[cfg(not(feature = "glue"))]
mod host {
    host_imports!("dom");
}
#[cfg(feature = "glue")]
mod host {
    host_imports!("glue");
}

use host::*;

fn by_id(id: &str) -> i32 {
    unsafe { get_element_by_id(id.as_ptr(), id.len()) }
}

/// An interned string id, passed as (id, usize::MAX) wherever (ptr, len) goes.
#[derive(Clone, Copy)]
struct IStr(i32);

impl IStr {
    fn ptr(self) -> *const u8 {
        self.0 as usize as *const u8
    }
    const LEN: usize = usize::MAX;
}

fn intern_str(s: &str) -> IStr {
    IStr(unsafe { intern(s.as_ptr(), s.len()) })
}

struct Interned {
    div: IStr,
    class: IStr,
    item: IStr,
    data_idx: IStr,
    even: IStr,
    odd: IStr,
    text_a: IStr,
    text_b: IStr,
    target: IStr,
    data_value: IStr,
}

static mut INTERNED: Option<Interned> = None;

fn interned() -> &'static Interned {
    unsafe { (*core::ptr::addr_of!(INTERNED)).as_ref().unwrap() }
}

fn set_text(node: i32, text: &str) {
    unsafe { text_content_set(node, text.as_ptr(), text.len()) };
}

// ---- workloads ----

const BUILD_N: usize = 2000;
const TEXT_N: usize = 4000;
const QUERY_N: usize = 5000;

fn bench_build() {
    let s = interned();
    let arena = by_id("arena");
    for i in 0..BUILD_N {
        let value = if i % 2 == 0 { s.even } else { s.odd };
        unsafe {
            let div = create_element(s.div.ptr(), IStr::LEN);
            set_attribute(div, s.class.ptr(), IStr::LEN, s.item.ptr(), IStr::LEN);
            set_attribute(div, s.data_idx.ptr(), IStr::LEN, value.ptr(), IStr::LEN);
            append_child(arena, div);
            release(div);
        }
    }
    unsafe { release(arena) };
}

fn bench_text() {
    let s = interned();
    let node = by_id("textnode");
    let buf = [0u8; 128];
    for i in 0..TEXT_N {
        let payload = if i % 2 == 0 { s.text_a } else { s.text_b };
        unsafe {
            text_content_set(node, payload.ptr(), IStr::LEN);
            text_content_get(node, buf.as_ptr(), buf.len());
        }
    }
    unsafe { release(node) };
}

fn bench_query() {
    let s = interned();
    let buf = [0u8; 128];
    let held = unsafe { get_element_by_id(s.target.ptr(), IStr::LEN) };
    for _ in 0..QUERY_N {
        unsafe {
            let e = get_element_by_id(s.target.ptr(), IStr::LEN);
            get_attribute(e, s.data_value.ptr(), IStr::LEN, buf.as_ptr(), buf.len());
            release(e);
        }
    }
    unsafe { release(held) };
}

// ---- harness ----

fn format_ms(ms: f64, buf: &mut [u8; 64], prefix: &str) -> usize {
    let p = prefix.as_bytes();
    buf[..p.len()].copy_from_slice(p);
    let mut n = p.len();
    // fixed-point with two decimals
    let scaled = (ms * 100.0 + 0.5) as u64;
    let (whole, frac) = (scaled / 100, scaled % 100);
    let mut digits = [0u8; 20];
    let mut i = digits.len();
    let mut v = whole;
    loop {
        i -= 1;
        digits[i] = b'0' + (v % 10) as u8;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    buf[n..n + digits.len() - i].copy_from_slice(&digits[i..]);
    n += digits.len() - i;
    buf[n] = b'.';
    buf[n + 1] = b'0' + (frac / 10) as u8;
    buf[n + 2] = b'0' + (frac % 10) as u8;
    n += 3;
    let suffix = b" ms";
    buf[n..n + 3].copy_from_slice(suffix);
    n + 3
}

fn report(line_buf: &[u8]) {
    unsafe {
        let results = by_id("results");
        let li = create_element("li".as_ptr(), 2);
        text_content_set(li, line_buf.as_ptr(), line_buf.len());
        append_child(results, li);
        release(li);
        release(results);
    }
}

fn run_and_report(name: &str, reps: u32, f: fn()) {
    for _ in 0..reps {
        let start = unsafe { now() };
        f();
        let elapsed = unsafe { now() } - start;
        let mut buf = [0u8; 64];
        let mut prefix = [0u8; 32];
        let plen = name.len();
        prefix[..plen].copy_from_slice(name.as_bytes());
        prefix[plen] = b' ';
        let len = format_ms(elapsed, &mut buf, unsafe { core::str::from_utf8_unchecked(&prefix[..plen + 1]) });
        report(&buf[..len]);
    }
}

#[no_mangle]
pub extern "C" fn _start() {
    unsafe {
        INTERNED = Some(Interned {
            div: intern_str("div"),
            class: intern_str("class"),
            item: intern_str("item"),
            data_idx: intern_str("data-idx"),
            even: intern_str("even-value"),
            odd: intern_str("odd-value"),
            text_a: intern_str("the quick brown fox jumps over it"),
            text_b: intern_str("a completely different string here"),
            target: intern_str("target"),
            data_value: intern_str("data-value"),
        });
    }
    run_and_report("build", 5, bench_build);
    run_and_report("text", 5, bench_text);
    run_and_report("query", 5, bench_query);
    let done = by_id("results");
    unsafe { set_attribute(done, "data-done".as_ptr(), 9, "yes".as_ptr(), 3) };
    set_text(by_id("status"), "done");
}
