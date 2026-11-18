// A list the window shows is handed over again every time the host publishes. A `for` row in a .slint file is
// rebuilt when its list is a DIFFERENT model object, and a row that is rebuilt between a real mouse press and its
// release never hears the click (a scripted click presses and releases inside one tick, a hand takes 50 to 150 ms
// and the host publishes every 33 ms). reuseModel keeps the model object the window already holds and changes only
// the rows that differ, in place, so the rows (and the press in progress on one of them) stay alive.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <slint.h>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace foxsdr::slinthost {

// `held` is the model the window holds now (it may be null, or of another kind); `rows` is what it must show.
// Returns the model to hand to the window: `held` itself, updated, when it is a VectorModel; otherwise a new one.
template <class T>
std::shared_ptr<slint::Model<T>> reuseModel(const std::shared_ptr<slint::Model<T>>& held, std::vector<T> rows) {
    auto vm = std::dynamic_pointer_cast<slint::VectorModel<T>>(held);
    if (!vm) { return std::make_shared<slint::VectorModel<T>>(std::move(rows)); }
    const std::size_t have = vm->row_count();
    const std::size_t common = have < rows.size() ? have : rows.size();
    for (std::size_t i = 0; i < common; ++i) {
        const auto cur = vm->row_data(i);
        if (!cur.has_value() || !(*cur == rows[i])) { vm->set_row_data(i, rows[i]); }
    }
    for (std::size_t i = common; i < rows.size(); ++i) { vm->push_back(std::move(rows[i])); }
    while (vm->row_count() > rows.size()) { vm->erase(vm->row_count() - 1); }
    return vm;
}

// The same, for a list already built as a model: its rows go into `held` (see above) and `fresh` is dropped. Null `fresh` stays null.
template <class T>
std::shared_ptr<slint::Model<T>> reuseModelFrom(const std::shared_ptr<slint::Model<T>>& held, const std::shared_ptr<slint::Model<T>>& fresh) {
    if (!fresh || !held) { return fresh; }
    std::vector<T> rows;
    rows.reserve(fresh->row_count());
    for (std::size_t i = 0; i < fresh->row_count(); ++i) { rows.push_back(*fresh->row_data(i)); }
    return reuseModel<T>(held, std::move(rows));
}

}  // namespace foxsdr::slinthost
