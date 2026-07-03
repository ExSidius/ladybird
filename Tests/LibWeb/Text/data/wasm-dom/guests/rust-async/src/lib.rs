//! Real Rust async/await on the wasm-dom host interface.
//!
//! Build (see ../README.md):
//!   RUSTFLAGS="-C link-args=--export-table" \
//!     cargo build --release --target wasm32-unknown-unknown
//!
//! Architecture note: tokio itself cannot run on wasm32-unknown-unknown (its
//! runtime needs threads, epoll, and a clock), but this module uses tokio's
//! *shape* scaled to the browser's constraints: the `futures` crate's
//! single-threaded executor (LocalPool) as the scheduler, plus a hand-rolled
//! reactor that parks futures on host operations (dom.fetch / dom.set_timeout)
//! and wakes them when the host calls back. The host provides operations and
//! wakeups; all scheduling lives in this module — exactly the "guest brings its
//! own runtime" division of labor.

use core::cell::RefCell;
use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll, Waker};
use futures::executor::{LocalPool, LocalSpawner};
use futures::task::LocalSpawnExt;
use std::collections::HashMap;

mod dom {
    #[link(wasm_import_module = "dom")]
    extern "C" {
        pub fn get_element_by_id(ptr: *const u8, len: usize) -> i32;
        pub fn create_element(ptr: *const u8, len: usize) -> i32;
        pub fn append_child(parent: i32, child: i32) -> i32;
        pub fn set_attribute(node: i32, nptr: *const u8, nlen: usize, vptr: *const u8, vlen: usize) -> i32;
        pub fn text_content_set(node: i32, ptr: *const u8, len: usize) -> i32;
        pub fn add_event_listener(node: i32, tptr: *const u8, tlen: usize, callback: i32, user_data: i32) -> i32;
        pub fn set_timeout(ms: i32, callback: i32, user_data: i32) -> i32;
        pub fn fetch(url: *const u8, len: usize, callback: i32, user_data: i32) -> i32;
        pub fn response_status(response: i32) -> i32;
        pub fn response_read(response: i32, dst: *const u8, cap: usize) -> i32;
    }
}

// ---- Minimal typed DOM layer (what codegen would produce) ----

#[derive(Clone, Copy)]
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
        let index = callback as usize as i32;
        unsafe { dom::add_event_listener(self.0, event.as_ptr(), event.len(), index, user_data) };
    }
}

fn push_line(text: &str) {
    let li = Node::create("li");
    li.set_text(text);
    if let Some(log) = Node::by_id("log") {
        log.append(li);
    }
}

// ---- Reactor: parks futures on host operations, wakes them on completion ----

enum OpResult {
    Timer,
    Fetch { status: i32, body: Vec<u8> },
}

#[derive(Default)]
struct OpState {
    result: Option<OpResult>,
    waker: Option<Waker>,
}

struct Runtime {
    pool: LocalPool,
    spawner: LocalSpawner,
    ops: HashMap<i32, OpState>,
    next_op: i32,
}

thread_local! {
    static RUNTIME: RefCell<Runtime> = {
        let pool = LocalPool::new();
        let spawner = pool.spawner();
        RefCell::new(Runtime { pool, spawner, ops: HashMap::new(), next_op: 1 })
    };
}

fn spawn(future: impl Future<Output = ()> + 'static) {
    RUNTIME.with(|rt| rt.borrow().spawner.spawn_local(future).unwrap());
}

/// Drive every runnable task to its next await point. Called after each host
/// wakeup; equivalent to one turn of a tokio worker, cooperative-only.
fn turn() {
    // LocalPool::run_until_stalled re-enters user futures, which may touch the
    // runtime; take the pool out of the RefCell for the duration.
    let mut pool = RUNTIME.with(|rt| core::mem::replace(&mut rt.borrow_mut().pool, LocalPool::new()));
    pool.run_until_stalled();
    RUNTIME.with(|rt| rt.borrow_mut().pool = pool);
}

/// The single funcref-table entry every async host op completes through.
/// user_data is the reactor's operation id.
extern "C" fn host_wakeup(arg: i32, user_data: i32) {
    RUNTIME.with(|rt| {
        let mut rt = rt.borrow_mut();
        let Some(op) = rt.ops.get_mut(&user_data) else { return };
        op.result = Some(if arg != 0 {
            // A fetch completion: the response handle is only valid during this
            // callback, so copy status and body out now (probe-then-read ABI).
            let status = unsafe { dom::response_status(arg) };
            let full_len = unsafe { dom::response_read(arg, core::ptr::null(), 0) } as usize;
            let mut body = vec![0u8; full_len];
            unsafe { dom::response_read(arg, body.as_ptr(), body.len()) };
            OpResult::Fetch { status, body }
        } else {
            // Timers complete with argument 0 (as do failed fetches; a failed
            // fetch surfaces as status 0 with an empty body).
            OpResult::Timer
        });
        if let Some(waker) = rt.ops.get_mut(&user_data).unwrap().waker.take() {
            drop(rt);
            waker.wake();
        }
    });
    turn();
}

struct OpFuture(i32);

impl Future for OpFuture {
    type Output = OpResult;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<OpResult> {
        RUNTIME.with(|rt| {
            let mut rt = rt.borrow_mut();
            let op = rt.ops.get_mut(&self.0).expect("op vanished");
            if let Some(result) = op.result.take() {
                rt.ops.remove(&self.0);
                Poll::Ready(result)
            } else {
                op.waker = Some(cx.waker().clone());
                Poll::Pending
            }
        })
    }
}

fn new_op() -> i32 {
    RUNTIME.with(|rt| {
        let mut rt = rt.borrow_mut();
        let id = rt.next_op;
        rt.next_op += 1;
        rt.ops.insert(id, OpState::default());
        id
    })
}

fn wakeup_index() -> i32 {
    host_wakeup as usize as i32
}

fn sleep(ms: i32) -> OpFuture {
    let id = new_op();
    unsafe { dom::set_timeout(ms, wakeup_index(), id) };
    OpFuture(id)
}

async fn fetch_text(url: &str) -> String {
    let id = new_op();
    unsafe { dom::fetch(url.as_ptr(), url.len(), wakeup_index(), id) };
    match OpFuture(id).await {
        OpResult::Fetch { status: 200, body } => String::from_utf8_lossy(&body).into_owned(),
        _ => String::from("<fetch failed>"),
    }
}

// ---- The actual program: non-trivial async control flow ----

async fn async_main() {
    let status = Node::by_id("status").unwrap();

    // Timed sequencing.
    for i in [3, 2, 1] {
        status.set_text(&format!("starting in {i}"));
        sleep(15).await;
    }
    status.set_text("running");

    // Two fetches in flight concurrently, joined.
    let (a, b) = futures::join!(
        fetch_text("../../data/wasm-dom/data-a.txt"),
        fetch_text("../../data/wasm-dom/data-b.txt"),
    );
    push_line(&format!("joined: {a} + {b}"));

    // A dependent chain: the first response names the next resource.
    let pointer = fetch_text("../../data/wasm-dom/chain-start.txt").await;
    let followed = fetch_text(&format!("../../data/wasm-dom/{}", pointer.trim())).await;
    push_line(&format!("chain: {followed}"));

    Node::by_id("async-root").unwrap().set_attribute("data-done", "yes");
}

static mut CLICK_COUNT: u32 = 0;

extern "C" fn on_click(_event: i32, _user_data: i32) {
    let n = unsafe {
        CLICK_COUNT += 1;
        CLICK_COUNT
    };
    // Spawn a task per click, tokio-handler style: the listener returns
    // immediately and the work finishes later on the executor.
    spawn(async move {
        sleep(10).await;
        push_line(&format!("spawned task {n} done"));
    });
    turn();
}

#[no_mangle]
pub extern "C" fn _start() {
    if let Some(button) = Node::by_id("async-btn") {
        button.on("click", on_click, 0);
    }
    spawn(async_main());
    turn();
}
