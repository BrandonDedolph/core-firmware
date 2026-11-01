//go:build !windows

package app

// systemDark has no signal to read off Windows yet (the plan leaves
// macOS and Linux dark-mode detection out of this pass), so System is
// Linen there.
func systemDark() bool { return false }
