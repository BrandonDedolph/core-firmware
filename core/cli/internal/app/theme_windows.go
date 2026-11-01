//go:build windows

package app

import "golang.org/x/sys/windows/registry"

// systemDark is Windows' "Choose your app mode": AppsUseLightTheme under
// the user's Personalize key, 0 for Dark. A missing key or value (older
// Windows, a stripped image) is light, which is what those systems are.
func systemDark() bool {
	k, err := registry.OpenKey(registry.CURRENT_USER,
		`Software\Microsoft\Windows\CurrentVersion\Themes\Personalize`, registry.QUERY_VALUE)
	if err != nil {
		return false
	}
	defer k.Close()
	v, _, err := k.GetIntegerValue("AppsUseLightTheme")
	if err != nil {
		return false
	}
	return v == 0
}
