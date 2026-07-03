//! Extended benchmark suite: pure compute, crossing-cost by op weight, payload
//! size sweep, and tree walks over the handle table. Same dual-build scheme as
//! rust-bench (default = "dom" host interface, --features glue = JS import
//! object with the same ABI).

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
            pub fn text_content_set(node: i32, ptr: *const u8, len: usize) -> i32;
            pub fn text_content_get(node: i32, dst: *const u8, cap: usize) -> i32;
            pub fn release(handle: i32);
            pub fn intern(ptr: *const u8, len: usize) -> i32;
            pub fn noop();
            pub fn now() -> f64;
            pub fn node_owner_document(node: i32) -> i32;
            pub fn document_query_selector(document: i32, ptr: *const u8, len: usize) -> i32;
            pub fn node_first_child(node: i32) -> i32;
            pub fn node_next_sibling(node: i32) -> i32;
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

const SENTINEL: usize = usize::MAX;

fn by_id(id: &str) -> i32 {
    unsafe { get_element_by_id(id.as_ptr(), id.len()) }
}

// ---- pure compute (no crossings inside the timed region) ----

fn fib(n: u32) -> u64 {
    if n < 2 {
        n as u64
    } else {
        fib(n - 1) + fib(n - 2)
    }
}

const DIM: usize = 64;
static mut MAT_A: [f64; DIM * DIM] = [0.0; DIM * DIM];
static mut MAT_B: [f64; DIM * DIM] = [0.0; DIM * DIM];
static mut MAT_C: [f64; DIM * DIM] = [0.0; DIM * DIM];

fn bench_matmul() -> f64 {
    unsafe {
        let a = &*core::ptr::addr_of!(MAT_A);
        let b = &*core::ptr::addr_of!(MAT_B);
        let c = &mut *core::ptr::addr_of_mut!(MAT_C);
        for _ in 0..4 {
            for i in 0..DIM {
                for j in 0..DIM {
                    let mut sum = 0.0;
                    for k in 0..DIM {
                        sum += a[i * DIM + k] * b[k * DIM + j];
                    }
                    c[i * DIM + j] = sum;
                }
            }
        }
        c[1]
    }
}

const PAYLOAD_MAX: usize = 512 * 1024;
static mut PAYLOAD: [u8; PAYLOAD_MAX] = [0; PAYLOAD_MAX];
static mut READBACK: [u8; PAYLOAD_MAX + 16] = [0; PAYLOAD_MAX + 16];

// 32-bit FNV-1a so JS can run the byte-identical algorithm (Math.imul).
fn bench_hash() -> u64 {
    let data = unsafe { &*core::ptr::addr_of!(PAYLOAD) };
    let mut h: u32 = 0x811c9dc5;
    for _ in 0..8 {
        for &b in data.iter() {
            h = (h ^ b as u32).wrapping_mul(0x01000193);
        }
    }
    h as u64
}

// ---- crossing cost by op weight ----

fn bench_noop() {
    for _ in 0..100_000 {
        unsafe { noop() };
    }
}

fn bench_setattr(target: i32, name: i32, v1: i32, v2: i32) {
    for i in 0..20_000 {
        let v = if i % 2 == 0 { v1 } else { v2 };
        unsafe { set_attribute(target, name as usize as *const u8, SENTINEL, v as usize as *const u8, SENTINEL) };
    }
}

fn bench_qsel(document: i32, selector: i32) {
    for _ in 0..2_000 {
        unsafe {
            let e = document_query_selector(document, selector as usize as *const u8, SENTINEL);
            if e != 0 {
                release(e);
            }
        }
    }
}

// ---- payload sweep (raw strings on purpose: measures the copy) ----

fn bench_payload(node: i32, size: usize, iterations: usize) {
    unsafe {
        let payload = &*core::ptr::addr_of!(PAYLOAD);
        let readback = &*core::ptr::addr_of!(READBACK);
        for _ in 0..iterations {
            text_content_set(node, payload.as_ptr(), size);
            text_content_get(node, readback.as_ptr(), readback.len());
        }
    }
}

// ---- tree walks over the handle table ----

fn walk(list: i32, out: Option<&mut [i32]>) -> (i32, usize) {
    let mut count = 0usize;
    let mut stored = out;
    unsafe {
        let mut node = node_first_child(list);
        while node != 0 {
            if let Some(buffer) = stored.as_deref_mut() {
                buffer[count] = node;
            }
            count += 1;
            node = node_next_sibling(node);
        }
    }
    (0, count)
}

fn walk_release(list: i32) -> usize {
    let mut count = 0usize;
    unsafe {
        let mut node = node_first_child(list);
        while node != 0 {
            let next = node_next_sibling(node);
            release(node);
            count += 1;
            node = next;
        }
    }
    count
}

const WALK_MAX: usize = 30_000;
static mut HELD: [i32; WALK_MAX] = [0; WALK_MAX];

// ---- harness ----

fn format_ms(ms: f64, buf: &mut [u8; 96], prefix: &str) -> usize {
    let p = prefix.as_bytes();
    buf[..p.len()].copy_from_slice(p);
    let mut n = p.len();
    buf[n] = b' ';
    n += 1;
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
    buf[n..n + 3].copy_from_slice(b" ms");
    n + 3
}

static mut SINK: u64 = 0;

fn report_time(name: &str, start: f64, end: f64) {
    let mut buf = [0u8; 96];
    let len = format_ms(end - start, &mut buf, name);
    unsafe {
        let results = by_id("results");
        let li = create_element("li".as_ptr(), 2);
        text_content_set(li, buf.as_ptr(), len);
        append_child(results, li);
        release(li);
        release(results);
    }
}

macro_rules! timed {
    ($name:expr, $body:expr) => {{
        let start = unsafe { now() };
        let _r = $body;
        let end = unsafe { now() };
        report_time($name, start, end);
    }};
}

#[no_mangle]
pub extern "C" fn _start() {
    unsafe {
        // deterministic data
        let a = &mut *core::ptr::addr_of_mut!(MAT_A);
        let b = &mut *core::ptr::addr_of_mut!(MAT_B);
        for i in 0..DIM * DIM {
            a[i] = (i % 97) as f64 * 0.5;
            b[i] = (i % 89) as f64 * 0.25;
        }
        let payload = &mut *core::ptr::addr_of_mut!(PAYLOAD);
        for (i, byte) in payload.iter_mut().enumerate() {
            *byte = b'a' + (i % 26) as u8;
        }
    }

    let attr_name = unsafe { intern("data-x".as_ptr(), 6) };
    let val1 = unsafe { intern("alpha".as_ptr(), 5) };
    let val2 = unsafe { intern("beta".as_ptr(), 4) };
    let selector = {
        let s = "div.item:nth-of-type(9)";
        unsafe { intern(s.as_ptr(), s.len()) }
    };

    let target = by_id("target");
    let textnode = by_id("textnode");
    let arena = by_id("walklist");
    let document = unsafe { node_owner_document(target) };

    for rep in 0..3 {
        let _ = rep;
        // compute
        timed!("fib", unsafe { SINK = SINK.wrapping_add(fib(27)) });
        timed!("matmul", unsafe { SINK = SINK.wrapping_add(bench_matmul() as u64) });
        timed!("hash", unsafe { SINK = SINK.wrapping_add(bench_hash()) });

        // crossing weights
        timed!("xnoop", bench_noop());
        timed!("xsetattr", bench_setattr(target, attr_name, val1, val2));
        timed!("xqsel", bench_qsel(document, selector));

        // payload sizes (ascii)
        timed!("pay16", bench_payload(textnode, 16, 8000));
        timed!("pay256", bench_payload(textnode, 256, 4000));
        timed!("pay4096", bench_payload(textnode, 4096, 1000));
        timed!("pay65536", bench_payload(textnode, 65536, 120));
        timed!("pay524288", bench_payload(textnode, 524288, 16));

        // non-ascii payload (2-byte code points -> non-ascii utf16 storage)
        unsafe {
            let payload = &mut *core::ptr::addr_of_mut!(PAYLOAD);
            let mut i = 0;
            while i + 1 < 65536 {
                payload[i] = 0xC3;
                payload[i + 1] = 0xA9;
                i += 2;
            }
        }
        timed!("paynonascii", bench_payload(textnode, 65536, 120));
        unsafe {
            let payload = &mut *core::ptr::addr_of_mut!(PAYLOAD);
            for (i, byte) in payload.iter_mut().enumerate() {
                *byte = b'a' + (i % 26) as u8;
            }
        }

        // walks
        let held = unsafe { &mut *core::ptr::addr_of_mut!(HELD) };
        let mut walked = 0usize;
        timed!("walkhold", {
            let (_, n) = walk(arena, Some(held));
            walked = n;
        });
        timed!("walkwarm", {
            let (_, n) = walk(arena, None);
            walked = n;
        });
        // balance: each held node was acquired twice (hold + warm walks)
        for &h in held[..walked].iter() {
            unsafe {
                release(h);
                release(h);
            }
        }
        timed!("walkrelease", walk_release(arena));
    }

    let results = by_id("results");
    unsafe { set_attribute(results, "data-done".as_ptr(), 9, "yes".as_ptr(), 3) };
    unsafe { release(results) };
}
