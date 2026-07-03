//! Async wakeup-chain benchmark: 500 sequential dom.set_timeout(0) hops, each
//! callback re-arming the next; reports total elapsed (thus per-wakeup cost:
//! event-loop task + machine.invoke entry + timer plumbing).

#![no_std]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    core::arch::wasm32::unreachable()
}

#[link(wasm_import_module = "dom")]
extern "C" {
    fn get_element_by_id(ptr: *const u8, len: usize) -> i32;
    fn create_element(ptr: *const u8, len: usize) -> i32;
    fn append_child(parent: i32, child: i32) -> i32;
    fn set_attribute(node: i32, nptr: *const u8, nlen: usize, vptr: *const u8, vlen: usize) -> i32;
    fn text_content_set(node: i32, ptr: *const u8, len: usize) -> i32;
    fn set_timeout(ms: i32, callback: i32, user_data: i32) -> i32;
    fn now() -> f64;
    fn release(handle: i32);
}

const DEPTH: i32 = 500;
static mut START_TIME: f64 = 0.0;

fn report(elapsed: f64) {
    // "chain <ms>" with two decimals
    let mut buf = [0u8; 32];
    buf[..6].copy_from_slice(b"chain ");
    let mut n = 6;
    let scaled = (elapsed * 100.0 + 0.5) as u64;
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
    n += 3;

    unsafe {
        let results = get_element_by_id("results".as_ptr(), 7);
        let li = create_element("li".as_ptr(), 2);
        text_content_set(li, buf.as_ptr(), n);
        append_child(results, li);
        release(li);
        set_attribute(results, "data-done".as_ptr(), 9, "yes".as_ptr(), 3);
        release(results);
    }
}

extern "C" fn on_timeout(_arg: i32, count: i32) {
    if count < DEPTH {
        unsafe { set_timeout(0, on_timeout as usize as i32, count + 1) };
    } else {
        let elapsed = unsafe { now() - START_TIME };
        report(elapsed);
    }
}

#[no_mangle]
pub extern "C" fn _start() {
    unsafe {
        START_TIME = now();
        set_timeout(0, on_timeout as usize as i32, 1);
    }
}
