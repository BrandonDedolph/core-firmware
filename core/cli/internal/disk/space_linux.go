//go:build linux

package disk

import "syscall"

// volumeSpace on Linux: statfs(2) counts f_blocks and f_bavail in units of
// the FRAGMENT size (f_frsize), not f_bsize — which is only the preferred
// I/O size and differs on some file systems. Bsize is the fallback for a
// kernel that leaves Frsize zero.
func volumeSpace(root string) (int64, int64, error) {
	var st syscall.Statfs_t
	if err := syscall.Statfs(root, &st); err != nil {
		return 0, 0, err
	}
	unit := int64(st.Frsize)
	if unit == 0 {
		unit = int64(st.Bsize)
	}
	return int64(st.Bavail) * unit, int64(st.Blocks) * unit, nil
}
