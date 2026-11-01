package app

// snapshotDialog is TestSnapshotFlashDialog's renderer: the demo with the
// untested-hardware write dialog open, the one that still has a typed field
// (partly typed, so the disabled Write it is in the picture).
func snapshotDialog(st State, w, h int, out string) error {
	return SnapshotDialog(st, ThemeLinen, "untested", w, h, out)
}
