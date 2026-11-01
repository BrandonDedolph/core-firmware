//go:build windows

package disk

import "golang.org/x/sys/windows"

func volumeSpace(root string) (int64, int64, error) {
	p, err := windows.UTF16PtrFromString(root)
	if err != nil {
		return 0, 0, err
	}
	var free, total, totalFree uint64
	if err := windows.GetDiskFreeSpaceEx(p, &free, &total, &totalFree); err != nil {
		return 0, 0, err
	}
	return int64(free), int64(total), nil
}
