//go:build !windows && !linux && !darwin

package disk

func volumeSpace(string) (int64, int64, error) { return 0, 0, ErrNoSpaceInfo }
