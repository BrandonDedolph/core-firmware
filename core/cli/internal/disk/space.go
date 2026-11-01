package disk

import "errors"

// ErrNoSpaceInfo is VolumeSpace on a platform with no way to ask.
var ErrNoSpaceInfo = errors.New("disk: free space is not available on this platform")

// VolumeSpace is the mounted volume's free and total bytes, as the file
// system reports them — free being what this user may still write, the
// figure Explorer shows. root is a mount point or drive root ("D:\",
// "/media/IPOD").
func VolumeSpace(root string) (free, total int64, err error) {
	return volumeSpace(root)
}
