// Shared setup for the extended suite: fixture DOM the workloads run against.
// Runs before the measured code in every variant; not part of any timing.
(function () {
    const body = document.body;
    function el(tag, id, parent) {
        const e = document.createElement(tag);
        if (id) e.id = id;
        (parent || body).appendChild(e);
        return e;
    }
    el("div", "target").hidden = true;
    el("div", "textnode").hidden = true;
    el("ol", "results");
    const arena = el("div", "arena");
    arena.hidden = true;
    for (let i = 0; i < 100; i++) {
        const d = document.createElement("div");
        d.className = "item";
        arena.appendChild(d);
    }
    const walklist = el("div", "walklist");
    walklist.hidden = true;
    for (let i = 0; i < 30000; i++)
        walklist.appendChild(document.createElement("div"));
})();
