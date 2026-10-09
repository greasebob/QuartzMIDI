// fake_dom.js: just enough of a browser document for app_test.js to run page/app.js on page/index.html:
// elements, attributes, classes, simple selectors (tag, #id, .class, [attr], [attr=value], comma lists),
// events with capture and bubbling, focus, and a page's default actions for Space and Tab on a button.

const SVG = "http://www.w3.org/2000/svg";
const VOID = new Set(["meta", "link", "img", "input", "br", "hr", "source"]);
const FOCUSABLE = new Set(["BUTTON", "INPUT", "SELECT", "TEXTAREA", "A", "IFRAME"]);

class FakeEvent {
  constructor(type, init = {}) {
    Object.assign(this, { bubbles: true, isTrusted: false, key: "", shiftKey: false, ctrlKey: false, altKey: false, metaKey: false, clientX: 0, clientY: 0 }, init);
    this.type = type;
    this.defaultPrevented = false;
    this.stopped = false;
    this.halted = false;
  }
  preventDefault() { this.defaultPrevented = true; }
  stopPropagation() { this.stopped = true; }
  stopImmediatePropagation() { this.stopped = this.halted = true; }
}

class Listeners {
  constructor() { this.list = []; }
  addEventListener(type, fn, opts) {
    const capture = opts === true || !!(opts && opts.capture);
    this.list.push({ type, fn, capture, once: !!(opts && opts.once) });
  }
  removeEventListener(type, fn, opts) {
    const capture = opts === true || !!(opts && opts.capture);
    this.list = this.list.filter((l) => !(l.type === type && l.fn === fn && l.capture === capture));
  }
  fire(event, capture) {
    for (const l of [...this.list]) {
      if (event.halted) return;
      if (l.type !== event.type || (capture !== null && l.capture !== capture)) continue;
      if (l.once) this.list = this.list.filter((x) => x !== l);
      event.currentTarget = this;
      l.fn.call(this, event);
    }
  }
}

class FakeNode extends Listeners {
  constructor(doc) {
    super();
    this.ownerDocument = doc;
    this.parentNode = null;
    this.childNodes = [];
  }
  get parentElement() { return this.parentNode && this.parentNode.nodeType === 1 ? this.parentNode : null; }
  get isConnected() { return this.ownerDocument.contains(this); }
  sibling(step) {
    const list = this.parentNode ? this.parentNode.childNodes : [];
    const i = list.indexOf(this);
    return i < 0 ? null : list[i + step] || null;
  }
  get previousSibling() { return this.sibling(-1); }
  get nextSibling() { return this.sibling(1); }
  remove() {
    if (!this.parentNode) return;
    const list = this.parentNode.childNodes;
    list.splice(list.indexOf(this), 1);
    this.parentNode = null;
  }
  insertAt(index, nodes) {
    let i = index;
    for (let n of nodes) {
      if (n == null) continue;
      if (typeof n !== "object") n = new FakeText(this.ownerDocument || this, String(n));
      if (n.parentNode === this && this.childNodes.indexOf(n) < i) i--;
      n.remove();
      n.parentNode = this;
      this.childNodes.splice(i++, 0, n);
    }
  }
  append(...nodes) { this.insertAt(this.childNodes.length, nodes); }
  prepend(...nodes) { this.insertAt(0, nodes); }
  insertBefore(node, ref) { this.insertAt(ref ? this.childNodes.indexOf(ref) : this.childNodes.length, [node]); return node; }
  replaceChildren(...nodes) {
    for (const c of this.childNodes) c.parentNode = null;
    this.childNodes = [];
    this.append(...nodes);
  }
  after(...nodes) { if (this.parentNode) this.parentNode.insertAt(this.parentNode.childNodes.indexOf(this) + 1, nodes); }
  before(...nodes) { if (this.parentNode) this.parentNode.insertAt(this.parentNode.childNodes.indexOf(this), nodes); }
  replaceWith(...nodes) {
    const p = this.parentNode;
    if (!p) return;
    const i = p.childNodes.indexOf(this);
    this.remove();
    p.insertAt(i, nodes);
  }
  contains(node) {
    for (let n = node; n; n = n.parentNode) if (n === this) return true;
    return false;
  }
  get children() { return this.childNodes.filter((n) => n.nodeType === 1); }
  get firstElementChild() { return this.children[0] || null; }
  get lastElementChild() { return this.children.at(-1) || null; }
  get nextElementSibling() {
    for (let n = this.nextSibling; n; n = n.nextSibling) if (n.nodeType === 1) return n;
    return null;
  }
  get previousElementSibling() {
    for (let n = this.previousSibling; n; n = n.previousSibling) if (n.nodeType === 1) return n;
    return null;
  }
  get textContent() { return this.childNodes.map((c) => c.textContent).join(""); }
  set textContent(value) { this.replaceChildren(...(value === "" || value == null ? [] : [String(value)])); }
  *walk() {
    for (const c of this.childNodes) {
      if (c.nodeType !== 1) continue;
      yield c;
      yield* c.walk();
    }
  }
  querySelectorAll(selector) { return [...this.walk()].filter((e) => e.matches(selector)); }
  querySelector(selector) {
    for (const e of this.walk()) if (e.matches(selector)) return e;
    return null;
  }
  // Capture from the document down, the target itself, then bubbling back up.
  dispatchEvent(event) {
    event.target = this;
    const path = [];
    for (let n = this.parentNode; n; n = n.parentNode) path.push(n);
    for (const n of [...path].reverse()) { if (!event.stopped) n.fire(event, true); }
    if (!event.stopped) this.fire(event, null);
    if (event.bubbles) for (const n of path) { if (!event.stopped) n.fire(event, false); }
    return !event.defaultPrevented;
  }
}

class FakeText extends FakeNode {
  constructor(doc, data) {
    super(doc);
    this.nodeType = 3;
    this.data = data;
  }
  get textContent() { return this.data; }
  set textContent(value) { this.data = String(value); }
}

const parseSelector = (selector) => selector.split(",").map((part) => {
  const s = part.trim();
  const parts = [];
  const re = /([a-zA-Z][\w-]*)|#([\w-]+)|\.([\w-]+)|\[([\w-]+)(?:=["']?([^\]"']*)["']?)?\]/gy;
  let m;
  while (re.lastIndex < s.length && (m = re.exec(s))) parts.push(m);
  if (re.lastIndex !== s.length && parts.length === 0) throw new Error("selector not supported: " + selector);
  return parts;
});

class FakeElement extends FakeNode {
  constructor(doc, tag, ns = null) {
    super(doc);
    this.nodeType = 1;
    this.namespaceURI = ns;
    this.localName = ns === SVG ? tag : tag.toLowerCase();
    this.tagName = ns === SVG ? tag : tag.toUpperCase();
    this.attrs = new Map();
    this.dataset = {};
    const props = new Map();
    this.style = new Proxy({
      setProperty: (k, v) => props.set(k, String(v)),
      removeProperty: (k) => props.delete(k),
      getPropertyValue: (k) => props.get(k) || "",
    }, { set: (_, k, v) => { props.set(k, String(v)); return true; }, get: (t, k) => (k in t ? t[k] : props.get(k) || "") });
    this.scrollTop = 0;
    this.value = "";
    this.contentWindow = null;
    this.contentDocument = null;
  }
  getAttribute(name) { return this.attrs.has(name) ? this.attrs.get(name) : null; }
  setAttribute(name, value) {
    this.attrs.set(name, String(value));
    if (name.startsWith("data-")) this.dataset[name.slice(5).replace(/-(\w)/g, (_, c) => c.toUpperCase())] = String(value);
  }
  removeAttribute(name) { this.attrs.delete(name); }
  hasAttribute(name) { return this.attrs.has(name); }
  get classList() {
    const names = () => (this.getAttribute("class") || "").split(/\s+/).filter(Boolean);
    const put = (list) => this.setAttribute("class", list.join(" "));
    return {
      contains: (c) => names().includes(c),
      add: (...cs) => put([...new Set([...names(), ...cs])]),
      remove: (...cs) => put(names().filter((n) => !cs.includes(n))),
      toggle: (c, force) => {
        const on = force === undefined ? !names().includes(c) : !!force;
        if (on) put([...new Set([...names(), c])]);
        else put(names().filter((n) => n !== c));
        return on;
      },
    };
  }
  get tabIndex() {
    const t = this.getAttribute("tabindex");
    return t != null ? Number(t) : FOCUSABLE.has(this.tagName) ? 0 : -1;
  }
  set tabIndex(value) { this.setAttribute("tabindex", String(value)); }
  matches(selector) {
    return parseSelector(selector).some((parts) => parts.every((m) => {
      if (m[1]) return this.localName === m[1].toLowerCase();
      if (m[2]) return this.getAttribute("id") === m[2];
      if (m[3]) return this.classList.contains(m[3]);
      return m[5] === undefined ? this.hasAttribute(m[4]) : this.getAttribute(m[4]) === m[5];
    }));
  }
  closest(selector) {
    if (this.matches(selector)) return this;
    return this.parentElement ? this.parentElement.closest(selector) : null;
  }
  focus() { this.ownerDocument.focusOn(this); }
  blur() { if (this.ownerDocument.activeElement === this) this.ownerDocument.focusOn(this.ownerDocument.body); }
  click() { this.dispatchEvent(new FakeEvent("click")); }
  select() {}
  scrollIntoView() {}
  setPointerCapture() {}
  getBoundingClientRect() { return { left: 0, top: 0, right: 0, bottom: 0, width: 0, height: 0, x: 0, y: 0 }; }
  get offsetWidth() { return 0; }
  get offsetHeight() { return 0; }
  get offsetParent() { return null; }
}
// Attributes the page sets as properties.
for (const [prop, attr] of [["id", "id"], ["className", "class"], ["title", "title"], ["src", "src"], ["alt", "alt"], ["type", "type"]]) {
  Object.defineProperty(FakeElement.prototype, prop, {
    get() { return this.getAttribute(attr) || ""; },
    set(value) { this.setAttribute(attr, value); },
  });
}
for (const prop of ["hidden", "disabled"]) {
  Object.defineProperty(FakeElement.prototype, prop, {
    get() { return this.hasAttribute(prop); },
    set(value) { if (value) this.setAttribute(prop, ""); else this.removeAttribute(prop); },
  });
}

class FakeDocument extends FakeNode {
  constructor() {
    super(null);
    this.nodeType = 9;
    this.ownerDocument = this;
    this.cookie = "";
    this.activeElement = null;
  }
  get documentElement() { return this.firstElementChild; }
  get body() { return this.documentElement && this.documentElement.querySelector("body"); }
  getElementById(id) { return this.querySelector("#" + id); }
  createElement(tag) { return new FakeElement(this, tag); }
  createElementNS(ns, tag) { return new FakeElement(this, tag, ns); }
  createTextNode(data) { return new FakeText(this, data); }
  execCommand() { return false; }
  focusOn(el) {
    if (this.activeElement === el) return;
    const old = this.activeElement;
    this.activeElement = el;
    if (old && old !== this.body) old.dispatchEvent(new FakeEvent("focusout"));
    if (el !== this.body) el.dispatchEvent(new FakeEvent("focusin")); // the body is not focusable: focus there fires nothing
  }
}

// index.html into a document: tags, attributes and text; no comments or scripts are run.
export function parseHtml(html) {
  const doc = new FakeDocument();
  let at = doc;
  const re = /<!doctype[^>]*>|<\/([\w-]+)\s*>|<([\w-]+)((?:\s+[\w:-]+(?:="[^"]*")?)*)\s*(\/?)>|([^<]+)/gi;
  let m;
  while ((m = re.exec(html))) {
    if (m[1]) {
      for (let n = at; n && n !== doc; n = n.parentNode) {
        if (n.localName === m[1].toLowerCase()) { at = n.parentNode; break; }
      }
    } else if (m[2]) {
      const inSvg = m[2] === "svg" || (at.namespaceURI === SVG);
      const el = new FakeElement(doc, m[2], inSvg ? SVG : null);
      for (const a of m[3].matchAll(/([\w:-]+)(?:="([^"]*)")?/g)) {
        el.setAttribute(a[1], (a[2] || "").replace(/&amp;/g, "&").replace(/&quot;/g, '"').replace(/&lt;/g, "<").replace(/&gt;/g, ">"));
      }
      at.append(el);
      if (!m[4] && !VOID.has(el.localName)) at = el;
    } else if (m[5] && at !== doc) at.append(new FakeText(doc, m[5]));
  }
  doc.activeElement = doc.body;
  return doc;
}

// Real input as the page sees it: a pointer press, its events trusted and at `at` when given, focuses unless its
// mousedown was prevented, and clicks; Tab focuses; a key press runs its listeners, and Space or Enter on a
// focused button then presses it unless a listener prevented that.
export const input = {
  press(el, at = {}) {
    const event = (type) => new FakeEvent(type, { isTrusted: true, ...at });
    el.dispatchEvent(event("pointerdown"));
    const down = event("mousedown");
    el.dispatchEvent(down);
    if (!down.defaultPrevented && (FOCUSABLE.has(el.tagName) || el.hasAttribute("tabindex"))) el.focus();
    el.dispatchEvent(event("pointerup"));
    el.dispatchEvent(event("mouseup"));
    el.dispatchEvent(event("click"));
  },
  tabTo(el) { el.focus(); },
  key(doc, key, init = {}) {
    const target = doc.activeElement || doc.body;
    const event = new FakeEvent("keydown", { key, ...init });
    target.dispatchEvent(event);
    if (!event.defaultPrevented && (key === " " || key === "Enter") && target.tagName === "BUTTON") target.click();
    return event;
  },
  // Typing into the focused field: each character's keydown, then, unless prevented, the character and an input event.
  type(doc, text) {
    for (const ch of text) {
      const target = doc.activeElement || doc.body;
      if (input.key(doc, ch).defaultPrevented || target.tagName !== "INPUT") continue;
      target.value = (target.value || "") + ch;
      target.dispatchEvent(new FakeEvent("input"));
    }
  },
};

// The served page's location and history, started on `route` ("?q=…", or "" for /quartzmidi): pushState and
// replaceState move the location, and back(), forward() and go() move it and fire popstate through `fire`, as a
// browser does (here at once).
export function fakeHistory(route, fire) {
  const base = "https://www.youtube.com";
  const location = { pathname: "/quartzmidi", search: "", hash: "" };
  Object.defineProperty(location, "href", {
    get: () => base + location.pathname + location.search + location.hash,
    set: (url) => move(url),
    enumerable: true,
  });
  const move = (url) => {
    const u = new URL(url, location.href);
    Object.assign(location, { pathname: u.pathname, search: u.search, hash: u.hash });
  };
  move(base + "/quartzmidi" + route);
  const entries = [{ state: null, url: location.href }];
  let at = 0;
  const history = {
    get state() { return entries[at].state; },
    get length() { return entries.length; },
    pushState(state, _title, url) {
      if (url != null) move(url);
      entries.splice(at + 1, entries.length, { state, url: location.href });
      at++;
    },
    replaceState(state, _title, url) {
      if (url != null) move(url);
      entries[at] = { state, url: location.href };
    },
    go(step = 0) {
      const to = at + step;
      if (!step || to < 0 || to >= entries.length) return;
      at = to;
      move(entries[at].url);
      fire("popstate");
    },
    back() { this.go(-1); },
    forward() { this.go(1); },
  };
  return { location, history };
}

export { FakeEvent };
