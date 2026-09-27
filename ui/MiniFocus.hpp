#pragma once

namespace shell {
// Mini leaves the keyboard with the game. When a control starts to read keys
// (a text field, a key being bound) while another program has the keyboard, mini
// takes it; when that ends, or mini closes, it gives it back.
enum class KeyboardMove { Stay, Take, GiveBack };

// `reads`: a control reads keys now; `read`: it did on the last pass; `ours`: a
// window of the app has the keyboard; `taken`: mini took it and still holds it.
inline KeyboardMove MiniKeyboard(bool mini, bool reads, bool read, bool ours, bool taken) {
    if (mini && reads && !read && !ours) return KeyboardMove::Take;
    if (taken && (!reads || !mini)) return KeyboardMove::GiveBack;
    return KeyboardMove::Stay;
}
}
