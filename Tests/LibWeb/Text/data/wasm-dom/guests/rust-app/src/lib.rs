//! App-shaped benchmark: a keyed list with guest-owned state. 500 state
//! operations over a 200-item list; after each, the renderer applies the
//! minimal DOM update (toggle -> attribute, relabel -> textContent,
//! rotate -> removeChild + append). Element handles for every rendered row are
//! held across the whole run — the identity-cache working as a wrapper cache.

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
            pub fn node_remove_child(parent: i32, child: i32) -> i32;
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

const SENTINEL: usize = usize::MAX;
const ITEMS: usize = 200;
const OPS: usize = 500;

#[derive(Clone, Copy)]
struct Item {
    id: u32,
    version: u32,
    done: bool,
    element: i32,
}

static mut LIST: [Item; ITEMS] = [Item { id: 0, version: 0, done: false, element: 0 }; ITEMS];

fn format_label(item: &Item, buf: &mut [u8; 32]) -> usize {
    let mut n = 0;
    for &b in b"item-" {
        buf[n] = b;
        n += 1;
    }
    n += itoa(item.id, &mut buf[n..]);
    buf[n] = b'-';
    buf[n + 1] = b'v';
    n += 2;
    n += itoa(item.version, &mut buf[n..]);
    n
}

fn itoa(mut v: u32, out: &mut [u8]) -> usize {
    let mut digits = [0u8; 10];
    let mut i = digits.len();
    loop {
        i -= 1;
        digits[i] = b'0' + (v % 10) as u8;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    let n = digits.len() - i;
    out[..n].copy_from_slice(&digits[i..]);
    n
}

struct Env {
    list_el: i32,
    li_tag: i32,
    data_done: i32,
    yes: i32,
    no: i32,
}

fn render_new_item(env: &Env, item: &mut Item) {
    let mut buf = [0u8; 32];
    let len = format_label(item, &mut buf);
    unsafe {
        let li = create_element(env.li_tag as usize as *const u8, SENTINEL);
        text_content_set(li, buf.as_ptr(), len);
        set_attribute(li, env.data_done as usize as *const u8, SENTINEL,
            (if item.done { env.yes } else { env.no }) as usize as *const u8, SENTINEL);
        append_child(env.list_el, li);
        item.element = li;
    }
}

fn run_ops(env: &Env, next_id: &mut u32) {
    let list = unsafe { &mut *core::ptr::addr_of_mut!(LIST) };
    for op in 0..OPS {
        match op % 3 {
            0 => {
                // toggle: attribute update on an existing row
                let item = &mut list[(op * 7) % ITEMS];
                item.done = !item.done;
                unsafe {
                    set_attribute(item.element, env.data_done as usize as *const u8, SENTINEL,
                        (if item.done { env.yes } else { env.no }) as usize as *const u8, SENTINEL);
                }
            }
            1 => {
                // relabel: textContent update on an existing row
                let item = &mut list[(op * 13) % ITEMS];
                item.version += 1;
                let mut buf = [0u8; 32];
                let len = format_label(item, &mut buf);
                unsafe { text_content_set(item.element, buf.as_ptr(), len) };
            }
            _ => {
                // rotate: remove the oldest row, append a fresh one
                let victim = list[0];
                unsafe {
                    node_remove_child(env.list_el, victim.element);
                    release(victim.element);
                }
                for i in 0..ITEMS - 1 {
                    list[i] = list[i + 1];
                }
                *next_id += 1;
                let mut fresh = Item { id: *next_id, version: 0, done: false, element: 0 };
                render_new_item(env, &mut fresh);
                list[ITEMS - 1] = fresh;
            }
        }
    }
}

fn report(name: &str, ms: f64) {
    let mut buf = [0u8; 64];
    let p = name.as_bytes();
    buf[..p.len()].copy_from_slice(p);
    let mut n = p.len();
    buf[n] = b' ';
    n += 1;
    let scaled = (ms * 100.0 + 0.5) as u64;
    n += itoa((scaled / 100) as u32, &mut buf[n..]);
    buf[n] = b'.';
    buf[n + 1] = b'0' + ((scaled % 100) / 10) as u8;
    buf[n + 2] = b'0' + (scaled % 10) as u8;
    n += 3;
    buf[n..n + 3].copy_from_slice(b" ms");
    n += 3;
    unsafe {
        let results = get_element_by_id("results".as_ptr(), 7);
        let li = create_element("li".as_ptr(), 2);
        text_content_set(li, buf.as_ptr(), n);
        append_child(results, li);
        release(li);
        release(results);
    }
}

#[no_mangle]
pub extern "C" fn _start() {
    let env = Env {
        list_el: unsafe { get_element_by_id("list".as_ptr(), 4) },
        li_tag: unsafe { intern("li".as_ptr(), 2) },
        data_done: unsafe { intern("data-done-state".as_ptr(), 15) },
        yes: unsafe { intern("yes".as_ptr(), 3) },
        no: unsafe { intern("no".as_ptr(), 2) },
    };

    let mut next_id: u32 = 0;
    {
        let list = unsafe { &mut *core::ptr::addr_of_mut!(LIST) };
        for slot in list.iter_mut() {
            next_id += 1;
            let mut item = Item { id: next_id, version: 0, done: false, element: 0 };
            render_new_item(&env, &mut item);
            *slot = item;
        }
    }

    for _ in 0..3 {
        let start = unsafe { now() };
        run_ops(&env, &mut next_id);
        let elapsed = unsafe { now() } - start;
        report("app", elapsed);
    }

    unsafe {
        let results = get_element_by_id("results".as_ptr(), 7);
        set_attribute(results, "data-done".as_ptr(), 9, "yes".as_ptr(), 3);
        release(results);
    }
}
