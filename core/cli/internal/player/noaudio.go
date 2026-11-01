//go:build noaudio

package player

// Available reports whether this build can play audio at all. This one is
// built with -tags noaudio: the headless tests, which must never open an
// audio device.
const Available = false

func openDevice() (device, error) { return nil, ErrUnavailable }
