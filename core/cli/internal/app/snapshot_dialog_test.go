package app

// snapshotDialog is TestSnapshotFlashDialog's renderer: the demo with the
// write dialog open, mid-hold.
func snapshotDialog(st State, w, h int, out string) error {
	return SnapshotDialog(st, ThemeLinen, "holding", w, h, out)
}
