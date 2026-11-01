package disk

import (
	"os"
	"testing"
)

// VolumeSpace on the temp directory's file system: whatever the numbers
// are, free cannot exceed total and total cannot be zero.
func TestVolumeSpaceOfTempDir(t *testing.T) {
	free, total, err := VolumeSpace(os.TempDir())
	if err == ErrNoSpaceInfo {
		t.Skip(err)
	}
	if err != nil {
		t.Fatal(err)
	}
	if total <= 0 || free < 0 || free > total {
		t.Errorf("VolumeSpace = %d free of %d", free, total)
	}
}
