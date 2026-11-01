//go:build darwin

package disk

import "syscall"

// volumeSpace on macOS: statfs's f_bsize is the fundamental block size the
// counts are in (there is no f_frsize).
func volumeSpace(root string) (int64, int64, error) {
	var st syscall.Statfs_t
	if err := syscall.Statfs(root, &st); err != nil {
		return 0, 0, err
	}
	bs := int64(st.Bsize)
	return int64(st.Bavail) * bs, int64(st.Blocks) * bs, nil
}
