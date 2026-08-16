#pragma once
// Ported from the D4 browser: install Ctrl+C on a table/tree view so the selected
// rows (or all rows if none selected) are copied to the clipboard as CSV — header
// plus every column, RFC-4180 quoted. Also adds a right-click Copy / Copy all menu
// UNLESS the view already declared its own context-menu policy.
//
// PORTING TRAP (documented in the D4 source, preserved here): install() only skips
// its own context menu if the view's policy is not already DefaultContextMenu.
// Called BEFORE the caller sets CustomContextMenu, it installs anyway and the
// caller's later connect() becomes a second handler on the same signal — Qt runs
// both, this one is connected first, its menu wins, and the caller's is silently
// unreachable. Set your context-menu policy BEFORE calling install().

class QAbstractItemView;

namespace CsvCopy {
void install(QAbstractItemView* view);
}
