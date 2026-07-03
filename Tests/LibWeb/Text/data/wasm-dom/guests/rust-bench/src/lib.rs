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

fn set_text(node: i32, text: &str) {
    unsafe { text_content_set(node, text.as_ptr(), text.len()) };
}

// ---- workloads ----

const BUILD_N: usize = 2000;
const TEXT_N: usize = 4000;
const QUERY_N: usize = 5000;

fn bench_build() {
    let arena = by_id("arena");
    for i in 0..BUILD_N {
        let value: &str = if i % 2 == 0 { "even-value" } else { "odd-value" };
        unsafe {
            let div = create_element("div".as_ptr(), 3);
            set_attribute(div, "class".as_ptr(), 5, "item".as_ptr(), 4);
            set_attribute(div, "data-idx".as_ptr(), 8, value.as_ptr(), value.len());
            append_child(arena, div);
            release(div);
        }
    }
    unsafe { release(arena) };
}

fn bench_text() {
    let node = by_id("textnode");
    let a = "the quick brown fox jumps over it";
    let b = "a completely different string here";
    let mut buf = [0u8; 128];
    for i in 0..TEXT_N {
        let s = if i % 2 == 0 { a } else { b };
        unsafe {
            text_content_set(node, s.as_ptr(), s.len());
            text_content_get(node, buf.as_ptr(), buf.len());
        }
    }
    unsafe { release(node) };
}

fn bench_query() {
    let mut buf = [0u8; 128];
    for _ in 0..QUERY_N {
        unsafe {
            let e = get_element_by_id("target".as_ptr(), 6);
            get_attribute(e, "data-value".as_ptr(), 10, buf.as_ptr(), buf.len());
            release(e);
        }
    }
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
    run_and_report("build", 5, bench_build);
    run_and_report("text", 5, bench_text);
    run_and_report("query", 5, bench_query);
    let done = by_id("results");
    unsafe { set_attribute(done, "data-done".as_ptr(), 9, "yes".as_ptr(), 3) };
    set_text(by_id("status"), "done");
}
